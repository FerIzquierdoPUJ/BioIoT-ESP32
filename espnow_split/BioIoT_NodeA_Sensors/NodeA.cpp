// Nodo A: integracion de adquisicion, calibracion persistente, enlace ESP-NOW,
// diagnosticos y Serial. Ver BioIoT_NodeA_Sensors.ino.
#include "NodeA.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <BioIoTCommon.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>
#include <time.h>

#include <new>

#include "CalibrationLogicA.h"
#include "SensorMathA.h"
#include "SensorsA.h"
#include "node_a_config.h"

#if __has_include("node_secrets.h")
#include "node_secrets.h"
#else
#include "node_secrets.example.h"
#endif

using namespace bioiot;
using namespace nodea;

namespace {

// ---------------- Secretos ----------------
const uint8_t kGatewayMac[6] = BIOIOT_GATEWAY_MAC;
const uint8_t kPmk[kEspNowKeySize] = BIOIOT_ESPNOW_PMK;
const uint8_t kLmk[kEspNowKeySize] = BIOIOT_LINK_LMK;
const uint8_t kAppKey[kAppKeySize] = BIOIOT_LINK_APP_KEY;

bool allZero(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i)
    if (p[i]) return false;
  return true;
}
bool secretsArePlaceholders() {
#ifdef BIOIOT_SECRETS_PLACEHOLDER
  return true;
#else
  return BIOIOT_SYSTEM_ID == 0 || allZero(kGatewayMac, 6) || allZero(kPmk, 16) || allZero(kLmk, 16) ||
         allZero(kAppKey, 32);
#endif
}

uint8_t resetReasonCode() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return kResetPowerOn;
    case ESP_RST_EXT: return kResetExternal;
    case ESP_RST_SW: return kResetSoftware;
    case ESP_RST_PANIC: return kResetPanic;
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return kResetWatchdog;
    case ESP_RST_BROWNOUT: return kResetBrownout;
    case ESP_RST_DEEPSLEEP: return kResetDeepSleep;
    default: return kResetUnknown;
  }
}

// ---------------- Almacenamiento (NVS, doble ranura) ----------------
class PrefsSlots : public SlotBackend {
 public:
  size_t readSlot(uint8_t slot, uint8_t* buf, size_t cap) override {
    Preferences p;
    if (!p.begin("bioiot_a", true)) return 0;  // namespace ausente en primer arranque
    const char* key = slot ? "s1" : "s0";
    const size_t len = p.getBytesLength(key);
    const size_t n = (len && len <= cap) ? p.getBytes(key, buf, len) : 0;
    p.end();
    return n;
  }
  bool writeSlot(uint8_t slot, const uint8_t* buf, size_t len) override {
    Preferences p;
    if (!p.begin("bioiot_a", false)) return false;
    const bool ok = p.putBytes(slot ? "s1" : "s0", buf, len) == len;
    p.end();
    return ok;
  }
};

constexpr uint16_t kStoreFormat = 1;
constexpr size_t kConfigBytes = 6;  // reportIntervalMs u32 + expectedMask u16

PrefsSlots g_slots;
RecordStore g_store(g_slots, kStoreFormat);
NodeACal g_cal;
uint32_t g_reportIntervalMs = NODE_A_REPORT_INTERVAL_MS;
uint16_t g_expectedMask = 0;

uint16_t defaultExpectedMask() {
  uint16_t m = 0;
  if (EXPECT_PH_DEFAULT) m |= 1u << kSensorPh;
  if (EXPECT_CO2_1_DEFAULT) m |= 1u << kSensorCo2_1;
  if (EXPECT_CO2_2_DEFAULT) m |= 1u << kSensorCo2_2;
  if (EXPECT_TURB_DEFAULT) m |= 1u << kSensorTurbidity;
  if (EXPECT_DO_DEFAULT) m |= 1u << kSensorDissolvedOxygen;
  if (EXPECT_TDS_DEFAULT) m |= 1u << kSensorTds;
  if (EXPECT_TEMP_DEFAULT) m |= 1u << kSensorTemperature;
  if (EXPECT_TCS1_DEFAULT) m |= 1u << kSensorColor1;
  if (EXPECT_TCS2_DEFAULT) m |= 1u << kSensorColor2;
  return m;
}

bool storageWritable() { return g_store.state() != StoreState::FutureFormat; }

bool persist(const NodeACal& cal, uint32_t interval, uint16_t expected) {
  uint8_t buf[kNodeACalEncodedSize + kConfigBytes];
  if (encodeNodeACal(cal, buf, sizeof(buf)) != kNodeACalEncodedSize) return false;
  Writer w(buf + kNodeACalEncodedSize, kConfigBytes);
  w.u32(interval); w.u16(expected);
  const bool ok = g_store.save(buf, sizeof(buf));
  Serial.printf("NVS nodo A: generacion %lu %s\n", (unsigned long)g_store.generation(), ok ? "guardada" : "ESCRITURA FALLIDA");
  return ok;
}

void loadPersistent() {
  uint8_t buf[kStoreMaxPayload];
  const size_t n = g_store.load(buf, sizeof(buf));
  g_expectedMask = defaultExpectedMask();
  if (n == kNodeACalEncodedSize + kConfigBytes && decodeNodeACal(buf, kNodeACalEncodedSize, g_cal)) {
    Reader r(buf + kNodeACalEncodedSize, kConfigBytes);
    const uint32_t interval = r.u32();
    const uint16_t expected = r.u16();
    if (interval >= kMinReportIntervalMs && interval <= kMaxReportIntervalMs) g_reportIntervalMs = interval;
    g_expectedMask = expected;
    Serial.printf("Calibracion nodo A cargada (rev nodo %lu).\n", (unsigned long)g_cal.nodeRevision);
    return;
  }
  g_cal = NodeACal();  // curvas iniciales vigentes; NO hereda la NVS del firmware v4
  if (g_store.state() == StoreState::FutureFormat) {
    Serial.println("NVS con formato futuro: escrituras bloqueadas, se usan curvas iniciales en RAM.");
  } else {
    Serial.printf("NVS nodo A: %s; curvas iniciales. Importar calibraciones v4 (ver MIGRACION.md).\n",
                  storeStateName(g_store.state()));
    persist(g_cal, g_reportIntervalMs, g_expectedMask);
  }
}

// ---------------- Radio ----------------
class Esp32Channel : public ChannelControl {
 public:
  explicit Esp32Channel(EspNowPort& port) : port_(port) {}
  bool setChannel(uint8_t ch) override { return port_.setChannel(ch); }
  uint8_t channel() const override { return port_.channel(); }
  void persistChannel(uint8_t ch) override {
    Preferences p;
    if (p.begin("bioiot_a", false)) { p.putUChar("ch", ch); p.end(); }
    Serial.printf("Canal ESP-NOW confirmado %u guardado.\n", ch);
  }

 private:
  EspNowPort& port_;
};

uint32_t hwRandom() { return esp_random(); }

EspNowPort g_port;
Esp32Channel g_channel(g_port);
Endpoint* g_ep = nullptr;
NodeLink* g_link = nullptr;
bool g_radioEnabled = false;
bool g_selfTestOk = false;
uint8_t g_resetReason = 0;

AnalogSampler g_analog;
ColorSampler g_color;
TempSampler g_temp;

// ---------------- Comandos ----------------
CommandQueue<4> g_commands;
CommandCache g_cmdCache;

// ---------------- Diagnostico ----------------
struct DiagState {
  bool active = false, fromSerial = false;
  uint8_t scope = 0;
  uint32_t requestId = 0, startedMs = 0, analogTarget = 0, colorTarget = 0;
  bool waitAnalog = false, waitColor = false, waitTemp = false;
  uint32_t reportSeq = 0;
} g_diag;
char g_diagBuf[kDiagMaxBytes];
DiagSender g_diagSender;

LineAssembler<2048> g_serialLine;
uint32_t g_snapshotId = 0;
uint32_t g_lastReportMs = 0, g_reportJitter = 0, g_lastPlaceholderLogMs = 0;

bool expected(uint8_t sensor) { return (g_expectedMask >> sensor) & 1u; }

// ---------------- Telemetria ----------------
TelemetryRecord analogRecord(uint8_t sensor, uint8_t idx, uint16_t calRev) {
  TelemetryRecord rec;
  rec.sensor = sensor;
  const AnalogResult& a = g_analog.result(idx);
  rec.flags = uint16_t((a.connected ? kRecConnected : 0) | (expected(sensor) ? kRecExpected : 0) |
                       (a.sampled ? kRecObserved : 0));
  rec.calRevision = calRev;
  rec.sampleUptimeMs = a.sampledAtMs;
  rec.valueCount = val::kAnalogCount;
  rec.vals[val::kAnalogRaw] = a.sampled ? float(a.raw) : NAN;
  rec.vals[val::kAnalogVoltage] = a.sampled ? a.voltage : NAN;
  float value = NAN;
  if (a.sampled && a.connected) {
    if (sensor == kSensorPh) value = calculatePh(g_cal, a.voltage);
    else if (sensor == kSensorTurbidity) value = calculateTurbidityNtu(g_cal, a.voltage);
    else if (sensor == kSensorTds) value = calculateTdsPpm(a.voltage);
  }
  rec.vals[val::kAnalogValue] = value;
  rec.quality = a.sampled ? analogQuality(a.connected, isfinite(value)) : kQNotSampled;
  return rec;
}

TelemetryRecord co2Record(uint8_t i) {
  const uint8_t sensor = i == 0 ? kSensorCo2_1 : kSensorCo2_2;
  const AnalogResult& a = g_analog.result(i == 0 ? kACo2_1 : kACo2_2);
  const Co2Calibration& c = g_cal.co2[i];
  TelemetryRecord rec;
  rec.sensor = sensor;
  rec.calRevision = g_cal.rev[kCalCo2_1 + i];
  rec.sampleUptimeMs = a.sampledAtMs;
  rec.valueCount = val::kCo2Count;
  rec.vals[val::kCo2Raw] = a.sampled ? float(a.raw) : NAN;
  rec.vals[val::kCo2Voltage] = a.sampled ? a.voltage : NAN;
  uint16_t flags = uint16_t((a.connected ? kRecConnected : 0) | (expected(sensor) ? kRecExpected : 0) |
                            (a.sampled ? kRecObserved : 0) | (c.provisional ? kRecProvisional : 0) |
                            (c.mode ? kRecCalLegacyMode : 0) | (c.user ? kRecCalUser : 0) |
                            (c.enabled ? kRecCalEnabled : 0));
  if (a.sampled) {
    const Co2Reading r = evaluateCo2(a.voltage, a.connected, c);  // CalibrationModel.h v4
    rec.vals[val::kCo2Value] = r.value;
    rec.vals[val::kCo2ModuleV] = r.moduleVoltage;
    rec.vals[val::kCo2SensorV] = r.sensorVoltage;
    if (r.valid) flags |= kRecValid;
    if (r.below) flags |= kRecBelowRef;
    rec.quality = qualityFromName(r.quality);
  } else {
    rec.vals[val::kCo2Value] = rec.vals[val::kCo2ModuleV] = rec.vals[val::kCo2SensorV] = NAN;
    rec.quality = kQNotSampled;
  }
  rec.flags = flags;
  return rec;
}

float currentCalibratedTemp(uint32_t nowMs) {
  const TempResult& t = g_temp.result();
  if (!t.sampled || !t.connected || nowMs - t.sampledAtMs > NODE_A_DO_TEMP_MAX_AGE_MS) return NAN;
  return calibrateTemperature(g_cal, t.raw);
}

TelemetryRecord doRecord(uint32_t nowMs) {
  const AnalogResult& a = g_analog.result(kADo);
  TelemetryRecord rec;
  rec.sensor = kSensorDissolvedOxygen;
  rec.flags = uint16_t((a.connected ? kRecConnected : 0) | (expected(kSensorDissolvedOxygen) ? kRecExpected : 0) |
                       (a.sampled ? kRecObserved : 0));
  rec.calRevision = g_cal.rev[kCalDo];
  rec.sampleUptimeMs = a.sampledAtMs;
  rec.valueCount = val::kDoCount;
  rec.vals[val::kDoRaw] = a.sampled ? float(a.raw) : NAN;
  rec.vals[val::kDoVoltage] = a.sampled ? a.voltage : NAN;
  const bool use = a.sampled && a.connected;
  const float sat = use ? calculateDoSaturationPct(g_cal, a.voltage) : NAN;
  const float mgl = use ? calculateDoMgL(g_cal, a.voltage, currentCalibratedTemp(nowMs)) : NAN;
  rec.vals[val::kDoSatPct] = sat;
  rec.vals[val::kDoValue] = mgl;
  rec.quality = a.sampled ? analogQuality(a.connected, isfinite(mgl)) : kQNotSampled;
  return rec;
}

TelemetryRecord tempRecord() {
  const TempResult& t = g_temp.result();
  TelemetryRecord rec;
  rec.sensor = kSensorTemperature;
  rec.flags = uint16_t((t.connected ? kRecConnected : 0) | (expected(kSensorTemperature) ? kRecExpected : 0) |
                       (t.sampled ? kRecObserved : 0) | (g_cal.temp_user ? kRecCalUser : 0));
  rec.calRevision = g_cal.rev[kCalTemp];
  rec.sampleUptimeMs = t.sampledAtMs;
  rec.valueCount = val::kTempCount;
  rec.vals[val::kTempRaw] = t.sampled ? t.raw : NAN;
  const float value = t.sampled && t.connected ? calibrateTemperature(g_cal, t.raw) : NAN;
  rec.vals[val::kTempValue] = value;
  rec.quality = t.sampled ? analogQuality(t.connected, isfinite(value)) : kQNotSampled;
  return rec;
}

TelemetryRecord colorRecord(uint8_t i) {
  const ColorResult& c = g_color.result(i);
  const uint8_t sensor = i == 0 ? kSensorColor1 : kSensorColor2;
  TelemetryRecord rec;
  rec.sensor = sensor;
  rec.flags = uint16_t((c.connected ? kRecConnected : 0) | (expected(sensor) ? kRecExpected : 0) |
                       (c.sampled ? kRecObserved : 0));
  rec.sampleUptimeMs = c.sampledAtMs;
  rec.valueCount = val::kColorCount;
  if (c.sampled) {
    rec.vals[val::kColorRPulse] = float(c.rPulse); rec.vals[val::kColorGPulse] = float(c.gPulse);
    rec.vals[val::kColorBPulse] = float(c.bPulse);
    rec.vals[val::kColorR] = c.r; rec.vals[val::kColorG] = c.g; rec.vals[val::kColorB] = c.b;
    rec.vals[val::kColorH] = c.h; rec.vals[val::kColorS] = c.s; rec.vals[val::kColorL] = c.l;
    rec.quality = analogQuality(c.connected, isfinite(c.h) && isfinite(c.s) && isfinite(c.l));
  } else {
    for (uint8_t v = 0; v < val::kColorCount; ++v) rec.vals[v] = NAN;
    rec.quality = kQNotSampled;
  }
  return rec;
}

void sendTelemetryPart(TelemetryMsg& t, uint32_t nowMs) {
  uint8_t buf[kMaxPayload];
  t.txUptimeMs = nowMs;
  const size_t n = encodeTelemetry(t, buf, sizeof(buf));
  if (!n) { Serial.println("Telemetria: codificacion fallida (no enviada)."); return; }
  SendOptions o;
  o.cookie = t.snapshotId;
  o.ttlMs = NODE_A_TELEMETRY_TTL_MS;
  o.persistent = true;
  o.evictable = true;
  o.rebindOnReboot = true;  // las muestras siguen siendo validas para un gateway reiniciado
  o.txTimeOffset = int16_t(kTelemetryTxTimeOffset);
  g_ep->sendReliable(kNodeGateway, MsgType::Telemetry, buf, n, o, nowMs);
}

void reportTelemetry(uint32_t nowMs) {
  g_snapshotId++;
  TelemetryMsg t0;
  t0.snapshotId = g_snapshotId; t0.part = 0; t0.parts = 2;
  t0.records[t0.recordCount++] = analogRecord(kSensorPh, kAPh, g_cal.rev[kCalPh]);
  t0.records[t0.recordCount++] = co2Record(0);
  t0.records[t0.recordCount++] = co2Record(1);
  t0.records[t0.recordCount++] = analogRecord(kSensorTurbidity, kATurb, g_cal.rev[kCalTurb]);
  t0.records[t0.recordCount++] = doRecord(nowMs);
  t0.records[t0.recordCount++] = analogRecord(kSensorTds, kATds, 0);
  t0.records[t0.recordCount++] = tempRecord();
  sendTelemetryPart(t0, nowMs);
  TelemetryMsg t1;
  t1.snapshotId = g_snapshotId; t1.part = 1; t1.parts = 2;
  t1.records[t1.recordCount++] = colorRecord(0);
  t1.records[t1.recordCount++] = colorRecord(1);
  sendTelemetryPart(t1, nowMs);
}

// ---------------- Resultados y estado de calibracion ----------------
void sendCalState(uint32_t nowMs) {
  if (!g_radioEnabled) return;
  CalStateMsg s;
  s.node = kNodeA;
  s.storageState = g_store.state() == StoreState::Ok ? kCalStorageOk
                 : g_store.state() == StoreState::FutureFormat ? kCalStorageFuture
                 : g_store.lastWriteOk() ? kCalStorageDefaults : kCalStorageFailed;
  s.blobLen = uint8_t(encodeNodeACal(g_cal, s.blob, sizeof(s.blob)));
  uint8_t buf[kMaxPayload];
  const size_t n = encodeCalState(s, buf, sizeof(buf));
  SendOptions o;
  o.ttlMs = 120000; o.persistent = true; o.rebindOnReboot = true;
  if (n) g_ep->sendReliable(kNodeGateway, MsgType::CalState, buf, n, o, nowMs);
}

void sendResult(const CommandResultMsg& r, uint32_t gatewayBoot, uint32_t nowMs) {
  g_cmdCache.store(gatewayBoot, r);
  if (!g_radioEnabled) return;
  uint8_t buf[kCommandResultSize];
  const size_t n = encodeCommandResult(r, buf, sizeof(buf));
  SendOptions o;
  o.cookie = r.cmdId; o.ttlMs = 120000; o.persistent = true; o.maxAttempts = 5;
  if (n) g_ep->sendReliable(kNodeGateway, MsgType::CommandResult, buf, n, o, nowMs);
}

void startDiagnostics(uint8_t scope, uint32_t requestId, bool fromSerial, uint32_t nowMs);

void processCommand(const PendingCommand& pc, uint32_t nowMs) {
  // Identificador comun en los primeros 4 bytes de todos los comandos.
  const uint32_t cmdId = getU32(pc.payload);
  if (const CommandResultMsg* cached = g_cmdCache.find(pc.gatewayBoot, cmdId)) {
    sendResult(*cached, pc.gatewayBoot, nowMs);  // idempotente: no se re-ejecuta
    return;
  }
  CommandResultMsg res;
  res.cmdId = cmdId;
  res.msgType = uint8_t(pc.type);
  if (pc.type == MsgType::Calibration) {
    CalibrationCmd cmd;
    if (!decodeCalibration(pc.payload, pc.len, cmd)) return;
    res.op = cmd.op; res.target = cmd.target;
    NodeACal candidate;
    CalApplyResult r = applyCalibrationA(g_cal, cmd, storageWritable(), candidate);
    if (r.needsPersist) {
      if (persist(candidate, g_reportIntervalMs, g_expectedMask)) {
        g_cal = candidate;  // activa SOLO tras guardar y verificar
      } else {
        r.outcome = kOutcomeRejected;
        r.status = kStRejectedStorageFailed;
      }
    }
    res.outcome = r.outcome; res.status = r.status;
    res.nodeRevision = g_cal.nodeRevision;
    res.sensorRevision = sensorRevisionForTarget(g_cal, cmd.target);
    res.storageOk = g_store.lastWriteOk() && storageWritable();
    sendResult(res, pc.gatewayBoot, nowMs);
    if (res.outcome == kOutcomeApplied) sendCalState(nowMs);
  } else if (pc.type == MsgType::Config) {
    ConfigCmd cfg;
    if (!decodeConfig(pc.payload, pc.len, cfg)) return;
    uint32_t interval = (cfg.present & 1) ? cfg.reportIntervalMs : g_reportIntervalMs;
    uint16_t mask = (cfg.present & 2) ? uint16_t(cfg.expectedMask & 0x1FF) : g_expectedMask;
    const bool ok = storageWritable() && persist(g_cal, interval, mask);
    if (ok) { g_reportIntervalMs = interval; g_expectedMask = mask; }
    res.outcome = ok ? kOutcomeApplied : kOutcomeRejected;
    res.status = ok ? kStConfigSaved : kStRejectedStorageFailed;
    res.nodeRevision = g_cal.nodeRevision;
    res.storageOk = ok;
    sendResult(res, pc.gatewayBoot, nowMs);
  } else if (pc.type == MsgType::Command) {
    GenericCmd g;
    if (!decodeGeneric(pc.payload, pc.len, g)) return;
    res.op = g.op;
    if (g.op == kOpSendState) {
      sendCalState(nowMs);
      g_link->requestStatusSoon();
      res.outcome = kOutcomeApplied; res.status = kStStateSent;
      sendResult(res, pc.gatewayBoot, nowMs);
    } else if (g.op == kOpReboot) {
      res.outcome = kOutcomeStarted; res.status = kStRebooting;
      sendResult(res, pc.gatewayBoot, nowMs);
      // Esperar la entrega del resultado (max 3 s) antes de reiniciar.
      const uint32_t t0 = millis();
      while (g_ep->hasPending(kNodeGateway, MsgType::CommandResult, cmdId) && millis() - t0 < 3000) {
        RxFrame f;
        while (g_port.pop(f)) g_link->deliver(f.data, f.len, millis(), f.rssi);
        g_link->loop(millis());
        esp_task_wdt_reset();
        delay(10);
      }
      Serial.println("Reinicio solicitado por el gateway.");
      delay(100);
      ESP.restart();
    }
  } else if (pc.type == MsgType::DiagRequest) {
    DiagRequestMsg d;
    if (!decodeDiagRequest(pc.payload, pc.len, d)) return;
    startDiagnostics(d.scope, d.requestId, false, nowMs);
  }
}

class AppA : public NodeApp {
 public:
  uint8_t onGatewayMessage(const FrameHeader& h, const uint8_t* payload, size_t len) override {
    bool valid = false;
    switch (h.type) {
      case MsgType::Calibration: { CalibrationCmd c; valid = decodeCalibration(payload, len, c); break; }
      case MsgType::Config: { ConfigCmd c; valid = decodeConfig(payload, len, c); break; }
      case MsgType::Command: { GenericCmd c; valid = decodeGeneric(payload, len, c); break; }
      case MsgType::DiagRequest: { DiagRequestMsg d; valid = decodeDiagRequest(payload, len, d); break; }
      default: return kAckUnsupported;
    }
    if (!valid) return kAckRejectedInvalid;
    // ACK = recibido y encolado. La ejecucion y persistencia se informan en COMMAND_RESULT.
    return g_commands.push(h.type, payload, len, h.srcBoot) ? kAckAccepted : kAckQueueFull;
  }
  void onGatewaySession(uint32_t gatewayBoot) override {
    Serial.printf("Sesion con gateway confirmada (boot %08lx, canal %u).\n", (unsigned long)gatewayBoot,
                  g_port.channel());
    sendCalState(millis());
  }
  void fillStatus(StatusMsg& s) override {
    const uint32_t nowMs = millis();
    s.freeHeap = ESP.getFreeHeap();
    s.minFreeHeap = ESP.getMinFreeHeap();
    s.maxAllocHeap = ESP.getMaxAllocHeap();
    s.flags = uint16_t((g_store.state() == StoreState::FutureFormat ? kSfStorageFuture : 0) |
                       (g_diag.active ? kSfDiagBusy : 0));
    s.reportIntervalMs = g_reportIntervalMs;
    uint16_t d = 0;
    if (g_analog.batchValid) d |= kDiagAAnalogSampled;
    if (g_analog.allNearZero) d |= kDiagAAllNearZero;
    if (g_analog.tooSimilar) d |= kDiagATooSimilar;
    for (uint8_t i = 0; i < 2; ++i) {
      const ColorResult& c = g_color.result(i);
      if (c.sampled) d |= i == 0 ? kDiagAColor1Sampled : kDiagAColor2Sampled;
      if (c.sampled && c.connected) d |= i == 0 ? kDiagAColor1PulseOk : kDiagAColor2PulseOk;
    }
    if (g_temp.result().sampled) d |= kDiagATempSampled;
    if (g_temp.result().connected) d |= kDiagATempDetected;
    s.diagFlags = d;
    s.diagCounter = g_analog.allZeroCount;
    s.diagSampledAgeMs = g_analog.lastSweepMs ? nowMs - g_analog.lastSweepMs : 0xFFFFFFFFu;
  }
} g_app;

// ---------------- Diagnosticos ----------------
void writeSystem(JsonObject o, uint32_t nowMs) {
  o["uptime_ms"] = nowMs;
  o["free_heap"] = ESP.getFreeHeap();
  o["min_free_heap"] = ESP.getMinFreeHeap();
  o["max_alloc_heap"] = ESP.getMaxAllocHeap();
  o["reset_reason"] = resetReasonText(g_resetReason);
  o["protocol_self_test_ok"] = g_selfTestOk;
  o["storage_state"] = storeStateName(g_store.state());
  o["storage_generation"] = g_store.generation();
  o["report_interval_ms"] = g_reportIntervalMs;
  JsonObject link = o["espnow"].to<JsonObject>();
  link["enabled"] = g_radioEnabled;
  link["provisioning_required"] = secretsArePlaceholders();
  link["encrypted"] = g_port.encrypted();
  if (g_radioEnabled) {
    link["online"] = g_link->online(nowMs);
    link["channel"] = g_port.channel();
    link["channel_state"] = channelStateName(g_link->channelState());
    link["channel_hunts"] = g_link->hunts();
    link["gateway_boot"] = g_ep->peerBoot(kNodeGateway);
    link["queue_depth"] = g_ep->queueDepth();
    const LinkStats& st = g_ep->stats();
    link["tx_frames"] = st.txFrames; link["retries"] = st.retries; link["delivered"] = st.delivered;
    link["failed"] = st.failed; link["dropped"] = st.dropped; link["expired"] = st.expired;
    link["rx_ok"] = st.rxOk; link["rx_rejected"] = st.rxRejected; link["rx_bad_tag"] = st.rxBadTag;
    link["rx_ring_dropped"] = g_port.rxDropped();
    link["driver_send_ok"] = g_port.sendOk; link["driver_send_fail"] = g_port.sendFail;
    link["utc_synced"] = g_link->utc().utcValid(nowMs);
  }
}

void writeAnalog(JsonObject o, JsonArray findings) {
  o["sig_gpio"] = MUX_SIG_PIN;
  o["enable_wiring_configured"] = "GND (not measured)";
  o["mux_profile"] = NODE_A_MUX_PROFILE == NODE_A_MUX_PROFILE_WORKING_COPY ? "working_copy_15_14_13_3_12_11"
                                                                            : "sequential_0_5";
  o["all_near_zero"] = g_analog.allNearZero;
  o["channels_too_similar"] = g_analog.tooSimilar;
  o["similar_spread_threshold"] = kAnalogSimilarSpread;
  o["min_raw"] = g_analog.minAverage;
  o["max_raw"] = g_analog.maxAverage;
  o["spread"] = g_analog.maxAverage - g_analog.minAverage;
  o["all_zero_count"] = g_analog.allZeroCount;
  o["suspected_fault"] = g_analog.allNearZero || g_analog.tooSimilar;
  o["suspected_area"] = g_analog.allNearZero ? "mux_common_path_power_enable_or_sig"
                      : g_analog.tooSimilar ? "mux_channel_selection_common_path_or_similar_inputs" : "none";
  JsonObject channels = o["channels"].to<JsonObject>();
  for (uint8_t i = 0; i < kAnalogChannels; ++i) {
    const AnalogResult& a = g_analog.result(i);
    JsonObject ch = channels[String(i)].to<JsonObject>();
    ch["sensor"] = kAnalogSensorNames[i];
    ch["channel"] = kAnalogMuxChannel[i];
    ch["samples"] = a.sampled ? kAnalogSamples : 0;
    ch["raw_min"] = a.rawMin; ch["raw_max"] = a.rawMax; ch["raw_avg"] = a.rawAvg;
    ch["raw_span"] = a.rawMax - a.rawMin; ch["voltage_avg"] = a.voltageAvg;
    ch["sampled_at_ms"] = a.sampledAtMs;
    if (i == kACo2_1 || i == kACo2_2) {
      const uint8_t k = i == kACo2_1 ? 0 : 1;
      const Co2Calibration& c = g_cal.co2[k];
      const Co2Reading r = evaluateCo2(a.voltage, a.connected, c);
      JsonObject m = ch["measurement"].to<JsonObject>();
      m["model"] = "SEN0159"; m["adc_voltage"] = a.voltage;
      m["module_voltage"] = r.moduleVoltage; m["sensor_voltage"] = r.sensorVoltage;
      m["zero_point_v"] = c.zero_point_v; m["reaction_voltage_v"] = c.reaction_voltage_v;
      m["dc_gain"] = SEN0159_DC_GAIN; m["divider_ratio"] = CO2_DIVIDER_RATIO;
      if (isfinite(r.value)) m["value"] = r.value; else m["value"] = nullptr;
      m["quality"] = r.quality;
    }
    ch["connection_confidence"] = !a.sampled ? "unknown"
        : g_analog.tooSimilar || a.rawAvg <= 5 || a.rawAvg >= 4090 ? "weak_evidence" : "plausible_signal";
  }
  if (g_analog.allNearZero) findings.add("analog_mux_common_path_suspected");
  if (g_analog.tooSimilar) findings.add("analog_mux_channels_suspiciously_similar");
}

void writeColor(JsonObject o, JsonArray findings) {
  bool ok[2], zero[2];
  for (uint8_t i = 0; i < 2; ++i) {
    const ColorResult& c = g_color.result(i);
    JsonObject x = o[i == 0 ? "color_1" : "color_2"].to<JsonObject>();
    x["out_gpio"] = i == 0 ? TCS_OUT_1 : TCS_OUT_2;
    x["sampled"] = c.sampled; x["sampled_at_ms"] = c.sampledAtMs;
    x["rPulse"] = c.rPulse; x["gPulse"] = c.gPulse; x["bPulse"] = c.bPulse;
    x["r_timeout"] = c.rPulse == 0; x["g_timeout"] = c.gPulse == 0; x["b_timeout"] = c.bPulse == 0;
    zero[i] = c.sampled && c.rPulse == 0 && c.gPulse == 0 && c.bPulse == 0;
    ok[i] = c.sampled && c.connected;
    x["all_pulses_zero"] = zero[i];
    x["pulse_ok"] = ok[i];
    if (!ok[i]) findings.add(i == 0 ? "color_1_pulse_failure" : "color_2_pulse_failure");
  }
  const bool individual1 = zero[0] && ok[1];
  const bool individual2 = zero[1] && ok[0];
  o["shared_control_lines_likely_working"] = individual1 || individual2;
  o["suspected_area"] = individual1 ? "tcs1_power_oe_out_or_gpio34" : individual2 ? "tcs2_power_oe_out_or_gpio35" : "not_localized";
  if (individual1 || individual2) {
    findings.add(individual1 ? "color_1_individual_path_suspected" : "color_2_individual_path_suspected");
    findings.add("tcs_shared_control_lines_likely_working");
  }
}

void writeTemperature(JsonObject o, JsonArray findings) {
  const TempResult& t = g_temp.result();
  o["gpio"] = ONE_WIRE_BUS;
  o["device_count"] = t.deviceCount;
  o["device_count_source"] = "DallasTemperature last bus enumeration";
  o["detected"] = t.connected;
  o["sampled_at_ms"] = t.sampledAtMs;
  if (t.sampled && isfinite(t.raw)) o["raw_temperature"] = t.raw; else o["raw_temperature"] = nullptr;
  if (!t.connected) findings.add("ds18b20_no_valid_reading");
}

size_t buildDiagnosticJson(uint32_t nowMs) {
  JsonDocument doc;
  doc["node"] = "node_a";
  doc["report_id"] = g_diag.requestId;
  doc["scope"] = diagScopeName(g_diag.scope);
  doc["uptime_ms"] = nowMs;
  doc["started_uptime_ms"] = g_diag.startedMs;
  JsonArray findings = doc["findings"].to<JsonArray>();
  const uint8_t s = g_diag.scope;
  writeSystem(doc["system"].to<JsonObject>(), nowMs);
  if (s == kDiagI2c || s == kDiagRecover || s == kDiagO2) {
    doc["not_applicable"] = true;  // el bus I2C y el O2 estan en el nodo B
  }
  if (s == kDiagFull || s == kDiagAnalog || s == kDiagQuick) writeAnalog(doc["analog_mux"].to<JsonObject>(), findings);
  if (s == kDiagFull || s == kDiagColor || s == kDiagQuick) writeColor(doc["color"].to<JsonObject>(), findings);
  if (s == kDiagFull || s == kDiagTemperature) writeTemperature(doc["temperature"].to<JsonObject>(), findings);
  doc["fresh_sampling"] = s != kDiagQuick && s != kDiagSystem;
  doc["physical_fault_confirmed"] = false;
  const size_t need = measureJson(doc);
  if (doc.overflowed() || need >= sizeof(g_diagBuf)) {
    return snprintf(g_diagBuf, sizeof(g_diagBuf),
                    "{\"node\":\"node_a\",\"report_id\":%lu,\"error\":\"diagnostic_report_too_large\"}",
                    (unsigned long)g_diag.requestId);
  }
  return serializeJson(doc, g_diagBuf, sizeof(g_diagBuf));
}

void startDiagnostics(uint8_t scope, uint32_t requestId, bool fromSerial, uint32_t nowMs) {
  if (g_diag.active || g_diagSender.active()) {
    const int n = snprintf(g_diagBuf, sizeof(g_diagBuf),
                           "{\"node\":\"node_a\",\"report_id\":%lu,\"error\":\"diagnostics_busy\"}",
                           (unsigned long)requestId);
    if (fromSerial) Serial.println(g_diagBuf);
    else if (g_radioEnabled && !g_diagSender.active()) g_diagSender.start(requestId, (const uint8_t*)g_diagBuf, n);
    return;
  }
  const uint32_t seq = g_diag.reportSeq;
  g_diag = DiagState();
  g_diag.reportSeq = seq;
  g_diag.active = true;
  g_diag.fromSerial = fromSerial;
  g_diag.scope = scope;
  g_diag.requestId = requestId;
  g_diag.startedMs = nowMs;
  const bool fresh = scope != kDiagQuick && scope != kDiagSystem;
  if (fresh && (scope == kDiagFull || scope == kDiagAnalog)) {
    // Un barrido ya en curso empezo antes de la peticion: se espera el siguiente completo.
    g_diag.waitAnalog = true;
    g_diag.analogTarget = g_analog.sweeps() + (g_analog.busy() ? 2 : 1);
    g_analog.requestSweep();
  }
  if (fresh && (scope == kDiagFull || scope == kDiagColor)) {
    g_diag.waitColor = true;
    g_diag.colorTarget = g_color.reads() + (g_color.busy() ? 4 : 2);
    g_color.requestRead();
  }
  if (fresh && (scope == kDiagFull || scope == kDiagTemperature)) {
    g_diag.waitTemp = true; g_temp.requestRead();
  }
  Serial.printf("Diagnostico #%lu (%s) iniciado.\n", (unsigned long)requestId, diagScopeName(scope));
}

void serviceDiagnostics(uint32_t nowMs) {
  if (g_diag.active) {
    // Espera por estados, nunca bloqueante; plazo maximo 10 s.
    const bool analogDone = !g_diag.waitAnalog || g_analog.sweeps() >= g_diag.analogTarget;
    const bool colorDone = !g_diag.waitColor || g_color.reads() >= g_diag.colorTarget;
    const bool tempDone = !g_diag.waitTemp || (!g_temp.busy() && nowMs - g_diag.startedMs > 900);
    if ((analogDone && colorDone && tempDone) || nowMs - g_diag.startedMs > 10000) {
      g_diag.active = false;
      const size_t n = buildDiagnosticJson(nowMs);
      Serial.println(g_diagBuf);
      Serial.printf("Diagnostico nodo A: %u bytes.\n", unsigned(n));
      if (!g_diag.fromSerial && g_radioEnabled) g_diagSender.start(g_diag.requestId, (const uint8_t*)g_diagBuf, n);
    }
  }
  if (g_radioEnabled) g_diagSender.pump(*g_ep, kNodeGateway, nowMs);
}

// ---------------- Serial ----------------
void printStatus(uint32_t nowMs) {
  JsonDocument doc;
  doc["node"] = "node_a";
  writeSystem(doc["system"].to<JsonObject>(), nowMs);
  serializeJson(doc, Serial);
  Serial.println();
}

void handleSerialLine(const char* line, uint32_t nowMs) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) { Serial.println("Serial: JSON invalido."); return; }
  const char* action = doc["action"] | "";
  if (strcmp(action, "diagnostics") == 0) {
    const uint8_t scope = diagScopeFromName(doc["scope"] | "full");
    if (scope == 255) { Serial.println("Diagnostics: scope desconocido."); return; }
    startDiagnostics(scope, ++g_diag.reportSeq | 0x80000000u, true, nowMs);
  } else if (strcmp(action, "status") == 0) {
    printStatus(nowMs);
  } else if (strcmp(action, "pairing_info") == 0) {
    Serial.printf("{\"node\":\"node_a\",\"sta_mac\":\"%s\",\"channel\":%u,\"provisioning_required\":%s}\n",
                  WiFi.macAddress().c_str(), g_port.channel(), secretsArePlaceholders() ? "true" : "false");
  } else if (strcmp(action, "calibration_export") == 0) {
    JsonDocument out;
    writeCalibrationExport(out.to<JsonObject>(), "node_a", "node_a", "", &g_cal, nullptr);
    serializeJson(out, Serial);
    Serial.println();
  } else if (strcmp(action, "calibration_import") == 0) {
    NodeACal a = NodeACal();
    NodeBCal unused;
    const CalImportResult r = parseCalibrationExport(doc["export"].as<JsonObjectConst>(), a, unused);
    if (!r.formatOk || !r.hasA || !r.aValid) {
      Serial.printf("{\"result\":\"rejected\",\"error\":\"%s\"}\n", r.error);
      return;
    }
    CalibrationCmd cmd;
    cmd.op = kCalOpImport; cmd.target = kTargetAll; cmd.force = (doc["force"] | false) ? 1 : 0;
    cmd.blobLen = uint8_t(encodeNodeACal(a, cmd.blob, sizeof(cmd.blob)));
    NodeACal candidate;
    CalApplyResult ar = applyCalibrationA(g_cal, cmd, storageWritable(), candidate);
    if (ar.needsPersist) {
      if (persist(candidate, g_reportIntervalMs, g_expectedMask)) g_cal = candidate;
      else { ar.outcome = kOutcomeRejected; ar.status = kStRejectedStorageFailed; }
    }
    Serial.printf("{\"result\":\"%s\",\"status\":\"%s\",\"node_revision\":%lu}\n", outcomeName(ar.outcome),
                  statusName(ar.status), (unsigned long)g_cal.nodeRevision);
    if (ar.outcome == kOutcomeApplied) sendCalState(nowMs);
  } else {
    Serial.println("Serial: acciones admitidas: diagnostics, status, pairing_info, calibration_export, calibration_import.");
  }
}

void pollSerial(uint32_t nowMs) {
  for (uint16_t budget = 0; budget < 256 && Serial.available(); ++budget) {
    if (g_serialLine.feed(char(Serial.read()))) handleSerialLine(g_serialLine.line(), nowMs);
    else if (g_serialLine.lastOverflowed()) Serial.println("Serial: linea demasiado larga.");
  }
}

}  // namespace

void nodeASetup() {
  Serial.begin(115200);
  delay(300);
  g_resetReason = resetReasonCode();
  Serial.println("\n===== BioIoT Nodo A (ESP32 sensores analogicos/color/OneWire) =====");
  g_selfTestOk = protocolSelfTest();
  Serial.printf("Autoprueba de protocolo: %s\n", g_selfTestOk ? "OK" : "FALLO (no interoperable)");
  loadPersistent();
  g_analog.begin();
  g_color.begin();
  g_temp.begin();

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  WiFi.setSleep(false);  // ESP-NOW necesita la radio despierta
  Serial.printf("MAC STA nodo A: %s\n", WiFi.macAddress().c_str());

  if (secretsArePlaceholders()) {
    Serial.println("ESP-NOW DESHABILITADO: faltan node_secrets.h reales (ver docs/EMPAREJAMIENTO.md).");
  } else if (!g_selfTestOk) {
    Serial.println("ESP-NOW DESHABILITADO: la autoprueba de serializacion fallo.");
  } else {
    uint8_t ch = BIOIOT_INITIAL_CHANNEL;
    Preferences p;
    if (p.begin("bioiot_a", true)) { ch = p.getUChar("ch", ch); p.end(); }
    EspNowPeerConfig peer;
    peer.node = kNodeGateway;
    memcpy(peer.mac, kGatewayMac, 6);
    memcpy(peer.lmk, kLmk, kEspNowKeySize);
    if (!g_port.begin(kPmk, ch, BIOIOT_SYSTEM_ID, kNodeA) || !g_port.addPeer(peer)) {
      // Sin cifrado configurado no se transmite: nunca hay degradacion a texto plano.
      Serial.printf("ESP-NOW DESHABILITADO: %s\n", g_port.lastError());
    } else {
      // Endpoint y NodeLink se referencian mutuamente: almacenamiento estatico, sin heap.
      alignas(Endpoint) static uint8_t epStorage[sizeof(Endpoint)];
      alignas(NodeLink) static uint8_t linkStorage[sizeof(NodeLink)];
      NodeLinkConfig cfg;
      cfg.linkTimeoutMs = NODE_A_LINK_TIMEOUT_MS;
      g_ep = reinterpret_cast<Endpoint*>(epStorage);
      g_link = new (linkStorage) NodeLink(*g_ep, g_channel, g_app, cfg);
      new (epStorage) Endpoint(BIOIOT_SYSTEM_ID, kNodeA, esp_random() | 1u, g_port, *g_link, hwRandom);
      g_ep->addPeer(kNodeGateway, kAppKey);
      g_link->begin(ch, millis(), g_resetReason, g_selfTestOk, g_store.state() == StoreState::Ok);
      g_radioEnabled = true;
      Serial.printf("ESP-NOW cifrado activo en canal %u; boot %08lx.\n", g_port.channel(),
                    (unsigned long)g_ep->bootId());
    }
  }
  g_reportJitter = esp_random() % 200;
  g_lastReportMs = millis() + NODE_A_REPORT_PHASE_MS;

  // Vigilancia de loop(): todas las esperas son por estados y acotadas.
  const esp_task_wdt_config_t wdt = {
      .timeout_ms = NODE_A_LOOP_WDT_TIMEOUT_MS, .idle_core_mask = 1 << 0, .trigger_panic = true};
  if (esp_task_wdt_reconfigure(&wdt) != ESP_OK || esp_task_wdt_add(NULL) != ESP_OK)
    Serial.println("Watchdog de loop no disponible.");
  Serial.printf("Ultimo reinicio: %s\n", resetReasonText(g_resetReason));
}

void nodeALoop() {
  esp_task_wdt_reset();
  const uint32_t nowMs = millis();
  if (g_radioEnabled) {
    RxFrame f;
    for (uint8_t i = 0; i < 8 && g_port.pop(f); ++i) g_link->deliver(f.data, f.len, millis(), f.rssi);
    g_link->loop(nowMs);
    PendingCommand pc;
    if (g_commands.pop(pc)) processCommand(pc, nowMs);
  } else if (nowMs - g_lastPlaceholderLogMs > 30000) {
    g_lastPlaceholderLogMs = nowMs;
    Serial.println("Nodo A midiendo sin enlace ESP-NOW (aprovisionamiento pendiente o error de radio).");
  }
  // Adquisicion local continua, independiente del enlace y del reporte.
  g_analog.loop(nowMs);
  g_color.loop(millis());
  g_temp.loop(millis());
  serviceDiagnostics(millis());
  pollSerial(millis());
  const uint32_t now2 = millis();
  if (g_radioEnabled && int32_t(now2 - g_lastReportMs) >= int32_t(g_reportIntervalMs + g_reportJitter)) {
    g_lastReportMs = now2;
    g_reportJitter = esp_random() % 200;  // pequeno desfase aleatorio por reporte
    reportTelemetry(now2);
  }
}
