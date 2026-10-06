#pragma once
// Bus I2C del nodo B (ESP8266) con PCA9548A/TCA9548A. Conserva los codigos
// exactos de Wire.endTransmission() (en ESP8266 3.1.2: 0, 2, 3, 4) y la evidencia
// de diagnostico de v4. Un solo propietario del bus: el planificador de SensorsB.
#include <Arduino.h>
#include <Wire.h>

#include "node_b_config.h"

namespace nodeb {

struct I2CProbeResult {
  uint8_t address = 0;
  bool detected = false;
  uint8_t errorCode = 0;
};
struct TcaSelectResult {
  bool attempted = false, selected = false;
  uint8_t channel = 0, errorCode = 0;
};
struct DiagnosticHistory {
  uint32_t success = 0, failure = 0, consecutiveFailures = 0;
  uint32_t lastSuccessMs = 0, lastFailureMs = 0;
  void record(bool ok, uint32_t nowMs) {
    if (ok) { success++; consecutiveFailures = 0; lastSuccessMs = nowMs; }
    else { failure++; consecutiveFailures++; lastFailureMs = nowMs; }
  }
};
struct RecoveryEvidence {
  bool performed = false;
  uint8_t statusBefore = 0, statusAfter = 0;  // twi_status(): 0 ok, 1/2 SCL bajo, 3/4 SDA bajo
  uint8_t pulses = 0;
  bool sdaReleased = false;
  I2CProbeResult tcaBefore, tcaAfter;
};

const char* i2cErrorName(uint8_t code);
const char* twiStatusName(uint8_t status);

class I2CBusB {
 public:
  void begin();
  I2CProbeResult probe(uint8_t address);
  TcaSelectResult select(uint8_t channel);
  TcaSelectResult disableAll();
  uint8_t lineStatus();
  // Recuperacion acotada (~100 us): 9 pulsos SCL si SDA esta retenida, STOP y
  // reinicio de Wire. No acciona RESET fisico; no confirma reparacion.
  RecoveryEvidence recover();

  I2CProbeResult lastTcaProbe;
  bool tcaObserved = false;
  DiagnosticHistory tcaProbeHistory, tcaSelectHistory, tcaDisableHistory;
};

}  // namespace nodeb
