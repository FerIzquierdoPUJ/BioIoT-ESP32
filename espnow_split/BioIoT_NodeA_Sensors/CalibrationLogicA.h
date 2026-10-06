#pragma once
// Aplicacion de comandos de calibracion del nodo A sobre una COPIA candidata.
// Portado de handleCalibrationCommand()/setCo2Profile() de v4. El llamador
// persiste la candidata y solo si se guarda correctamente la activa: nunca se
// reporta una calibracion aplicada antes de guardarla.
#include <stdint.h>

#include "bioiot_calibration.h"
#include "bioiot_messages.h"

namespace nodea {

struct CalApplyResult {
  uint8_t outcome = bioiot::kOutcomeRejected;
  uint8_t status = bioiot::kStRejectedInvalidParameters;
  uint8_t changedMask = 0;  // bits CalEntryA modificados
  bool needsPersist = false;
};

void loadDefaultsA(bioiot::NodeACal& c);  // loadCalibrationDefaults() de v4 (solo parte A)
CalApplyResult applyCalibrationA(const bioiot::NodeACal& current, const bioiot::CalibrationCmd& cmd,
                                 bool storageWritable, bioiot::NodeACal& candidate);
// Revision por sensor y del nodo para las entradas cambiadas.
void bumpRevisionsA(bioiot::NodeACal& c, uint8_t changedMask);
uint16_t sensorRevisionForTarget(const bioiot::NodeACal& c, uint8_t target);

}  // namespace nodea
