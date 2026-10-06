#pragma once
// Capa de enlace portable: sesiones por arranque, deduplicacion, ACK de aplicacion,
// reintentos acotados no bloqueantes y cola de salida limitada.
//
// Reglas de sesion (resumen; ver PROTOCOLO.md):
//  * Cada arranque genera bootId aleatorio != 0.
//  * HELLO con dstBoot == mi bootId es prueba de frescura: confirma la sesion del par.
//  * Cualquier otra trama exige dstBoot == mi bootId y srcBoot == sesion confirmada
//    (o candidata, que se promueve). Si no, se rechaza y se responde HELLO.
//  * Los bootId retirados en este arranque no se vuelven a aceptar (anti-replay).
//  * La deduplicacion es por (par, srcBoot, seq) con ventana de 64.
#include <stddef.h>
#include <stdint.h>

#include "bioiot_messages.h"
#include "bioiot_protocol.h"

namespace bioiot {

class DedupWindow {
 public:
  enum Result : uint8_t { kFresh = 0, kDuplicate = 1, kTooOld = 2 };
  void reset() { valid_ = false; highest_ = 0; mask_ = 0; }
  Result classify(uint32_t seq) const;
  void mark(uint32_t seq);

 private:
  bool valid_ = false;
  uint32_t highest_ = 0;
  uint64_t mask_ = 0;  // bit i => (highest_ - i) visto
};

enum class Delivery : uint8_t {
  Delivered = 0,     // ACK de aplicacion recibido (no implica ejecucion)
  Failed = 1,        // reintentos agotados
  Expired = 2,       // vencio el plazo antes de entregarse
  Dropped = 3,       // expulsado por cola llena
  PeerRebooted = 4,  // el receptor reinicio; no se reenvia a la nueva sesion
  Rejected = 5,      // el receptor rechazo el contenido
};
const char* deliveryName(Delivery d);

class EndpointHandler {
 public:
  virtual ~EndpointHandler() {}
  // Devuelve AckStatus. kAckQueueFull => no se marca como visto (el emisor reintenta).
  virtual uint8_t onMessage(uint8_t src, const FrameHeader& h, const uint8_t* payload, size_t len) = 0;
  virtual void onDelivery(uint8_t dst, MsgType type, uint32_t cookie, Delivery result) {}
  virtual void onSessionConfirmed(uint8_t peer, uint32_t peerBoot) {}
  virtual void onHello(uint8_t peer, const HelloMsg& hello) {}
};

class RadioPort {
 public:
  virtual ~RadioPort() {}
  // Entrega la trama completa al driver ESP-NOW (unicast cifrado). No bloquea.
  virtual bool transmit(uint8_t dst, const uint8_t* frame, size_t len) = 0;
};

struct SendOptions {
  uint32_t cookie = 0;
  uint32_t ttlMs = 30000;     // plazo maximo desde el encolado
  uint8_t maxAttempts = 4;    // por ronda
  bool persistent = false;    // tras agotar una ronda, reintentar mas tarde hasta ttl
  bool evictable = false;     // puede expulsarse si la cola se llena (telemetria)
  bool rebindOnReboot = false;  // reenviar a la nueva sesion si el receptor reinicia
  int16_t txTimeOffset = -1;  // offset de un u32 que se reescribe con el reloj en cada envio
};

struct LinkStats {
  uint32_t rxOk = 0, rxRejected = 0, rxDuplicates = 0, rxStaleSession = 0, rxBadTag = 0;
  uint32_t txFrames = 0, txRadioErrors = 0, retries = 0, delivered = 0, failed = 0, expired = 0;
  uint32_t dropped = 0, sessionsConfirmed = 0, helloSent = 0, replayRejected = 0;
  uint8_t lastRejectReason = 0;
};

class Endpoint {
 public:
#if defined(ARDUINO_ARCH_ESP8266)
  static constexpr uint8_t kOutboxSlots = 16;  // ESP8266: ~80 KB de RAM total
#else
  static constexpr uint8_t kOutboxSlots = 24;
#endif
  static constexpr uint8_t kRetiredBoots = 4;
  static constexpr uint32_t kBaseRetryMs = 250;
  static constexpr uint32_t kRoundBackoffMs = 3000;
  static constexpr uint32_t kHelloReplyMinGapMs = 300;

  Endpoint(uint32_t systemId, uint8_t self, uint32_t bootId, RadioPort& radio, EndpointHandler& handler,
           uint32_t (*random)());

  bool addPeer(uint8_t node, const uint8_t key[kAppKeySize]);
  // Recibe una trama cruda (desde la cola del callback). Verifica tag y sesion.
  void receive(const uint8_t* data, size_t len, uint32_t nowMs, int8_t rssi);
  // Reintentos, vencimientos y envio de la cola.
  void poll(uint32_t nowMs);

  bool sendReliable(uint8_t dst, MsgType type, const uint8_t* payload, size_t len,
                    const SendOptions& opt, uint32_t nowMs);
  // Mensajes sin ACK (STATUS, TIME_SYNC). Requieren sesion confirmada.
  bool sendUnreliable(uint8_t dst, MsgType type, const uint8_t* payload, size_t len);
  // HELLO explicito (no necesita sesion). dstBoot: arranque del par si se conoce.
  bool sendHello(uint8_t dst, const HelloMsg& hello, uint32_t dstBoot, uint32_t nowMs);

  bool sessionConfirmed(uint8_t peer) const;
  uint32_t peerBoot(uint8_t peer) const;
  uint32_t lastFreshRxMs(uint8_t peer) const;
  bool everReceived(uint8_t peer) const;
  int8_t lastRssi(uint8_t peer) const;
  uint8_t queueDepth() const;
  uint8_t queueDepthFor(uint8_t peer) const;
  bool hasPending(uint8_t peer, MsgType type, uint32_t cookie) const;
  uint32_t bootId() const { return boot_; }
  uint8_t self() const { return self_; }
  uint32_t systemId() const { return systemId_; }
  // Desplaza el envio pendiente (p. ej. cambio de canal en curso).
  void deferAll(uint32_t nowMs, uint32_t delayMs);
  // Plantilla para el HELLO de respuesta automatica.
  void setHelloTemplate(const HelloMsg& h) { helloTemplate_ = h; }
  // Datos de la ultima trama aceptada (para TIME_SYNC y diagnosticos).
  const LinkStats& stats() const { return stats_; }

 private:
  struct Peer {
    bool configured = false;
    uint8_t key[kAppKeySize] = {};
    uint32_t confirmedBoot = 0, candidateBoot = 0;
    uint32_t retired[kRetiredBoots] = {};
    uint8_t retiredNext = 0;
    DedupWindow window;
    bool everRx = false;
    uint32_t lastFreshRxMs = 0, lastHelloReplyMs = 0;
    bool helloReplyEver = false;
    int8_t rssi = 0;
  };
  struct Entry {
    bool used = false, inFlight = false;
    uint8_t dst = 0, len = 0, attempts = 0;
    MsgType type = MsgType::Hello;
    uint8_t payload[kMaxPayload] = {};
    uint32_t seq = 0, boundBoot = 0, nextMs = 0, createdMs = 0, order = 0;
    SendOptions opt;
  };

  Peer* peer(uint8_t node);
  const Peer* peer(uint8_t node) const;
  bool isRetired(const Peer& p, uint32_t boot) const;
  void confirm(uint8_t node, Peer& p, uint32_t boot);
  void replyHello(uint8_t node, Peer& p, uint32_t dstBoot, uint32_t nowMs);
  bool transmitFrame(uint8_t dst, MsgType type, uint8_t flags, uint32_t dstBoot, uint32_t seq,
                     const uint8_t* payload, size_t len);
  void sendAck(uint8_t dst, uint32_t seq, MsgType type, uint8_t status);
  void handleAck(uint8_t src, const AckMsg& ack, uint32_t nowMs);
  void finish(Entry& e, Delivery result);
  uint32_t backoff(uint8_t attempts);
  Entry* oldestFor(uint8_t dst);

  uint32_t systemId_;
  uint8_t self_;
  uint32_t boot_;
  uint32_t seq_ = 0;
  uint32_t order_ = 0;
  RadioPort& radio_;
  EndpointHandler& handler_;
  uint32_t (*random_)();
  Peer peers_[kMaxNodeId + 1];
  Entry outbox_[kOutboxSlots];
  LinkStats stats_;
  HelloMsg helloTemplate_;
  uint32_t nowMs_ = 0;
};

}  // namespace bioiot
