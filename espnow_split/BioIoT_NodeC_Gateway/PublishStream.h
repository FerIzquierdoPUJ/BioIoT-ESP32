#pragma once
// Publicacion de JSON sin buffer del tamano del mensaje (portable, probado en PC):
//  * jsonVerdict(): la falta de heap al construir el documento es pasajera => se
//    conserva el elemento y se reintenta; solo un JSON que excede el maximo del
//    contrato se descarta.
//  * ChunkWriter: escritor para serializeJson() que agrupa la salida en trozos de
//    `cap` bytes y los entrega al destino (un registro TLS por trozo, no uno por
//    caracter). Con un trozo de 1 KB se publica un JSON de cualquier tamano permitido.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace gw {

enum class JsonVerdict : uint8_t { Publish = 0, RetryNoMemory = 1, DropTooLarge = 2 };

inline JsonVerdict jsonVerdict(bool overflowed, size_t len, size_t maxLen) {
  if (overflowed) return JsonVerdict::RetryNoMemory;
  if (len == 0 || len > maxLen) return JsonVerdict::DropTooLarge;
  return JsonVerdict::Publish;
}

inline const char* jsonVerdictName(JsonVerdict v) {
  switch (v) {
    case JsonVerdict::Publish: return "publish";
    case JsonVerdict::RetryNoMemory: return "retry_no_memory";
    case JsonVerdict::DropTooLarge: return "drop_too_large";
  }
  return "unknown";
}

class ChunkWriter {
 public:
  using Sink = bool (*)(void* ctx, const uint8_t* data, size_t len);
  ChunkWriter(uint8_t* buf, size_t cap, Sink sink, void* ctx) : buf_(buf), cap_(cap), sink_(sink), ctx_(ctx) {}

  size_t write(uint8_t c) { return write(&c, 1); }
  size_t write(const uint8_t* s, size_t n) {
    if (failed_ || !buf_ || !cap_) { failed_ = true; return 0; }
    size_t done = 0;
    while (done < n) {
      const size_t room = cap_ - used_;
      const size_t k = n - done < room ? n - done : room;
      memcpy(buf_ + used_, s + done, k);
      used_ += k;
      done += k;
      if (used_ == cap_ && !flush()) return 0;
    }
    total_ += n;
    return n;
  }
  // Entrega lo pendiente. false si el destino fallo (el mensaje queda incompleto).
  bool flush() {
    if (failed_) return false;
    if (!used_) return true;
    if (!sink_(ctx_, buf_, used_)) {
      failed_ = true;
      return false;
    }
    chunks_++;
    used_ = 0;
    return true;
  }
  bool failed() const { return failed_; }
  size_t total() const { return total_; }
  uint32_t chunks() const { return chunks_; }

 private:
  uint8_t* buf_;
  size_t cap_;
  Sink sink_;
  void* ctx_;
  size_t used_ = 0, total_ = 0;
  uint32_t chunks_ = 0;
  bool failed_ = false;
};

}  // namespace gw
