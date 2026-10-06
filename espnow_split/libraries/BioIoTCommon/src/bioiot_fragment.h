#pragma once
// Fragmentacion solo para DIAGNOSTIC_RESULT: limite 4096 bytes, 20 fragmentos,
// un ensamblado por nodo, bitmap anti-duplicado y plazo de ensamblado.
#include <stddef.h>
#include <stdint.h>

#include "bioiot_messages.h"

namespace bioiot {

class Reassembler {
 public:
  enum Result : uint8_t {
    kAccepted = 0,   // fragmento nuevo guardado
    kDuplicate = 1,  // ya recibido (se confirma igualmente)
    kComplete = 2,   // ultimo fragmento: datos listos
    kRejected = 3,   // inconsistente con el ensamblado en curso
  };
  static constexpr uint32_t kTimeoutMs = 20000;

  // Inicia la espera de un requestId concreto (descarta lo anterior).
  void expect(uint32_t requestId, uint32_t nowMs);
  void cancel() { active_ = false; complete_ = false; }
  Result add(const DiagFragmentMsg& f, uint32_t nowMs);
  bool timedOut(uint32_t nowMs) const { return active_ && !complete_ && nowMs - lastMs_ >= kTimeoutMs; }
  bool active() const { return active_; }
  bool complete() const { return complete_; }
  uint32_t requestId() const { return requestId_; }
  const uint8_t* data() const { return buf_; }
  size_t length() const { return totalLen_; }
  uint8_t received() const;
  uint8_t expectedCount() const { return count_; }

 private:
  bool active_ = false, complete_ = false;
  uint32_t requestId_ = 0, lastMs_ = 0, mask_ = 0;
  uint16_t totalLen_ = 0;
  uint8_t count_ = 0;
  uint8_t buf_[kDiagMaxBytes];
};

// Divide un buffer en fragmentos; el llamador los encola uno a uno.
class Fragmenter {
 public:
  bool start(uint32_t requestId, const uint8_t* data, size_t len);
  bool hasNext() const { return active_ && next_ < count_; }
  // Escribe el payload del siguiente fragmento; devuelve longitud o 0.
  size_t next(uint8_t* out, size_t cap);
  uint8_t count() const { return count_; }
  uint8_t sent() const { return next_; }
  void reset() { active_ = false; }

 private:
  bool active_ = false;
  uint32_t requestId_ = 0;
  const uint8_t* data_ = nullptr;
  uint16_t len_ = 0;
  uint8_t count_ = 0, next_ = 0;
};

}  // namespace bioiot
