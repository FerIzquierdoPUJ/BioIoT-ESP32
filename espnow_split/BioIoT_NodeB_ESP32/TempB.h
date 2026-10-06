#pragma once
// DS18B20 del nodo B (ESP32) en ONE_WIRE_BUS (GPIO18). Misma lectura asincrona que el
// nodo A: requestTemperatures() sin esperar y lectura tras el tiempo de conversion,
// asi loop() nunca se bloquea 750 ms. Sin calibracion propia: value = lectura del sensor.
#include <Arduino.h>
#include <DallasTemperature.h>
#include <OneWire.h>

#include "node_b_config.h"

namespace nodeb {

// -127 = DEVICE_DISCONNECTED_C; fuera de -55..125 C no es una lectura del DS18B20.
inline bool ds18Connected(float tempC) { return !(tempC == -127.0f || tempC < -55.0f || tempC > 125.0f); }

struct TempResultB {
  bool sampled = false;
  float raw = NAN;
  bool connected = false;
  uint8_t deviceCount = 0;
  uint32_t sampledAtMs = 0;
};

class TempSamplerB {
 public:
  TempSamplerB() : oneWire_(ONE_WIRE_BUS), ds18_(&oneWire_) {}
  void begin();
  void loop(uint32_t nowMs);
  void requestRead() { requested_ = true; }
  bool busy() const { return converting_; }
  const TempResultB& result() const { return result_; }

 private:
  OneWire oneWire_;
  DallasTemperature ds18_;
  bool converting_ = false, requested_ = true;
  uint32_t startedMs_ = 0, lastMs_ = 0, waitMs_ = 750;
  TempResultB result_;
};

}  // namespace nodeb
