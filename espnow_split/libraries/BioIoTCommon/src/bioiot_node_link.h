#pragma once
// Enlace de un nodo sensor (A o B) con el gateway: HELLO, recuperacion acotada de
// canal, STATUS periodico, TIME_SYNC y despacho de comandos a la aplicacion.
// Portable: el canal y su persistencia se inyectan (ChannelControl).
//
// Estados de canal:
//   LOCKED   : canal conocido; sesion activa o esperando respuesta.
//   HUNTING  : barrido 1..13 empezando por el ultimo bueno; HELLO por canal y
//              espera breve de respuesta autenticada. Nunca acepta una central
//              que no supere HMAC + sesion.
//   BACKOFF  : tras un barrido sin exito, vuelve al ultimo canal bueno y solo
//              envia HELLO espaciados; el periodo crece hasta 5 min.
// La adquisicion de sensores continua en todos los estados (no bloquea).
#include <stdint.h>

#include "bioiot_link.h"
#include "bioiot_time.h"

namespace bioiot {

class ChannelControl {
 public:
  virtual ~ChannelControl() {}
  virtual bool setChannel(uint8_t ch) = 0;
  virtual uint8_t channel() const = 0;
  // Solo se llama cuando el canal confirmado cambia (evita escrituras frecuentes).
  virtual void persistChannel(uint8_t ch) = 0;
};

class NodeApp {
 public:
  virtual ~NodeApp() {}
  // Comandos del gateway ya autenticados y deduplicados. Devuelve AckStatus.
  virtual uint8_t onGatewayMessage(const FrameHeader& h, const uint8_t* payload, size_t len) = 0;
  virtual void onGatewaySession(uint32_t gatewayBoot) = 0;
  virtual void fillStatus(StatusMsg& s) = 0;
  virtual void onDelivery(MsgType type, uint32_t cookie, Delivery result) {}
};

struct NodeLinkConfig {
  uint32_t linkTimeoutMs = 20000;  // sin trama fresca del gateway => offline
  uint32_t helloIntervalMs = 1500;
  uint32_t statusIntervalMs = 30000;
  uint32_t huntDwellMs = 250;
  uint32_t backoffMinMs = 30000;
  uint32_t backoffMaxMs = 300000;
  bool placeholderKeys = false;
};

class NodeLink : public EndpointHandler {
 public:
  enum class ChannelState : uint8_t { Locked = 0, Hunting = 1, Backoff = 2 };

  NodeLink(Endpoint& ep, ChannelControl& ch, NodeApp& app, const NodeLinkConfig& cfg)
      : ep_(ep), ch_(ch), app_(app), cfg_(cfg) {}
  void begin(uint8_t startChannel, uint32_t nowMs, uint8_t resetReason, bool selfTestOk, bool storageOk);
  // Entrega una trama de la cola del callback (verifica HMAC y sesion).
  void deliver(const uint8_t* data, size_t len, uint32_t nowMs, int8_t rssi);
  void loop(uint32_t nowMs);
  uint32_t lastTimeSyncMs() const { return lastTimeSyncMs_; }
  bool online(uint32_t nowMs) const;
  ChannelState channelState() const { return state_; }
  uint8_t lastGoodChannel() const { return lastGood_; }
  uint16_t hunts() const { return hunts_; }
  const UtcReference& utc() const { return utc_; }
  void setStorageOk(bool ok) { storageOk_ = ok; }
  void requestStatusSoon() { statusDue_ = true; }

  // EndpointHandler
  uint8_t onMessage(uint8_t src, const FrameHeader& h, const uint8_t* payload, size_t len) override;
  void onDelivery(uint8_t dst, MsgType type, uint32_t cookie, Delivery result) override;
  void onSessionConfirmed(uint8_t peer, uint32_t peerBoot) override;
  void onHello(uint8_t peer, const HelloMsg& hello) override {}

 private:
  void sendHello(uint32_t nowMs);
  void sendStatus(uint32_t nowMs);
  void startHunt(uint32_t nowMs);
  void huntStep(uint32_t nowMs);
  HelloMsg makeHello(uint32_t nowMs) const;

  Endpoint& ep_;
  ChannelControl& ch_;
  NodeApp& app_;
  NodeLinkConfig cfg_;
  UtcReference utc_;
  ChannelState state_ = ChannelState::Locked;
  uint8_t lastGood_ = 1, huntIndex_ = 0, huntOrder_[13] = {};
  uint8_t resetReason_ = 0;
  bool selfTestOk_ = false, storageOk_ = false, statusDue_ = false;
  uint16_t hunts_ = 0;
  uint32_t bootMs_ = 0, lockedSinceMs_ = 0, lastHelloMs_ = 0, lastStatusMs_ = 0;
  uint32_t huntStepMs_ = 0, backoffUntilMs_ = 0, backoffMs_ = 0, lastTimeSyncMs_ = 0;
  uint32_t freshAtHuntStart_ = 0, now_ = 0;
};

const char* channelStateName(NodeLink::ChannelState s);

}  // namespace bioiot
