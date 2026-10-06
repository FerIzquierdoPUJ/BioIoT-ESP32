#pragma once
// SHA-256, HMAC-SHA256 y CRC32 portables (sin mbedTLS/BearSSL) para que ESP32,
// ESP8266 y las pruebas en PC calculen exactamente los mismos bytes.
#include <stddef.h>
#include <stdint.h>

namespace bioiot {

class Sha256 {
 public:
  Sha256() { reset(); }
  void reset();
  void update(const uint8_t* data, size_t len);
  void finish(uint8_t out[32]);

 private:
  void block(const uint8_t* p);
  uint32_t h_[8];
  uint8_t buf_[64];
  uint64_t total_ = 0;
  size_t used_ = 0;
};

void hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t len, uint8_t out[32]);
// Comparacion en tiempo constante.
bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t n);
uint32_t crc32(const uint8_t* data, size_t len, uint32_t seed = 0);

}  // namespace bioiot
