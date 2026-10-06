#include "bioiot_store.h"

#include <string.h>

#include "bioiot_codec.h"
#include "bioiot_crypto.h"

namespace bioiot {

const char* storeStateName(StoreState s) {
  switch (s) {
    case StoreState::Empty: return "empty_defaults";
    case StoreState::Ok: return "ok";
    case StoreState::Corrupt: return "corrupt_defaults";
    case StoreState::FutureFormat: return "future_format_writes_blocked";
  }
  return "unknown";
}

size_t RecordStore::encode(uint16_t format, uint32_t generation, const uint8_t* payload, size_t len,
                           uint8_t* out, size_t cap) {
  if (len > kStoreMaxPayload) return 0;
  Writer w(out, cap);
  w.u32(kStoreMagic); w.u16(format); w.u16(uint16_t(len)); w.u32(generation);
  w.bytes(payload, len);
  if (!w.ok()) return 0;
  const uint32_t crc = crc32(out, w.size());
  w.u32(crc);
  return w.ok() ? w.size() : 0;
}

bool RecordStore::parse(const uint8_t* buf, size_t len, uint16_t& format, uint32_t& generation,
                        const uint8_t*& payload, size_t& payloadLen) {
  if (len < kStoreOverhead) return false;
  Reader r(buf, len);
  if (r.u32() != kStoreMagic) return false;
  format = r.u16();
  const uint16_t plen = r.u16();
  generation = r.u32();
  if (plen > kStoreMaxPayload || size_t(plen) + kStoreOverhead != len) return false;
  const uint32_t stored = getU32(buf + len - 4);
  if (crc32(buf, len - 4) != stored) return false;
  payload = buf + kStoreHeaderSize;
  payloadLen = plen;
  return true;
}

size_t RecordStore::load(uint8_t* payload, size_t cap) {
  uint8_t buf[2][kStoreMaxPayload + kStoreOverhead];
  bool valid[2] = {false, false};
  bool anyData = false, future = false;
  uint16_t fmt[2] = {0, 0};
  uint32_t gen[2] = {0, 0};
  const uint8_t* p[2] = {nullptr, nullptr};
  size_t plen[2] = {0, 0};
  for (uint8_t s = 0; s < 2; ++s) {
    const size_t n = backend_.readSlot(s, buf[s], sizeof(buf[s]));
    if (!n) continue;
    anyData = true;
    if (!parse(buf[s], n, fmt[s], gen[s], p[s], plen[s])) continue;
    if (fmt[s] > format_) { future = true; continue; }
    if (fmt[s] != format_) continue;  // formatos anteriores: aun no existen en v1
    valid[s] = true;
  }
  haveValid_ = false;
  if (future) {
    // Nunca sobrescribir datos de un firmware mas nuevo.
    state_ = StoreState::FutureFormat;
    return 0;
  }
  int best = -1;
  for (uint8_t s = 0; s < 2; ++s) {
    if (!valid[s]) continue;
    if (best < 0 || int32_t(gen[s] - gen[best]) > 0) best = s;
  }
  if (best < 0) {
    state_ = anyData ? StoreState::Corrupt : StoreState::Empty;
    return 0;
  }
  if (plen[best] > cap) {
    state_ = StoreState::Corrupt;
    return 0;
  }
  memcpy(payload, p[best], plen[best]);
  generation_ = gen[best];
  activeSlot_ = uint8_t(best);
  storedFormat_ = fmt[best];
  haveValid_ = true;
  state_ = StoreState::Ok;
  return plen[best];
}

bool RecordStore::save(const uint8_t* payload, size_t len) {
  lastWriteOk_ = false;
  if (state_ == StoreState::FutureFormat) return false;
  uint8_t buf[kStoreMaxPayload + kStoreOverhead];
  const uint32_t gen = haveValid_ ? generation_ + 1 : 1;
  const size_t n = encode(format_, gen, payload, len, buf, sizeof(buf));
  if (!n) return false;
  // Nunca sobre la ranura del registro valido vigente (inicialmente activeSlot_=1 -> ranura 0).
  const uint8_t target = uint8_t(1 - activeSlot_);
  if (!backend_.writeSlot(target, buf, n)) return false;
  uint8_t verify[kStoreMaxPayload + kStoreOverhead];
  const size_t m = backend_.readSlot(target, verify, sizeof(verify));
  if (m != n || memcmp(verify, buf, n) != 0) {
    // Lo escrito no es verificable: invalidar la ranura para que un registro que
    // se reporto como NO guardado nunca gane al reiniciar.
    const uint8_t tombstone = 0;
    if (!backend_.writeSlot(target, &tombstone, 1)) lastWriteUncertain_ = true;
    return false;
  }
  generation_ = gen;
  activeSlot_ = target;
  haveValid_ = true;
  storedFormat_ = format_;
  state_ = StoreState::Ok;
  lastWriteOk_ = true;
  return true;
}

}  // namespace bioiot
