#pragma once
// Utilidades portables comunes a los nodos sensores A y B: cache de resultados
// por commandId (idempotencia), cola acotada de comandos, ensamblador de lineas
// Serial y envio progresivo de diagnosticos fragmentados.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bioiot_fragment.h"
#include "bioiot_link.h"
#include "bioiot_messages.h"

namespace bioiot {

// Un comando repetido (mismo arranque del gateway y mismo cmdId) no se vuelve a
// ejecutar: se reenvia el resultado ya calculado.
class CommandCache {
 public:
  static constexpr uint8_t kSize = 8;
  const CommandResultMsg* find(uint32_t gatewayBoot, uint32_t cmdId) const {
    for (uint8_t i = 0; i < kSize; ++i)
      if (used_[i] && boot_[i] == gatewayBoot && results_[i].cmdId == cmdId) return &results_[i];
    return nullptr;
  }
  void store(uint32_t gatewayBoot, const CommandResultMsg& r) {
    used_[next_] = true;
    boot_[next_] = gatewayBoot;
    results_[next_] = r;
    next_ = uint8_t((next_ + 1) % kSize);
  }

 private:
  bool used_[kSize] = {};
  uint32_t boot_[kSize] = {};
  CommandResultMsg results_[kSize];
  uint8_t next_ = 0;
};

struct PendingCommand {
  MsgType type = MsgType::Command;
  uint8_t len = 0;
  uint8_t payload[kMaxPayload] = {};
  uint32_t gatewayBoot = 0;
};

template <uint8_t N>
class CommandQueue {
 public:
  bool push(MsgType type, const uint8_t* p, size_t len, uint32_t gatewayBoot) {
    if (count_ >= N || len > kMaxPayload) return false;
    PendingCommand& c = items_[(head_ + count_) % N];
    c.type = type; c.len = uint8_t(len); c.gatewayBoot = gatewayBoot;
    memcpy(c.payload, p, len);
    count_++;
    return true;
  }
  bool pop(PendingCommand& out) {
    if (!count_) return false;
    out = items_[head_];
    head_ = uint8_t((head_ + 1) % N);
    count_--;
    return true;
  }
  uint8_t size() const { return count_; }

 private:
  PendingCommand items_[N];
  uint8_t head_ = 0, count_ = 0;
};

// Lineas terminadas en '\n'; descarta las demasiado largas sin desbordar.
template <size_t N>
class LineAssembler {
 public:
  // Devuelve true cuando hay una linea completa en line().
  bool feed(char c) {
    if (c == '\r') return false;
    if (c == '\n') {
      const bool ready = !overflow_ && len_ > 0;
      buf_[len_] = 0;
      overflowed_ = overflow_;
      len_ = 0;
      overflow_ = false;
      return ready;
    }
    if (len_ < N - 1) buf_[len_++] = c;
    else overflow_ = true;
    return false;
  }
  const char* line() const { return buf_; }
  bool lastOverflowed() const { return overflowed_; }

 private:
  char buf_[N];
  size_t len_ = 0;
  bool overflow_ = false, overflowed_ = false;
};

// Encola fragmentos de diagnostico de forma progresiva (sin llenar la cola).
class DiagSender {
 public:
  bool start(uint32_t requestId, const uint8_t* data, size_t len) { return frag_.start(requestId, data, len); }
  bool active() const { return frag_.hasNext(); }
  // Llamar en cada loop.
  void pump(Endpoint& ep, uint8_t dst, uint32_t nowMs, uint8_t maxQueued = 4) {
    while (frag_.hasNext() && ep.queueDepthFor(dst) < maxQueued) {
      uint8_t buf[kMaxPayload];
      const size_t n = frag_.next(buf, sizeof(buf));
      if (!n) { frag_.reset(); return; }
      SendOptions o;
      o.cookie = 0xD1A00000u | frag_.sent();
      o.ttlMs = 60000;
      o.maxAttempts = 5;
      if (!ep.sendReliable(dst, MsgType::DiagResult, buf, n, o, nowMs)) { frag_.reset(); return; }
    }
  }
  void reset() { frag_.reset(); }

 private:
  Fragmenter frag_;
};

}  // namespace bioiot
