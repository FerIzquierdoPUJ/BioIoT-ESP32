#pragma once
// Formato JSON versionado de exportacion/importacion de calibraciones:
//   {"format":"bioiot-calibration-export","format_version":1, "source":..., "device_id":...,
//    "exported_at_utc":..., "legacy":{"schema":2,"ver":N},
//    "node_a":{...}, "node_b":{...}}
// Lo produce la herramienta de exportacion del firmware v4 (lectura NVS solo
// lectura), los nodos por Serial y el gateway (copias confirmadas). Cada nodo
// importa solo su seccion. Cualquier campo invalido rechaza la seccion completa.
#include <ArduinoJson.h>

#include "bioiot_calibration.h"

namespace bioiot {

constexpr const char* kCalExportFormat = "bioiot-calibration-export";
constexpr int kCalExportFormatVersion = 1;

struct CalImportResult {
  bool formatOk = false;
  bool hasA = false, hasB = false;
  bool aValid = false, bValid = false;
  const char* error = "";
};

// Lee el documento y llena a/b (con importedMask, legacyVersion y revisiones +1).
CalImportResult parseCalibrationExport(JsonObjectConst root, NodeACal& a, NodeBCal& b);

void writeNodeACalExport(JsonObject o, const NodeACal& a);
void writeNodeBCalExport(JsonObject o, const NodeBCal& b);
// Documento completo; a o b pueden ser nullptr.
void writeCalibrationExport(JsonObject root, const char* source, const char* deviceId, const char* utcIso,
                            const NodeACal* a, const NodeBCal* b);

}  // namespace bioiot
