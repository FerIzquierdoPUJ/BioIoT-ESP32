#pragma once
#include <math.h>
#include <stdint.h>

// CALIBRATION PROVENANCE
// pH: experimental project calibration, V (NOT mV):
// pH=-6.112458587*V+15.012668489, R2=0.999958607, buffers 4.01/7.01/10.01.
// DO: experimental project calibration, saturation%=0.133519121*V_mV.
// 0mV=0%; 748.9564mV=100%; existing temperature compensation retained.
// CO2: DFRobot SEN0159 / MG-811 vendor reference, provisional, individual
// calibration required. https://wiki.dfrobot.com/sen0159/docs/20109
// O2: SEN0322 digital I2C %Vol, 0..25 nominal, 20.9 air calibration;
// no host voltage curve. https://wiki.dfrobot.com/sen0322/docs/19924
constexpr uint32_t CALIBRATION_SCHEMA_VERSION = 2;
constexpr float SEN0159_DC_GAIN = 8.5f;
constexpr float SEN0159_VENDOR_ZERO_POINT_V = 0.220f;
constexpr float SEN0159_VENDOR_REACTION_V = 0.030f;
constexpr float CO2_DIVIDER_R_TOP = 12000.0f;
constexpr float CO2_DIVIDER_R_BOTTOM = 22000.0f;
constexpr float CO2_DIVIDER_RATIO = CO2_DIVIDER_R_BOTTOM / (CO2_DIVIDER_R_TOP + CO2_DIVIDER_R_BOTTOM);
constexpr float CO2_LOG_REFERENCE = 2.602f;
constexpr float CO2_REFERENCE_MAX_PPM = 10000.0f;
// Default equivalence ONLY for traceability: ppm ~=331639.82*exp(-5.55411435*Vadc).
// Calculation below uses divider/gain/individual zero/reaction instead.
#ifndef O2_FILTER_SAMPLES
#define O2_FILTER_SAMPLES 10
#endif
static_assert(O2_FILTER_SAMPLES > 0 && O2_FILTER_SAMPLES <= 100, "O2 filter size 1..100");

struct Co2Calibration {
  float zero_point_v = SEN0159_VENDOR_ZERO_POINT_V;
  float reaction_voltage_v = SEN0159_VENDOR_REACTION_V;
  float a = 0, b = 0;
  uint8_t mode = 0; // 0 sen0159_vendor; 1 legacy_exponential
  uint8_t user = 0, enabled = 1, provisional = 1;
};
static_assert(sizeof(Co2Calibration) == 20, "NVS CO2 schema 2 blob layout");

struct O2Filter {
  float samples[O2_FILTER_SAMPLES] = {};
  uint8_t count = 0, next = 0;
  void clear() { count = 0; next = 0; }
  float add(float value) {
    samples[next] = value;
    next = (next + 1) % O2_FILTER_SAMPLES;
    if (count < O2_FILTER_SAMPLES) ++count;
    float sum = 0;
    for (uint8_t i = 0; i < count; ++i) sum += samples[i];
    return sum / count;
  }
};
struct O2Calibration {
  float gain = 1, offset = 0, reference = 0;
  int64_t commandUtc = 0;
  uint32_t commandCount = 0;
};
struct O2Reading {
  bool observed = false, i2cDetected = false, connected = false;
  bool warming = false, valid = false, stabilizing = false;
  float raw = NAN, filtered = NAN, value = NAN;
  uint32_t sampledAt = 0;
  uint8_t wireError = 0, readCount = 0;
  const char* quality = "not_sampled";
};
struct Co2Reading {
  float value = NAN, moduleVoltage = NAN, sensorVoltage = NAN;
  bool below = false, valid = false;
  const char* quality = "uncalibrated";
};

inline bool isOldPhDefault(float m, float b) {
  return isfinite(m) && isfinite(b) && fabsf(m + 6.1125f) <= 0.000002f &&
      fabsf(b - 15.013f) <= 0.000002f;
}
inline bool isOldDoDefault(float m) {
  return isfinite(m) && fabsf(m - 0.1335f) <= 0.0000001f;
}
inline bool validCo2Calibration(const Co2Calibration &c) {
  return c.mode <= 1 && c.enabled <= 1 && c.user <= 1 && c.provisional <= 1 &&
      isfinite(c.zero_point_v) && c.zero_point_v > 0 && c.zero_point_v <= 1 &&
      isfinite(c.reaction_voltage_v) && c.reaction_voltage_v > 0 && c.reaction_voltage_v <= 1 &&
      isfinite(c.a) && isfinite(c.b) && (c.mode == 0 || c.a > 0);
}
inline Co2Reading evaluateCo2(float adcVoltage, bool connected, const Co2Calibration &c) {
  Co2Reading r;
  r.moduleVoltage = adcVoltage / CO2_DIVIDER_RATIO;
  r.sensorVoltage = r.moduleVoltage / SEN0159_DC_GAIN;
  if (!connected) { r.quality = "disconnected"; return r; }
  if (!c.enabled) return r;
  if (!validCo2Calibration(c) || !isfinite(adcVoltage) || adcVoltage < 0) {
    r.quality = "out_of_range"; return r;
  }
  if (c.mode == 1) {
    // Legacy coefficients are defined on Vadc: do not retroactively apply divider.
    r.value = c.a * expf(c.b * adcVoltage);
  } else {
    if (r.sensorVoltage >= c.zero_point_v) {
      r.below = true; r.quality = "below_reference_range"; return r;
    }
    const float slope = c.reaction_voltage_v / (CO2_LOG_REFERENCE - 3.0f);
    r.value = powf(10.0f, (r.sensorVoltage - c.zero_point_v) / slope + CO2_LOG_REFERENCE);
  }
  if (!isfinite(r.value) || r.value < 0 || (c.mode == 0 && r.value > CO2_REFERENCE_MAX_PPM)) {
    r.value = NAN; r.quality = "out_of_range"; return r;
  }
  r.valid = true;
  r.quality = c.user ? "user_calibrated" : "vendor_reference";
  return r;
}

