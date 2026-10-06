#pragma once
// Referencia UTC propagada por el gateway. Un nodo nunca inventa UTC: sin
// TIME_SYNC valido reciente, utcValid() es falso. Las edades de muestras se
// calculan siempre dentro del reloj de un mismo equipo (nunca se mezclan
// millis() de placas distintas).
#include <stdint.h>

#include "bioiot_messages.h"

namespace bioiot {

class UtcReference {
 public:
  static constexpr uint32_t kMaxValidityMs = 24UL * 3600UL * 1000UL;  // deriva de cristal acotada

  void apply(const TimeSyncMsg& m, uint32_t nowMs, uint32_t gatewayBoot) {
    if (!m.utcValid) return;  // conserva la referencia anterior si aun es valida
    valid_ = true;
    utcMsAtSync_ = m.utcMs;
    localMsAtSync_ = nowMs;
    gatewayBoot_ = gatewayBoot;
  }
  void invalidate() { valid_ = false; }
  bool utcValid(uint32_t nowMs) const { return valid_ && nowMs - localMsAtSync_ < kMaxValidityMs; }
  int64_t utcMs(uint32_t nowMs) const {
    return utcValid(nowMs) ? utcMsAtSync_ + int64_t(uint32_t(nowMs - localMsAtSync_)) : 0;
  }
  int64_t utcSeconds(uint32_t nowMs) const { return utcValid(nowMs) ? utcMs(nowMs) / 1000 : 0; }
  uint32_t syncAgeMs(uint32_t nowMs) const { return valid_ ? nowMs - localMsAtSync_ : 0xFFFFFFFFu; }
  uint32_t gatewayBoot() const { return gatewayBoot_; }

 private:
  bool valid_ = false;
  int64_t utcMsAtSync_ = 0;
  uint32_t localMsAtSync_ = 0;
  uint32_t gatewayBoot_ = 0;
};

}  // namespace bioiot
