#include "SensorMathA.h"

#include <math.h>

#include "bioiot_types.h"
#include "node_a_config.h"

namespace nodea {

float calculatePh(const bioiot::NodeACal& cal, float voltage_V) {
  float ph = cal.ph_m * voltage_V + cal.ph_b;
  if (ph < 0.0f || ph > 14.0f) return NAN;
  return ph;
}

float calculateTurbidityNtu(const bioiot::NodeACal& cal, float voltage_V) {
  float ntu = cal.turb_m * voltage_V + cal.turb_b;
  if (ntu < 0.0f || ntu > 4000.0f) return NAN;
  return ntu;
}

float calculateDoSaturationPct(const bioiot::NodeACal& cal, float voltage_V) {
  float voltage_mV = voltage_V * 1000.0f;
  float pct = cal.do_m * voltage_mV;
  if (pct < 0.0f) pct = 0.0f;
  if (pct > 150.0f) pct = 150.0f;
  return pct;
}

float doSaturationConcentrationMgL(float tempC) {
  if (isnan(tempC)) return NAN;
  float t = tempC, t2 = t * t, t3 = t2 * t;
  return 14.652f - 0.41022f * t + 0.0079910f * t2 - 0.000077774f * t3;
}

float calculateDoMgL(const bioiot::NodeACal& cal, float voltage_V, float tempC) {
  float satPct = calculateDoSaturationPct(cal, voltage_V);
  if (isnan(satPct)) return NAN;
  float doSat = doSaturationConcentrationMgL(tempC);
  if (isnan(doSat)) return NAN;
  return (satPct / 100.0f) * doSat;
}

float calibrateTemperature(const bioiot::NodeACal& cal, float reading) { return cal.temp_m * reading + cal.temp_b; }

float calculateTdsPpm(float voltage_V) {
  return (133.42f * voltage_V * voltage_V * voltage_V - 255.86f * voltage_V * voltage_V + 857.39f * voltage_V) * 0.5f;
}

long arduinoMap(long x, long inMin, long inMax, long outMin, long outMax) {
  const long run = inMax - inMin;
  if (run == 0) return -1;
  const long rise = outMax - outMin;
  const long delta = x - inMin;
  return (delta * rise) / run + outMin;
}

uint8_t pulseToIntensity(unsigned long pulse) {
  if (pulse == 0 || pulse >= 30000UL) return 0;
  const float mapped = ((float)pulse - (float)TCS_BLACK_PULSE_US) * 255.0f /
                       ((float)TCS_WHITE_PULSE_US - (float)TCS_BLACK_PULSE_US);
  if (mapped <= 0.0f) return 0;
  if (mapped >= 255.0f) return 255;
  return (uint8_t)lroundf(mapped);
}

void rgbToHsl(uint8_t r, uint8_t g, uint8_t b, float& h, float& s, float& l) {
  float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
  float cMax = fmaxf(rf, fmaxf(gf, bf));
  float cMin = fminf(rf, fminf(gf, bf));
  float delta = cMax - cMin;
  l = (cMax + cMin) * 0.5f;
  if (delta < 1e-6f) { h = 0; s = 0; return; }
  s = (l < 0.5f) ? (delta / (cMax + cMin)) : (delta / (2.0f - cMax - cMin));
  if (cMax == rf) h = 60.0f * fmodf((gf - bf) / delta, 6.0f);
  else if (cMax == gf) h = 60.0f * (((bf - rf) / delta) + 2.0f);
  else h = 60.0f * (((rf - gf) / delta) + 4.0f);
  if (h < 0) h += 360.0f;
}

uint8_t analogQuality(bool connected, bool validValue) {
  if (!connected) return bioiot::kQDisconnected;
  return validValue ? bioiot::kQGood : bioiot::kQOutOfRange;
}

}  // namespace nodea
