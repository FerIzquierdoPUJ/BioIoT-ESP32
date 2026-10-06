#pragma once
// Adquisicion por estados del nodo A: nunca espera mas de un pulseIn (<=30 ms)
// o un analogRead por vuelta de loop(), para atender ESP-NOW y watchdog.
#include <Arduino.h>
#include <DallasTemperature.h>
#include <OneWire.h>

#include "node_a_config.h"

namespace nodea {

struct AnalogResult {
  bool sampled = false;
  int raw = 0;             // promedio entero (escala de calibracion v4)
  float voltage = 0;
  bool connected = false;
  int rawMin = 0, rawMax = 0;
  float rawAvg = 0, voltageAvg = 0;
  uint32_t sampledAtMs = 0;
};

// Orden fijo de sensores analogicos (mismo orden que analogSensorNames v4).
enum AnalogIndex : uint8_t { kAPh = 0, kACo2_1, kACo2_2, kATurb, kADo, kATds, kAnalogChannels };
extern const uint8_t kAnalogMuxChannel[kAnalogChannels];
extern const char* const kAnalogSensorNames[kAnalogChannels];

class AnalogSampler {
 public:
  void begin();
  void loop(uint32_t nowMs);
  void requestSweep() { sweepRequested_ = true; }
  bool busy() const { return state_ != Idle; }
  uint32_t sweeps() const { return sweeps_; }
  const AnalogResult& result(uint8_t i) const { return results_[i]; }
  // updateAnalogDiagnostics() de v4
  bool batchValid = false, allNearZero = false, tooSimilar = false;
  float minAverage = 0, maxAverage = 0;
  uint32_t allZeroCount = 0, lastSweepMs = 0;

 private:
  enum State : uint8_t { Idle, Settling, Sampling };
  void select(uint8_t channel);
  void finishChannel(uint32_t nowMs);
  void updateDiagnostics();
  State state_ = Idle;
  bool sweepRequested_ = true;
  uint8_t index_ = 0;
  int count_ = 0;
  long acc_ = 0;
  int rawMin_ = 4095, rawMax_ = 0;
  uint32_t stepUs_ = 0, sweeps_ = 0;
  AnalogResult results_[kAnalogChannels];
};

struct ColorResult {
  bool sampled = false;
  unsigned long rPulse = 0, gPulse = 0, bPulse = 0;
  uint8_t r = 0, g = 0, b = 0;
  float h = 0, s = 0, l = 0;
  bool connected = false;
  uint32_t sampledAtMs = 0;
};

class ColorSampler {
 public:
  void begin();
  void loop(uint32_t nowMs);
  void requestRead() { requested_ = true; }
  bool busy() const { return step_ != 0; }
  const ColorResult& result(uint8_t i) const { return results_[i]; }
  uint32_t reads() const { return reads_; }

 private:
  uint8_t sensor_ = 0, step_ = 0;  // step 0 = inactivo; 1..3 = R,G,B
  bool requested_ = true;
  bool filterSelected_ = false;
  uint32_t nextMs_ = 0, lastCycleMs_ = 0, reads_ = 0;
  ColorResult working_;
  ColorResult results_[2];
};

struct TempResult {
  bool sampled = false;
  float raw = NAN;
  bool connected = false;
  uint8_t deviceCount = 0;
  uint32_t sampledAtMs = 0;
};

class TempSampler {
 public:
  TempSampler() : oneWire_(ONE_WIRE_BUS), ds18_(&oneWire_) {}
  void begin();
  void loop(uint32_t nowMs);
  void requestRead() { requested_ = true; }
  bool busy() const { return converting_; }
  const TempResult& result() const { return result_; }

 private:
  OneWire oneWire_;
  DallasTemperature ds18_;
  bool converting_ = false, requested_ = true;
  uint32_t startedMs_ = 0, lastMs_ = 0, waitMs_ = 750;
  TempResult result_;
};

}  // namespace nodea
