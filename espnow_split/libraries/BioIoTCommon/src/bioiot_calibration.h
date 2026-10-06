#pragma once
// Datos de calibracion por nodo propietario. A posee pH/turbidez/DO/temperatura/CO2;
// B posee O2 gas. El gateway solo guarda copias confirmadas para reportarlas;
// nunca vuelve a aplicar curvas a valores ya calibrados.
//
// Curvas iniciales: copiadas literalmente de BioIoT_Azure_Integrated.ino v4
// (PH_CAL_*_DEFAULT, TURB_*, DO_*, TEMP_*, CO2_*) y de CalibrationModel.h.
#include <stddef.h>
#include <stdint.h>

#include "bioiot_calibration_model.h"

namespace bioiot {

constexpr float kPhCalMDefault = -6.112459f;
constexpr float kPhCalBDefault = 15.012668f;
constexpr float kTurbCalMDefault = -2310.5f;
constexpr float kTurbCalBDefault = 5435.4f;
constexpr float kDoCalMDefault = 0.13351912f;
constexpr float kTempCalMDefault = 0.9741f;
constexpr float kTempCalBDefault = 1.0038f;
constexpr float kCo2LegacyADefault = 0.0f;
constexpr float kCo2LegacyBDefault = 0.0f;

// Formato binario de calibracion (radio, almacenamiento e importacion).
constexpr uint8_t kCalFormatVersion = 1;

enum CalEntryA : uint8_t { kCalPh = 0, kCalTurb, kCalDo, kCalTemp, kCalCo2_1, kCalCo2_2, kCalACount };
enum CalEntryB : uint8_t { kCalO2_1 = 0, kCalO2_2, kCalBCount };

struct NodeACal {
  uint32_t legacyVersion = 0;  // 'ver' global de v4 importado; solo trazabilidad
  uint32_t legacySchema = 0;   // schema NVS de v4 al exportar
  uint32_t nodeRevision = 0;   // cambios persistidos en este nodo
  float ph_m = kPhCalMDefault, ph_b = kPhCalBDefault;
  float turb_m = kTurbCalMDefault, turb_b = kTurbCalBDefault;
  float do_m = kDoCalMDefault;
  float temp_m = kTempCalMDefault, temp_b = kTempCalBDefault;
  float co2_a = kCo2LegacyADefault, co2_b = kCo2LegacyBDefault;  // instantanea legacy v4
  uint8_t ph_user = 0, turb_user = 0, do_user = 0, temp_user = 0, co2_enabled = 0;
  uint8_t importedMask = 0;  // bit CalEntryA importado desde v4
  Co2Calibration co2[2];
  uint16_t rev[kCalACount] = {};
};

struct NodeBCal {
  uint32_t legacyVersion = 0;
  uint32_t nodeRevision = 0;
  uint8_t importedMask = 0;
  O2Calibration o2[2];
  uint16_t rev[kCalBCount] = {};
};

constexpr size_t kNodeACalEncodedSize = 4 + 4 + 4 + 4 * 10 + 6 + 2 * 20 + 2 * kCalACount;  // 110
constexpr size_t kNodeBCalEncodedSize = 4 + 4 + 1 + 2 * 24 + 2 * kCalBCount;               // 61

size_t encodeNodeACal(const NodeACal& c, uint8_t* out, size_t cap);
bool decodeNodeACal(const uint8_t* in, size_t len, NodeACal& c);
size_t encodeNodeBCal(const NodeBCal& c, uint8_t* out, size_t cap);
bool decodeNodeBCal(const uint8_t* in, size_t len, NodeBCal& c);

bool validNodeACal(const NodeACal& c);
bool validNodeBCal(const NodeBCal& c);
// Ajustes del usuario o importados: impiden importar sin force.
bool nodeACalHasUserData(const NodeACal& c);
bool nodeBCalHasUserData(const NodeBCal& c);

// Objetivos de comandos de calibracion (nombres de sensor v4).
enum CalTarget : uint8_t {
  kTargetNone = 0,
  kTargetPh = 1,
  kTargetTurb = 2,
  kTargetDo = 3,
  kTargetTemp = 4,
  kTargetCo2Both = 5,
  kTargetCo2_1 = 6,
  kTargetCo2_2 = 7,
  kTargetO2Gas1 = 8,
  kTargetO2Gas2 = 9,
  kTargetAll = 10,
};
uint8_t calTargetFromName(const char* sensor);
const char* calTargetName(uint8_t target);
uint8_t calTargetOwner(uint8_t target);  // kNodeA, kNodeB o kNodeNone

// Textos de procedencia identicos a calibrationJson() v4.
const char* co2ModeName(const Co2Calibration& c);
const char* co2SourceName(const Co2Calibration& c);
const char* o2SourceName(const O2Calibration& c);

}  // namespace bioiot
