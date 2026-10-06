// Parte comun del adaptador (filtrado en callback). Sin llamadas de plataforma.
#include "bioiot_espnow_port.h"

#if defined(ARDUINO_ARCH_ESP32) || defined(ARDUINO_ARCH_ESP8266)
#include <string.h>

namespace bioiot {

EspNowPort* EspNowPort::instance = nullptr;

const EspNowPeerConfig* EspNowPort::byNode(uint8_t node) const {
  for (uint8_t i = 0; i < peerCount_; ++i)
    if (peers_[i].node == node) return &peers_[i];
  return nullptr;
}

// Contexto de callback: solo comprobaciones baratas y copia a la cola acotada.
// Sin HMAC, flash, TLS ni adquisicion. La verificacion completa ocurre en loop/tarea.
void EspNowPort::onReceive(const uint8_t* mac, const uint8_t* data, size_t len, int8_t rssi, uint32_t nowMs) {
  const EspNowPeerConfig* peer = nullptr;
  for (uint8_t i = 0; i < peerCount_; ++i)
    if (mac && memcmp(peers_[i].mac, mac, 6) == 0) { peer = &peers_[i]; break; }
  if (!peer) { rxUnknownMac = rxUnknownMac + 1; return; }
  if (precheckFrame(data, len, systemId_, self_) != FrameError::None) { rxPrecheckFail = rxPrecheckFail + 1; return; }
  // La MAC no autentica, pero debe coincidir con el nodo declarado en la trama.
  if (frameSource(data) != peer->node) { rxMacMismatch = rxMacMismatch + 1; return; }
  rx_.push(data, len, mac, rssi, nowMs);
}

}  // namespace bioiot
#endif
