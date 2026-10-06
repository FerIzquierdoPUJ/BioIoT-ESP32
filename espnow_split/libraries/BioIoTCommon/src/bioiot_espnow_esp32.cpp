// Adaptador ESP-NOW para Arduino-ESP32 3.3.x (ESP-IDF 5.5). Verificado contra
// esp_now.h de esp32-libs 3.3.12: send_cb recibe const wifi_tx_info_t*.
#if defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>
#include <WiFi.h>
#include <esp_arduino_version.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <string.h>

#include "bioiot_espnow_port.h"

#if ESP_ARDUINO_VERSION_MAJOR < 3
#error "Este adaptador requiere Arduino-ESP32 3.x (ESP-IDF 5.x)."
#endif

namespace bioiot {
namespace {
void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (!EspNowPort::instance || !info || len <= 0) return;
  const int8_t rssi = info->rx_ctrl ? int8_t(info->rx_ctrl->rssi) : 0;
  EspNowPort::instance->onReceive(info->src_addr, data, size_t(len), rssi, millis());
}
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
void onSend(const esp_now_send_info_t* info, esp_now_send_status_t status) {
#else
void onSend(const uint8_t* mac, esp_now_send_status_t status) {
#endif
  if (EspNowPort::instance) EspNowPort::instance->onSent(status == ESP_NOW_SEND_SUCCESS);
}
}  // namespace

bool EspNowPort::begin(const uint8_t pmk[kEspNowKeySize], uint8_t channel, uint32_t systemId, uint8_t self) {
  systemId_ = systemId;
  self_ = self;
  instance = this;
  if (channel && !setChannel(channel)) { lastError_ = "set_channel_failed"; return false; }
  if (esp_now_init() != ESP_OK) { lastError_ = "esp_now_init_failed"; return false; }
  encrypted_ = !BIOIOT_ALLOW_UNENCRYPTED_ESPNOW;
  if (encrypted_ && esp_now_set_pmk(pmk) != ESP_OK) {
    lastError_ = "set_pmk_failed";
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
  esp_now_peer_info_t info = {};
  memcpy(info.peer_addr, p.mac, 6);
  info.channel = 0;  // canal actual de la interfaz (sigue al AP en el gateway)
  info.ifidx = WIFI_IF_STA;
  info.encrypt = encrypted_;
  if (encrypted_) memcpy(info.lmk, p.lmk, kEspNowKeySize);
  if (esp_now_is_peer_exist(p.mac)) esp_now_del_peer(p.mac);
  if (esp_now_add_peer(&info) != ESP_OK) { lastError_ = "add_peer_failed"; return false; }
  peers_[peerCount_++] = p;
  return true;
}

bool EspNowPort::transmit(uint8_t dst, const uint8_t* frame, size_t len) {
  const EspNowPeerConfig* p = byNode(dst);
  if (!ready_ || !p || len > kEspNowMaxFrame) return false;
  return esp_now_send(p->mac, frame, len) == ESP_OK;
}

bool EspNowPort::setChannel(uint8_t ch) {
  if (ch < 1 || ch > 13) return false;
  if (WiFi.status() == WL_CONNECTED) return false;  // el canal lo impone el AP
  return esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) == ESP_OK;
}

uint8_t EspNowPort::channel() const {
  uint8_t primary = 0;
  wifi_second_chan_t second;
  if (esp_wifi_get_channel(&primary, &second) != ESP_OK) return 0;
  return primary;
}

}  // namespace bioiot
#endif
