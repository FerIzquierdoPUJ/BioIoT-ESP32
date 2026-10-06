#pragma once
// Comandos de calibracion del nodo B (O2 gas), portados de v4:
//  set/reset o2_gas_N (gain>0, offset finito), reset_all (correcciones del host a
//  gain=1/offset=0 conservando reference/commandUtc/commandCount), importacion y
//  validacion de la calibracion en aire (calibrate, reference_vol=20.9 exacto).
#include <stdint.h>

#include "bioiot_calibration.h"
#include "bioiot_messages.h"

namespace nodeb {

struct CalApplyResultB {
  uint8_t outcome = bioiot::kOutcomeRejected;
  uint8_t status = bioiot::kStRejectedInvalidParameters;
  uint8_t changedMask = 0;      // bits CalEntryB
  uint8_t resetFilters = 0;     // bits de sensores cuyo filtro debe vaciarse
  bool needsPersist = false;
  bool startAirCalibration = false;
  uint8_t airSensor = 0;
};

CalApplyResultB applyCalibrationB(const bioiot::NodeBCal& current, const bioiot::CalibrationCmd& cmd,
                                  bool storageWritable, bool warming, bool busy, bioiot::NodeBCal& candidate);
// Sin LittleFS montado nada puede guardarse: el rechazo por "ocupado/esquema futuro"
// se informa como rejected_storage_failed (codigo existente).
void reportStorageUnavailableB(CalApplyResultB& r, bool storageMounted);
void bumpRevisionsB(bioiot::NodeBCal& c, uint8_t changedMask);
// Registro tras una calibracion en aire con escritura I2C correcta.
void recordAirCalibration(bioiot::NodeBCal& c, uint8_t sensor, int64_t utcSeconds);
uint16_t sensorRevisionForTargetB(const bioiot::NodeBCal& c, uint8_t target);

}  // namespace nodeb
