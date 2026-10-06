#include "bioiot_link.h"

#include <string.h>

#include "bioiot_codec.h"

namespace bioiot {

// ---------------- DedupWindow ----------------
DedupWindow::Result DedupWindow::classify(uint32_t seq) const {
  if (!valid_) return kFresh;
  const int32_t d = int32_t(seq - highest_);
  if (d > 0) return kFresh;
  const uint32_t back = uint32_t(-d);
  if (back >= 64) return kTooOld;
  return (mask_ >> back) & 1u ? kDuplicate : kFresh;
}

void DedupWindow::mark(uint32_t seq) {
  if (!valid_) {
    valid_ = true; highest_ = seq; mask_ = 1;
    return;
  }
  const int32_t d = int32_t(seq - highest_);
  if (d > 0) {
    mask_ = uint32_t(d) >= 64 ? 0 : mask_ << d;
    mask_ |= 1;
    highest_ = seq;
  } else {
    const uint32_t back = uint32_t(-d);
    if (back < 64) mask_ |= (uint64_t(1) << back);
  }
}

const char* deliveryName(Delivery d) {
  switch (d) {
    case Delivery::Delivered: return "delivered";
    case Delivery::Failed: return "failed";
    case Delivery::Expired: return "expired";
    case Delivery::Dropped: return "dropped";
    case Delivery::PeerRebooted: return "peer_rebooted";
    case Delivery::Rejected: return "rejected";
  }
  return "unknown";
}

// ---------------- Endpoint ----------------
Endpoint::Endpoint(uint32_t systemId, uint8_t self, uint32_t bootId, RadioPort& radio,
                   EndpointHandler& handler, uint32_t (*random)())
    : systemId_(systemId), self_(self), boot_(bootId ? bootId : 1), radio_(radio), handler_(handler),
      random_(random) {
  helloTemplate_.role = self;
}

bool Endpoint::addPeer(uint8_t node, const uint8_t key[kAppKeySize]) {
  Peer* p = peer(node);
  if (!p || node == self_) return false;
  *p = Peer();
  memcpy(p->key, key, kAppKeySize);
  p->configured = true;
  return true;
}

Endpoint::Peer* Endpoint::peer(uint8_t node) {
  return node > kNodeNone && node <= kMaxNodeId ? &peers_[node] : nullptr;
}
const Endpoint::Peer* Endpoint::peer(uint8_t node) const {
  return node > kNodeNone && node <= kMaxNodeId ? &peers_[node] : nullptr;
}

bool Endpoint::isRetired(const Peer& p, uint32_t boot) const {
  for (uint8_t i = 0; i < kRetiredBoots; ++i)
    if (p.retired[i] && p.retired[i] == boot) return true;
  return false;
}

void Endpoint::confirm(uint8_t node, Peer& p, uint32_t boot) {
  if (p.confirmedBoot == boot) return;
  if (p.confirmedBoot) {
    p.retired[p.retiredNext] = p.confirmedBoot;
    p.retiredNext = uint8_t((p.retiredNext + 1) % kRetiredBoots);
  }
  p.confirmedBoot = boot;
  if (p.candidateBoot == boot) p.candidateBoot = 0;
  p.window.reset();
  stats_.sessionsConfirmed++;
  // Comandos dirigidos a la sesion anterior: no se reenvian salvo rebind explicito.
  for (Entry& e : outbox_) {
    if (!e.used || e.dst != node || !e.boundBoot || e.boundBoot == boot) continue;
    if (e.opt.rebindOnReboot) {
      e.boundBoot = 0; e.inFlight = false; e.attempts = 0; e.nextMs = nowMs_;
    } else {
      finish(e, Delivery::PeerRebooted);
    }
  }
  handler_.onSessionConfirmed(node, boot);
}

void Endpoint::replyHello(uint8_t node, Peer& p, uint32_t dstBoot, uint32_t nowMs) {
  if (p.helloReplyEver && nowMs - p.lastHelloReplyMs < kHelloReplyMinGapMs) return;
  p.helloReplyEver = true;
  p.lastHelloReplyMs = nowMs;
  HelloMsg h = helloTemplate_;
  h.role = self_;
  h.flags |= kHelloReply;
  sendHello(node, h, dstBoot, nowMs);
}

bool Endpoint::transmitFrame(uint8_t dst, MsgType type, uint8_t flags, uint32_t dstBoot, uint32_t seq,
                             const uint8_t* payload, size_t len) {
  const Peer* p = peer(dst);
  if (!p || !p->configured || len > kMaxPayload) return false;
  FrameHeader h;
  h.type = type; h.systemId = systemId_; h.src = self_; h.dst = dst; h.flags = flags;
  h.payloadLen = uint8_t(len); h.srcBoot = boot_; h.dstBoot = dstBoot; h.seq = seq;
  uint8_t frame[kEspNowMaxFrame];
  const size_t n = encodeFrame(h, payload, p->key, kAppKeySize, frame, sizeof(frame));
  if (!n) return false;
  stats_.txFrames++;
  if (!radio_.transmit(dst, frame, n)) {
    stats_.txRadioErrors++;
    return false;
  }
  return true;
}

bool Endpoint::sendHello(uint8_t dst, const HelloMsg& hello, uint32_t dstBoot, uint32_t nowMs) {
  nowMs_ = nowMs;
  uint8_t buf[kHelloSize];
  const size_t n = encodeHello(hello, buf, sizeof(buf));
  if (!n) return false;
  stats_.helloSent++;
  return transmitFrame(dst, MsgType::Hello, 0, dstBoot, ++seq_, buf, n);
}

bool Endpoint::sendUnreliable(uint8_t dst, MsgType type, const uint8_t* payload, size_t len) {
  const Peer* p = peer(dst);
  if (!p || !p->confirmedBoot) return false;
  return transmitFrame(dst, type, 0, p->confirmedBoot, ++seq_, payload, len);
}

void Endpoint::sendAck(uint8_t dst, uint32_t seq, MsgType type, uint8_t status) {
  AckMsg a;
  a.ackSeq = seq; a.ackType = uint8_t(type); a.status = status;
  uint8_t buf[kAckSize];
  const size_t n = encodeAck(a, buf, sizeof(buf));
  const Peer* p = peer(dst);
  if (n && p && p->confirmedBoot) transmitFrame(dst, MsgType::Ack, 0, p->confirmedBoot, ++seq_, buf, n);
}

bool Endpoint::sendReliable(uint8_t dst, MsgType type, const uint8_t* payload, size_t len,
                            const SendOptions& opt, uint32_t nowMs) {
  nowMs_ = nowMs;
  const Peer* p = peer(dst);
  if (!p || !p->configured || len > kMaxPayload || type == MsgType::Hello || type == MsgType::Ack ||
      opt.maxAttempts == 0 || (opt.txTimeOffset >= 0 && size_t(opt.txTimeOffset) + 4 > len))
    return false;
  Entry* slot = nullptr;
  for (Entry& e : outbox_)
    if (!e.used) { slot = &e; break; }
  if (!slot && opt.evictable) {
    // Cola llena: expulsar la telemetria mas antigua que no este en vuelo.
    Entry* victim = nullptr;
    for (Entry& e : outbox_)
      if (e.used && e.opt.evictable && !e.inFlight && (!victim || int32_t(e.order - victim->order) < 0))
        victim = &e;
    if (victim) {
      finish(*victim, Delivery::Dropped);
      slot = victim;
    }
  }
  if (!slot) return false;
  *slot = Entry();
  slot->used = true; slot->dst = dst; slot->type = type; slot->len = uint8_t(len);
  memcpy(slot->payload, payload, len);
  slot->createdMs = nowMs; slot->nextMs = nowMs; slot->order = ++order_; slot->opt = opt;
  return true;
}

Endpoint::Entry* Endpoint::oldestFor(uint8_t dst) {
  Entry* best = nullptr;
  for (Entry& e : outbox_)
    if (e.used && e.dst == dst && (!best || int32_t(e.order - best->order) < 0)) best = &e;
  return best;
}

uint32_t Endpoint::backoff(uint8_t attempts) {
  uint32_t delay = kBaseRetryMs << (attempts > 3 ? 3 : attempts - 1);
  return delay + (random_ ? random_() % 100 : 0);  // pequeno desfase entre nodos
}

void Endpoint::finish(Entry& e, Delivery result) {
  if (!e.used) return;
  e.used = false;
  switch (result) {
    case Delivery::Delivered: stats_.delivered++; break;
    case Delivery::Expired: stats_.expired++; break;
    case Delivery::Dropped: stats_.dropped++; break;
    default: stats_.failed++; break;
  }
  handler_.onDelivery(e.dst, e.type, e.opt.cookie, result);
}

void Endpoint::poll(uint32_t nowMs) {
  nowMs_ = nowMs;
  // Vencimientos.
  for (Entry& e : outbox_)
    if (e.used && nowMs - e.createdMs >= e.opt.ttlMs) finish(e, Delivery::Expired);
  for (uint8_t node = 1; node <= kMaxNodeId; ++node) {
    Peer& p = peers_[node];
    if (!p.configured || !p.confirmedBoot) continue;
    Entry* e = oldestFor(node);
    if (!e || int32_t(nowMs - e->nextMs) < 0) continue;
    if (e->boundBoot && e->boundBoot != p.confirmedBoot) {
      // Seguridad adicional: confirm() ya resolvio la mayoria de los casos.
      if (e->opt.rebindOnReboot) { e->boundBoot = 0; e->inFlight = false; e->attempts = 0; }
      else { finish(*e, Delivery::PeerRebooted); continue; }
    }
    if (e->inFlight && e->attempts >= e->opt.maxAttempts) {
      if (e->opt.persistent) {
        e->inFlight = false; e->attempts = 0; e->nextMs = nowMs + kRoundBackoffMs;
      } else {
        finish(*e, Delivery::Failed);
      }
      continue;
    }
    if (!e->seq) e->seq = ++seq_;
    if (e->opt.txTimeOffset >= 0) putU32(e->payload + e->opt.txTimeOffset, nowMs);
    const uint8_t flags = uint8_t(kFlagAckRequested | (e->attempts ? kFlagRetransmission : 0));
    if (e->attempts) stats_.retries++;
    e->attempts++;
    e->inFlight = true;
    e->boundBoot = p.confirmedBoot;
    e->nextMs = nowMs + backoff(e->attempts);
    transmitFrame(node, e->type, flags, p.confirmedBoot, e->seq, e->payload, e->len);
  }
}

void Endpoint::handleAck(uint8_t src, const AckMsg& ack, uint32_t nowMs) {
  for (Entry& e : outbox_) {
    if (!e.used || !e.inFlight || e.dst != src || e.seq != ack.ackSeq || uint8_t(e.type) != ack.ackType) continue;
    if (ack.status == kAckQueueFull) {
      // Recibido pero no aceptado: reintentar despues sin agotar la ronda.
      e.inFlight = false; e.attempts = 0; e.nextMs = nowMs + 500;
    } else if (ack.status == kAckAccepted || ack.status == kAckDuplicate) {
      finish(e, Delivery::Delivered);
    } else {
      finish(e, Delivery::Rejected);
    }
    return;
  }
}

void Endpoint::receive(const uint8_t* data, size_t len, uint32_t nowMs, int8_t rssi) {
  nowMs_ = nowMs;
  FrameError err = precheckFrame(data, len, systemId_, self_);
  Peer* p = err == FrameError::None ? peer(frameSource(data)) : nullptr;
  if (err == FrameError::None && (!p || !p->configured)) err = FrameError::UnknownSource;
  FrameHeader h;
  const uint8_t* payload = nullptr;
  if (err == FrameError::None) err = decodeFrame(data, len, systemId_, self_, p->key, kAppKeySize, h, payload);
  if (err != FrameError::None) {
    stats_.rxRejected++;
    stats_.lastRejectReason = uint8_t(err);
    if (err == FrameError::BadTag) stats_.rxBadTag++;
    return;
  }
  const uint8_t src = h.src;
  if (h.srcBoot == 0 || isRetired(*p, h.srcBoot)) {
    stats_.rxRejected++; stats_.replayRejected++;
    return;
  }

  if (h.type == MsgType::Hello) {
    HelloMsg hello;
    if (!decodeHello(payload, h.payloadLen, hello) || hello.role != src) {
      stats_.rxRejected++;
      return;
    }
    p->rssi = rssi;
    if (h.dstBoot == boot_) {
      // Prueba de frescura: el par conoce mi arranque actual.
      confirm(src, *p, h.srcBoot);
      p->everRx = true;
      p->lastFreshRxMs = nowMs;
      if (!(hello.flags & kHelloReply)) replyHello(src, *p, h.srcBoot, nowMs);
    } else {
      if (h.srcBoot != p->confirmedBoot) p->candidateBoot = h.srcBoot;
      replyHello(src, *p, h.srcBoot, nowMs);
    }
    stats_.rxOk++;
    handler_.onHello(src, hello);
    return;
  }

  // Resto de tipos: sesion vigente obligatoria.
  if (h.dstBoot != boot_) {
    stats_.rxStaleSession++; stats_.rxRejected++;
    if (h.srcBoot != p->confirmedBoot) p->candidateBoot = h.srcBoot;
    replyHello(src, *p, h.srcBoot, nowMs);
    return;
  }
  if (h.srcBoot != p->confirmedBoot) {
    if (h.srcBoot == p->candidateBoot) {
      confirm(src, *p, h.srcBoot);
    } else {
      stats_.rxStaleSession++; stats_.rxRejected++;
      p->candidateBoot = h.srcBoot;
      replyHello(src, *p, h.srcBoot, nowMs);
      return;
    }
  }
  p->everRx = true;
  p->lastFreshRxMs = nowMs;
  p->rssi = rssi;

  const DedupWindow::Result dup = p->window.classify(h.seq);
  if (dup != DedupWindow::kFresh) {
    stats_.rxDuplicates++;
    if (h.flags & kFlagAckRequested) sendAck(src, h.seq, h.type, kAckDuplicate);
    return;
  }
  if (h.type == MsgType::Ack) {
    AckMsg ack;
    p->window.mark(h.seq);
    if (decodeAck(payload, h.payloadLen, ack)) handleAck(src, ack, nowMs);
    else stats_.rxRejected++;
    stats_.rxOk++;
    return;
  }
  const uint8_t status = handler_.onMessage(src, h, payload, h.payloadLen);
  if (status != kAckQueueFull) p->window.mark(h.seq);
  if (status == kAckRejectedInvalid) stats_.rxRejected++;
  else stats_.rxOk++;
  if (h.flags & kFlagAckRequested) sendAck(src, h.seq, h.type, status);
}

bool Endpoint::sessionConfirmed(uint8_t node) const {
  const Peer* p = peer(node);
  return p && p->confirmedBoot;
}
uint32_t Endpoint::peerBoot(uint8_t node) const {
  const Peer* p = peer(node);
  return p ? p->confirmedBoot : 0;
}
uint32_t Endpoint::lastFreshRxMs(uint8_t node) const {
  const Peer* p = peer(node);
  return p ? p->lastFreshRxMs : 0;
}
bool Endpoint::everReceived(uint8_t node) const {
  const Peer* p = peer(node);
  return p && p->everRx;
}
int8_t Endpoint::lastRssi(uint8_t node) const {
  const Peer* p = peer(node);
  return p ? p->rssi : 0;
}
uint8_t Endpoint::queueDepth() const {
  uint8_t n = 0;
  for (const Entry& e : outbox_) n += e.used ? 1 : 0;
  return n;
}
uint8_t Endpoint::queueDepthFor(uint8_t node) const {
  uint8_t n = 0;
  for (const Entry& e : outbox_) n += e.used && e.dst == node ? 1 : 0;
  return n;
}
bool Endpoint::hasPending(uint8_t node, MsgType type, uint32_t cookie) const {
  for (const Entry& e : outbox_)
    if (e.used && e.dst == node && e.type == type && e.opt.cookie == cookie) return true;
  return false;
}
void Endpoint::deferAll(uint32_t nowMs, uint32_t delayMs) {
  for (Entry& e : outbox_)
    if (e.used) { e.inFlight = false; e.attempts = 0; e.nextMs = nowMs + delayMs; }
}

}  // namespace bioiot
