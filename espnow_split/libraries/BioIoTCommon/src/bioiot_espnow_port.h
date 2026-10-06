#pragma once
// Adaptador ESP-NOW por plataforma. Interfaz comun; implementaciones separadas:
//  * bioiot_espnow_esp32.cpp   -> Arduino-ESP32 3.3.x (ESP-IDF 5.5):
//      recv_cb(const esp_now_recv_info_t*, const uint8_t*, int)
//      send_cb(const esp_now_send_info_t* (=wifi_tx_info_t), esp_now_send_status_t)
//  * bioiot_espnow_esp8266.cpp -> ESP8266 Arduino 3.1.2 (SDK NONOS espnow.h):
//      recv_cb(u8* mac, u8* data, u8 len), send_cb(u8* mac, u8 status),
//      roles, esp_now_set_kok() como PMK y clave LMK en esp_now_add_peer().
// Siempre unicast cifrado. Si el cifrado no puede configurarse, begin()/addPeer()
// fallan y NO se transmite en claro (salvo BIOIOT_ALLOW_UNENCRYPTED_ESPNOW=1,
// opcion explicita de diagnostico, que se informa en STATUS/HELLO).
#include <stddef.h>
#include <stdint.h>

#include "bioiot_link.h"
#include "bioiot_ring.h"

#ifndef BIOIOT_ALLOW_UNENCRYPTED_ESPNOW
#define BIOIOT_ALLOW_UNENCRYPTED_ESPNOW 0
#endif

#if defined(ARDUINO_ARCH_ESP32) || defined(ARDUINO_ARCH_ESP8266)
namespace bioiot {

struct EspNowPeerConfig {
  uint8_t node = 0;
  uint8_t mac[6] = {};
  uint8_t lmk[kEspNowKeySize] = {};
};

class EspNowPort : public RadioPort {
 public:
  static constexpr uint8_t kMaxPeers = 3;
#if defined(ARDUINO_ARCH_ESP8266)
  static constexpr size_t kRxSlots = 8;
#else
  static constexpr size_t kRxSlots = 16;
#endif

  // Requiere WiFi en modo STA iniciado. channel=0: no cambia el canal actual.
  bool begin(const uint8_t pmk[kEspNowKeySize], uint8_t channel, uint32_t systemId, uint8_t self);
  bool addPeer(const EspNowPeerConfig& peer);
  bool transmit(uint8_t dst, const uint8_t* frame, size_t len) override;
  // Cambia el canal de radio. Solo valido con STA NO asociado a un AP.
  bool setChannel(uint8_t channel);
  uint8_t channel() const;
  bool pop(RxFrame& f) { return rx_.pop(f); }
  bool ready() const { return ready_; }
  bool encrypted() const { return encrypted_; }

  // Contadores del driver (no confirman entrega de aplicacion).
  uint32_t rxDropped() const { return rx_.dropped(); }
  volatile uint32_t sendOk = 0, sendFail = 0, rxUnknownMac = 0, rxPrecheckFail = 0, rxMacMismatch = 0;
  const char* lastError() const { return lastError_; }

  // Llamados desde los callbacks de plataforma.
  void onReceive(const uint8_t* mac, const uint8_t* data, size_t len, int8_t rssi, uint32_t nowMs);
  void onSent(bool ok) { if (ok) sendOk = sendOk + 1; else sendFail = sendFail + 1; }
  static EspNowPort* instance;

 private:
  const EspNowPeerConfig* byNode(uint8_t node) const;
  bool ready_ = false, encrypted_ = false;
  uint32_t systemId_ = 0;
  uint8_t self_ = 0;
  uint8_t peerCount_ = 0;
  EspNowPeerConfig peers_[kMaxPeers];
  SpscFrameRing<kRxSlots> rx_;
  const char* lastError_ = "";
};

}  // namespace bioiot
#endif
