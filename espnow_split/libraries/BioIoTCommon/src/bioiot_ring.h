#pragma once
// Cola SPSC acotada sin bloqueos: el callback de radio (productor) solo copia la
// trama; loop/tarea (consumidor) valida HMAC y procesa. Si esta llena se descarta
// y se cuenta: nunca crece ni asigna memoria.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bioiot_protocol.h"

namespace bioiot {

struct RxFrame {
  uint8_t len = 0;
  int8_t rssi = 0;
  uint8_t mac[6] = {};
  uint32_t rxMs = 0;
  uint8_t data[kEspNowMaxFrame] = {};
};

template <size_t N>
class SpscFrameRing {
  static_assert(N >= 2 && (N & (N - 1)) == 0, "N potencia de 2");

 public:
  bool push(const uint8_t* data, size_t len, const uint8_t* mac, int8_t rssi, uint32_t nowMs) {
    if (len > kEspNowMaxFrame) { dropped_ = dropped_ + 1; return false; }
    const uint32_t head = __atomic_load_n(&head_, __ATOMIC_RELAXED);
    const uint32_t tail = __atomic_load_n(&tail_, __ATOMIC_ACQUIRE);
    if (head - tail >= N) { dropped_ = dropped_ + 1; return false; }
    RxFrame& f = slots_[head & (N - 1)];
    memcpy(f.data, data, len);
    f.len = uint8_t(len); f.rssi = rssi; f.rxMs = nowMs;
    if (mac) memcpy(f.mac, mac, 6); else memset(f.mac, 0, 6);
    __atomic_store_n(&head_, head + 1, __ATOMIC_RELEASE);
    return true;
  }
  bool pop(RxFrame& out) {
    const uint32_t tail = __atomic_load_n(&tail_, __ATOMIC_RELAXED);
    const uint32_t head = __atomic_load_n(&head_, __ATOMIC_ACQUIRE);
    if (tail == head) return false;
    out = slots_[tail & (N - 1)];
    __atomic_store_n(&tail_, tail + 1, __ATOMIC_RELEASE);
    return true;
  }
  size_t size() const {
    return __atomic_load_n(&head_, __ATOMIC_ACQUIRE) - __atomic_load_n(&tail_, __ATOMIC_ACQUIRE);
  }
  uint32_t dropped() const { return dropped_; }

 private:
  RxFrame slots_[N];
  uint32_t head_ = 0, tail_ = 0;
  volatile uint32_t dropped_ = 0;
};

}  // namespace bioiot
