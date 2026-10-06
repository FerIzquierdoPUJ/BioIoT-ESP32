#include "bioiot_node_link.h"

namespace bioiot {

const char* channelStateName(NodeLink::ChannelState s) {
  switch (s) {
    case NodeLink::ChannelState::Locked: return "locked";
    case NodeLink::ChannelState::Hunting: return "hunting";
    case NodeLink::ChannelState::Backoff: return "backoff";
  }
  return "unknown";
}

void NodeLink::begin(uint8_t startChannel, uint32_t nowMs, uint8_t resetReason, bool selfTestOk, bool storageOk) {
  lastGood_ = startChannel >= 1 && startChannel <= 13 ? startChannel : 1;
  ch_.setChannel(lastGood_);
  bootMs_ = lockedSinceMs_ = nowMs;
  resetReason_ = resetReason;
  selfTestOk_ = selfTestOk;
  storageOk_ = storageOk;
  backoffMs_ = cfg_.backoffMinMs;
  lastHelloMs_ = nowMs - cfg_.helloIntervalMs;  // primer HELLO inmediato
  lastStatusMs_ = nowMs;
  ep_.setHelloTemplate(makeHello(nowMs));
}

HelloMsg NodeLink::makeHello(uint32_t nowMs) const {
  HelloMsg h;
  h.role = ep_.self();
  h.channel = ch_.channel();
  h.uptimeMs = nowMs - bootMs_;
  h.resetReason = resetReason_;
  h.flags = uint16_t((selfTestOk_ ? kHelloSelfTestOk : 0) | (storageOk_ ? kHelloStorageOk : 0) |
                     (utc_.utcValid(nowMs) ? kHelloTimeSynced : 0) |
                     (cfg_.placeholderKeys ? kHelloPlaceholderKeys : 0));
  return h;
}

bool NodeLink::online(uint32_t nowMs) const {
  return ep_.sessionConfirmed(kNodeGateway) && ep_.everReceived(kNodeGateway) &&
         nowMs - ep_.lastFreshRxMs(kNodeGateway) < cfg_.linkTimeoutMs;
}

void NodeLink::sendHello(uint32_t nowMs) {
  lastHelloMs_ = nowMs;
  const HelloMsg h = makeHello(nowMs);
  ep_.setHelloTemplate(h);
  ep_.sendHello(kNodeGateway, h, ep_.peerBoot(kNodeGateway), nowMs);
}

void NodeLink::sendStatus(uint32_t nowMs) {
  lastStatusMs_ = nowMs;
  statusDue_ = false;
  StatusMsg s;
  app_.fillStatus(s);
  s.uptimeMs = nowMs - bootMs_;
  s.resetReason = resetReason_;
  s.queueDepth = ep_.queueDepth();
  const LinkStats& st = ep_.stats();
  s.txFailures = uint16_t(st.failed + st.txRadioErrors);
  s.retries = uint16_t(st.retries);
  s.rxRejected = uint16_t(st.rxRejected);
  s.telemetryDropped = uint16_t(st.dropped + st.expired);
  s.channelHunts = hunts_;
  s.channel = ch_.channel();
  s.rssi = ep_.lastRssi(kNodeGateway);
  s.flags |= uint16_t((selfTestOk_ ? kSfSelfTestOk : 0) | (storageOk_ ? kSfStorageOk : 0) |
                      (utc_.utcValid(nowMs) ? kSfTimeSynced : 0) |
                      (state_ != ChannelState::Locked ? kSfChannelHunting : 0) |
                      (cfg_.placeholderKeys ? kSfPlaceholderKeys : 0));
  uint8_t buf[kStatusSize];
  const size_t n = encodeStatus(s, buf, sizeof(buf));
  if (n) ep_.sendUnreliable(kNodeGateway, MsgType::Status, buf, n);
}

void NodeLink::startHunt(uint32_t nowMs) {
  state_ = ChannelState::Hunting;
  hunts_++;
  freshAtHuntStart_ = ep_.lastFreshRxMs(kNodeGateway);
  // Orden: ultimo canal bueno primero, luego el resto ascendente.
  uint8_t n = 0;
  huntOrder_[n++] = lastGood_;
  for (uint8_t c = 1; c <= 13; ++c)
    if (c != lastGood_) huntOrder_[n++] = c;
  huntIndex_ = 0;
  huntStepMs_ = nowMs - cfg_.huntDwellMs;  // paso inmediato
}

void NodeLink::huntStep(uint32_t nowMs) {
  // Exito: trama fresca autenticada del gateway despues de empezar el barrido.
  if (ep_.everReceived(kNodeGateway) && ep_.lastFreshRxMs(kNodeGateway) != freshAtHuntStart_ &&
      ep_.sessionConfirmed(kNodeGateway)) {
    const uint8_t found = ch_.channel();
    state_ = ChannelState::Locked;
    lockedSinceMs_ = nowMs;
    backoffMs_ = cfg_.backoffMinMs;
    if (found != lastGood_) {
      lastGood_ = found;
      ch_.persistChannel(found);
    }
    return;
  }
  if (nowMs - huntStepMs_ < cfg_.huntDwellMs) return;
  if (huntIndex_ >= 13) {
    // Barrido sin respuesta: operar en el ultimo canal bueno y esperar.
    ch_.setChannel(lastGood_);
    state_ = ChannelState::Backoff;
    backoffUntilMs_ = nowMs + backoffMs_;
    backoffMs_ = backoffMs_ * 2 > cfg_.backoffMaxMs ? cfg_.backoffMaxMs : backoffMs_ * 2;
    return;
  }
  huntStepMs_ = nowMs;
  ch_.setChannel(huntOrder_[huntIndex_++]);
  ep_.deferAll(nowMs, cfg_.huntDwellMs);
  sendHello(nowMs);
}

void NodeLink::deliver(const uint8_t* data, size_t len, uint32_t nowMs, int8_t rssi) {
  now_ = nowMs;
  ep_.receive(data, len, nowMs, rssi);
}

void NodeLink::loop(uint32_t nowMs) {
  now_ = nowMs;
  ep_.poll(nowMs);
  const bool isOnline = online(nowMs);
  switch (state_) {
    case ChannelState::Locked:
      if (isOnline) {
        lockedSinceMs_ = nowMs;
      } else {
        if (nowMs - lastHelloMs_ >= cfg_.helloIntervalMs) sendHello(nowMs);
        // Sin respuesta durante el timeout de enlace: buscar al gateway.
        const uint32_t since = ep_.everReceived(kNodeGateway) ? ep_.lastFreshRxMs(kNodeGateway) : lockedSinceMs_;
        if (nowMs - since >= cfg_.linkTimeoutMs && nowMs - lockedSinceMs_ >= cfg_.linkTimeoutMs)
          startHunt(nowMs);
      }
      break;
    case ChannelState::Hunting:
      huntStep(nowMs);
      break;
    case ChannelState::Backoff:
      if (isOnline) {
        state_ = ChannelState::Locked;
        lockedSinceMs_ = nowMs;
        backoffMs_ = cfg_.backoffMinMs;
      } else if (int32_t(nowMs - backoffUntilMs_) >= 0) {
        startHunt(nowMs);
      } else if (nowMs - lastHelloMs_ >= cfg_.helloIntervalMs * 4) {
        sendHello(nowMs);
      }
      break;
  }
  if (isOnline && (statusDue_ || nowMs - lastStatusMs_ >= cfg_.statusIntervalMs)) sendStatus(nowMs);
}

uint8_t NodeLink::onMessage(uint8_t src, const FrameHeader& h, const uint8_t* payload, size_t len) {
  if (src != kNodeGateway) return kAckRejectedInvalid;
  if (h.type == MsgType::TimeSync) {
    TimeSyncMsg t;
    if (!decodeTimeSync(payload, len, t)) return kAckRejectedInvalid;
    lastTimeSyncMs_ = now_;
    utc_.apply(t, now_, ep_.peerBoot(kNodeGateway));
    return kAckAccepted;
  }
  return app_.onGatewayMessage(h, payload, len);
}

void NodeLink::onDelivery(uint8_t dst, MsgType type, uint32_t cookie, Delivery result) {
  app_.onDelivery(type, cookie, result);
}

void NodeLink::onSessionConfirmed(uint8_t peer, uint32_t peerBoot) {
  if (peer != kNodeGateway) return;
  app_.onGatewaySession(peerBoot);
  statusDue_ = true;
}

}  // namespace bioiot
