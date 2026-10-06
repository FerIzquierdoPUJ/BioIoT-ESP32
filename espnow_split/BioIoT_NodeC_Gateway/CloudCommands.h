#pragma once
// Interpretacion de comandos Cloud-to-Device (y Serial de diagnostico) en estructuras
// tipadas. Mantiene los nombres y validaciones de v4 y los enruta al nodo propietario.
#include <ArduinoJson.h>

#include "ActuatorCore.h"
#include "BioIoTCommon.h"

namespace gw {

enum class CloudKind : uint8_t {
  Invalid = 0,
  Unknown,
  Diagnostics,     // {"action":"diagnostics","scope":"full|i2c|analog|color|system|quick|i2c_recover|temperature|o2"}
  Actuator,        // {"action":"control",...} | {"action":"all_off"}
  Calibration,     // set / reset / calibrate (un nodo)
  ResetAll,        // reset_all: un comando por nodo, resultado por nodo (sin falsa atomicidad)
  Reboot,          // {"action":"reboot"[,"node":"gateway|node_a|node_b|all"]}
  Config,          // {"action":"config","node":"node_a","report_interval_s":5,"expected":{"turbidity":true}}
  CalImport,       // {"action":"calibration_import","export":{...},"force":false}
  CalExport,       // {"action":"calibration_export"}
};

enum RebootTarget : uint8_t { kRebootGateway = 0, kRebootNodeA = 1, kRebootNodeB = 2, kRebootAll = 3 };

struct CloudCommand {
  CloudKind kind = CloudKind::Invalid;
  bool hasCommandId = false;
  char commandId[65] = "";
  const char* rejectStatus = nullptr;  // rechazo ya decidido en el gateway (texto v4)
  char sensor[16] = "";
  uint8_t node = 0;
  uint8_t scope = 0;
  uint8_t rebootTarget = kRebootGateway;
  bool hasExpiresAt = false;
  uint64_t expiresAt = 0;  // opcional en calibraciones: vencimiento de entrega
  ActuatorCommand act;
  bioiot::CalibrationCmd cal;
  bioiot::ConfigCmd cfg;
  bioiot::NodeACal importA;
  bioiot::NodeBCal importB;
  bool importHasA = false, importHasB = false, force = false;
  uint16_t expectedTouched = 0;  // config: sensores cuyo 'expected' se cambia
};

CloudCommand parseCloudCommand(JsonObjectConst doc);
uint8_t nodeFromName(const char* name);  // "node_a"/"a" -> kNodeA, ...

}  // namespace gw
