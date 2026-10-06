#include "O2Logic.h"

#include <math.h>

namespace nodeb {

float evaluateO2(const O2Transport& t, const O2Calibration& cal, O2Filter& filter, O2Reading& r, uint32_t nowMs) {
  r = O2Reading();
  r.observed = true;
  r.sampledAt = nowMs;
  r.warming = t.warming;
  r.quality = "communication_fault";
  if (!t.selected) { r.wireError = t.selectError; filter.clear(); return NAN; }
  if (!t.probeOk) { r.wireError = t.probeError; filter.clear(); return NAN; }
  r.i2cDetected = true;
  if (t.warming) {
    // Transporte comprobable; la concentracion no se adquiere deliberadamente.
    r.connected = true;
    r.quality = "warming_up";
    filter.clear();
    return NAN;
  }
  r.wireError = t.wireError;
  r.readCount = t.readCount;
  if (!t.beginOk) { filter.clear(); return NAN; }
  r.raw = t.raw;
  r.connected = t.commOk;
  if (!r.connected) { r.raw = NAN; filter.clear(); return NAN; }
  const float corrected = r.raw * cal.gain + cal.offset;
  if (!isfinite(r.raw) || r.raw < 0 || r.raw > 30 || !isfinite(corrected) || corrected < 0 || corrected > 30) {
    r.quality = "out_of_range";
    filter.clear();
    return NAN;
  }
  r.filtered = filter.add(r.raw);
  r.value = r.filtered * cal.gain + cal.offset;
  r.valid = true;
  r.stabilizing = filter.count < O2_FILTER_SAMPLES;
  r.quality = corrected > 25 || r.value > 25 ? "above_nominal_range" : r.stabilizing ? "stabilizing" : "good";
  return r.value;
}

}  // namespace nodeb
