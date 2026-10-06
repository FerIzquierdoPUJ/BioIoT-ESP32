#pragma once
// Serializacion explicita little-endian. Nunca se copian structs C++ a la radio:
// cada campo se escribe byte a byte, independiente de padding y endianness.
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <limits>

namespace bioiot {

static_assert(sizeof(float) == 4, "float de 32 bits requerido");
static_assert(std::numeric_limits<float>::is_iec559, "float IEEE-754 requerido");

class Writer {
 public:
  Writer(uint8_t* buf, size_t cap) : buf_(buf), cap_(cap) {}
  bool u8(uint8_t v) {
    if (!room(1)) return false;
    buf_[len_++] = v;
    return true;
  }
  bool i8(int8_t v) { return u8(static_cast<uint8_t>(v)); }
  bool u16(uint16_t v) {
    if (!room(2)) return false;
    buf_[len_++] = uint8_t(v);
    buf_[len_++] = uint8_t(v >> 8);
    return true;
  }
  bool u32(uint32_t v) {
    if (!room(4)) return false;
    for (int i = 0; i < 4; ++i) buf_[len_++] = uint8_t(v >> (8 * i));
    return true;
  }
  bool i64(int64_t v) {
    const uint64_t u = static_cast<uint64_t>(v);
    if (!room(8)) return false;
    for (int i = 0; i < 8; ++i) buf_[len_++] = uint8_t(u >> (8 * i));
    return true;
  }
  bool f32(float v) {
    uint32_t u;
    memcpy(&u, &v, sizeof(u));
    return u32(u);
  }
  bool bytes(const uint8_t* p, size_t n) {
    if (!room(n)) return false;
    if (n) memcpy(buf_ + len_, p, n);
    len_ += n;
    return true;
  }
  size_t size() const { return len_; }
  bool ok() const { return ok_; }

 private:
  bool room(size_t n) {
    if (!ok_ || len_ + n > cap_) {
      ok_ = false;
      return false;
    }
    return true;
  }
  uint8_t* buf_;
  size_t cap_;
  size_t len_ = 0;
  bool ok_ = true;
};

class Reader {
 public:
  Reader(const uint8_t* buf, size_t len) : buf_(buf), len_(len) {}
  uint8_t u8() {
    if (!need(1)) return 0;
    return buf_[pos_++];
  }
  int8_t i8() { return static_cast<int8_t>(u8()); }
  uint16_t u16() {
    if (!need(2)) return 0;
    const uint16_t v = uint16_t(buf_[pos_]) | uint16_t(uint16_t(buf_[pos_ + 1]) << 8);
    pos_ += 2;
    return v;
  }
  uint32_t u32() {
    if (!need(4)) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= uint32_t(buf_[pos_ + i]) << (8 * i);
    pos_ += 4;
    return v;
  }
  int64_t i64() {
    if (!need(8)) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= uint64_t(buf_[pos_ + i]) << (8 * i);
    pos_ += 8;
    return static_cast<int64_t>(v);
  }
  float f32() {
    const uint32_t u = u32();
    float v;
    memcpy(&v, &u, sizeof(v));
    return v;
  }
  bool bytes(uint8_t* out, size_t n) {
    if (!need(n)) return false;
    if (n) memcpy(out, buf_ + pos_, n);
    pos_ += n;
    return true;
  }
  size_t remaining() const { return ok_ ? len_ - pos_ : 0; }
  size_t position() const { return pos_; }
  bool ok() const { return ok_; }
  // Exige consumo exacto: bytes sobrantes indican version/formato incompatible.
  bool done() const { return ok_ && pos_ == len_; }

 private:
  bool need(size_t n) {
    if (!ok_ || pos_ + n > len_) {
      ok_ = false;
      return false;
    }
    return true;
  }
  const uint8_t* buf_;
  size_t len_;
  size_t pos_ = 0;
  bool ok_ = true;
};

inline void putU32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}
inline uint32_t getU32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

}  // namespace bioiot
