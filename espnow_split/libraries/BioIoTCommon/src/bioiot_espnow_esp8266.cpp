// Adaptador ESP-NOW para ESP8266 Arduino core 3.1.2 (SDK NONOS, espnow.h).
// Diferencias con ESP32: roles obligatorios, esp_now_set_kok() para la PMK,
// clave LMK por par en esp_now_add_peer(), callbacks con punteros no const y
// sin RSSI, y el canal del par se ajusta con esp_now_set_peer_channel().
#if defined(ARDUINO_ARCH_ESP8266)
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <string.h>

extern "C" {
#include <espnow.h>
#include <user_interface.h>
}

#include "bioiot_espnow_port.h"

namespace bioiot {
namespace {
void onRecv(uint8_t* mac, uint8_t* data, uint8_t len) {
  if (EspNowPort::instance) EspNowPort::instance->onReceive(mac, data, len, 0, millis());
}
void onSend(uint8_t* mac, uint8_t status) {
  if (EspNowPort::instance) EspNowPort::instance->onSent(status == 0);
}
}  // namespace

bool EspNowPort::begin(const uint8_t pmk[kEspNowKeySize], uint8_t channel, uint32_t systemId, uint8_t self) {
  systemId_ = systemId;
  self_ = self;
  instance = this;
  if (channel && !setChannel(channel)) { lastError_ = "set_channel_failed"; return false; }
  if (esp_now_init() != 0) { lastError_ = "esp_now_init_failed"; return false; }
  if (esp_now_set_self_role(ESP_NOW_ROLE_COMBO) != 0) { lastError_ = "set_role_failed"; return false; }
  encrypted_ = !BIOIOT_ALLOW_UNENCRYPTED_ESPNOW;
  if (encrypted_ && esp_now_set_kok(const_cast<uint8_t*>(pmk), kEspNowKeySize) != 0) {
    lastError_ = "set_kok_failed";
    esp_now_deinit();
    return false;
  }
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(onSend);
  ready_ = true;
  return true;
}

bool EspNowPort::addPeer(const EspNowPeerConfig& p) {
  if (!ready_ || peerCount_ >= kMaxPeers) { lastError_ = "peer_table_full"; return false; }
  uint8_t mac[6], lmk[kEspNowKeySize];
  memcpy(mac, p.mac, 6);
  memcpy(lmk, p.lmk, kEspNowKeySize);
  if (esp_now_is_peer_exist(mac) > 0) esp_now_del_peer(mac);
  const int rc = encrypted_ ? esp_now_add_peer(mac, ESP_NOW_ROLE_COMBO, channel(), lmk, kEspNowKeySize)
                            : esp_now_add_peer(mac, ESP_NOW_ROLE_COMBO, channel(), nullptr, 0);
  memset(lmk, 0, sizeof(lmk));
  if (rc != 0) { lastError_ = "add_peer_failed"; return false; }
  peers_[peerCount_++] = p;
  return true;
}

bool EspNowPort::transmit(uint8_t dst, const uint8_t* frame, size_t len) {
  const EspNowPeerConfig* p = byNode(dst);
  if (!ready_ || !p || len > kEspNowMaxFrame) return false;
  uint8_t mac[6];
  memcpy(mac, p->mac, 6);
  return esp_now_send(mac, const_cast<uint8_t*>(frame), int(len)) == 0;
}

bool EspNowPort::setChannel(uint8_t ch) {
  if (ch < 1 || ch > 13) return false;
  if (WiFi.status() == WL_CONNECTED) return false;
  if (!wifi_set_channel(ch)) return false;
  for (uint8_t i = 0; i < peerCount_; ++i) {
    uint8_t mac[6];
    memcpy(mac, peers_[i].mac, 6);
    esp_now_set_peer_channel(mac, ch);
  }
  return true;
}

uint8_t EspNowPort::channel() const { return wifi_get_channel(); }

}  // namespace bioiot
#endif
