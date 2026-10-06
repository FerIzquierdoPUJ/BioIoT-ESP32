// Nodo B: integracion de sensores I2C, calibracion O2 persistente, enlace ESP-NOW,
// diagnosticos y Serial. Ver BioIoT_NodeB_I2C.ino.
#include "NodeB.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <BioIoTCommon.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>

#include <new>

#include "BusB.h"
#include "CalibrationLogicB.h"
#include "O2Logic.h"
#include "SensorsB.h"
#include "StorageGuardB.h"
#include "WarmupPolicy.h"
#include "node_b_config.h"

#if __has_include("node_secrets.h")
#include "node_secrets.h"
#else
#include "node_secrets.example.h"
#endif

extern "C" {
#include <user_interface.h>
}

using namespace bioiot;
using namespace nodeb;

namespace {

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

// ---------------- Causa de reinicio y warm-up ----------------
uint8_t g_resetReason = 0;
ResetKind g_resetKind = ResetKind::Unknown;

void readResetReason() {
  const rst_info* info = ESP.getResetInfoPtr();
  switch (info ? info->reason : 255) {
    case REASON_DEFAULT_RST: g_resetReason = kResetPowerOn; g_resetKind = ResetKind::PowerOn; break;
    case REASON_WDT_RST: case REASON_SOFT_WDT_RST:
      g_resetReason = kResetWatchdog; g_resetKind = ResetKind::SoftOrWatchdog; break;
    case REASON_EXCEPTION_RST: g_resetReason = kResetException; g_resetKind = ResetKind::SoftOrWatchdog; break;
    case REASON_SOFT_RESTART: g_resetReason = kResetSoftware; g_resetKind = ResetKind::SoftOrWatchdog; break;
    case REASON_DEEP_SLEEP_AWAKE: g_resetReason = kResetDeepSleep; g_resetKind = ResetKind::DeepSleep; break;
    case REASON_EXT_SYS_RST: g_resetReason = kResetExternal; g_resetKind = ResetKind::External; break;
    default: g_resetReason = kResetUnknown; g_resetKind = ResetKind::Unknown; break;
  }
}

// Memoria RTC de usuario: sobrevive a reinicios sin corte de energia, se pierde
// al apagar. Guarda una cota inferior del tiempo alimentado del sensor O2.
struct RtcPower {
  uint32_t magic;
  uint32_t poweredMs;
  uint32_t check;
};
constexpr uint32_t kRtcMagic = 0xB10E0002;
constexpr uint32_t kRtcOffset = 32;  // bloques de 4 bytes; evita la zona baja usada por otros

bool readRtc(uint32_t& poweredMs) {
  RtcPower r;
  if (!ESP.rtcUserMemoryRead(kRtcOffset, reinterpret_cast<uint32_t*>(&r), sizeof(r))) return false;
  if (r.magic != kRtcMagic || r.check != (r.magic ^ r.poweredMs ^ 0x5A5A5A5Au)) return false;
  poweredMs = r.poweredMs;
  return true;
}
void writeRtc(uint32_t poweredMs) {
  RtcPower r = {kRtcMagic, poweredMs, kRtcMagic ^ poweredMs ^ 0x5A5A5A5Au};
  ESP.rtcUserMemoryWrite(kRtcOffset, reinterpret_cast<uint32_t*>(&r), sizeof(r));
}

WarmupPolicy g_warmup;
uint32_t g_lastRtcWriteMs = 0;

// ---------------- Almacenamiento LittleFS de doble ranura ----------------
class LittleFsSlots : public SlotBackend {
 public:
  bool mounted = false;
  size_t readSlot(uint8_t slot, uint8_t* buf, size_t cap) override {
    if (!mounted) return 0;
    File f = LittleFS.open(slot ? "/cal1.bin" : "/cal0.bin", "r");
    if (!f) return 0;
    const size_t len = f.size();
    size_t n = 0;
    if (len && len <= cap) n = f.read(buf, len);
    f.close();
    return n == len ? n : 0;
  }
  bool writeSlot(uint8_t slot, const uint8_t* buf, size_t len) override {
    if (!mounted) return false;
    File f = LittleFS.open(slot ? "/cal1.bin" : "/cal0.bin", "w");
    if (!f) return false;
    const size_t n = f.write(buf, len);
    f.close();
    return n == len;
  }
};

constexpr uint16_t kStoreFormat = 1;
constexpr size_t kConfigBytes = 6;
LittleFsSlots g_slots;
RecordStore g_store(g_slots, kStoreFormat);
NodeBCal g_cal;
uint32_t g_reportIntervalMs = NODE_B_REPORT_INTERVAL_MS;
uint16_t g_expectedMask = 0;

uint16_t defaultExpectedMask() {
  uint16_t m = 0;
  if (EXPECT_BH1_DEFAULT) m |= 1u << kSensorLight1;
  if (EXPECT_BH2_DEFAULT) m |= 1u << kSensorLight2;
  if (EXPECT_O2_1_DEFAULT) m |= 1u << kSensorO2Gas1;
  if (EXPECT_O2_2_DEFAULT) m |= 1u << kSensorO2Gas2;
  return m;
}
bool storageWritable() { return g_slots.mounted && g_store.state() != StoreState::FutureFormat; }

bool persist(const NodeBCal& cal, uint32_t interval, uint16_t expected) {
  uint8_t buf[kNodeBCalEncodedSize + kConfigBytes];
  if (encodeNodeBCal(cal, buf, sizeof(buf)) != kNodeBCalEncodedSize) return false;
  Writer w(buf + kNodeBCalEncodedSize, kConfigBytes);
  w.u32(interval); w.u16(expected);
  const bool ok = g_store.save(buf, sizeof(buf));
  Serial.printf("LittleFS nodo B: generacion %lu %s\n", (unsigned long)g_store.generation(), ok ? "guardada" : "ESCRITURA FALLIDA");
  return ok;
}

FsMount g_fsMount = FsMount::MountFailed;

// Autoformato desactivado: el nucleo 3.1.2 formatea dentro de begin() si no se
// indica lo contrario. Un fallo de montaje nunca borra la particion.
void mountStorage() {
  LittleFSConfig cfg;
  cfg.setAutoFormat(false);
  LittleFS.setConfig(cfg);
  g_slots.mounted = FS_PHYS_SIZE > 0 && LittleFS.begin();
  g_fsMount = g_slots.mounted ? FsMount::Mounted : FS_PHYS_SIZE > 0 ? FsMount::MountFailed : FsMount::NoPartition;
}

void reportStorageUnavailable() {
  Serial.printf("ALMACENAMIENTO NO DISPONIBLE (%s): LittleFS NO se formatea. Se mide con correcciones O2 "
                "por defecto en RAM; calibraciones, importaciones y configuracion se rechazan "
                "(rejected_storage_failed). Placa nueva: docs/INSTALACION.md, 3.1.\n",
                fsMountName(g_fsMount));
#if NODE_B_STORAGE_COMMISSIONING
  char token[kFormatTokenSize];
  formatConfirmToken(ESP.getChipId(), token);
  if (g_fsMount == FsMount::MountFailed)
    Serial.printf("PUESTA EN MARCHA: para formatear SOLO esta placa envie "
                  "{\"action\":\"storage_format\",\"confirm\":\"%s\"}\n", token);
#endif
}

void loadCalibration() {
  g_expectedMask = defaultExpectedMask();
  uint8_t buf[kStoreMaxPayload];
  const size_t n = g_store.load(buf, sizeof(buf));
  if (n == kNodeBCalEncodedSize + kConfigBytes && decodeNodeBCal(buf, kNodeBCalEncodedSize, g_cal)) {
    Reader r(buf + kNodeBCalEncodedSize, kConfigBytes);
    const uint32_t interval = r.u32();
    const uint16_t expected = r.u16();
    if (interval >= kMinReportIntervalMs && interval <= kMaxReportIntervalMs) g_reportIntervalMs = interval;
    g_expectedMask = expected;
    Serial.printf("Calibracion nodo B cargada (rev nodo %lu).\n", (unsigned long)g_cal.nodeRevision);
    return;
  }
  g_cal = NodeBCal();
  if (g_store.state() == StoreState::FutureFormat) {
    Serial.println("LittleFS con formato futuro: escrituras bloqueadas, correcciones O2 por defecto en RAM.");
  } else if (g_slots.mounted) {
    Serial.printf("Almacenamiento nodo B: %s; correcciones O2 por defecto. Importar v4 (MIGRACION.md).\n",
                  storeStateName(g_store.state()));
    persist(g_cal, g_reportIntervalMs, g_expectedMask);
  }
}

void loadPersistent() {
  mountStorage();
  if (!g_slots.mounted) reportStorageUnavailable();
  loadCalibration();
}

// ---------------- Radio ----------------
uint8_t g_channelFileValue = 0;
class Esp8266Channel : public ChannelControl {
 public:
  explicit Esp8266Channel(EspNowPort& port) : port_(port) {}
  bool setChannel(uint8_t ch) override { return port_.setChannel(ch); }
  uint8_t channel() const override { return port_.channel(); }
  void persistChannel(uint8_t ch) override {
    if (!g_slots.mounted) return;
    File f = LittleFS.open("/channel.bin", "w");
    if (f) { f.write(ch); f.close(); }
    Serial.printf("Canal ESP-NOW confirmado %u guardado.\n", ch);
  }

 private:
  EspNowPort& port_;
};
uint8_t loadChannel() {
  uint8_t ch = BIOIOT_INITIAL_CHANNEL;
  if (g_slots.mounted) {
    File f = LittleFS.open("/channel.bin", "r");
    if (f && f.size() == 1) {
      const int v = f.read();
      if (v >= 1 && v <= 13) ch = uint8_t(v);
    }
    if (f) f.close();
  }
  return ch;
}

uint32_t hwRandom() { return ESP.random(); }

EspNowPort g_port;
Esp8266Channel g_channel(g_port);
Endpoint* g_ep = nullptr;
NodeLink* g_link = nullptr;
bool g_radioEnabled = false, g_selfTestOk = false;
uint32_t g_minFreeHeap = 0xFFFFFFFFu;

I2CBusB g_bus;
SensorsB g_sensors;

CommandQueue<4> g_commands;
CommandCache g_cmdCache;

struct AirCalJob {
  bool active = false;
  uint32_t cmdId = 0, gatewayBoot = 0;
  uint8_t sensor = 0, target = 0;
} g_airJob;

struct DiagState {
  bool active = false, fromSerial = false, scanStarted = false;
  uint8_t scope = 0;
  uint32_t requestId = 0, startedMs = 0, lightStart = 0, o2Start = 0;
  bool waitLight = false, waitO2 = false;
  uint32_t serialSeq = 0;
} g_diag;
char g_diagBuf[kDiagMaxBytes];
DiagSender g_diagSender;
LineAssembler<1536> g_serialLine;
uint32_t g_snapshotId = 0, g_lastReportMs = 0, g_reportJitter = 0, g_lastPlaceholderLogMs = 0, g_lastStorageLogMs = 0;

bool expected(uint8_t sensor) { return (g_expectedMask >> sensor) & 1u; }

// ---------------- Telemetria ----------------
TelemetryRecord lightRecord(uint8_t i) {
  const LightResult& l = g_sensors.light[i];
  const uint8_t sensor = i == 0 ? kSensorLight1 : kSensorLight2;
  TelemetryRecord rec;
  rec.sensor = sensor;
  rec.flags = uint16_t((l.connected ? kRecConnected : 0) | (expected(sensor) ? kRecExpected : 0) |
                       (l.sampled ? kRecObserved : 0));
  rec.sampleUptimeMs = l.sampledAtMs;
  rec.valueCount = val::kLightCount;
  rec.vals[val::kLightValue] = l.lux;
  rec.vals[val::kLightDriverCode] = l.driverCode;
  rec.quality = l.sampled ? (l.connected ? (isfinite(l.lux) ? kQGood : kQOutOfRange) : kQDisconnected) : kQNotSampled;
  return rec;
}

TelemetryRecord o2Record(uint8_t i, uint32_t nowMs) {
  const O2Reading& r = g_sensors.o2[i];
  const uint8_t sensor = i == 0 ? kSensorO2Gas1 : kSensorO2Gas2;
  TelemetryRecord rec;
  rec.sensor = sensor;
  const bool warming = g_warmup.warming(nowMs);
  rec.flags = uint16_t((r.connected ? kRecConnected : 0) | (expected(sensor) ? kRecExpected : 0) |
                       (r.observed ? kRecObserved : 0) | (r.i2cDetected ? kRecI2cDetected : 0) |
                       (r.valid ? kRecValid : 0) | (r.stabilizing ? kRecStabilizing : 0) |
                       ((r.warming || (!r.observed && warming)) ? kRecWarming : 0) |
                       ((g_cal.o2[i].gain != 1 || g_cal.o2[i].offset != 0) ? kRecCalUser : 0));
  rec.calRevision = g_cal.rev[i];
  rec.sampleUptimeMs = r.sampledAt;
  rec.valueCount = val::kO2Count;
  rec.vals[val::kO2Value] = r.value;
  rec.vals[val::kO2Raw] = r.raw;
  rec.vals[val::kO2Filtered] = r.filtered;
  rec.vals[val::kO2WireError] = r.wireError;
  rec.vals[val::kO2ReadCount] = r.readCount;
  rec.vals[val::kO2FilterCount] = g_sensors.filter[i].count;
  // Prioridad v4: warming_up antes que disconnected.
  rec.quality = r.observed ? qualityFromName(r.quality) : (warming ? kQWarmingUp : kQNotSampled);
  return rec;
}

void reportTelemetry(uint32_t nowMs) {
  TelemetryMsg t;
  t.snapshotId = ++g_snapshotId; t.part = 0; t.parts = 1;
  t.records[t.recordCount++] = lightRecord(0);
  t.records[t.recordCount++] = lightRecord(1);
  t.records[t.recordCount++] = o2Record(0, nowMs);
  t.records[t.recordCount++] = o2Record(1, nowMs);
  uint8_t buf[kMaxPayload];
  t.txUptimeMs = nowMs;
  const size_t n = encodeTelemetry(t, buf, sizeof(buf));
  if (!n) return;
  SendOptions o;
  o.cookie = t.snapshotId; o.ttlMs = NODE_B_TELEMETRY_TTL_MS; o.persistent = true; o.evictable = true;
  o.rebindOnReboot = true; o.txTimeOffset = int16_t(kTelemetryTxTimeOffset);
  g_ep->sendReliable(kNodeGateway, MsgType::Telemetry, buf, n, o, nowMs);
}

void sendCalState(uint32_t nowMs) {
  if (!g_radioEnabled) return;
  CalStateMsg s;
  s.node = kNodeB;
  s.storageState = g_store.state() == StoreState::Ok ? kCalStorageOk
                 : g_store.state() == StoreState::FutureFormat ? kCalStorageFuture
                 : (g_slots.mounted && g_store.lastWriteOk()) ? kCalStorageDefaults : kCalStorageFailed;
  s.blobLen = uint8_t(encodeNodeBCal(g_cal, s.blob, sizeof(s.blob)));
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

bool busy() { return g_diag.active || g_sensors.scanActive() || g_sensors.airCalibrationPending() || g_airJob.active; }

void applyFilterResets(uint8_t mask) {
  for (uint8_t i = 0; i < 2; ++i)
    if (mask & (1u << i)) g_sensors.clearFilter(i);
}

void processCommand(const PendingCommand& pc, uint32_t nowMs) {
  const uint32_t cmdId = getU32(pc.payload);
  if (const CommandResultMsg* cached = g_cmdCache.find(pc.gatewayBoot, cmdId)) {
    sendResult(*cached, pc.gatewayBoot, nowMs);
    return;
  }
  CommandResultMsg res;
  res.cmdId = cmdId;
  res.msgType = uint8_t(pc.type);
  if (pc.type == MsgType::Calibration) {
    CalibrationCmd cmd;
    if (!decodeCalibration(pc.payload, pc.len, cmd)) return;
    res.op = cmd.op; res.target = cmd.target;
    NodeBCal candidate;
    CalApplyResultB r = applyCalibrationB(g_cal, cmd, storageWritable(), g_warmup.warming(nowMs), busy(), candidate);
    reportStorageUnavailableB(r, g_slots.mounted);
    if (r.startAirCalibration) {
      if (g_sensors.startAirCalibration(r.airSensor)) {
        g_airJob.active = true; g_airJob.cmdId = cmdId; g_airJob.gatewayBoot = pc.gatewayBoot;
        g_airJob.sensor = r.airSensor; g_airJob.target = cmd.target;
        return;  // el resultado se envia al terminar la escritura I2C
      }
      r.outcome = kOutcomeRejected; r.status = kStRejectedWarmingUpOrBusy;
    }
    if (r.needsPersist) {
      if (persist(candidate, g_reportIntervalMs, g_expectedMask)) {
        g_cal = candidate;
        applyFilterResets(r.resetFilters);
      } else {
        r.outcome = kOutcomeRejected; r.status = kStRejectedStorageFailed;
      }
    }
    res.outcome = r.outcome; res.status = r.status;
    res.nodeRevision = g_cal.nodeRevision;
    res.sensorRevision = sensorRevisionForTargetB(g_cal, cmd.target);
    res.storageOk = g_store.lastWriteOk() && storageWritable();
    sendResult(res, pc.gatewayBoot, nowMs);
    if (res.outcome == kOutcomeApplied) sendCalState(nowMs);
  } else if (pc.type == MsgType::Config) {
    ConfigCmd cfg;
    if (!decodeConfig(pc.payload, pc.len, cfg)) return;
    const uint32_t interval = (cfg.present & 1) ? cfg.reportIntervalMs : g_reportIntervalMs;
    const uint16_t mask = (cfg.present & 2) ? uint16_t(cfg.expectedMask & 0x1E00) : g_expectedMask;
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
      const uint32_t t0 = millis();
      while (g_ep->hasPending(kNodeGateway, MsgType::CommandResult, cmdId) && millis() - t0 < 3000) {
        RxFrame f;
        while (g_port.pop(f)) g_link->deliver(f.data, f.len, millis(), f.rssi);
        g_link->loop(millis());
        delay(10);
      }
      writeRtc(g_warmup.poweredLowerBoundMs(millis()));  // el warm-up sobrevive al reinicio
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

void serviceAirCalibration(uint32_t nowMs) {
  bool commOk = false;
  TcaSelectResult disabled;
  if (!g_airJob.active || !g_sensors.airCalibrationFinished(commOk, disabled)) return;
  g_airJob.active = false;
  CommandResultMsg res;
  res.cmdId = g_airJob.cmdId;
  res.msgType = uint8_t(MsgType::Calibration);
  res.op = kCalOpO2Air;
  res.target = g_airJob.target;
  res.extra1 = disabled.selected;
  res.extra2 = disabled.errorCode;
  if (commOk) {
    NodeBCal candidate = g_cal;
    const int64_t utc = g_radioEnabled ? g_link->utc().utcSeconds(nowMs) : 0;
    recordAirCalibration(candidate, g_airJob.sensor, utc);
    if (persist(candidate, g_reportIntervalMs, g_expectedMask)) {
      g_cal = candidate;
      res.outcome = kOutcomeApplied; res.status = kStCommandSent;
    } else {
      // El sensor recibio la orden, pero el registro del host no quedo guardado.
      res.outcome = kOutcomeApplied; res.status = kStCommandSentRecordNotPersisted;
    }
  } else {
    res.outcome = kOutcomeFailed; res.status = kStCommunicationFault;
  }
  res.nodeRevision = g_cal.nodeRevision;
  res.sensorRevision = sensorRevisionForTargetB(g_cal, g_airJob.target);
  res.storageOk = g_store.lastWriteOk() && storageWritable();
  sendResult(res, g_airJob.gatewayBoot, nowMs);
  if (commOk) sendCalState(nowMs);
}

class AppB : public NodeApp {
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
    s.minFreeHeap = g_minFreeHeap;
    s.maxAllocHeap = ESP.getMaxFreeBlockSize();
    s.warmupRemainingMs = g_warmup.remainingMs(nowMs);
    s.flags = uint16_t((g_store.state() == StoreState::FutureFormat ? kSfStorageFuture : 0) |
                       (busy() ? kSfDiagBusy : 0) | (s.warmupRemainingMs ? kSfWarmupActive : 0) |
                       (g_warmup.assumedFull() ? kSfWarmupAssumed : 0));
    s.reportIntervalMs = g_reportIntervalMs;
    uint16_t d = 0;
    if (g_bus.tcaObserved) d |= kDiagBTcaObserved;
    if (g_bus.lastTcaProbe.detected) d |= kDiagBTcaDetected;
    bool observedAll = true;
    const uint16_t okBits[kDevCount] = {kDiagBBh1Ok, kDiagBBh2Ok, kDiagBO2_1Ok, kDiagBO2_2Ok};
    for (uint8_t i = 0; i < kDevCount; ++i) {
      const I2CReadingDiag& r = g_sensors.readingDiag[i];
      observedAll = observedAll && r.observed;
      if (r.observed && (strcmp(r.status, "measurement_ok") == 0 || strcmp(r.status, "warming_up") == 0))
        d |= okBits[i];
    }
    if (observedAll) d |= kDiagBReadingsObserved;
    s.diagFlags = d;
    s.diagCounter = g_bus.tcaProbeHistory.consecutiveFailures;
    s.diagError = g_bus.lastTcaProbe.errorCode;
    s.diagSampledAgeMs = g_bus.tcaObserved ? nowMs - g_bus.tcaProbeHistory.lastSuccessMs : 0xFFFFFFFFu;
  }
} g_app;

// ---------------- Diagnosticos ----------------
void addrText(char* out, uint8_t a) { snprintf(out, 5, "0x%02X", a); }

void writeSegment(JsonObject o, const ScanSegment& s, bool downstreamOnly) {
  JsonArray devices = o["devices"].to<JsonArray>();
  JsonObject counts = o["error_counts"].to<JsonObject>();
  uint16_t c[6] = {};
  JsonArray odd = o["non_nack_errors"].to<JsonArray>();
  uint8_t oddCount = 0;
  for (uint8_t a = 1; a <= 0x7E; ++a) {
    if (!s.tested[a]) continue;
    const uint8_t code = s.codes[a];
    c[code < 5 ? code : 5]++;
    const bool upstream = g_sensors.mainScan.tested[a] && g_sensors.mainScan.codes[a] == 0;
    if (code == 0 && (!downstreamOnly || !upstream)) {
      char t[5]; addrText(t, a); devices.add(t);
    }
    if (code != 0 && code != 2 && oddCount < 8) {
      JsonObject e = odd.add<JsonObject>();
      char t[5]; addrText(t, a);
      e["address"] = t; e["code"] = code; e["name"] = i2cErrorName(code);
      oddCount++;
    }
  }
  for (uint8_t k = 0; k < 6; ++k)
    if (c[k]) counts[k == 5 ? "other" : String(k)] = c[k];
  o["scan_complete"] = s.completed;
}

void writeI2C(JsonObject o, JsonArray findings, uint32_t nowMs) {
  const bool warming = g_warmup.warming(nowMs);
  JsonObject mainBus = o["main_bus"].to<JsonObject>();
  mainBus["sda_gpio"] = I2C_SDA_PIN; mainBus["scl_gpio"] = I2C_SCL_PIN;
  mainBus["board_profile"] = NODE_B_BOARD_NAME;
  mainBus["line_status"] = twiStatusName(g_bus.lineStatus());
  mainBus["isolated"] = g_sensors.isolation.selected;
  mainBus["isolation_error"] = g_sensors.isolation.errorCode;
  writeSegment(mainBus, g_sensors.mainScan, false);
  if (!g_sensors.isolation.selected) findings.add("main_bus_isolation_failed");
  JsonObject tca = o["tca9548a"].to<JsonObject>();
  char t[5];
  addrText(t, TCA_ADDR); tca["expected_address"] = t;
  tca["detected"] = g_sensors.tcaProbe.detected;
  tca["probe_error"] = g_sensors.tcaProbe.errorCode;
  tca["probe_error_name"] = i2cErrorName(g_sensors.tcaProbe.errorCode);
  addrText(t, TCA_ALT_ADDR); tca["alternate_profile_address"] = t;
  tca["alternate_address_ack"] = g_sensors.altTcaProbe.detected;
  tca["channels_disabled"] = g_sensors.cleanup.selected;
  tca["disable_error"] = g_sensors.cleanup.errorCode;
  if (!g_sensors.tcaProbe.detected) {
    findings.add("tca9548a_not_detected");
    if (g_sensors.altTcaProbe.detected) findings.add("tca9548a_answers_at_alternate_profile_address");
  }
  if (!g_sensors.cleanup.selected) findings.add("tca_disable_failed");
  JsonObject channels = tca["channels"].to<JsonObject>();
  unsigned missing = 0;
  for (uint8_t i = 0; i < 8; ++i) {
    const ScanSegment& s = g_sensors.channelScans[i];
    JsonObject ch = channels[String(i)].to<JsonObject>();
    const uint8_t expectedAddr = g_sensors.expectedAddress(i);
    const bool o2Channel = i == TCA_CH_O2_1 || i == TCA_CH_O2_2;
    ch["select_attempted"] = s.selection.attempted;
    ch["select_ok"] = s.selection.selected;
    if (s.selection.attempted) ch["select_error"] = s.selection.errorCode; else ch["select_error"] = nullptr;
    if (expectedAddr) { addrText(t, expectedAddr); ch["expected"] = t; } else ch["expected"] = nullptr;
    ch["warming_up"] = o2Channel && warming;
    ch["measurement_testable"] = !(o2Channel && warming);
    writeSegment(ch, s, true);
    const bool ambiguous = !g_sensors.isolation.selected ||
        (expectedAddr && g_sensors.mainScan.tested[expectedAddr] && g_sensors.mainScan.codes[expectedAddr] == 0);
    const bool testable = g_sensors.tcaProbe.detected && s.selection.selected && s.completed && !ambiguous;
    ch["testable"] = testable;
    if (expectedAddr && testable && !s.warmupSkipped) {
      const bool found = s.tested[expectedAddr] && s.codes[expectedAddr] == 0;
      ch["expected_found"] = found;
      ch["status"] = found ? "expected_device_found" : "expected_device_missing";
      if (!found) { missing++; findings.add("tca_ch" + String(i) + "_expected_device_missing"); }
    } else {
      ch["expected_found"] = nullptr;
      ch["status"] = !g_sensors.tcaProbe.detected ? "not_testable"
                   : !s.selection.selected ? "tca_channel_select_failed"
                   : s.warmupSkipped ? "warming_up_not_tested"
                   : ambiguous ? "ambiguous_upstream_address" : "channel_empty_or_unexpected";
    }
    if (o2Channel) {
      JsonArray cand = ch["o2_candidate_addresses_ack"].to<JsonArray>();
      for (uint8_t a = 0x70; a <= 0x73; ++a)
        if (s.tested[a] && s.codes[a] == 0) { addrText(t, a); cand.add(t); }
    }
    if (g_sensors.tcaProbe.detected && s.selection.attempted && !s.selection.selected)
      findings.add("tca_ch" + String(i) + "_select_failed");
    if (g_sensors.tcaProbe.detected && ambiguous) findings.add("tca_ch" + String(i) + "_attribution_uncertain");
  }
  if (missing >= 4) findings.add("multiple_tca_channels_missing_devices");
  JsonObject last = o["last_measurement_attempts"].to<JsonObject>();
  const char* names[kDevCount] = {"bh1750_1", "bh1750_2", "o2_1", "o2_2"};
  for (uint8_t i = 0; i < kDevCount; ++i) {
    const I2CReadingDiag& d = g_sensors.readingDiag[i];
    JsonObject x = last[names[i]].to<JsonObject>();
    x["status"] = d.status;
    x["select_error"] = d.selection.attempted ? int(d.selection.errorCode) : -1;
    x["probe_error"] = d.probeAttempted ? int(d.probe.errorCode) : -1;
    x["success_count"] = g_sensors.deviceHistory[i].success;
    x["failure_count"] = g_sensors.deviceHistory[i].failure;
    x["consecutive_failures"] = g_sensors.deviceHistory[i].consecutiveFailures;
  }
  if (g_sensors.recovery.performed) {
    JsonObject r = o["recovery"].to<JsonObject>();
    r["line_status_before"] = twiStatusName(g_sensors.recovery.statusBefore);
    r["line_status_after"] = twiStatusName(g_sensors.recovery.statusAfter);
    r["scl_pulses"] = g_sensors.recovery.pulses;
    r["sda_released"] = g_sensors.recovery.sdaReleased;
    r["tca_before_detected"] = g_sensors.recovery.tcaBefore.detected;
    r["tca_before_error"] = g_sensors.recovery.tcaBefore.errorCode;
    r["tca_after_detected"] = g_sensors.recovery.tcaAfter.detected;
    r["tca_after_error"] = g_sensors.recovery.tcaAfter.errorCode;
    r["physical_repair_confirmed"] = false;
  }
}

void writeO2Detail(JsonObject o, uint8_t i, uint32_t nowMs) {
  const O2Reading& r = g_sensors.o2[i];
  char t[5];
  addrText(t, i == 0 ? O2_ADDR_1 : O2_ADDR_2);
  o["expected_address"] = t;
  o["connected"] = r.connected; o["i2c_detected"] = r.i2cDetected; o["warming_up"] = g_warmup.warming(nowMs);
  o["stabilizing"] = r.stabilizing; o["measurement_valid"] = r.valid; o["quality"] = r.quality;
  o["observed"] = r.observed; o["sampled_at_ms"] = r.sampledAt;
  if (isfinite(r.value)) o["value"] = r.value; else o["value"] = nullptr;
  if (isfinite(r.raw)) o["raw_value"] = r.raw; else o["raw_value"] = nullptr;
  if (isfinite(r.filtered)) o["filtered_value"] = r.filtered; else o["filtered_value"] = nullptr;
  o["wire_error"] = r.wireError; o["received_bytes"] = r.readCount;
  JsonArray range = o["nominal_range"].to<JsonArray>(); range.add(0); range.add(25);
  o["measurement_limit"] = 30; o["resolution_vol"] = 0.15;
  o["filter_samples"] = g_sensors.filter[i].count; o["filter_target_samples"] = O2_FILTER_SAMPLES;
  o["filter_sample_interval_ms"] = NODE_B_O2_SAMPLE_INTERVAL_MS;
  o["calibration_source"] = o2SourceName(g_cal.o2[i]);
  o["gain"] = g_cal.o2[i].gain; o["offset"] = g_cal.o2[i].offset;
  o["calibration_command_count"] = g_cal.o2[i].commandCount;
  o["last_calibration_status"] = g_cal.o2[i].commandCount ? "command_sent" : "not_requested";
  o["last_reference_vol"] = g_cal.o2[i].reference;
  o["last_calibration_command_utc_s"] = g_cal.o2[i].commandUtc;
}

void writeSystem(JsonObject o, uint32_t nowMs) {
  o["uptime_ms"] = nowMs;
  o["free_heap"] = ESP.getFreeHeap();
  o["min_free_heap_observed"] = g_minFreeHeap;
  o["max_alloc_heap"] = ESP.getMaxFreeBlockSize();
  o["heap_fragmentation_pct"] = ESP.getHeapFragmentation();
  o["reset_reason"] = resetReasonText(g_resetReason);
  o["board_profile"] = NODE_B_BOARD_NAME;
  o["protocol_self_test_ok"] = g_selfTestOk;
  o["storage_mounted"] = g_slots.mounted;
  o["storage_state"] = storeStateName(g_store.state());
  o["storage_generation"] = g_store.generation();
  JsonObject w = o["o2_warmup"].to<JsonObject>();
  w["remaining_ms"] = g_warmup.remainingMs(nowMs);
  w["from_power_on"] = g_warmup.fromPowerOn();
  w["assumed_full_warmup"] = g_warmup.assumedFull();
  w["credited_powered_ms"] = g_warmup.creditedMs();
  w["shared_supply_configured"] = bool(O2_SENSOR_SHARED_SUPPLY);
  JsonObject link = o["espnow"].to<JsonObject>();
  link["enabled"] = g_radioEnabled;
  link["provisioning_required"] = secretsArePlaceholders();
  link["encrypted"] = g_port.encrypted();
  if (g_radioEnabled) {
    link["online"] = g_link->online(nowMs);
    link["channel"] = g_port.channel();
    link["channel_state"] = channelStateName(g_link->channelState());
    link["channel_hunts"] = g_link->hunts();
    link["queue_depth"] = g_ep->queueDepth();
    const LinkStats& st = g_ep->stats();
    link["retries"] = st.retries; link["delivered"] = st.delivered; link["failed"] = st.failed;
    link["dropped"] = st.dropped; link["expired"] = st.expired; link["rx_rejected"] = st.rxRejected;
    link["rx_ring_dropped"] = g_port.rxDropped();
    link["utc_synced"] = g_link->utc().utcValid(nowMs);
  }
}

size_t buildDiagnosticJson(uint32_t nowMs) {
  JsonDocument doc;
  doc["node"] = "node_b";
  doc["report_id"] = g_diag.requestId;
  doc["scope"] = diagScopeName(g_diag.scope);
  doc["uptime_ms"] = nowMs;
  doc["started_uptime_ms"] = g_diag.startedMs;
  JsonArray findings = doc["findings"].to<JsonArray>();
  const uint8_t s = g_diag.scope;
  writeSystem(doc["system"].to<JsonObject>(), nowMs);
  if (s == kDiagAnalog || s == kDiagColor || s == kDiagTemperature) doc["not_applicable"] = true;
  if (s == kDiagFull || s == kDiagI2c || s == kDiagRecover) writeI2C(doc["i2c"].to<JsonObject>(), findings, nowMs);
  if (s == kDiagQuick) {
    const TcaSelectResult iso = g_bus.disableAll();  // dos transacciones cortas, sin barrido
    const I2CProbeResult tca = g_bus.probe(TCA_ADDR);
    JsonObject q = doc["i2c_quick"].to<JsonObject>();
    char t[5]; addrText(t, TCA_ADDR);
    q["tca_address"] = t; q["tca_detected"] = tca.detected; q["last_error"] = tca.errorCode;
    q["last_error_name"] = i2cErrorName(tca.errorCode); q["main_bus_isolated"] = iso.selected;
    q["consecutive_failures"] = g_bus.tcaProbeHistory.consecutiveFailures;
    if (!tca.detected) findings.add("tca9548a_not_detected");
  }
  if (s == kDiagFull || s == kDiagO2) {
    for (uint8_t i = 0; i < 2; ++i) {
      writeO2Detail(doc[i == 0 ? "o2_gas_1" : "o2_gas_2"].to<JsonObject>(), i, nowMs);
      const O2Reading& r = g_sensors.o2[i];
      if (!r.connected) findings.add(i == 0 ? "o2_gas_1_communication_fault" : "o2_gas_2_communication_fault");
      else if (!r.warming && !r.valid) findings.add(i == 0 ? "o2_gas_1_invalid_measurement" : "o2_gas_2_invalid_measurement");
    }
    JsonObject light = doc["light"].to<JsonObject>();
    for (uint8_t i = 0; i < 2; ++i) {
      JsonObject l = light[i == 0 ? "light_1" : "light_2"].to<JsonObject>();
      l["connected"] = g_sensors.light[i].connected;
      if (isfinite(g_sensors.light[i].lux)) l["lux"] = g_sensors.light[i].lux; else l["lux"] = nullptr;
      if (isfinite(g_sensors.light[i].driverCode)) l["driver_code"] = g_sensors.light[i].driverCode;
      l["sampled_at_ms"] = g_sensors.light[i].sampledAtMs;
    }
  }
  doc["physical_fault_confirmed"] = false;
  const size_t need = measureJson(doc);
  if (doc.overflowed() || need >= sizeof(g_diagBuf)) {
    return snprintf(g_diagBuf, sizeof(g_diagBuf),
                    "{\"node\":\"node_b\",\"report_id\":%lu,\"error\":\"diagnostic_report_too_large\",\"bytes\":%u}",
                    (unsigned long)g_diag.requestId, unsigned(need));
  }
  return serializeJson(doc, g_diagBuf, sizeof(g_diagBuf));
}

void startDiagnostics(uint8_t scope, uint32_t requestId, bool fromSerial, uint32_t nowMs) {
  if (g_diag.active || g_diagSender.active() || g_sensors.airCalibrationPending()) {
    const int n = snprintf(g_diagBuf, sizeof(g_diagBuf), "{\"node\":\"node_b\",\"report_id\":%lu,\"error\":\"diagnostics_busy\"}",
                           (unsigned long)requestId);
    if (fromSerial) Serial.println(g_diagBuf);
    else if (g_radioEnabled && !g_diagSender.active()) g_diagSender.start(requestId, (const uint8_t*)g_diagBuf, n);
    return;
  }
  const uint32_t seq = g_diag.serialSeq;
  g_diag = DiagState();
  g_diag.serialSeq = seq;
  g_diag.active = true;
  g_diag.fromSerial = fromSerial;
  g_diag.scope = scope;
  g_diag.requestId = requestId;
  g_diag.startedMs = nowMs;
  if (scope == kDiagFull || scope == kDiagI2c || scope == kDiagRecover)
    g_diag.scanStarted = g_sensors.startScan(scope == kDiagRecover);
  if (scope == kDiagFull || scope == kDiagO2) {
    g_diag.waitLight = true; g_diag.lightStart = g_sensors.lightCycles;
    g_diag.waitO2 = !g_warmup.warming(nowMs); g_diag.o2Start = g_sensors.o2Cycles;
    g_sensors.requestFreshReads(true, g_diag.waitO2);
  }
  Serial.printf("Diagnostico #%lu (%s) iniciado.\n", (unsigned long)requestId, diagScopeName(scope));
}

void serviceDiagnostics(uint32_t nowMs) {
  if (g_diag.active) {
    const bool scanDone = !g_diag.scanStarted || g_sensors.takeScanResult();
    if (scanDone) g_diag.scanStarted = false;
    const bool lightDone = !g_diag.waitLight || g_sensors.lightCycles > g_diag.lightStart;
    const bool o2Done = !g_diag.waitO2 || g_sensors.o2Cycles > g_diag.o2Start;
    // Plazo: 1134 sondas con stretch de 50 ms podrian tardar ~60 s con bus colgado.
    if ((scanDone && lightDone && o2Done) || nowMs - g_diag.startedMs > 90000) {
      g_diag.active = false;
      const size_t n = buildDiagnosticJson(nowMs);
      Serial.println(g_diagBuf);
      Serial.printf("Diagnostico nodo B: %u bytes.\n", unsigned(n));
      if (!g_diag.fromSerial && g_radioEnabled) g_diagSender.start(g_diag.requestId, (const uint8_t*)g_diagBuf, n);
    }
  }
  if (g_radioEnabled) g_diagSender.pump(*g_ep, kNodeGateway, nowMs);
}

// ---------------- Serial ----------------
// Unica ruta hacia LittleFS.format(): orden expresa, firmware de puesta en marcha,
// LittleFS sin montar y token del propio chip. En produccion ni se compila.
void handleStorageFormat(const char* confirm) {
  const FormatDecision d = decideFormat(NODE_B_STORAGE_COMMISSIONING, g_fsMount, confirm, ESP.getChipId());
  if (d != FormatDecision::Allowed) {
    Serial.printf("{\"result\":\"rejected\",\"error\":\"%s\",\"storage\":\"%s\"}\n", formatDecisionName(d),
                  fsMountName(g_fsMount));
    return;
  }
#if NODE_B_STORAGE_COMMISSIONING
  Serial.println("Formateando LittleFS del nodo B por orden expresa de puesta en marcha...");
  const bool formatted = LittleFS.format();
  mountStorage();
  if (g_slots.mounted) loadCalibration();  // placa vacia: guarda las correcciones por defecto
  const bool ok = formatted && g_slots.mounted && g_store.state() == StoreState::Ok;
  Serial.printf("{\"result\":\"%s\",\"storage_mounted\":%s,\"storage_state\":\"%s\"}\n", ok ? "formatted" : "failed",
                g_slots.mounted ? "true" : "false", storeStateName(g_store.state()));
  if (ok) Serial.println("Puesta en marcha completa: recompile con NODE_B_STORAGE_COMMISSIONING=0 y vuelva a cargar.");
#endif
}

void handleSerialLine(const char* line, uint32_t nowMs) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) { Serial.println("Serial: JSON invalido."); return; }
  const char* action = doc["action"] | "";
  if (strcmp(action, "diagnostics") == 0) {
    const uint8_t scope = diagScopeFromName(doc["scope"] | "full");
    if (scope == 255) { Serial.println("Diagnostics: scope desconocido."); return; }
    startDiagnostics(scope, ++g_diag.serialSeq | 0x80000000u, true, nowMs);
  } else if (strcmp(action, "status") == 0) {
    JsonDocument out;
    out["node"] = "node_b";
    writeSystem(out["system"].to<JsonObject>(), nowMs);
    serializeJson(out, Serial);
    Serial.println();
  } else if (strcmp(action, "pairing_info") == 0) {
    Serial.printf("{\"node\":\"node_b\",\"sta_mac\":\"%s\",\"channel\":%u,\"provisioning_required\":%s}\n",
                  WiFi.macAddress().c_str(), g_port.channel(), secretsArePlaceholders() ? "true" : "false");
  } else if (strcmp(action, "calibration_export") == 0) {
    JsonDocument out;
    writeCalibrationExport(out.to<JsonObject>(), "node_b", "node_b", "", nullptr, &g_cal);
    serializeJson(out, Serial);
    Serial.println();
  } else if (strcmp(action, "calibration_import") == 0) {
    NodeACal unused;
    NodeBCal b;
    const CalImportResult r = parseCalibrationExport(doc["export"].as<JsonObjectConst>(), unused, b);
    if (!r.formatOk || !r.hasB || !r.bValid) {
      Serial.printf("{\"result\":\"rejected\",\"error\":\"%s\"}\n", r.error);
      return;
    }
    CalibrationCmd cmd;
    cmd.op = kCalOpImport; cmd.target = kTargetAll; cmd.force = (doc["force"] | false) ? 1 : 0;
    cmd.blobLen = uint8_t(encodeNodeBCal(b, cmd.blob, sizeof(cmd.blob)));
    NodeBCal candidate;
    CalApplyResultB ar = applyCalibrationB(g_cal, cmd, storageWritable(), g_warmup.warming(nowMs), busy(), candidate);
    reportStorageUnavailableB(ar, g_slots.mounted);
    if (ar.needsPersist) {
      if (persist(candidate, g_reportIntervalMs, g_expectedMask)) { g_cal = candidate; applyFilterResets(ar.resetFilters); }
      else { ar.outcome = kOutcomeRejected; ar.status = kStRejectedStorageFailed; }
    }
    Serial.printf("{\"result\":\"%s\",\"status\":\"%s\",\"node_revision\":%lu}\n", outcomeName(ar.outcome),
                  statusName(ar.status), (unsigned long)g_cal.nodeRevision);
    if (ar.outcome == kOutcomeApplied) sendCalState(nowMs);
  } else if (strcmp(action, "storage_format") == 0) {
    handleStorageFormat(doc["confirm"] | "");
  } else {
    Serial.println("Serial: acciones admitidas: diagnostics, status, pairing_info, calibration_export, calibration_import"
                   " (storage_format solo en firmware de puesta en marcha).");
  }
}

void pollSerial(uint32_t nowMs) {
  for (uint16_t budget = 0; budget < 256 && Serial.available(); ++budget) {
    if (g_serialLine.feed(char(Serial.read()))) handleSerialLine(g_serialLine.line(), nowMs);
    else if (g_serialLine.lastOverflowed()) Serial.println("Serial: linea demasiado larga.");
  }
}

}  // namespace

void nodeBSetup() {
  Serial.begin(115200);
  delay(300);
  readResetReason();
  Serial.println("\n===== BioIoT Nodo B (ESP8266 sensores I2C) =====");
  uint32_t rtcPowered = 0;
  const bool rtcValid = readRtc(rtcPowered);
  g_warmup.begin(O2_WARMUP_MS, g_resetKind, rtcValid, rtcPowered, O2_SENSOR_SHARED_SUPPLY, millis());
  Serial.printf("Warm-up SEN0322: restante %lu ms (reinicio=%s, rtc=%s, %s)\n",
                (unsigned long)g_warmup.remainingMs(millis()), resetReasonText(g_resetReason),
                rtcValid ? "valida" : "invalida", g_warmup.assumedFull() ? "politica conservadora" : "medido");
  g_selfTestOk = protocolSelfTest();
  Serial.printf("Autoprueba de protocolo: %s\n", g_selfTestOk ? "OK" : "FALLO (no interoperable)");
  loadPersistent();
  g_bus.begin();
  g_bus.disableAll();
  Serial.printf("TCA9548A en 0x%02X: %s\n", TCA_ADDR, g_bus.probe(TCA_ADDR).detected ? "detectado" : "NO detectado");
  g_sensors.begin(&g_bus, &g_warmup, &g_cal);

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect();
  WiFi.setSleepMode(WIFI_NONE_SLEEP);  // ESP-NOW necesita la radio despierta
  Serial.printf("MAC STA nodo B: %s\n", WiFi.macAddress().c_str());

  if (NODE_B_STORAGE_COMMISSIONING) {
    Serial.println("ESP-NOW DESHABILITADO: firmware de PUESTA EN MARCHA de LittleFS (NODE_B_STORAGE_COMMISSIONING=1).");
  } else if (secretsArePlaceholders()) {
    Serial.println("ESP-NOW DESHABILITADO: faltan node_secrets.h reales (ver docs/EMPAREJAMIENTO.md).");
  } else if (!g_selfTestOk) {
    Serial.println("ESP-NOW DESHABILITADO: la autoprueba de serializacion fallo.");
  } else {
    const uint8_t ch = loadChannel();
    EspNowPeerConfig peer;
    peer.node = kNodeGateway;
    memcpy(peer.mac, kGatewayMac, 6);
    memcpy(peer.lmk, kLmk, kEspNowKeySize);
    if (!g_port.begin(kPmk, ch, BIOIOT_SYSTEM_ID, kNodeB) || !g_port.addPeer(peer)) {
      Serial.printf("ESP-NOW DESHABILITADO: %s\n", g_port.lastError());
    } else {
      alignas(Endpoint) static uint8_t epStorage[sizeof(Endpoint)];
      alignas(NodeLink) static uint8_t linkStorage[sizeof(NodeLink)];
      NodeLinkConfig cfg;
      cfg.linkTimeoutMs = NODE_B_LINK_TIMEOUT_MS;
      g_ep = reinterpret_cast<Endpoint*>(epStorage);
      g_link = new (linkStorage) NodeLink(*g_ep, g_channel, g_app, cfg);
      new (epStorage) Endpoint(BIOIOT_SYSTEM_ID, kNodeB, ESP.random() | 1u, g_port, *g_link, hwRandom);
      g_ep->addPeer(kNodeGateway, kAppKey);
      g_link->begin(ch, millis(), g_resetReason, g_selfTestOk, g_store.state() == StoreState::Ok);
      g_radioEnabled = true;
      Serial.printf("ESP-NOW cifrado activo en canal %u; boot %08lx.\n", g_port.channel(), (unsigned long)g_ep->bootId());
    }
  }
  g_reportJitter = ESP.random() % 200;
  g_lastReportMs = millis() + NODE_B_REPORT_PHASE_MS;
  // El watchdog de software del nucleo ESP8266 (~3 s) sigue activo: loop() nunca bloquea.
}

void nodeBLoop() {
  const uint32_t nowMs = millis();
  const uint32_t heap = ESP.getFreeHeap();
  if (heap < g_minFreeHeap) g_minFreeHeap = heap;
  if (g_radioEnabled) {
    RxFrame f;
    for (uint8_t i = 0; i < 8 && g_port.pop(f); ++i) g_link->deliver(f.data, f.len, millis(), f.rssi);
    g_link->loop(nowMs);
    PendingCommand pc;
    if (g_commands.pop(pc)) processCommand(pc, nowMs);
  } else if (nowMs - g_lastPlaceholderLogMs > 30000) {
    g_lastPlaceholderLogMs = nowMs;
    Serial.println(NODE_B_STORAGE_COMMISSIONING
                       ? "Nodo B en PUESTA EN MARCHA de LittleFS: sin ESP-NOW; recargar con NODE_B_STORAGE_COMMISSIONING=0."
                       : "Nodo B midiendo sin enlace ESP-NOW (aprovisionamiento pendiente o error de radio).");
  }
  if (!g_slots.mounted && nowMs - g_lastStorageLogMs > 60000) {
    g_lastStorageLogMs = nowMs;
    reportStorageUnavailable();
  }
  g_sensors.loop(millis());
  serviceAirCalibration(millis());
  serviceDiagnostics(millis());
  pollSerial(millis());
  // Cota inferior del tiempo alimentado (sin flash: memoria RTC, cada 5 s).
  if (nowMs - g_lastRtcWriteMs >= 5000) {
    g_lastRtcWriteMs = nowMs;
    writeRtc(g_warmup.poweredLowerBoundMs(nowMs));
  }
  const uint32_t now2 = millis();
  if (g_radioEnabled && int32_t(now2 - g_lastReportMs) >= int32_t(g_reportIntervalMs + g_reportJitter)) {
    g_lastReportMs = now2;
    g_reportJitter = ESP.random() % 200;
    reportTelemetry(now2);
  }
}
