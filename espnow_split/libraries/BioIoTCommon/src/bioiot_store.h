#pragma once
// Registro persistente de doble ranura con version, generacion y CRC32.
//  * Escribe siempre en la ranura que NO contiene el registro valido mas nuevo.
//  * Relee y compara tras escribir; si falla, el registro anterior sigue intacto.
//  * Al cargar elige la ranura valida de mayor generacion (aritmetica serial).
//  * Un formato futuro (CRC valido, version mayor) bloquea escrituras.
// El backend (Preferences en ESP32, LittleFS en ESP8266, RAM en pruebas) solo
// lee/escribe blobs completos.
#include <stddef.h>
#include <stdint.h>

namespace bioiot {

class SlotBackend {
 public:
  virtual ~SlotBackend() {}
  // Devuelve bytes leidos (0 si no existe o error).
  virtual size_t readSlot(uint8_t slot, uint8_t* buf, size_t cap) = 0;
  virtual bool writeSlot(uint8_t slot, const uint8_t* buf, size_t len) = 0;
};

constexpr uint32_t kStoreMagic = 0x53494F42;  // "BOIS" little-endian
constexpr size_t kStoreHeaderSize = 4 + 2 + 2 + 4;
constexpr size_t kStoreOverhead = kStoreHeaderSize + 4;
constexpr size_t kStoreMaxPayload = 200;

enum class StoreState : uint8_t {
  Empty = 0,         // ninguna ranura valida (primer arranque)
  Ok = 1,
  Corrupt = 2,       // habia datos pero ninguno valido
  FutureFormat = 3,  // escrito por firmware mas nuevo: escrituras bloqueadas
};
const char* storeStateName(StoreState s);

class RecordStore {
 public:
  RecordStore(SlotBackend& backend, uint16_t formatVersion) : backend_(backend), format_(formatVersion) {}
  // Carga el registro mas nuevo. Devuelve longitud del payload (0 si no hay).
  size_t load(uint8_t* payload, size_t cap);
  bool save(const uint8_t* payload, size_t len);
  StoreState state() const { return state_; }
  uint32_t generation() const { return generation_; }
  uint8_t activeSlot() const { return activeSlot_; }
  uint16_t storedFormat() const { return storedFormat_; }
  bool lastWriteOk() const { return lastWriteOk_; }
  // true si una verificacion fallida no pudo invalidar su ranura (requiere revision).
  bool lastWriteUncertain() const { return lastWriteUncertain_; }

  // Utilidades expuestas para pruebas.
  static size_t encode(uint16_t format, uint32_t generation, const uint8_t* payload, size_t len,
                       uint8_t* out, size_t cap);
  static bool parse(const uint8_t* buf, size_t len, uint16_t& format, uint32_t& generation,
                    const uint8_t*& payload, size_t& payloadLen);

 private:
  SlotBackend& backend_;
  uint16_t format_;
  StoreState state_ = StoreState::Empty;
  uint32_t generation_ = 0;
  uint8_t activeSlot_ = 1;  // primera escritura va a la ranura 0
  bool haveValid_ = false;
  uint16_t storedFormat_ = 0;
  bool lastWriteOk_ = true;
  bool lastWriteUncertain_ = false;
};

}  // namespace bioiot
