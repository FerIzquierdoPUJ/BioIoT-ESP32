#include "WarmupPolicy.h"

namespace nodeb {

void WarmupPolicy::begin(uint32_t warmupMs, ResetKind reset, bool rtcValid, uint32_t rtcPoweredMs, bool sharedSupply,
                         uint32_t nowMs) {
  warmupMs_ = warmupMs;
  bootMs_ = nowMs;
  credited_ = 0;
  done_ = false;
  powerOn_ = reset == ResetKind::PowerOn;
  assumed_ = false;
  const bool softLike = reset == ResetKind::SoftOrWatchdog || reset == ResetKind::External;
  if (softLike && rtcValid && sharedSupply) {
    credited_ = rtcPoweredMs;  // la alimentacion del sensor no se interrumpio
  } else if (!powerOn_) {
    // Reinicio sin evidencia fiable de continuidad: tiempo alimentado desconocido.
    assumed_ = true;
  }
  if (credited_ >= warmupMs_) done_ = true;
}

bool WarmupPolicy::warming(uint32_t nowMs) {
  if (!done_ && uint32_t(nowMs - bootMs_) >= warmupMs_ - credited_) done_ = true;
  return !done_;
}

uint32_t WarmupPolicy::remainingMs(uint32_t nowMs) {
  if (!warming(nowMs)) return 0;
  return warmupMs_ - credited_ - uint32_t(nowMs - bootMs_);
}

uint32_t WarmupPolicy::poweredLowerBoundMs(uint32_t nowMs) const {
  const uint64_t total = uint64_t(credited_) + uint32_t(nowMs - bootMs_);
  return total > 0xFFFFFFF0ULL ? 0xFFFFFFF0UL : uint32_t(total);
}

}  // namespace nodeb
