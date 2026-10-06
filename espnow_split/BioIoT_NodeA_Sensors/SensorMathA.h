#pragma once
// Conversiones del nodo A: ecuaciones analogicas v4 y curva provisional TCS3200
// compartida con el integrado (referencias en node_a_config.h). Portable para PC.
#include <stdint.h>

#include "bioiot_calibration.h"

namespace nodea {

constexpr int kAnalogSamples = 16;            // v4: N=16, descarte inicial, 3 ms
constexpr uint32_t kMuxSettleUs = 3250;       // delayMicroseconds(250) + delay(3)
constexpr uint32_t kAnalogSampleGapUs = 3000; // delay(3) entre muestras
constexpr unsigned long kTcsPulseTimeoutUs = 30000;
constexpr uint32_t kTcsStepGapMs = 10;
constexpr int kAnalogSimilarSpread = 8;

inline float adcToVoltage(float raw) { return raw * (3.3f / 4095.0f); }
inline bool analogConnected(int raw) { return !(raw <= 5 || raw >= 4090); }
inline bool ds18Connected(float tempC) { return !(tempC == -127.0f || tempC < -55.0f || tempC > 125.0f); }

float calculatePh(const bioiot::NodeACal& cal, float voltage_V);
float calculateTurbidityNtu(const bioiot::NodeACal& cal, float voltage_V);
float calculateDoSaturationPct(const bioiot::NodeACal& cal, float voltage_V);
float doSaturationConcentrationMgL(float tempC);
float calculateDoMgL(const bioiot::NodeACal& cal, float voltage_V, float tempC);
float calibrateTemperature(const bioiot::NodeACal& cal, float reading);
float calculateTdsPpm(float voltage_V);

// map() de Arduino-ESP32 reproducido (division entera truncada).
long arduinoMap(long x, long inMin, long inMax, long outMin, long outMax);
uint8_t pulseToIntensity(unsigned long pulse);
void rgbToHsl(uint8_t r, uint8_t g, uint8_t b, float& h, float& s, float& l);

// sensorQuality() de v4 sin warm-up: disconnected / good / out_of_range.
uint8_t analogQuality(bool connected, bool validValue);

}  // namespace nodea
