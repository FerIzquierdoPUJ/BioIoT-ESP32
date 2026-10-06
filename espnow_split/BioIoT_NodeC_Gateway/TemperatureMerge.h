#pragma once
// Temperatura del sistema a partir de los dos DS18B20 (portable, probado en PC):
//  * temperature   (nodo A, GPIO23, con su calibracion)
//  * temperature_b (nodo B en ESP32, GPIO18, lectura directa)
// Una lectura es utilizable si es actual (no vencida ni de un nodo sin enlace), su
// calidad es good/user_calibrated y el valor es finito. Con las dos => promedio;
// con una => esa; sin ninguna => sin valor (se publica el estado de A como antes).
#include <math.h>
#include <stdint.h>

#include "GatewayState.h"

namespace gw {

enum class TempSource : uint8_t { None = 0, NodeA = 1, NodeB = 2, Average = 3 };

inline const char* tempSourceName(TempSource s) {
  switch (s) {
    case TempSource::NodeA: return "node_a";
    case TempSource::NodeB: return "node_b";
    case TempSource::Average: return "average";
    default: return nullptr;
  }
}

struct TempCombined {
  TempSource source = TempSource::None;
  float value = NAN;
  bool haveA = false, haveB = false;
  float a = NAN, b = NAN;
};

inline bool tempUsable(const SensorSample& s, uint8_t state) {
  if (state != kSampleFresh || !s.present) return false;
  if (s.quality != bioiot::kQGood && s.quality != bioiot::kQUserCalibrated) return false;
  return s.valueCount > bioiot::val::kTempValue && isfinite(s.vals[bioiot::val::kTempValue]);
}

inline TempCombined combineTemperature(const SensorSample& a, uint8_t stateA, const SensorSample& b, uint8_t stateB) {
  TempCombined t;
  t.haveA = tempUsable(a, stateA);
  t.haveB = tempUsable(b, stateB);
  if (t.haveA) t.a = a.vals[bioiot::val::kTempValue];
  if (t.haveB) t.b = b.vals[bioiot::val::kTempValue];
  if (t.haveA && t.haveB) {
    t.source = TempSource::Average;
    t.value = (t.a + t.b) * 0.5f;
  } else if (t.haveA) {
    t.source = TempSource::NodeA;
    t.value = t.a;
  } else if (t.haveB) {
    t.source = TempSource::NodeB;
    t.value = t.b;
  }
  return t;
}

}  // namespace gw
