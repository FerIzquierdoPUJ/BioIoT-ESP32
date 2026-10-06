#pragma once
// Salidas fisicas del gateway (ESP32, Arduino-ESP32 3.x LEDC). Solo se configuran
// como salida los actuadores con parametros electricos confirmados y fuera del
// modo de puesta en marcha; el resto permanece en alta impedancia.
#include "ActuatorCore.h"

class Esp32Outputs : public gw::OutputDriver {
 public:
  // Devuelve outputsReady (false si falla el PWM de un actuador confirmado).
  bool begin(bool commissioning, bool compressorConfirmed, bool ledConfirmed);
  void writeCompressor(bool on) override;
  void writeLed(uint8_t dutyR, uint8_t dutyG, uint8_t dutyB) override;
  // Apagado inmediato de lo que este configurado (reinicio solicitado).
  void forceOff();

 private:
  bool compressorEnabled_ = false, ledEnabled_ = false;
};
