#include "TempB.h"

namespace nodeb {

void TempSamplerB::begin() {
  ds18_.begin();
  ds18_.setWaitForConversion(false);  // conversion asincrona: sin esperas de 750 ms
  result_.deviceCount = ds18_.getDeviceCount();
  waitMs_ = ds18_.millisToWaitForConversion(ds18_.getResolution());
  if (waitMs_ < 94 || waitMs_ > 750) waitMs_ = 750;
}

void TempSamplerB::loop(uint32_t nowMs) {
  if (!converting_) {
    if (!requested_ && nowMs - lastMs_ < NODE_B_TEMP_INTERVAL_MS) return;
    requested_ = false;
    lastMs_ = nowMs;
    ds18_.requestTemperatures();
    startedMs_ = nowMs;
    converting_ = true;
    return;
  }
  if (nowMs - startedMs_ < waitMs_) return;
  converting_ = false;
  const float tempC = ds18_.getTempCByIndex(0);
  result_.sampled = true;
  result_.raw = tempC;
  result_.connected = ds18Connected(tempC);
  result_.deviceCount = ds18_.getDeviceCount();
  result_.sampledAtMs = nowMs;
}

}  // namespace nodeb
