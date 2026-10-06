// Pruebas de la politica Wi-Fi del gateway (WifiPolicy) con un hotspot simulado:
// orden de credenciales, plazos, portal (automatico, Serial, 180 s, guardado),
// hotspot que vuelve con otro canal/BSSID, busquedas acotadas y ESP-NOW durante
// la perdida de Internet (con Endpoint/NodeLink reales).
#include <deque>

#include "BioIoTCommon.h"
#include "WifiPolicy.h"
#include "test_framework.h"

using namespace bioiot;
using gateway::LinkStatus;
using gateway::WifiAction;
using gateway::WifiCred;
using gateway::WifiPhase;
using gateway::WifiPolicy;
using gateway::WifiTimings;

namespace {

// Red Wi-Fi: SSID (entero) y contrasena correcta (entero).
struct Net {
  int ssid = 0;
  int pass = 0;
};

struct Hotspot {
  bool on = true;
  int ssid = 1, pass = 42;
  uint8_t channel = 6;
  uint8_t bssid = 0xA1;
};

// Ejecuta las acciones de WifiPolicy como lo hace AzureLink, sobre un modelo de radio.
struct SimGateway {
  WifiPolicy pol;
  WifiTimings t;
  Hotspot* hs = nullptr;
  bool hasStored = false, hasDefault = false;
  Net stored, def;
  uint8_t espnowCh = 1;  // pista inicial (canal guardado)
  uint8_t radioCh = 1;   // 0 = barriendo canales (ESP-NOW interrumpido)
  uint8_t knownBssid = 0;
  bool staUp = false, portal = false;
  uint32_t channelChanges = 0, scanMs = 0, portalOpens = 0, autoPortals = 0, closes = 0;
  struct Attempt {
    bool active = false, full = false;
    WifiCred cred = WifiCred::None;
    uint32_t start = 0;
  } at;
  std::vector<WifiAction> log;

  static constexpr uint32_t kScanMs = 2500;

  Net credNet(WifiCred c) const { return c == WifiCred::Stored ? stored : def; }

  void apply(const WifiAction& a, uint32_t now) {
    if (a.kind != WifiAction::None) log.push_back(a);
    switch (a.kind) {
      case WifiAction::None: break;
      case WifiAction::Connect:
        at.active = true; at.full = a.fullScan; at.cred = a.cred; at.start = now;
        staUp = false;
        break;
      case WifiAction::AbortConnect:
      case WifiAction::LinkLost:
        at.active = false; staUp = false; radioCh = espnowCh;
        break;
      case WifiAction::OpenPortal:
        portal = true; portalOpens++;
        if (a.automatic) autoPortals++;
        if (!staUp) { at.active = false; radioCh = espnowCh; }
        break;
      case WifiAction::ClosePortal:
        portal = false; closes++;
        if (!staUp) radioCh = espnowCh;
        break;
      case WifiAction::Connected:
        at.active = false;
        knownBssid = hs->bssid;
        if (espnowCh != hs->channel) { espnowCh = hs->channel; channelChanges++; }
        radioCh = hs->channel;
        break;
    }
  }

  // Estado del STA visto por WiFi.status().
  LinkStatus advance(uint32_t now) {
    if (staUp && (!hs->on || radioCh != hs->channel || knownBssid != hs->bssid)) staUp = false;  // hotspot reiniciado
    if (at.active && !staUp) {
      const Net n = credNet(at.cred);
      const bool ssidSeen = hs->on && n.ssid == hs->ssid;
      const uint32_t el = now - at.start;
      if (at.full) {
        if (el < kScanMs) { radioCh = 0; scanMs += 10; return LinkStatus::Pending; }
        if (ssidSeen && n.pass == hs->pass) { staUp = true; radioCh = hs->channel; return LinkStatus::Up; }
        radioCh = 13;  // la radio queda donde termino el barrido hasta que se restaure
        return ssidSeen ? LinkStatus::Failed : (el > kScanMs + 500 ? LinkStatus::Failed : LinkStatus::Pending);
      }
      // Rapido: solo el canal y BSSID conocidos, sin barrido.
      if (ssidSeen && n.pass == hs->pass && hs->channel == espnowCh && hs->bssid == knownBssid && el >= 800) {
        staUp = true;
        return LinkStatus::Up;
      }
      return LinkStatus::Pending;
    }
    return staUp ? LinkStatus::Up : LinkStatus::Pending;
  }

  void begin(uint32_t now) { apply(pol.begin(t, hasStored, hasDefault, now), now); }
  void step(uint32_t now) { apply(pol.tick(now, advance(now)), now); }
  // Usuario guarda una red en el portal: WiFiManager conecta (o no) y cierra el portal.
  bool userSaves(const Net& n, uint32_t now) {
    if (!portal || !hs->on || n.ssid != hs->ssid || n.pass != hs->pass) return false;
    staUp = true; portal = false;
    stored = n; hasStored = true;
    radioCh = hs->channel;
    apply(pol.onPortalConnected(now), now);
    return true;
  }
  void run(uint32_t& now, uint32_t ms) {
    for (uint32_t end = now + ms; int32_t(now - end) < 0; now += 10) step(now);
  }
  int count(WifiAction::Kind k) const {
    int n = 0;
    for (const auto& a : log) n += a.kind == k;
    return n;
  }
  int fullConnects() const {
    int n = 0;
    for (const auto& a : log) n += a.kind == WifiAction::Connect && a.fullScan;
    return n;
  }
};

}  // namespace

// ---------------- Credenciales y portal ----------------
TEST(wifi_stored_credentials_valid_connect_first) {
  Hotspot hs;
  SimGateway g; g.hs = &hs;
  g.hasStored = true; g.stored = {1, 42};
  g.hasDefault = true; g.def = {1, 999};  // predeterminadas incorrectas: no deben usarse
  uint32_t now = 1000;
  g.begin(now);
  CHECK(g.log[0].kind == WifiAction::Connect && g.log[0].cred == WifiCred::Stored && g.log[0].fullScan);
  g.run(now, 5000);
  CHECK(g.pol.phase() == WifiPhase::Connected && g.staUp);
  CHECK(g.pol.activeCred() == WifiCred::Stored);
  CHECK_EQ(g.portalOpens, 0u);
  CHECK_STR(g.pol.searchStateName(), "connected");
}

TEST(wifi_default_network_available_when_nothing_stored) {
  Hotspot hs;
  SimGateway g; g.hs = &hs;
  g.hasDefault = true; g.def = {1, 42};
  uint32_t now = 0;
  g.begin(now);
  CHECK(g.log[0].kind == WifiAction::Connect && g.log[0].cred == WifiCred::Default);
  g.run(now, 5000);
  CHECK(g.pol.phase() == WifiPhase::Connected && g.pol.activeCred() == WifiCred::Default);
}

TEST(wifi_default_network_absent_opens_portal_within_bound) {
  Hotspot hs; hs.on = false;
  SimGateway g; g.hs = &hs;
  g.hasDefault = true; g.def = {1, 42};
  uint32_t now = 0;
  g.begin(now);
  g.run(now, g.t.connectTimeoutMs + 100);
  CHECK(g.pol.portalOpen());
  CHECK_EQ(g.autoPortals, 1u);
  CHECK_EQ(g.radioCh, g.espnowCh);  // la radio vuelve al canal de ESP-NOW tras el barrido
}

TEST(wifi_wrong_default_password_cannot_block_portal) {
  Hotspot hs;
  SimGateway g; g.hs = &hs;
  g.hasDefault = true; g.def = {1, 7};  // SSID presente, contrasena incorrecta
  uint32_t now = 0;
  g.begin(now);
  g.run(now, g.t.connectTimeoutMs + 100);
  CHECK(g.pol.portalOpen() && g.autoPortals == 1);
  CHECK(g.pol.phase() != WifiPhase::Connected);
  // El fallo definitivo (WL_CONNECT_FAILED) abre el portal antes del plazo completo.
  SimGateway h; h.hs = &hs; h.hasDefault = true; h.def = {1, 7};
  uint32_t t2 = 0;
  h.begin(t2);
  h.run(t2, SimGateway::kScanMs + h.t.failGraceMs + 100);
  CHECK(h.pol.portalOpen());
}

TEST(wifi_no_credentials_opens_portal_immediately) {
  Hotspot hs;
  SimGateway g; g.hs = &hs;
  uint32_t now = 0;
  g.begin(now);
  CHECK(g.pol.portalOpen() && g.autoPortals == 1);
  CHECK_STR(g.pol.searchStateName(), "portal");
}

TEST(wifi_portal_by_serial_keeps_connection_and_espnow_channel) {
  Hotspot hs; hs.channel = 9;
  SimGateway g; g.hs = &hs;
  g.hasStored = true; g.stored = {1, 42};
  uint32_t now = 0;
  g.begin(now);
  g.run(now, 5000);
  CHECK(g.staUp && g.espnowCh == 9);
  const WifiAction a = g.pol.requestPortal(now);
  CHECK(a.kind == WifiAction::OpenPortal && !a.automatic);
  g.apply(a, now);
  CHECK(g.pol.requestPortal(now).kind == WifiAction::None);  // ya abierto
  g.run(now, 60000);
  CHECK(g.staUp && g.pol.phase() == WifiPhase::Connected);  // sigue conectado
  CHECK_EQ(g.radioCh, 9);                                   // AP en el mismo canal que ESP-NOW
  CHECK_STR(gateway::wifiPhaseName(g.pol.phase(), g.pol.portalOpen()), "connected_portal_open");
}

TEST(wifi_portal_expires_after_180_s_and_retries_spaced) {
  Hotspot hs; hs.on = false;
  SimGateway g; g.hs = &hs;
  g.hasStored = true; g.stored = {1, 42};
  uint32_t now = 0;
  g.begin(now);
  while (!g.pol.portalOpen() && now < g.t.connectTimeoutMs + 100) { g.step(now); now += 10; }
  CHECK(g.pol.portalOpen());
  const uint32_t opened = now - 10;
  const int connectsBefore = g.count(WifiAction::Connect);
  g.run(now, 179000);
  CHECK(g.pol.portalOpen());
  CHECK_EQ(g.count(WifiAction::Connect), connectsBefore);  // sin barridos con el portal abierto
  g.run(now, 1200);
  CHECK(!g.pol.portalOpen() && g.closes == 1);
  CHECK(now - opened >= 180000 && now - opened < 181500);
  // Despues: reintentos espaciados, nunca otro portal automatico.
  g.run(now, 600000);
  CHECK_EQ(g.autoPortals, 1u);
  CHECK(g.fullConnects() >= 2 && g.fullConnects() <= 6);
  CHECK(g.scanMs < 6 * SimGateway::kScanMs + 100);
}

TEST(wifi_new_network_saved_in_portal_is_reused) {
  Hotspot hs; hs.on = false;
  SimGateway g; g.hs = &hs;
  g.hasStored = true; g.stored = {5, 5};  // red antigua que ya no existe
  uint32_t now = 0;
  g.begin(now);
  g.run(now, g.t.connectTimeoutMs + 100);
  CHECK(g.pol.portalOpen());
  hs.on = true;
  CHECK(!g.userSaves({1, 7}, now));  // contrasena incorrecta: no se guarda, portal abierto
  CHECK(g.pol.portalOpen() && g.stored.ssid == 5);
  CHECK(g.userSaves({1, 42}, now));
  CHECK(!g.pol.portalOpen() && g.pol.phase() == WifiPhase::Connected && g.pol.hasStored());
  // Perdida y retorno: se reconecta con la red guardada (y tras "reiniciar" tambien).
  hs.on = false;
  g.run(now, 20000);
  CHECK(g.pol.phase() != WifiPhase::Connected);
  hs.on = true;
  g.run(now, 60000);
  CHECK(g.pol.phase() == WifiPhase::Connected && g.pol.activeCred() == WifiCred::Stored);
  SimGateway r; r.hs = &hs; r.hasStored = true; r.stored = g.stored;
  uint32_t t2 = 0;
  r.begin(t2);
  r.run(t2, 5000);
  CHECK(r.pol.phase() == WifiPhase::Connected && r.portalOpens == 0);
}

// ---------------- Hotspot movil ----------------
TEST(hotspot_available_at_boot_adopts_its_channel) {
  Hotspot hs; hs.channel = 11;
  SimGateway g; g.hs = &hs; g.espnowCh = 3; g.radioCh = 3;  // pista: canal guardado 3
  g.hasStored = true; g.stored = {1, 42};
  uint32_t now = 0;
  g.begin(now);
  g.run(now, 5000);
  CHECK(g.staUp && g.espnowCh == 11 && g.radioCh == 11 && g.channelChanges == 1);
  uint32_t since = 99;
  CHECK(g.pol.msSinceConnected(now, since) && since == 0);
}

TEST(hotspot_off_bounded_searches_and_no_restart) {
  Hotspot hs;
  SimGateway g; g.hs = &hs;
  g.hasStored = true; g.stored = {1, 42};
  uint32_t now = 0;
  g.begin(now);
  g.run(now, 5000);
  const uint32_t lost = now;
  hs.on = false;
  // Primera busqueda completa entre 30 y 60 s despues de la perdida.
  uint32_t firstFull = 0;
  const int fullBefore = g.fullConnects();
  for (; now - lost < 120000 && !firstFull; now += 10) {
    g.step(now);
    if (g.fullConnects() > fullBefore) firstFull = now - lost;
  }
  CHECK(firstFull >= 20000 && firstFull <= g.t.firstFullAfterLossMs);
  CHECK_EQ(g.radioCh == 0 ? g.espnowCh : g.radioCh, g.espnowCh);
  // 30 minutos sin hotspot: busquedas espaciadas, nunca continuas; ESP-NOW en su canal.
  const uint32_t scanBefore = g.scanMs;
  g.run(now, 1800000);
  const int fulls = g.fullConnects() - fullBefore;
  CHECK(fulls >= 4 && fulls <= 9);                        // 60,120,240,480,600,600... s
  CHECK(g.scanMs - scanBefore <= uint32_t(fulls) * SimGateway::kScanMs);
  CHECK(g.scanMs - scanBefore < 1800000 / 100);           // < 1 % del tiempo fuera de canal
  CHECK_EQ(g.radioCh, g.espnowCh);
  CHECK_EQ(g.autoPortals, 0u);                            // ya habia conectado: sin portal
  uint32_t since = 0;
  CHECK(g.pol.msSinceConnected(now, since) && since >= 1800000);
}

TEST(hotspot_returns_same_channel_and_bssid_quick_reconnect) {
  Hotspot hs;
  SimGateway g; g.hs = &hs; g.espnowCh = g.radioCh = 6;  // pista = canal anterior del hotspot
  g.hasStored = true; g.stored = {1, 42};
  uint32_t now = 0;
  g.begin(now);
  g.run(now, 5000);
  hs.on = false;
  g.run(now, 3000);
  hs.on = true;
  const int fullBefore = g.fullConnects();
  g.run(now, 15000);
  CHECK(g.staUp && g.pol.phase() == WifiPhase::Connected);
  CHECK_EQ(g.fullConnects(), fullBefore);  // rapido: sin barrido
  CHECK_EQ(g.channelChanges, 0u);
}

TEST(hotspot_same_ssid_different_channel) {
  Hotspot hs; hs.channel = 6;
  SimGateway g; g.hs = &hs; g.espnowCh = g.radioCh = 6;
  g.hasStored = true; g.stored = {1, 42};
  uint32_t now = 0;
  g.begin(now);
  g.run(now, 5000);
  CHECK(g.espnowCh == 6 && g.channelChanges == 0);
  hs.on = false;
  g.run(now, 2000);
  hs.on = true; hs.channel = 1;  // el hotspot reinicio en otro canal
  const uint32_t back = now;
  while (!g.staUp && now - back < 120000) { g.step(now); now += 10; }
  CHECK(g.staUp);
  CHECK(now - back <= g.t.firstFullAfterLossMs + g.t.connectTimeoutMs);
  CHECK(g.espnowCh == 1 && g.radioCh == 1 && g.channelChanges == 1);
}

TEST(hotspot_same_ssid_different_bssid) {
  Hotspot hs; hs.bssid = 0xA1;
  SimGateway g; g.hs = &hs; g.espnowCh = g.radioCh = 6;
  g.hasStored = true; g.stored = {1, 42};
  uint32_t now = 0;
  g.begin(now);
  g.run(now, 5000);
  hs.on = false;
  g.run(now, 2000);
  hs.on = true; hs.bssid = 0xB2;  // mismo canal, BSSID nuevo
  const uint32_t back = now;
  while (!g.staUp && now - back < 120000) { g.step(now); now += 10; }
  CHECK(g.staUp && g.knownBssid == 0xB2);
  CHECK(now - back <= g.t.firstFullAfterLossMs + g.t.connectTimeoutMs);
  CHECK_EQ(g.channelChanges, 0u);
  // Solo WIFI_QUICK_BEFORE_FULL intentos rapidos antes de buscar el SSID.
  int quick = 0;
  for (const auto& a : g.log) quick += a.kind == WifiAction::Connect && !a.fullScan;
  CHECK(quick <= g.t.quickBeforeFull);
}

TEST(hotspot_portal_opens_after_boot_failure_and_by_serial) {
  Hotspot hs; hs.on = false;
  SimGateway g; g.hs = &hs;
  g.hasStored = true; g.stored = {1, 42};
  uint32_t now = 0;
  g.begin(now);
  g.run(now, g.t.connectTimeoutMs + 100);
  CHECK(g.pol.portalOpen() && g.autoPortals == 1);
  g.run(now, 181000);
  CHECK(!g.pol.portalOpen());
  g.apply(g.pol.requestPortal(now), now);  // {"action":"wifi_portal"}
  CHECK(g.pol.portalOpen() && g.portalOpens == 2 && g.autoPortals == 1);
  CHECK_EQ(g.radioCh, g.espnowCh);
  // El hotspot vuelve con el portal abierto: se conecta al cerrarlo (sin barrer antes).
  hs.on = true;
  const int fullBefore = g.fullConnects();
  g.run(now, 170000);
  CHECK_EQ(g.fullConnects(), fullBefore);
  g.run(now, 40000);
  CHECK(g.staUp && !g.pol.portalOpen());
}

// ---------------- ESP-NOW durante la perdida de Internet ----------------
namespace {
struct Air;
struct AirRadio : RadioPort {
  Air* air;
  const uint8_t* channel;
  bool transmit(uint8_t dst, const uint8_t* frame, size_t len) override;
};
struct Air {
  struct P { uint8_t dst, ch; std::vector<uint8_t> b; };
  std::deque<P> q;
};
bool AirRadio::transmit(uint8_t dst, const uint8_t* frame, size_t len) {
  if (*channel == 0) return true;  // barriendo: la trama se pierde en el aire
  air->q.push_back({dst, *channel, std::vector<uint8_t>(frame, frame + len)});
  return true;
}
struct NodeChannel : ChannelControl {
  uint8_t ch = 1, persisted = 0;
  bool setChannel(uint8_t c) override { ch = c; return true; }
  uint8_t channel() const override { return ch; }
  void persistChannel(uint8_t c) override { persisted = c; }
};
struct QuietApp : NodeApp {
  uint8_t onGatewayMessage(const FrameHeader&, const uint8_t*, size_t) override { return kAckAccepted; }
  void onGatewaySession(uint32_t) override {}
  void fillStatus(StatusMsg&) override {}
};
struct CountingGw : EndpointHandler {
  uint32_t telemetry = 0;
  uint8_t onMessage(uint8_t, const FrameHeader& h, const uint8_t*, size_t) override {
    if (h.type == MsgType::Telemetry) telemetry++;
    return kAckAccepted;
  }
};
uint32_t wlcg = 11;
uint32_t wrnd() { wlcg = wlcg * 1664525u + 1013904223u; return wlcg >> 8; }

// Gateway (politica + Endpoint) y nodo A (NodeLink) sobre un aire con canales.
struct Bench {
  Hotspot hs;
  SimGateway g;
  Air air;
  AirRadio gwRadio, nodeRadio;
  NodeChannel nodeCh;
  CountingGw gh;
  QuietApp app;
  Endpoint gw;
  NodeLink* link = nullptr;
  Endpoint* ep = nullptr;
  alignas(Endpoint) uint8_t epStorage[sizeof(Endpoint)];
  alignas(NodeLink) uint8_t linkStorage[sizeof(NodeLink)];
  uint32_t sent = 0, lastSend = 0;

  Bench() : gw(0x55, kNodeGateway, 0x3333, gwRadio, gh, wrnd) {
    uint8_t key[kAppKeySize];
    for (uint8_t i = 0; i < kAppKeySize; ++i) key[i] = uint8_t(i * 3 + 1);
    g.hs = &hs;
    g.hasStored = true; g.stored = {1, 42};
    gwRadio.air = &air; gwRadio.channel = &g.radioCh;
    nodeRadio.air = &air; nodeRadio.channel = &nodeCh.ch;
    gw.addPeer(kNodeA, key);
    NodeLinkConfig cfg;
    ep = reinterpret_cast<Endpoint*>(epStorage);
    link = new (linkStorage) NodeLink(*ep, nodeCh, app, cfg);
    new (epStorage) Endpoint(0x55, kNodeA, 0x4444, nodeRadio, *link, wrnd);
    ep->addPeer(kNodeGateway, key);
  }
  ~Bench() {
    ep->~Endpoint();
    link->~NodeLink();
  }
  void step(uint32_t now) {
    g.step(now);
    link->loop(now);
    gw.poll(now);
    ep->poll(now);
    if (link->online(now) && now - lastSend >= 5000) {
      lastSend = now;
      uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
      SendOptions o;
      o.ttlMs = 600000; o.persistent = true;
      if (ep->sendReliable(kNodeGateway, MsgType::Telemetry, payload, sizeof(payload), o, now)) sent++;
    }
    std::deque<Air::P> batch;
    batch.swap(air.q);
    for (auto& p : batch) {
      const uint8_t rxCh = p.dst == kNodeGateway ? g.radioCh : nodeCh.ch;
      if (rxCh != p.ch) continue;
      if (p.dst == kNodeGateway) gw.receive(p.b.data(), p.b.size(), now, -50);
      else link->deliver(p.b.data(), p.b.size(), now, -50);
    }
  }
  void run(uint32_t& now, uint32_t ms) {
    for (uint32_t end = now + ms; int32_t(now - end) < 0; now += 10) step(now);
  }
};
}  // namespace

TEST(espnow_survives_internet_loss_and_follows_channel_change) {
  Bench b;
  b.hs.channel = 6;
  b.nodeCh.ch = 6;
  b.g.espnowCh = b.g.radioCh = 6;
  uint32_t now = 1000;
  b.link->begin(6, now, kResetPowerOn, true, true);
  b.g.begin(now);
  b.run(now, 30000);
  CHECK(b.g.staUp && b.link->online(now));
  // Hotspot apagado 15 min: Internet perdido; C sigue en el canal 6 para ESP-NOW.
  b.hs.on = false;
  const uint32_t sentBefore = b.sent, rxBefore = b.gh.telemetry;
  const uint16_t huntsBefore = b.link->hunts();
  uint32_t offlineMs = 0;
  for (uint32_t end = now + 900000; int32_t(now - end) < 0; now += 10) {
    b.step(now);
    if (!b.link->online(now)) offlineMs += 10;
  }
  const uint32_t sentDuring = b.sent - sentBefore;
  CHECK(sentDuring >= 170);                                 // un envio cada 5 s
  CHECK(b.gh.telemetry - rxBefore + 2 >= sentDuring);       // entregados (reintentos cubren barridos)
  CHECK_EQ(b.link->hunts(), huntsBefore);                   // A no perdio a C: mismo canal
  CHECK(offlineMs == 0);
  CHECK_EQ(b.nodeCh.ch, 6);
  CHECK(b.g.fullConnects() <= 10);                          // busquedas acotadas
  // El hotspot vuelve en otro canal: C lo adopta y A lo encuentra barriendo.
  b.hs.on = true; b.hs.channel = 11;
  const uint32_t back = now;
  bool relocked = false;
  for (uint32_t end = now + 180000; int32_t(now - end) < 0 && !relocked; now += 10) {
    b.step(now);
    relocked = b.g.staUp && b.nodeCh.ch == 11 && b.link->online(now) &&
               b.link->channelState() == NodeLink::ChannelState::Locked;
  }
  CHECK(relocked);
  CHECK(b.g.espnowCh == 11 && b.g.channelChanges == 1);
  CHECK_EQ(b.nodeCh.persisted, 11);
  // Busqueda de C (<= 45 s + intento) + timeout de enlace de A (20 s) + barrido.
  CHECK(now - back <= 45000 + 15000 + 20000 + 13 * 300 + 35000);
  const uint32_t rx = b.gh.telemetry;
  b.run(now, 30000);
  CHECK(b.gh.telemetry > rx);  // telemetria de A de nuevo en el canal 11
}
