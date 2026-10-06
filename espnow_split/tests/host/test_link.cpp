// Pruebas 2 y 7: duplicados, perdidas, desorden, timeouts, reinicios de nodo,
// replay, colas llenas y fragmentacion.
#include <algorithm>
#include <deque>

#include "BioIoTCommon.h"
#include "test_framework.h"

using namespace bioiot;

namespace {
uint32_t g_rand = 12345;
uint32_t testRandom() {
  g_rand = g_rand * 1103515245u + 12345u;
  return g_rand >> 8;
}

struct Packet {
  uint8_t dst;
  std::vector<uint8_t> bytes;
};

// Red simulada: perdida, duplicacion y entrega fuera de orden configurables.
struct Net {
  std::deque<Packet> inflight;
  std::vector<Packet> captured;
  int dropEvery = 0, dupEvery = 0, counter = 0;
  bool reverse = false, blackhole = false, capture = false;
};

struct FakeRadio : RadioPort {
  Net* net;
  explicit FakeRadio(Net* n) : net(n) {}
  bool transmit(uint8_t dst, const uint8_t* frame, size_t len) override {
    Packet p{dst, std::vector<uint8_t>(frame, frame + len)};
    if (net->capture) net->captured.push_back(p);
    net->counter++;
    if (net->blackhole) return true;
    if (net->dropEvery && net->counter % net->dropEvery == 0) return true;
    net->inflight.push_back(p);
    if (net->dupEvery && net->counter % net->dupEvery == 0) net->inflight.push_back(p);
    return true;
  }
};

struct Recorder : EndpointHandler {
  std::vector<std::pair<uint8_t, uint32_t>> messages;  // tipo, seq
  std::vector<std::vector<uint8_t>> payloads;
  std::vector<std::pair<uint32_t, Delivery>> deliveries;
  std::vector<uint32_t> sessions;
  uint8_t replyStatus = kAckAccepted;
  uint8_t onMessage(uint8_t, const FrameHeader& h, const uint8_t* p, size_t len) override {
    if (replyStatus != kAckQueueFull) {
      messages.push_back({uint8_t(h.type), h.seq});
      payloads.push_back(std::vector<uint8_t>(p, p + len));
    }
    return replyStatus;
  }
  void onDelivery(uint8_t, MsgType, uint32_t cookie, Delivery d) override { deliveries.push_back({cookie, d}); }
  void onSessionConfirmed(uint8_t, uint32_t boot) override { sessions.push_back(boot); }
};

uint8_t keyA[kAppKeySize];
void initKey() {
  for (uint8_t i = 0; i < kAppKeySize; ++i) keyA[i] = uint8_t(i * 3 + 1);
}

struct Pair {
  Net net;
  FakeRadio radioGw{&net}, radioNode{&net};
  Recorder gwRec, nodeRec;
  Endpoint* gw;
  Endpoint* node;
  uint32_t now = 1000;
  uint32_t nodeBoot = 0xA0000001;
  Pair() {
    initKey();
    gw = new Endpoint(0x5151, kNodeGateway, 0xC0000001, radioGw, gwRec, testRandom);
    gw->addPeer(kNodeA, keyA);
    node = new Endpoint(0x5151, kNodeA, nodeBoot, radioNode, nodeRec, testRandom);
    node->addPeer(kNodeGateway, keyA);
  }
  ~Pair() { delete gw; delete node; }
  void rebootNode(uint32_t boot) {
    delete node;
    nodeRec = Recorder();
    node = new Endpoint(0x5151, kNodeA, boot, radioNode, nodeRec, testRandom);
    node->addPeer(kNodeGateway, keyA);
  }
  void rebootGateway(uint32_t boot) {
    delete gw;
    gwRec = Recorder();
    gw = new Endpoint(0x5151, kNodeGateway, boot, radioGw, gwRec, testRandom);
    gw->addPeer(kNodeA, keyA);
  }
  void pump(int rounds = 50, uint32_t stepMs = 20) {
    for (int i = 0; i < rounds; ++i) {
      now += stepMs;
      gw->poll(now);
      node->poll(now);
      std::deque<Packet> batch;
      batch.swap(net.inflight);
      if (net.reverse) std::reverse(batch.begin(), batch.end());
      for (auto& p : batch) {
        Endpoint* target = p.dst == kNodeGateway ? gw : node;
        target->receive(p.bytes.data(), p.bytes.size(), now, -50);
      }
    }
  }
  void handshake() {
    HelloMsg h;
    h.role = kNodeA;
    node->sendHello(kNodeGateway, h, 0, now);
    pump(5);
  }
};

SendOptions telemetryOpts(uint32_t cookie) {
  SendOptions o;
  o.cookie = cookie; o.ttlMs = 600000; o.persistent = true; o.evictable = true; o.rebindOnReboot = true;
  o.txTimeOffset = 0;
  return o;
}
}  // namespace

TEST(dedup_window_classifies) {
  DedupWindow w;
  CHECK_EQ(w.classify(10), DedupWindow::kFresh);
  w.mark(10);
  CHECK_EQ(w.classify(10), DedupWindow::kDuplicate);
  CHECK_EQ(w.classify(9), DedupWindow::kFresh);  // fuera de orden pero nuevo
  w.mark(9);
  CHECK_EQ(w.classify(9), DedupWindow::kDuplicate);
  w.mark(100);
  CHECK_EQ(w.classify(37), DedupWindow::kFresh);  // 63 atras: dentro de la ventana
  CHECK_EQ(w.classify(36), DedupWindow::kTooOld);  // 64 atras: fuera
  CHECK_EQ(w.classify(30), DedupWindow::kTooOld);
  CHECK_EQ(w.classify(100), DedupWindow::kDuplicate);
  w.mark(0xFFFFFFFFu);  // salto enorme hacia "atras" en aritmetica serial: demasiado viejo
  CHECK_EQ(w.classify(101), DedupWindow::kFresh);
}

TEST(handshake_confirms_both_sides) {
  Pair p;
  CHECK(!p.node->sessionConfirmed(kNodeGateway));
  p.handshake();
  CHECK(p.node->sessionConfirmed(kNodeGateway));
  CHECK_EQ(p.node->peerBoot(kNodeGateway), 0xC0000001u);
  // El gateway confirma con la primera trama de datos que conoce su arranque.
  uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  CHECK(p.node->sendReliable(kNodeGateway, MsgType::Telemetry, data, 8, telemetryOpts(1), p.now));
  p.pump(10);
  CHECK(p.gw->sessionConfirmed(kNodeA));
  CHECK_EQ(p.gw->peerBoot(kNodeA), p.nodeBoot);
  CHECK_EQ(p.gwRec.messages.size(), 1u);
  CHECK_EQ(p.nodeRec.deliveries.size(), 1u);
  CHECK(p.nodeRec.deliveries[0].second == Delivery::Delivered);
}

TEST(lossy_duplicating_reordering_channel_delivers_exactly_once_to_app) {
  Pair p;
  p.handshake();
  p.net.dropEvery = 3;
  p.net.dupEvery = 4;
  p.net.reverse = true;
  for (uint32_t i = 0; i < 10; ++i) {
    uint8_t data[8] = {};
    putU32(data, 0);
    data[4] = uint8_t(i);
    CHECK(p.node->sendReliable(kNodeGateway, MsgType::Telemetry, data, 8, telemetryOpts(i), p.now));
  }
  p.pump(400, 50);
  CHECK_EQ(p.gwRec.messages.size(), 10u);
  std::vector<int> seen(10, 0);
  for (auto& pl : p.gwRec.payloads) seen[pl[4]]++;
  for (int i = 0; i < 10; ++i) CHECK_EQ(seen[i], 1);
  int delivered = 0;
  for (auto& d : p.nodeRec.deliveries) delivered += d.second == Delivery::Delivered;
  CHECK_EQ(delivered, 10);
  CHECK(p.gw->stats().rxDuplicates > 0);
}

TEST(tx_time_is_rewritten_on_each_retransmission) {
  Pair p;
  p.handshake();
  p.net.blackhole = true;
  p.net.capture = true;
  uint8_t data[8] = {};
  p.node->sendReliable(kNodeGateway, MsgType::Telemetry, data, 8, telemetryOpts(7), p.now);
  p.pump(100, 50);
  std::vector<uint32_t> times;
  for (auto& pk : p.net.captured) {
    if (pk.dst != kNodeGateway || pk.bytes[3] != uint8_t(MsgType::Telemetry)) continue;
    times.push_back(getU32(pk.bytes.data() + kHeaderSize));
    CHECK_EQ(getU32(pk.bytes.data() + 20), getU32(p.net.captured.back().bytes.data() + 20));  // mismo seq
  }
  CHECK(times.size() >= 4);
  for (size_t i = 1; i < times.size(); ++i) CHECK(times[i] > times[i - 1]);
}

TEST(unacked_command_times_out_and_reports_failed) {
  Pair p;
  p.handshake();
  uint8_t d[8] = {};
  p.node->sendReliable(kNodeGateway, MsgType::Telemetry, d, 8, telemetryOpts(1), p.now);
  p.pump(10);
  CHECK(p.gw->sessionConfirmed(kNodeA));
  p.net.blackhole = true;
  SendOptions cmd;
  cmd.cookie = 99; cmd.ttlMs = 30000; cmd.maxAttempts = 4;
  CHECK(p.gw->sendReliable(kNodeA, MsgType::Calibration, d, 8, cmd, p.now));
  p.pump(200, 50);
  CHECK_EQ(p.gwRec.deliveries.size(), 1u);
  CHECK(p.gwRec.deliveries[0].first == 99 && p.gwRec.deliveries[0].second == Delivery::Failed);
  CHECK(p.gw->stats().retries >= 3);
  CHECK_EQ(p.gw->queueDepth(), 0);
}

TEST(queue_full_ack_retries_without_marking_seen) {
  Pair p;
  p.handshake();
  p.gwRec.replyStatus = kAckQueueFull;
  uint8_t d[8] = {};
  p.node->sendReliable(kNodeGateway, MsgType::Telemetry, d, 8, telemetryOpts(5), p.now);
  p.pump(20);
  CHECK_EQ(p.gwRec.messages.size(), 0u);
  CHECK_EQ(p.nodeRec.deliveries.size(), 0u);
  p.gwRec.replyStatus = kAckAccepted;
  p.pump(60);
  CHECK_EQ(p.gwRec.messages.size(), 1u);
  CHECK_EQ(p.nodeRec.deliveries.size(), 1u);
}

TEST(node_reboot_new_session_old_commands_not_resent) {
  Pair p;
  p.handshake();
  uint8_t d[8] = {};
  p.node->sendReliable(kNodeGateway, MsgType::Telemetry, d, 8, telemetryOpts(1), p.now);
  p.pump(10);
  p.net.blackhole = true;
  SendOptions cmd;
  cmd.cookie = 77; cmd.ttlMs = 60000; cmd.maxAttempts = 50;
  p.gw->sendReliable(kNodeA, MsgType::Calibration, d, 8, cmd, p.now);
  p.pump(3);
  // El nodo reinicia: nuevo bootId, sin memoria de la sesion anterior.
  p.rebootNode(0xA0000002);
  p.net.blackhole = false;
  p.handshake();
  p.node->sendReliable(kNodeGateway, MsgType::Telemetry, d, 8, telemetryOpts(2), p.now);
  p.pump(20);
  CHECK_EQ(p.gw->peerBoot(kNodeA), 0xA0000002u);
  bool rebootReported = false;
  for (auto& x : p.gwRec.deliveries) rebootReported |= x.first == 77 && x.second == Delivery::PeerRebooted;
  CHECK(rebootReported);
  CHECK_EQ(p.nodeRec.messages.size(), 0u);  // el comando viejo nunca llega al nuevo arranque
}

TEST(gateway_reboot_rebinds_node_telemetry) {
  Pair p;
  p.handshake();
  uint8_t d[8] = {};
  p.node->sendReliable(kNodeGateway, MsgType::Telemetry, d, 8, telemetryOpts(1), p.now);
  p.pump(10);
  p.rebootGateway(0xC0000002);
  d[4] = 9;
  p.node->sendReliable(kNodeGateway, MsgType::Telemetry, d, 8, telemetryOpts(2), p.now);
  p.pump(200, 50);
  CHECK_EQ(p.node->peerBoot(kNodeGateway), 0xC0000002u);
  CHECK_EQ(p.gwRec.messages.size(), 1u);
  CHECK(p.gwRec.payloads.size() == 1 && p.gwRec.payloads[0][4] == 9);
}

TEST(replayed_old_session_frames_rejected) {
  Pair p;
  p.handshake();
  p.net.capture = true;
  uint8_t d[8] = {};
  p.node->sendReliable(kNodeGateway, MsgType::Telemetry, d, 8, telemetryOpts(1), p.now);
  p.pump(10);
  std::vector<Packet> old = p.net.captured;
  p.net.capture = false;
  // Nodo reinicia y establece nueva sesion.
  p.rebootNode(0xA0000003);
  p.handshake();
  p.node->sendReliable(kNodeGateway, MsgType::Telemetry, d, 8, telemetryOpts(2), p.now);
  p.pump(10);
  const size_t before = p.gwRec.messages.size();
  // Un atacante reinyecta tramas de la sesion anterior (HELLO incluido).
  for (int k = 0; k < 3; ++k)
    for (auto& pk : old)
      if (pk.dst == kNodeGateway) p.gw->receive(pk.bytes.data(), pk.bytes.size(), p.now, -40);
  CHECK_EQ(p.gwRec.messages.size(), before);
  CHECK_EQ(p.gw->peerBoot(kNodeA), 0xA0000003u);
  CHECK(p.gw->stats().replayRejected > 0);
}

TEST(outbox_bounded_evicts_oldest_telemetry) {
  Pair p;
  // Sin sesion: nada se envia, la cola se llena y expulsa lo mas antiguo.
  uint8_t d[8] = {};
  int accepted = 0;
  for (uint32_t i = 0; i < 100; ++i)
    accepted += p.node->sendReliable(kNodeGateway, MsgType::Telemetry, d, 8, telemetryOpts(i), p.now);
  CHECK_EQ(accepted, 100);
  CHECK_EQ(p.node->queueDepth(), Endpoint::kOutboxSlots);
  int dropped = 0;
  for (auto& x : p.nodeRec.deliveries) dropped += x.second == Delivery::Dropped;
  CHECK_EQ(dropped, 100 - Endpoint::kOutboxSlots);
  // Los no expulsables no desplazan nada cuando no hay hueco.
  SendOptions cmd;
  CHECK(!p.node->sendReliable(kNodeGateway, MsgType::CommandResult, d, 8, cmd, p.now));
  // Al recuperar la sesion se vacian sin crecer.
  p.handshake();
  p.pump(400, 50);
  CHECK_EQ(p.node->queueDepth(), 0);
  CHECK_EQ(p.gwRec.messages.size(), size_t(Endpoint::kOutboxSlots));
}

TEST(telemetry_expires_after_ttl) {
  Pair p;
  uint8_t d[8] = {};
  SendOptions o = telemetryOpts(3);
  o.ttlMs = 1000;
  p.node->sendReliable(kNodeGateway, MsgType::Telemetry, d, 8, o, p.now);
  p.pump(60, 50);
  CHECK(p.nodeRec.deliveries.size() == 1 && p.nodeRec.deliveries[0].second == Delivery::Expired);
}

TEST(fragmentation_roundtrip_with_duplicates_and_disorder) {
  uint8_t src[1000];
  for (size_t i = 0; i < sizeof(src); ++i) src[i] = uint8_t(i * 13);
  Fragmenter f;
  CHECK(f.start(55, src, sizeof(src)));
  std::vector<std::vector<uint8_t>> frags;
  uint8_t buf[kMaxPayload];
  while (f.hasNext()) {
    const size_t n = f.next(buf, sizeof(buf));
    CHECK(n > 0 && n <= kMaxPayload);
    frags.push_back(std::vector<uint8_t>(buf, buf + n));
  }
  CHECK_EQ(frags.size(), 5u);
  Reassembler r;
  r.expect(55, 0);
  int complete = 0;
  for (int i = int(frags.size()) - 1; i >= 0; --i) {
    for (int rep = 0; rep < 2; ++rep) {
      DiagFragmentMsg m;
      CHECK(decodeDiagFragment(frags[i].data(), frags[i].size(), m));
      if (r.add(m, 10) == Reassembler::kComplete) complete++;
    }
  }
  CHECK_EQ(complete, 1);
  CHECK(r.complete());
  CHECK_EQ(r.length(), sizeof(src));
  CHECK(memcmp(r.data(), src, sizeof(src)) == 0);
}

TEST(fragmentation_rejects_inconsistent_and_times_out) {
  uint8_t src[500] = {};
  Fragmenter f;
  f.start(1, src, sizeof(src));
  uint8_t buf[kMaxPayload];
  const size_t n = f.next(buf, sizeof(buf));
  DiagFragmentMsg m;
  CHECK(decodeDiagFragment(buf, n, m));
  CHECK(!decodeDiagFragment(buf, n - 1, m));  // fragmento intermedio truncado
  Reassembler r;
  r.expect(2, 0);
  CHECK_EQ(r.add(m, 0), Reassembler::kRejected);  // otro requestId
  r.expect(1, 0);
  CHECK_EQ(r.add(m, 0), Reassembler::kAccepted);
  CHECK(!r.timedOut(Reassembler::kTimeoutMs - 1));
  CHECK(r.timedOut(Reassembler::kTimeoutMs + 1));
  DiagFragmentMsg big = m;
  big.totalLen = kDiagMaxBytes + 1;
  uint8_t enc[kMaxPayload];
  const size_t e = encodeDiagFragment(big, enc, sizeof(enc));
  CHECK(!decodeDiagFragment(enc, e, m));
  CHECK(!f.start(1, src, kDiagMaxBytes + 1));
}

TEST(rx_ring_bounded_and_counts_drops) {
  static SpscFrameRing<4> ring;
  uint8_t data[10] = {};
  for (int i = 0; i < 10; ++i) ring.push(data, sizeof(data), nullptr, 0, 0);
  CHECK_EQ(ring.size(), 4u);
  CHECK_EQ(ring.dropped(), 6u);
  RxFrame f;
  int popped = 0;
  while (ring.pop(f)) popped++;
  CHECK_EQ(popped, 4);
  CHECK(!ring.push(data, 300, nullptr, 0, 0));
}

TEST(utc_reference_never_invented) {
  UtcReference ref;
  CHECK(!ref.utcValid(1000));
  CHECK_EQ(ref.utcMs(1000), 0);
  TimeSyncMsg m;
  m.utcValid = 0;
  ref.apply(m, 1000, 1);
  CHECK(!ref.utcValid(1000));
  m.utcValid = 1; m.utcMs = 1700000000000LL;
  ref.apply(m, 1000, 1);
  CHECK(ref.utcValid(2000));
  CHECK_EQ(ref.utcMs(3500), 1700000002500LL);
  CHECK(!ref.utcValid(1000 + UtcReference::kMaxValidityMs + 1));
}
