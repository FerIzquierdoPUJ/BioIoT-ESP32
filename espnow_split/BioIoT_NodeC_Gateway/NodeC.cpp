// Nodo C: integracion. Dos contextos:
//  * Tarea local (nucleo 1, prioridad 2): ESP-NOW, estado de nodos, actuadores,
//    TIME_SYNC, instantaneas. Nunca toca Wi-Fi/MQTT/TLS ni flash en caliente.
//  * loop() de Arduino: Wi-Fi, NTP, MQTT/TLS, SAS, publicacion y Serial.
// Comparten GatewayState bajo un mutex; las ordenes pasan por una cola acotada.
#include "NodeC.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <BioIoTCommon.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include <new>

#include "ActuatorCore.h"
#include "ActuatorOutputsEsp32.h"
#include "AzureLink.h"
#include "CloudCommands.h"
#include "GatewayState.h"
#include "TelemetryJson.h"
#include "gateway_config.h"

#if __has_include("gateway_secrets.h")
#include "gateway_secrets.h"
#else
#include "gateway_secrets.example.h"
#endif
#if __has_include("iot_configs.h")
#include "iot_configs.h"
#else
#include "iot_configs.example.h"
#endif

using namespace bioiot;
using namespace gw;

namespace {

// ---------------- Secretos ----------------
const uint8_t kPmk[kEspNowKeySize] = BIOIOT_ESPNOW_PMK;
const uint8_t kMacA[6] = BIOIOT_NODE_A_MAC;
const uint8_t kLmkA[kEspNowKeySize] = BIOIOT_NODE_A_LMK;
const uint8_t kKeyA[kAppKeySize] = BIOIOT_NODE_A_APP_KEY;
const uint8_t kMacB[6] = BIOIOT_NODE_B_MAC;
const uint8_t kLmkB[kEspNowKeySize] = BIOIOT_NODE_B_LMK;
const uint8_t kKeyB[kAppKeySize] = BIOIOT_NODE_B_APP_KEY;

bool allZero(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i)
    if (p[i]) return false;
  return true;
}
bool secretsArePlaceholders() {
#ifdef BIOIOT_SECRETS_PLACEHOLDER
  return true;
#else
  return BIOIOT_SYSTEM_ID == 0 || allZero(kPmk, 16) || allZero(kMacA, 6) || allZero(kMacB, 6) ||
         allZero(kLmkA, 16) || allZero(kLmkB, 16) || allZero(kKeyA, 32) || allZero(kKeyB, 32);
#endif
}

const char* resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_EXT: return "external_pin";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt_watchdog";
    case ESP_RST_TASK_WDT: return "task_watchdog";
    case ESP_RST_WDT: return "other_watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    default: return "other";
  }
}

int64_t monoMs() { return esp_timer_get_time() / 1000; }  // 64 bits: no desborda como millis()
uint32_t hwRandom() { return esp_random(); }

// ---------------- Estado compartido ----------------
SemaphoreHandle_t g_lock = nullptr;
struct Lock {
  Lock() { xSemaphoreTake(g_lock, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(g_lock); }
};

// ~48 KB (40 instantaneas, 2 ensambladores de 4 KB): una sola reserva en el heap al
// arrancar (la region DRAM estatica del ESP32 no alcanza). Nunca se libera ni crece.
GatewayState& g_state = *new GatewayState();
ActuatorState g_actShared;
bool g_actReportPending = false;
struct Rejection {
  bool pending = false;
  char type[24] = "", action[24] = "", status[40] = "", commandId[65] = "";
  int64_t mono = 0;
} g_rejection;
struct ExportRequest {
  bool pending = false;
  char commandId[65] = "";
} g_export;
struct GroupSummary {
  bool used = false;
  uint32_t groupId = 0;
  char kind[20] = "";
  char commandId[65] = "";
} g_groups[4];
uint32_t g_eventsDropped = 0;

struct Request {
  CloudCommand cmd;
  bool fromSerial = false;
};
QueueHandle_t g_requests = nullptr;

// Propiedad exclusiva de la tarea local:
ActuatorCore g_actuators;
Esp32Outputs g_outputs;
EspNowPort g_port;
Endpoint* g_ep = nullptr;
bool g_espnowEnabled = false;
uint32_t g_cmdCounter = 0, g_groupCounter = 0, g_diagCounter = 0;
uint32_t g_lastSyncMs[2] = {0, 0};
bool g_needSync[2] = {false, false};
volatile uint32_t g_localHeartbeat = 0;
volatile bool g_stopForReboot = false, g_stoppedForReboot = false;
// Canal nuevo de ESP-NOW (loop -> tarea local, que es la duena del Endpoint).
volatile uint8_t g_channelAnnounce = 0;
HelloMsg g_hello;  // plantilla HELLO del gateway (solo tarea local tras el arranque)

// Propiedad del loop():
AzureLink g_azure;
uint32_t g_rebootAtMs = 0;
uint32_t g_lastPublishMs = 0;
uint32_t g_minFreeHeap = 0xFFFFFFFF;
char* const g_pub = static_cast<char*>(malloc(MQTT_PACKET_SIZE));  // reserva unica
LineAssembler<512> g_serialLine;

uint8_t nodeIndex(uint8_t node) { return node == kNodeB ? 1 : 0; }

bool utcNow(int64_t& utcMs) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec < 1600000000) return false;
  utcMs = int64_t(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
  return true;
}

void rememberGroup(uint32_t groupId, const char* kind, const CloudCommand& c) {
  for (auto& g : g_groups) {
    if (g.used) continue;
    g.used = true;
    g.groupId = groupId;
    strncpy(g.kind, kind, sizeof(g.kind) - 1);
    strncpy(g.commandId, c.hasCommandId ? c.commandId : "", 64);
    return;
  }
}

void reject(const char* type, const char* action, const char* status, const CloudCommand& c) {
  Lock l;
  g_rejection.pending = true;
  strncpy(g_rejection.type, type, sizeof(g_rejection.type) - 1);
  strncpy(g_rejection.action, action, sizeof(g_rejection.action) - 1);
  strncpy(g_rejection.status, status, sizeof(g_rejection.status) - 1);
  strncpy(g_rejection.commandId, c.hasCommandId ? c.commandId : "", 64);
  g_rejection.mono = monoMs();
  Serial.printf("Comando rechazado en gateway: %s (%s)\n", action, status);
}

uint8_t statusFromName(const char* s) {
  for (uint8_t i = 0; i <= kMaxStatusCode; ++i)
    if (strcmp(statusName(i), s) == 0) return i;
  return kStRejectedInvalidParameters;
}

// ---------------- Tarea local: radio ----------------
class GatewayRadio : public EndpointHandler {
 public:
  uint8_t onMessage(uint8_t src, const FrameHeader& h, const uint8_t* p, size_t len) override {
    const int64_t mono = monoMs();
    switch (h.type) {
      case MsgType::Telemetry: {
        TelemetryMsg t;
        if (!decodeTelemetry(p, len, t)) return kAckRejectedInvalid;
        Lock l;
        g_state.onTelemetry(src, t, mono);
        return kAckAccepted;
      }
      case MsgType::Status: {
        StatusMsg s;
        if (!decodeStatus(p, len, s)) return kAckRejectedInvalid;
        Lock l;
        g_state.onStatus(src, s, mono);
        return kAckAccepted;
      }
      case MsgType::CalState: {
        CalStateMsg c;
        if (!decodeCalState(p, len, c)) return kAckRejectedInvalid;
        Lock l;
        return g_state.onCalState(src, c, mono) ? kAckAccepted : kAckRejectedInvalid;
      }
      case MsgType::CommandResult: {
        CommandResultMsg r;
        if (!decodeCommandResult(p, len, r)) return kAckRejectedInvalid;
        Lock l;
        g_state.commands.onResult(src, r);
        return kAckAccepted;
      }
      case MsgType::DiagResult: {
        DiagFragmentMsg f;
        if (!decodeDiagFragment(p, len, f)) return kAckRejectedInvalid;
        Lock l;
        DiagSession& d = g_state.diag;
        DiagNodePart& part = d.part[nodeIndex(src)];
        if (!d.active || !part.requested || part.done) return kAckAccepted;  // tardio: descartar
        const Reassembler::Result r = part.reasm.add(f, millis());
        if (r == Reassembler::kRejected) return kAckRejectedInvalid;
        if (r == Reassembler::kComplete) { part.done = true; part.result = 1; }
        return kAckAccepted;
      }
      default:
        return kAckUnsupported;
    }
  }
  void onDelivery(uint8_t dst, MsgType type, uint32_t cookie, Delivery result) override {
    Lock l;
    if (type == MsgType::DiagRequest) {
      DiagNodePart& part = g_state.diag.part[nodeIndex(dst)];
      if (result != Delivery::Delivered && g_state.diag.active && !part.done) {
        part.done = true;
        part.result = result == Delivery::PeerRebooted ? 3 : 2;
      }
      return;
    }
    if (type == MsgType::Calibration || type == MsgType::Config || type == MsgType::Command)
      g_state.commands.onDelivery(cookie, result, monoMs());
  }
  void onSessionConfirmed(uint8_t peer, uint32_t peerBoot) override {
    {
      Lock l;
      g_state.node(peer).boot = peerBoot;
    }
    g_needSync[nodeIndex(peer)] = true;
    Serial.printf("Sesion ESP-NOW confirmada con %s (boot %08lx).\n", nodeKey(peer), (unsigned long)peerBoot);
  }
  void onHello(uint8_t peer, const HelloMsg& hello) override {
    Lock l;
    g_state.onHello(peer, hello, 0, monoMs());
  }
} g_radio;

bool sendToNode(uint8_t node, MsgType type, const uint8_t* payload, size_t len, uint32_t cookie, uint32_t ttlMs) {
  if (!g_espnowEnabled) return false;
  SendOptions o;
  o.cookie = cookie;
  o.ttlMs = ttlMs;
  o.maxAttempts = 5;
  o.persistent = true;       // espera al nodo dentro del plazo
  o.rebindOnReboot = false;  // un comando nunca se reenvia a un arranque distinto
  return g_ep->sendReliable(node, type, payload, len, o, millis());
}

void trackAndSend(const CloudCommand& c, uint8_t node, MsgType type, const uint8_t* payload, size_t len,
                  uint32_t cmdId, uint8_t op, uint8_t target, uint32_t groupId, uint32_t ttlMs) {
  const int64_t mono = monoMs();
  TrackedCommand* t;
  {
    Lock l;
    t = g_state.commands.add(cmdId, node, uint8_t(type), op, target, c.sensor, c.commandId, c.hasCommandId, groupId,
                             mono, mono + ttlMs);
    if (!t) { g_eventsDropped++; return; }
  }
  const bool queued = sendToNode(node, type, payload, len, cmdId, ttlMs);
  if (!queued) {
    Lock l;
    TrackedCommand* x = g_state.commands.find(cmdId);
    if (x) { x->state = g_espnowEnabled ? kCmdFailed : kCmdRejected; x->needsPublish = true; }
  }
}

uint32_t ttlFor(const CloudCommand& c, int64_t utcMs, bool& expired) {
  expired = false;
  uint32_t ttl = COMMAND_RADIO_TTL_MS;
  if (c.hasExpiresAt && utcMs) {
    const int64_t remaining = int64_t(c.expiresAt) * 1000 - utcMs;
    if (remaining <= 0) expired = true;  // una reconexion no reactiva comandos vencidos
    else if (remaining < int64_t(ttl)) ttl = uint32_t(remaining);
  }
  return ttl;
}

void sendCalibration(const CloudCommand& c, uint8_t node, uint8_t op, uint8_t target, uint32_t groupId,
                     const uint8_t* blob, size_t blobLen, bool force) {
  int64_t utc = 0;
  utcNow(utc);
  bool expired = false;
  const uint32_t ttl = ttlFor(c, utc, expired);
  const uint32_t cmdId = ++g_cmdCounter;
  if (expired) {
    Lock l;
    TrackedCommand* t = g_state.commands.add(cmdId, node, uint8_t(MsgType::Calibration), op, target, c.sensor,
                                             c.commandId, c.hasCommandId, groupId, monoMs(), monoMs());
    if (t) t->state = kCmdExpired;
    return;
  }
  CalibrationCmd cmd = c.cal;
  cmd.cmdId = cmdId;
  cmd.op = op;
  cmd.target = target;
  cmd.force = force;
  if (blob) { cmd.blobLen = uint8_t(blobLen); memcpy(cmd.blob, blob, blobLen); }
  uint8_t buf[kMaxPayload];
  const size_t n = encodeCalibration(cmd, buf, sizeof(buf));
  if (!n) return;
  trackAndSend(c, node, MsgType::Calibration, buf, n, cmdId, op, target, groupId, ttl);
}

void startDiagnostics(const CloudCommand& c, bool fromSerial) {
  const uint8_t s = c.scope;
  const bool wantA = s == kDiagFull || s == kDiagAnalog || s == kDiagColor || s == kDiagSystem || s == kDiagTemperature;
  const bool wantB = s == kDiagFull || s == kDiagI2c || s == kDiagRecover || s == kDiagSystem || s == kDiagO2;
  uint32_t id;
  bool send[2] = {false, false};
  {
    Lock l;
    DiagSession& d = g_state.diag;
    if (d.active) {
      xSemaphoreGive(g_lock);
      reject("diagnostic_report", "diagnostics", "diagnostics_busy", c);
      xSemaphoreTake(g_lock, portMAX_DELAY);
      return;
    }
    id = ++g_diagCounter;
    d.active = true; d.ready = false; d.fromSerial = fromSerial; d.id = id; d.scope = s; d.startMono = monoMs();
    for (uint8_t i = 0; i < 2; ++i) {
      DiagNodePart& p = d.part[i];
      p.requested = i == 0 ? wantA : wantB;
      p.done = !p.requested;
      p.result = 0;
      p.reasm.cancel();
      if (!p.requested) continue;
      if (!g_espnowEnabled || !g_ep->sessionConfirmed(i == 0 ? kNodeA : kNodeB)) {
        p.done = true;
        p.result = 3;  // node_offline: no se espera
        continue;
      }
      p.reasm.expect(id, millis());
      send[i] = true;
    }
  }
  for (uint8_t i = 0; i < 2; ++i) {
    if (!send[i]) continue;
    DiagRequestMsg m;
    m.requestId = id;
    m.scope = s;
    uint8_t buf[kDiagRequestSize];
    const size_t n = encodeDiagRequest(m, buf, sizeof(buf));
    SendOptions o;
    o.cookie = 0xD0000000u | id; o.ttlMs = 20000; o.maxAttempts = 5; o.persistent = true;
    if (!g_ep->sendReliable(i == 0 ? kNodeA : kNodeB, MsgType::DiagRequest, buf, n, o, millis())) {
      Lock l;
      g_state.diag.part[i].done = true;
      g_state.diag.part[i].result = 2;
    }
  }
  Serial.printf("Diagnostico #%lu (%s) solicitado.\n", (unsigned long)id, diagScopeName(s));
}

uint16_t currentExpectedMask(uint8_t node) {
  uint16_t mask = 0;
  for (uint8_t s = 0; s < kSensorCount; ++s)
    if (sensorInfo(s).owner == node && (g_state.latest(s).flags & kRecExpected)) mask |= uint16_t(1u << s);
  return mask;
}

void handleRequest(const Request& r, uint32_t nowMs) {
  const CloudCommand& c = r.cmd;
  int64_t utc = 0;
  const bool utcValid = utcNow(utc);
  switch (c.kind) {
    case CloudKind::Actuator:
      g_actuators.handle(c.act, nowMs, utcValid, utc / 1000);
      break;
    case CloudKind::Calibration:
      if (c.rejectStatus) {
        Lock l;
        const uint32_t id = ++g_cmdCounter;
        TrackedCommand* t = g_state.commands.add(id, c.node ? c.node : kNodeGateway, uint8_t(MsgType::Calibration),
                                                 c.cal.op, c.cal.target, c.sensor, c.commandId, c.hasCommandId, 0,
                                                 monoMs(), monoMs());
        if (t) { t->state = kCmdRejected; t->status = statusFromName(c.rejectStatus); }
        break;
      }
      sendCalibration(c, c.node, c.cal.op, c.cal.target, 0, nullptr, 0, false);
      break;
    case CloudKind::ResetAll: {
      // Un comando por nodo; resultado por nodo y resumen parcial. Sin falsa atomicidad.
      const uint32_t group = ++g_groupCounter;
      {
        Lock l;
        rememberGroup(group, "reset_all", c);
      }
      sendCalibration(c, kNodeA, kCalOpResetAll, kTargetAll, group, nullptr, 0, false);
      sendCalibration(c, kNodeB, kCalOpResetAll, kTargetAll, group, nullptr, 0, false);
      break;
    }
    case CloudKind::CalImport: {
      if (c.rejectStatus) { reject("calibration_ack", "calibration_import", c.rejectStatus, c); break; }
      const uint32_t group = ++g_groupCounter;
      {
        Lock l;
        rememberGroup(group, "calibration_import", c);
      }
      uint8_t blob[kNodeACalEncodedSize];
      if (c.importHasA) {
        const size_t n = encodeNodeACal(c.importA, blob, sizeof(blob));
        sendCalibration(c, kNodeA, kCalOpImport, kTargetAll, group, blob, n, c.force);
      }
      if (c.importHasB) {
        const size_t n = encodeNodeBCal(c.importB, blob, sizeof(blob));
        sendCalibration(c, kNodeB, kCalOpImport, kTargetAll, group, blob, n, c.force);
      }
      break;
    }
    case CloudKind::Config: {
      if (c.rejectStatus) { reject("command_result", "config", c.rejectStatus, c); break; }
      ConfigCmd cfg = c.cfg;
      cfg.cmdId = ++g_cmdCounter;
      if (cfg.present & 2) {
        Lock l;
        const uint16_t cur = currentExpectedMask(c.node);
        cfg.expectedMask = uint16_t((cur & ~c.expectedTouched) | (c.cfg.expectedMask & c.expectedTouched));
      }
      uint8_t buf[kConfigSize];
      const size_t n = encodeConfig(cfg, buf, sizeof(buf));
      CloudCommand named = c;
      strncpy(named.sensor, "config", sizeof(named.sensor) - 1);
      trackAndSend(named, c.node, MsgType::Config, buf, n, cfg.cmdId, 0, 0, 0, COMMAND_RADIO_TTL_MS);
      break;
    }
    case CloudKind::Reboot: {
      for (uint8_t node : {uint8_t(kNodeA), uint8_t(kNodeB)}) {
        const bool want = c.rebootTarget == kRebootAll || (node == kNodeA && c.rebootTarget == kRebootNodeA) ||
                          (node == kNodeB && c.rebootTarget == kRebootNodeB);
        if (!want) continue;
        GenericCmd g;
        g.cmdId = ++g_cmdCounter;
        g.op = kOpReboot;
        uint8_t buf[kGenericCmdSize];
        const size_t n = encodeGeneric(g, buf, sizeof(buf));
        CloudCommand named = c;
        strncpy(named.sensor, "reboot", sizeof(named.sensor) - 1);
        trackAndSend(named, node, MsgType::Command, buf, n, g.cmdId, kOpReboot, 0, 0, COMMAND_RADIO_TTL_MS);
      }
      break;
    }
    case CloudKind::Diagnostics:
      if (c.rejectStatus) { Serial.println("Diagnostics: scope desconocido."); break; }
      startDiagnostics(c, r.fromSerial);
      break;
    default:
      break;
  }
}

void serviceDiagnostics(uint32_t nowMs) {
  Lock l;
  DiagSession& d = g_state.diag;
  if (!d.active || d.ready) return;
  const bool timedOut = monoMs() - d.startMono >= int64_t(DIAG_TIMEOUT_MS);
  bool allDone = true;
  for (auto& p : d.part) {
    if (!p.requested || p.done) continue;
    if (timedOut || p.reasm.timedOut(nowMs)) { p.done = true; p.result = 2; }
    else allDone = false;
  }
  if (allDone) d.ready = true;
}

void serviceTimeSync(uint32_t nowMs) {
  int64_t utc = 0;
  if (!g_espnowEnabled || !utcNow(utc)) return;
  for (uint8_t i = 0; i < 2; ++i) {
    const uint8_t node = i == 0 ? kNodeA : kNodeB;
    if (!g_ep->sessionConfirmed(node)) continue;
    if (!g_needSync[i] && nowMs - g_lastSyncMs[i] < TIME_SYNC_INTERVAL_MS) continue;
    TimeSyncMsg m;
    m.utcValid = 1;
    m.utcMs = utc;
    m.gatewayUptimeMs = nowMs;
    uint8_t buf[kTimeSyncSize];
    const size_t n = encodeTimeSync(m, buf, sizeof(buf));
    if (g_ep->sendUnreliable(node, MsgType::TimeSync, buf, n)) {
      g_needSync[i] = false;
      g_lastSyncMs[i] = nowMs;
    }
  }
}

void localTask(void*) {
  esp_task_wdt_add(nullptr);
  uint32_t lastSnapshotMs = millis();
  static Request req;  // grande: fuera de la pila
  uint32_t announceUntilMs = 0, lastAnnounceMs = 0;
  for (;;) {
    const uint32_t nowMs = millis();
    g_localHeartbeat = nowMs;
    esp_task_wdt_reset();
    if (g_espnowEnabled) {
      RxFrame f;
      for (uint8_t i = 0; i < 12 && g_port.pop(f); ++i) g_ep->receive(f.data, f.len, millis(), f.rssi);
      // Cambio de canal (hotspot en otro canal): HELLO con el canal nuevo y anuncio
      // durante 60 s a los nodos sin trama fresca. A/B lo encuentran al barrer.
      const uint8_t newCh = g_channelAnnounce;
      if (newCh) {
        g_channelAnnounce = 0;
        g_hello.channel = newCh;
        g_ep->setHelloTemplate(g_hello);
        announceUntilMs = nowMs + 60000;
        lastAnnounceMs = nowMs - 2000;
      }
      if (announceUntilMs && int32_t(nowMs - announceUntilMs) < 0 && nowMs - lastAnnounceMs >= 2000) {
        lastAnnounceMs = nowMs;
        for (uint8_t node : {uint8_t(kNodeA), uint8_t(kNodeB)})
          if (!g_ep->everReceived(node) || nowMs - g_ep->lastFreshRxMs(node) > 3000)
            g_ep->sendHello(node, g_hello, g_ep->peerBoot(node), nowMs);
      }
      g_ep->poll(millis());
      // Presencia de los nodos (tramas frescas autenticadas).
      const int64_t mono = monoMs();
      const uint32_t ms = millis();
      Lock l;
      for (uint8_t node : {uint8_t(kNodeA), uint8_t(kNodeB)})
        if (g_ep->everReceived(node))
          g_state.touch(node, mono - int64_t(uint32_t(ms - g_ep->lastFreshRxMs(node))), g_ep->lastRssi(node));
    }
    if (xQueueReceive(g_requests, &req, 0) == pdTRUE) handleRequest(req, millis());
    int64_t utc = 0;
    const bool utcValid = utcNow(utc);
    if (g_stopForReboot && !g_stoppedForReboot) {
      g_actuators.stopAll("reboot", millis());
      g_outputs.forceOff();
      g_stoppedForReboot = true;
    }
    g_actuators.service(millis(), utcValid, utc / 1000);
    {
      Lock l;
      g_actShared = g_actuators.state(millis());
      if (g_actuators.reportPending()) {
        g_actReportPending = true;
        g_actuators.clearReportPending();
      }
    }
    serviceTimeSync(millis());
    serviceDiagnostics(millis());
    if (millis() - lastSnapshotMs >= TELEMETRY_INTERVAL_MS) {
      lastSnapshotMs = millis();
      Lock l;
      g_state.takeSnapshot(monoMs(), utcValid ? utc : 0, g_actShared, NODE_LINK_TIMEOUT_MS);
    }
    {
      Lock l;
      g_state.commands.expire(monoMs(), COMMAND_RESULT_TIMEOUT_MS);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ---------------- loop(): comandos entrantes ----------------
Request g_incoming;  // solo loop()

void enqueue(bool fromSerial) {
  g_incoming.fromSerial = fromSerial;
  if (xQueueSend(g_requests, &g_incoming, 0) != pdTRUE) reject("command_result", "any", "gateway_busy", g_incoming.cmd);
}

void onCloudMessage(const uint8_t* payload, size_t len) {
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, payload, len);
  if (err) {
    Serial.printf("Error parseando JSON C2D: %s\n", err.c_str());
    return;
  }
  Serial.printf("C2D: %s\n", doc["action"] | "");
  g_incoming = Request();
  g_incoming.cmd = parseCloudCommand(doc.as<JsonObjectConst>());
  const CloudCommand& c = g_incoming.cmd;
  switch (c.kind) {
    case CloudKind::Unknown: case CloudKind::Invalid:
      Serial.println("Accion desconocida");
      return;
    case CloudKind::CalExport: {
      Lock l;
      g_export.pending = true;
      strncpy(g_export.commandId, c.hasCommandId ? c.commandId : "", 64);
      return;
    }
    case CloudKind::Reboot:
      if (c.rejectStatus) { reject("command_result", "reboot", c.rejectStatus, c); return; }
      if (c.rebootTarget == kRebootGateway || c.rebootTarget == kRebootAll) {
        // Tras el PUBACK (v4); "all" deja 5 s para entregar los reinicios de A y B.
        g_rebootAtMs = millis() + (c.rebootTarget == kRebootAll ? 5000 : 1000);
        if (!g_rebootAtMs) g_rebootAtMs = 1;
        Serial.println("Reboot solicitado por C2D.");
      }
      if (c.rebootTarget != kRebootGateway) enqueue(false);
      return;
    default:
      enqueue(false);
  }
}

// ---------------- loop(): publicacion ----------------
GatewayInfo makeInfo() {
  GatewayInfo i;
  i.deviceId = DEVICE_ID;
  i.experimentId = EXPERIMENT_ID;
  i.pinProfile = ACTUATOR_PIN_PROFILE_NAME;
  i.bootId = g_ep ? g_ep->bootId() : 0;
  i.nowMono = monoMs();
  i.uptimeMs = i.nowMono;
  i.wifiConnected = g_azure.wifiConnected();
  i.mqttConnected = g_azure.mqttConnected();
  i.wifiRssi = g_azure.rssi();
  i.wifiChannel = g_azure.wifiChannel();
  i.espnowChannel = g_espnowEnabled ? g_port.channel() : 0;
  i.espnowEnabled = g_espnowEnabled;
  i.wifiSearchState = g_azure.searchStateName();
  i.wifiEverConnected = g_azure.msSinceConnected(i.wifiMsSinceConnected);
  i.espnowChannelChanges = g_azure.channelChanges();
  i.freeHeap = ESP.getFreeHeap();
  i.minFreeHeap = ESP.getMinFreeHeap();
  i.maxAllocHeap = ESP.getMaxAllocHeap();
  i.resetReason = resetReasonName();
  i.utcValid = utcNow(i.utcMs);
  i.mqttPublishOk = g_azure.publishOk;
  i.mqttPublishFail = g_azure.publishFail;
  i.telemetryIntervalMs = TELEMETRY_INTERVAL_MS;
  return i;
}

bool publishBuffer(size_t n, const char* what) {
  if (!n) {
    Serial.printf("%s: JSON no generado (memoria/tamano).\n", what);
    return true;  // no reintentar indefinidamente un JSON imposible
  }
  Serial.printf("%s: %u bytes\n", what, unsigned(n));
  Serial.println(g_pub);
  if (!g_azure.mqttConnected()) return false;
  const bool ok = g_azure.publish(g_pub, n);
  Serial.println(ok ? "Publicado (QoS 0: aceptado por la biblioteca, sin confirmacion de IoT Hub)." : "Error publicando.");
  return ok;
}

void dropStaleEvents() {
  Lock l;
  const int64_t now = monoMs();
  for (uint8_t i = 0; i < CommandTracker::kSize; ++i) {
    TrackedCommand* t = g_state.commands.nextToPublish();
    if (!t || now - t->createdMono < int64_t(GATEWAY_EVENT_MAX_AGE_MS)) break;
    t->needsPublish = false;
    g_eventsDropped++;
  }
  if (g_rejection.pending && now - g_rejection.mono > int64_t(GATEWAY_EVENT_MAX_AGE_MS)) {
    g_rejection.pending = false;
    g_eventsDropped++;
  }
}

void publishPending() {
  const uint32_t nowMs = millis();
  if (!g_pub) return;
  if (!g_azure.mqttConnected()) {
    dropStaleEvents();
    // Sin nube: el informe de diagnostico solo se imprime por Serial.
    Lock l;
    if (g_state.diag.ready) {
      const GatewayInfo info = makeInfo();
      const size_t n = buildDiagnosticReport(g_state, info, g_pub, MQTT_PACKET_SIZE);
      Serial.println(n ? g_pub : "Diagnostico: informe demasiado grande.");
      if (g_state.diag.fromSerial || monoMs() - g_state.diag.startMono > int64_t(GATEWAY_EVENT_MAX_AGE_MS)) {
        g_state.diag.active = false;
        g_state.diag.ready = false;
      }
    }
    return;
  }
  if (nowMs - g_lastPublishMs < 250) return;
  g_lastPublishMs = nowMs;
  const GatewayInfo info = makeInfo();
  if (ESP.getMaxAllocHeap() < 30000) {
    Serial.println("Publicacion aplazada: heap contiguo insuficiente.");
    return;
  }
  // 1. Estado de actuadores tras un cambio.
  {
    Lock l;
    if (g_actReportPending) {
      const size_t n = buildActuatorEvent(g_actShared, info, g_pub, MQTT_PACKET_SIZE);
      xSemaphoreGive(g_lock);
      const bool ok = publishBuffer(n, "actuator_state");
      xSemaphoreTake(g_lock, portMAX_DELAY);
      if (ok) g_actReportPending = false;
      return;
    }
  }
  // 2. Eventos de comandos (pendiente/aplicado/rechazado/vencido/timeout) y resumenes de grupo.
  {
    Lock l;
    TrackedCommand* t = g_state.commands.nextToPublish();
    if (t) {
      const size_t n = buildCalibrationAck(*t, g_state, info, g_pub, MQTT_PACKET_SIZE);
      const uint32_t cmdId = t->cmdId;
      xSemaphoreGive(g_lock);
      const bool ok = publishBuffer(n, "calibration_ack");
      xSemaphoreTake(g_lock, portMAX_DELAY);
      TrackedCommand* again = g_state.commands.find(cmdId);
      if (ok && again) again->needsPublish = false;
      return;
    }
    for (auto& g : g_groups) {
      if (!g.used) continue;
      uint8_t applied, total;
      if (!g_state.commands.groupFinal(g.groupId, applied, total)) continue;
      const size_t n = buildGroupSummary(g.groupId, g.kind, g.commandId, g_state, info, applied, total, g_pub, MQTT_PACKET_SIZE);
      xSemaphoreGive(g_lock);
      const bool ok = publishBuffer(n, "calibration_ack (grupo)");
      xSemaphoreTake(g_lock, portMAX_DELAY);
      if (ok) g.used = false;
      return;
    }
    if (g_rejection.pending) {
      const size_t n = buildRejection(g_rejection.type, g_rejection.action, g_rejection.status, g_rejection.commandId,
                                      info, g_pub, MQTT_PACKET_SIZE);
      xSemaphoreGive(g_lock);
      const bool ok = publishBuffer(n, "rechazo");
      xSemaphoreTake(g_lock, portMAX_DELAY);
      if (ok) g_rejection.pending = false;
      return;
    }
    if (g_export.pending) {
      const size_t n = buildCalibrationExport(g_state, info, g_export.commandId, g_pub, MQTT_PACKET_SIZE);
      xSemaphoreGive(g_lock);
      const bool ok = publishBuffer(n, "calibration_export");
      xSemaphoreTake(g_lock, portMAX_DELAY);
      if (ok) g_export.pending = false;
      return;
    }
    // 3. Informe de diagnostico agregado.
    if (g_state.diag.ready) {
      const size_t n = buildDiagnosticReport(g_state, info, g_pub, MQTT_PACKET_SIZE);
      xSemaphoreGive(g_lock);
      const bool ok = publishBuffer(n, "diagnostic_report");
      xSemaphoreTake(g_lock, portMAX_DELAY);
      if (ok || !n) { g_state.diag.active = false; g_state.diag.ready = false; }
      return;
    }
    // 4. Telemetria: la instantanea mas antigua pendiente (conserva su instante original).
    Snapshot* s = g_state.oldestUnpublished();
    if (s) {
      const size_t n = buildTelemetryJson(*s, g_state, info, g_pub, MQTT_PACKET_SIZE);
      const uint32_t seq = s->seq;
      xSemaphoreGive(g_lock);
      const bool ok = publishBuffer(n, "Telemetria");
      xSemaphoreTake(g_lock, portMAX_DELAY);
      // Se marca por seq: la instantanea pudo sobrescribirse durante el publish.
      Snapshot* again = g_state.oldestUnpublished();
      if (ok && again && again->seq == seq) again->published = true;
    }
  }
}

// ---------------- loop(): Serial ----------------
void printStatus() {
  JsonDocument doc;
  const GatewayInfo info = makeInfo();
  doc["node"] = "gateway";
  doc["sta_mac"] = WiFi.macAddress();
  doc["wifi_state"] = g_azure.stateName();
  doc["wifi_channel"] = info.wifiChannel;
  doc["espnow_channel"] = info.espnowChannel;
  doc["wifi_search_state"] = info.wifiSearchState;
  if (info.wifiEverConnected) doc["wifi_ms_since_connected"] = info.wifiMsSinceConnected;
  else doc["wifi_ms_since_connected"] = nullptr;
  doc["espnow_channel_changes"] = info.espnowChannelChanges;
  doc["wifi_next_full_scan_s"] = g_azure.nextFullScanInS();
  doc["wifi_attempts"] = g_azure.wifiAttempts();
  doc["wifi_full_scans"] = g_azure.fullScans();
  doc["wifi_credential_source"] = g_azure.credentialSource();  // nunca el SSID
  doc["wifi_credentials_stored"] = g_azure.hasStoredCredentials();
  doc["wifi_portal_open"] = g_azure.portalOpen();
  doc["wifi_portal_remaining_s"] = g_azure.portalRemainingS();
  doc["espnow_enabled"] = g_espnowEnabled;
  doc["espnow_provisioning_required"] = secretsArePlaceholders();
  doc["azure_configured"] = g_azure.azureConfigured();
  doc["mqtt_connected"] = info.mqttConnected;
  doc["utc_valid"] = info.utcValid;
  doc["free_heap"] = info.freeHeap;
  doc["min_free_heap"] = info.minFreeHeap;
  doc["actuation"] = ACTUATOR_COMMISSIONING_MODE ? "commissioning_simulated" : "production";
  doc["pin_profile"] = ACTUATOR_PIN_PROFILE_NAME;
  doc["events_dropped"] = g_eventsDropped;
  doc["mqtt_publish_ok"] = g_azure.publishOk;
  doc["mqtt_publish_fail"] = g_azure.publishFail;
  {
    Lock l;
    doc["snapshots_unpublished"] = g_state.unpublishedCount();
    doc["snapshots_dropped"] = g_state.snapshotsDropped();
    for (uint8_t node : {uint8_t(kNodeA), uint8_t(kNodeB)}) {
      JsonObject n = doc[nodeKey(node)].to<JsonObject>();
      n["online"] = g_state.nodeOnline(node, monoMs(), NODE_LINK_TIMEOUT_MS);
      n["calibration_copy"] = g_state.node(node).calValid;
      n["telemetry_frames"] = g_state.node(node).telemetryFrames;
    }
  }
  if (g_espnowEnabled) {
    const LinkStats& st = g_ep->stats();
    JsonObject l = doc["espnow_link"].to<JsonObject>();
    l["rx_ok"] = st.rxOk; l["rx_rejected"] = st.rxRejected; l["rx_bad_tag"] = st.rxBadTag;
    l["rx_stale_session"] = st.rxStaleSession; l["replay_rejected"] = st.replayRejected;
    l["retries"] = st.retries; l["delivered"] = st.delivered; l["failed"] = st.failed;
    l["rx_ring_dropped"] = g_port.rxDropped(); l["unknown_mac"] = g_port.rxUnknownMac;
  }
  serializeJson(doc, Serial);
  Serial.println();
}

void pollSerial() {
  for (uint16_t budget = 0; budget < 128 && Serial.available(); ++budget) {
    if (!g_serialLine.feed(char(Serial.read()))) continue;
    JsonDocument doc;
    if (deserializeJson(doc, g_serialLine.line())) { Serial.println("Serial: JSON invalido."); continue; }
    const char* action = doc["action"] | "";
    if (!strcmp(action, "diagnostics")) {
      g_incoming = Request();
      g_incoming.cmd = parseCloudCommand(doc.as<JsonObjectConst>());
      enqueue(true);
    } else if (!strcmp(action, "status")) {
      printStatus();
    } else if (!strcmp(action, "pairing_info")) {
      Serial.printf("{\"node\":\"gateway\",\"sta_mac\":\"%s\",\"espnow_channel\":%u}\n", WiFi.macAddress().c_str(),
                    g_espnowEnabled ? g_port.channel() : 0);
    } else if (!strcmp(action, "wifi_portal")) {
      g_azure.startPortal();
    } else {
      // Como v4: por Serial no se abre control de cargas ni calibraciones.
      Serial.println("Serial: acciones admitidas: diagnostics, status, pairing_info, wifi_portal.");
    }
  }
}

}  // namespace

void nodeCSetup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n===== BioIoT Nodo C (gateway ESP-NOW + Azure + actuadores) =====");
  if (!g_pub) {
    Serial.println("Sin memoria para el buffer de publicacion: Azure deshabilitado.");
  }
  g_lock = xSemaphoreCreateMutex();
  g_requests = xQueueCreate(4, sizeof(Request));
  for (uint8_t s = 0; s < kSensorCount; ++s)
    g_state.maxAgeMs[s] = (s == kSensorO2Gas1 || s == kSensorO2Gas2) ? SENSOR_O2_MAX_AGE_MS : SENSOR_FAST_MAX_AGE_MS;

  // 1. Actuadores antes que cualquier red: arranque apagado, sin depender de Internet.
  const bool ready = g_outputs.begin(ACTUATOR_COMMISSIONING_MODE, COMPRESSOR_ELECTRICAL_CONFIRMED, LED_ELECTRICAL_CONFIRMED);
  ActuatorConfig acfg;
  acfg.commissioning = ACTUATOR_COMMISSIONING_MODE;
  acfg.compressorConfirmed = COMPRESSOR_ELECTRICAL_CONFIRMED;
  acfg.ledConfirmed = LED_ELECTRICAL_CONFIRMED;
  acfg.outputsReady = ready;
  acfg.compressorMinOffMs = COMPRESSOR_MIN_OFF_MS;
  acfg.maxLeaseSeconds = ACTUATOR_MAX_LEASE_SECONDS;
  LocalProgram program;
  program.enabled = LOCAL_PROGRAM_ENABLED;
  program.lightOnMinuteUtc = LOCAL_LIGHT_ON_MINUTE_UTC;
  program.lightOffMinuteUtc = LOCAL_LIGHT_OFF_MINUTE_UTC;
  program.r = LOCAL_LIGHT_R; program.g = LOCAL_LIGHT_G; program.b = LOCAL_LIGHT_B;
  program.brightness = LOCAL_LIGHT_BRIGHTNESS;
  program.aerationOnSeconds = LOCAL_AERATION_ON_SECONDS;
  program.aerationOffSeconds = LOCAL_AERATION_OFF_SECONDS;
  g_actuators.begin(acfg, program, &g_outputs, millis());
  g_actShared = g_actuators.state(millis());
  Serial.printf("Actuadores: %s, perfil %s, compresor %s, luces %s.\n",
                ACTUATOR_COMMISSIONING_MODE ? "PUESTA EN MARCHA (sin accionar cargas)" : "produccion",
                ACTUATOR_PIN_PROFILE_NAME, COMPRESSOR_ELECTRICAL_CONFIRMED ? "confirmado" : "sin confirmar",
                LED_ELECTRICAL_CONFIRMED ? "confirmadas" : "sin confirmar");

  // 2. Radio: STA sin auto-reconexion (evita barridos que muevan el canal de ESP-NOW).
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.setSleep(false);
  Serial.printf("MAC STA gateway: %s\n", WiFi.macAddress().c_str());
  uint8_t channel = BIOIOT_INITIAL_CHANNEL;
  {
    Preferences p;
    if (p.begin("bioiot_c", true)) { channel = p.getUChar("ch", channel); p.end(); }
  }
  const bool selfTest = protocolSelfTest();
  Serial.printf("Autoprueba de protocolo: %s\n", selfTest ? "OK" : "FALLO");
  if (secretsArePlaceholders()) {
    Serial.println("ESP-NOW DESHABILITADO: faltan gateway_secrets.h reales (ver docs/EMPAREJAMIENTO.md).");
  } else if (!selfTest) {
    Serial.println("ESP-NOW DESHABILITADO: autoprueba de serializacion fallida.");
  } else if (!g_port.begin(kPmk, channel, BIOIOT_SYSTEM_ID, kNodeGateway)) {
    Serial.printf("ESP-NOW DESHABILITADO: %s\n", g_port.lastError());
  } else {
    EspNowPeerConfig a, b;
    a.node = kNodeA; memcpy(a.mac, kMacA, 6); memcpy(a.lmk, kLmkA, 16);
    b.node = kNodeB; memcpy(b.mac, kMacB, 6); memcpy(b.lmk, kLmkB, 16);
    if (!g_port.addPeer(a) || !g_port.addPeer(b)) {
      Serial.printf("ESP-NOW DESHABILITADO: %s\n", g_port.lastError());
    } else {
      static Endpoint ep(BIOIOT_SYSTEM_ID, kNodeGateway, esp_random() | 1u, g_port, g_radio, hwRandom);
      g_ep = &ep;
      g_ep->addPeer(kNodeA, kKeyA);
      g_ep->addPeer(kNodeB, kKeyB);
      g_hello.role = kNodeGateway;
      g_hello.channel = channel;
      g_hello.flags = selfTest ? kHelloSelfTestOk : 0;
      g_ep->setHelloTemplate(g_hello);
      g_espnowEnabled = true;
      Serial.printf("ESP-NOW cifrado activo en canal %u; boot %08lx.\n", g_port.channel(), (unsigned long)g_ep->bootId());
    }
  }

  // 3. Tarea local (radio + actuadores), independiente de Wi-Fi/MQTT/TLS.
  const esp_task_wdt_config_t wdt = {.timeout_ms = LOOP_WDT_TIMEOUT_MS, .idle_core_mask = 1 << 0, .trigger_panic = true};
  if (esp_task_wdt_reconfigure(&wdt) != ESP_OK) Serial.println("Watchdog no reconfigurable.");
  xTaskCreatePinnedToCore(localTask, "bioiot_local", 8192, nullptr, 2, nullptr, 1);

  // 4. Red: nunca bloquea el arranque; sensado y control ya funcionan sin Internet.
  g_azure.begin(onCloudMessage, channel);
  if (esp_task_wdt_add(nullptr) != ESP_OK) Serial.println("Watchdog de loop no disponible.");
  Serial.printf("Ultimo reinicio: %s\n", resetReasonName());
}

void nodeCLoop() {
  esp_task_wdt_reset();
  const uint32_t heap = ESP.getFreeHeap();
  if (heap < g_minFreeHeap) g_minFreeHeap = heap;
  g_azure.loop();
  uint8_t ch;
  if (g_azure.takeChannelChange(ch)) {
    Preferences p;
    if (p.begin("bioiot_c", false)) { p.putUChar("ch", ch); p.end(); }  // pista para el proximo arranque
    g_channelAnnounce = ch;  // la tarea local actualiza HELLO y se reanuncia a A/B
    Serial.printf("Canal ESP-NOW ahora %u (impuesto por el AP); A y B lo buscaran (channel_hunting).\n", ch);
  }
  pollSerial();
  publishPending();
  // Vigilancia de la tarea local: si se detiene, los vencimientos no se aplican.
  if (millis() - g_localHeartbeat > LOCAL_TASK_WDT_TIMEOUT_MS && g_localHeartbeat) {
    Serial.println("Tarea local detenida: reinicio (las salidas arrancan apagadas).");
    delay(100);
    ESP.restart();
  }
  if (g_rebootAtMs && int32_t(millis() - g_rebootAtMs) >= 0) {
    g_stopForReboot = true;
    const uint32_t t0 = millis();
    while (!g_stoppedForReboot && millis() - t0 < 500) delay(10);
    Serial.println("Reiniciando gateway.");
    delay(100);
    ESP.restart();
  }
  delay(2);
}
