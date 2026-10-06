#pragma once
// Planificador del bus I2C del nodo B. Cada vuelta de loop() hace como maximo una
// transaccion corta; las esperas (BH1750 180 ms, SEN0322 50/100 ms) son por estados.
// Selecciona el canal TCA antes de cada operacion y nunca habilita dos canales.
#include <Arduino.h>
#include <BH1750.h>

#include "BusB.h"
#include "DFRobot_OxygenSensor_B.h"
#include "O2Logic.h"
#include "WarmupPolicy.h"
#include "bioiot_calibration.h"
#include "node_b_config.h"

namespace nodeb {

struct LightResult {
  bool sampled = false, connected = false;
  float lux = NAN;
  float driverCode = NAN;  // -1/-2 de BH1750::readLightLevel() conservados como evidencia
  uint32_t sampledAtMs = 0;
};

struct I2CReadingDiag {
  bool observed = false, probeAttempted = false;
  TcaSelectResult selection;
  I2CProbeResult probe;
  const char* status = "not_sampled";
};

struct ScanSegment {
  uint8_t codes[127] = {};
  bool tested[127] = {};
  TcaSelectResult selection;
  bool completed = false, warmupSkipped = false;
};

enum DeviceIndex : uint8_t { kDevBh1 = 0, kDevBh2, kDevO2_1, kDevO2_2, kDevCount };

class SensorsB {
 public:
  void begin(I2CBusB* bus, WarmupPolicy* warmup, const bioiot::NodeBCal* cal);
  void loop(uint32_t nowMs);

  // Diagnostico: escaneo completo por etapas (una sonda por vuelta).
  bool startScan(bool recoverFirst);
  bool scanActive() const { return scanPhase_ != ScanIdle && scanPhase_ != ScanDone; }
  // true una sola vez cuando el escaneo termino (vuelve a reposo).
  bool takeScanResult();
  void requestFreshReads(bool light, bool o2);
  uint32_t lightCycles = 0, o2Cycles = 0;  // lecturas completas de ambos sensores
  // Calibracion en aire (asincrona).
  bool startAirCalibration(uint8_t sensor);
  bool airCalibrationPending() const { return airPending_; }
  bool airCalibrationFinished(bool& commOk, TcaSelectResult& disabled);
  void clearFilter(uint8_t sensor);

  LightResult light[2];
  O2Reading o2[2];
  O2Filter filter[2];
  I2CReadingDiag readingDiag[kDevCount];
  DiagnosticHistory deviceHistory[kDevCount];

  // Evidencia de escaneo
  ScanSegment mainScan, channelScans[8];
  TcaSelectResult isolation, cleanup;
  I2CProbeResult tcaProbe, altTcaProbe;
  RecoveryEvidence recovery;
  uint32_t scanStartedMs = 0, scanCompletedMs = 0;
  uint8_t expectedAddress(uint8_t channel) const;

 private:
  enum Task : uint8_t { TaskNone, TaskBh1, TaskBh2, TaskO2_1, TaskO2_2, TaskAirCal };
  enum ScanPhase : uint8_t { ScanIdle, ScanMain, ScanSelect, ScanChannel, ScanDone };
  void startTask(Task t, uint32_t nowMs);
  void stepLight(uint8_t i, uint32_t nowMs);
  void stepO2(uint8_t i, uint32_t nowMs);
  void stepAirCal(uint32_t nowMs);
  void stepScan(uint32_t nowMs);
  bool prepare(uint8_t dev, uint8_t channel, uint8_t address);
  bool probeReading(uint8_t dev, uint8_t address);
  void finish(uint8_t dev, bool ok, const char* status);

  I2CBusB* bus_ = nullptr;
  WarmupPolicy* warmup_ = nullptr;
  const bioiot::NodeBCal* cal_ = nullptr;
  BH1750 bh_;
  DFRobot_OxygenSensor o2Driver_[2];
  Task task_ = TaskNone;
  uint8_t step_ = 0;
  uint32_t waitUntil_ = 0;
  uint32_t lastLightMs_ = 0, lastO2Ms_ = 0, lastWarmProbeMs_ = 0;
  bool o2EverAfterWarmup_ = false, scanNeedsInit_ = false;
  bool lightQueued_[2] = {}, o2Queued_[2] = {};
  O2Transport transport_;
  // Calibracion en aire
  bool airPending_ = false, airDone_ = false, airCommOk_ = false;
  uint8_t airSensor_ = 0;
  TcaSelectResult airDisabled_;
  // Escaneo
  ScanPhase scanPhase_ = ScanIdle;
  uint8_t scanAddress_ = 1, scanChannel_ = 0;
  bool scanRecover_ = false;
};

}  // namespace nodeb
