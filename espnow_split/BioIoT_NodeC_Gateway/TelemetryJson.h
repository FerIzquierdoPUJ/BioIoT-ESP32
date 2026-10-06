#pragma once
// JSON hacia Azure (UTF-8, mismas propiedades de contenido que v4). Contrato 1.1:
// todos los campos 1.0 se conservan; cambios inevitables documentados en
// docs/MIGRACION.md (calibration.version=null, connected=null y calidades
// stale/node_offline cuando el dato no es actual).
#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

#include "ActuatorCore.h"
#include "GatewayState.h"

namespace gw {

constexpr const char* kSchemaVersion = "1.1";

struct GatewayInfo {
  const char* deviceId = "";
  const char* experimentId = "";
  const char* pinProfile = "";
  uint32_t bootId = 0;
  int64_t uptimeMs = 0;
  bool wifiConnected = false, mqttConnected = false;
  int wifiRssi = 0;
  uint8_t wifiChannel = 0, espnowChannel = 0;
  bool espnowEnabled = false;
  // Busqueda Wi-Fi (hotspot): estado, tiempo sin enlace y cambios de canal de ESP-NOW.
  const char* wifiSearchState = "unknown";
  bool wifiEverConnected = false;
  uint32_t wifiMsSinceConnected = 0;
  uint32_t espnowChannelChanges = 0;
  uint32_t freeHeap = 0, minFreeHeap = 0, maxAllocHeap = 0;
  const char* resetReason = "";
  bool utcValid = false;
  int64_t utcMs = 0;  // ahora
  int64_t nowMono = 0;
  uint32_t mqttPublishOk = 0, mqttPublishFail = 0;
  uint32_t telemetryIntervalMs = 120000;
};

// "YYYY-MM-DDTHH:MM:SSZ" (formato v4). Devuelve false si utcMs no es valido.
bool formatIsoUtc(int64_t utcMs, char out[25]);
// UTC de un instante monotono del gateway (0 si no hay UTC valido ahora).
int64_t utcForMono(const GatewayInfo& info, int64_t mono);

size_t buildTelemetryJson(const Snapshot& snap, const GatewayState& st, const GatewayInfo& info, char* out, size_t cap);
size_t buildActuatorEvent(const ActuatorState& a, const GatewayInfo& info, char* out, size_t cap);
size_t buildCalibrationAck(const TrackedCommand& c, const GatewayState& st, const GatewayInfo& info, char* out,
                           size_t cap);
size_t buildGroupSummary(uint32_t groupId, const char* kind, const char* commandId, const GatewayState& st,
                         const GatewayInfo& info, uint8_t applied, uint8_t total, char* out, size_t cap);
size_t buildCalibrationExport(const GatewayState& st, const GatewayInfo& info, const char* commandId, char* out,
                              size_t cap);
size_t buildDiagnosticReport(const GatewayState& st, const GatewayInfo& info, char* out, size_t cap);
size_t buildRejection(const char* type, const char* action, const char* status, const char* commandId,
                      const GatewayInfo& info, char* out, size_t cap);

// Las mismas estructuras en un JsonDocument, sin buffer de salida: el gateway las
// envia por trozos a MQTT (PublishStream.h). Los textos del estado se copian, asi que
// el documento puede serializarse sin el mutex. doc.overflowed() => falta de heap.
void fillTelemetryJson(JsonDocument& doc, const Snapshot& snap, const GatewayState& st, const GatewayInfo& info);
void fillActuatorEvent(JsonDocument& doc, const ActuatorState& a, const GatewayInfo& info);
void fillCalibrationAck(JsonDocument& doc, const TrackedCommand& c, const GatewayState& st, const GatewayInfo& info);
void fillGroupSummary(JsonDocument& doc, uint32_t groupId, const char* kind, const char* commandId,
                      const GatewayState& st, const GatewayInfo& info, uint8_t applied, uint8_t total);
void fillCalibrationExport(JsonDocument& doc, const GatewayState& st, const GatewayInfo& info, const char* commandId);
void fillDiagnosticReport(JsonDocument& doc, const GatewayState& st, const GatewayInfo& info);
void fillRejection(JsonDocument& doc, const char* type, const char* action, const char* status, const char* commandId,
                   const GatewayInfo& info);

}  // namespace gw
