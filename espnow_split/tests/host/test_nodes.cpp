// Pruebas de logica de nodos A y B: ecuaciones v4, calibraciones (3), importacion,
// O2/warm-up (4) y recuperacion de canal.
#include <ArduinoJson.h>

#include <deque>

#include "BioIoTCommon.h"
#include "CalibrationLogicA.h"
#include "CalibrationLogicB.h"
#include "O2Logic.h"
#include "SensorMathA.h"
#include "WarmupPolicy.h"
#include "test_framework.h"

using namespace bioiot;

// ---------------- Ecuaciones conservadas ----------------
TEST(node_a_equations_match_v4) {
  NodeACal cal;
  CHECK_NEAR(nodea::calculatePh(cal, 1.1f), -6.112459f * 1.1f + 15.012668f, 1e-6);
  CHECK(isnan(nodea::calculatePh(cal, 3.0f)));  // fuera de 0..14 => null, nunca 0
  CHECK(isnan(nodea::calculateTurbidityNtu(cal, 3.0f)));
  CHECK_NEAR(nodea::calculateTurbidityNtu(cal, 2.0f), -2310.5f * 2 + 5435.4f, 1e-3);
  CHECK_NEAR(nodea::calculateDoSaturationPct(cal, 0.5f), 0.13351912f * 500.0f, 1e-4);
  CHECK_NEAR(nodea::calculateDoSaturationPct(cal, 3.0f), 150.0f, 0);  // limitado como v4
  CHECK(isnan(nodea::calculateDoMgL(cal, 0.5f, NAN)));  // sin temperatura => null
  const float sat = nodea::doSaturationConcentrationMgL(25.0f);
  CHECK_NEAR(sat, 14.652f - 0.41022f * 25 + 0.0079910f * 625 - 0.000077774f * 15625, 1e-4);
  CHECK_NEAR(nodea::calibrateTemperature(cal, 20.0f), 0.9741f * 20 + 1.0038f, 1e-5);
  CHECK_NEAR(nodea::calculateTdsPpm(1.0f), (133.42f - 255.86f + 857.39f) * 0.5f, 1e-3);
  CHECK_NEAR(nodea::adcToVoltage(4095), 3.3f, 1e-6);
  CHECK(!nodea::analogConnected(5) && !nodea::analogConnected(4090) && nodea::analogConnected(6));
  CHECK(!nodea::ds18Connected(-127.0f) && nodea::ds18Connected(25.0f));
  float h, s, l;
  nodea::rgbToHsl(255, 0, 0, h, s, l);
  CHECK_NEAR(h, 0, 1e-6); CHECK_NEAR(s, 1, 1e-6); CHECK_NEAR(l, 0.5, 1e-6);
  CHECK_EQ(nodea::analogQuality(false, true), kQDisconnected);
  CHECK_EQ(nodea::analogQuality(true, false), kQOutOfRange);
}

TEST(color_assumed_references_and_screenshot) {
  CHECK_EQ(nodea::pulseToIntensity(0), 0);       // timeout no se interpreta como blanco
  CHECK_EQ(nodea::pulseToIntensity(1), 0);
  CHECK_EQ(nodea::pulseToIntensity(15), 0);      // negro asumido
  CHECK_EQ(nodea::pulseToIntensity(200), 255);   // blanco asumido
  CHECK_EQ(nodea::pulseToIntensity(1000), 255);  // limita extremos validos
  CHECK_EQ(nodea::pulseToIntensity(30000), 0);
  CHECK_EQ(nodea::pulseToIntensity(23), 11);
  CHECK_EQ(nodea::pulseToIntensity(22), 10);
  CHECK_EQ(nodea::pulseToIntensity(16), 1);
  CHECK_EQ(nodea::pulseToIntensity(142), 175);
  CHECK_EQ(nodea::pulseToIntensity(192), 244);
  CHECK_EQ(nodea::pulseToIntensity(153), 190);
  float h, s, l;
  nodea::rgbToHsl(0, 0, 0, h, s, l);
  CHECK_NEAR(h, 0, 0); CHECK_NEAR(s, 0, 0); CHECK_NEAR(l, 0, 0);
  nodea::rgbToHsl(255, 255, 255, h, s, l);
  CHECK_NEAR(h, 0, 0); CHECK_NEAR(s, 0, 0); CHECK_NEAR(l, 1, 0);
}

TEST(co2_model_is_the_v4_header_verbatim) {
  // La copia de la biblioteca debe ser identica a CalibrationModel.h de v4. Se ignora
  // '\r': con core.autocrlf=true, un checkout puede dejar CRLF en una copia y LF en otra.
  FILE* a = fopen("../CalibrationModel.h", "rb");
  FILE* b = fopen("libraries/BioIoTCommon/src/bioiot_calibration_model.h", "rb");
  CHECK(a && b);
  if (a && b) {
    auto next = [](FILE* f) { int c; do c = fgetc(f); while (c == '\r'); return c; };
    int ca, cb, same = 1;
    do { ca = next(a); cb = next(b); if (ca != cb) same = 0; } while (ca != EOF && cb != EOF);
    CHECK(same);
  }
  if (a) fclose(a);
  if (b) fclose(b);
}

// ---------------- Calibracion nodo A ----------------
static CalibrationCmd calCmd(uint8_t op, uint8_t target) {
  CalibrationCmd c;
  c.op = op; c.target = target; c.cmdId = 1;
  return c;
}

TEST(calibration_a_set_reset_and_rollback) {
  NodeACal cur, cand;
  CalibrationCmd c = calCmd(kCalOpSet, kTargetPh);
  c.present = 3; c.params[calp::kM] = -6.0f; c.params[calp::kB] = 15.0f;
  auto r = nodea::applyCalibrationA(cur, c, true, cand);
  CHECK(r.outcome == kOutcomeApplied && r.status == kStSaved && r.needsPersist);
  CHECK(cand.ph_m == -6.0f && cand.ph_user == 1 && cand.rev[kCalPh] == 1 && cand.nodeRevision == 1);
  CHECK(cur.ph_m == kPhCalMDefault);  // la activa no cambia hasta persistir
  cur = cand;
  c = calCmd(kCalOpReset, kTargetPh);
  r = nodea::applyCalibrationA(cur, c, true, cand);
  CHECK(cand.ph_m == kPhCalMDefault && cand.ph_user == 0 && cand.rev[kCalPh] == 2);
  // DO solo admite m (v4 ignora b).
  c = calCmd(kCalOpSet, kTargetDo);
  c.present = 2; c.params[calp::kB] = 1;
  r = nodea::applyCalibrationA(cur, c, true, cand);
  CHECK(r.status == kStRejectedInvalidParameters);
  CHECK(cand.do_m == cur.do_m);
  // Almacenamiento con formato futuro: no se acepta nada.
  c = calCmd(kCalOpSet, kTargetPh);
  c.present = 1; c.params[0] = -6;
  r = nodea::applyCalibrationA(cur, c, false, cand);
  CHECK(r.status == kStRejectedBusyOrFutureSchema && !r.needsPersist);
}

TEST(calibration_a_co2_profiles_like_v4) {
  NodeACal cur, cand;
  CalibrationCmd c = calCmd(kCalOpSet, kTargetCo2Both);
  c.present = (1u << calp::kA) | (1u << calp::kLegacyB);
  c.params[calp::kA] = 12.34f; c.params[calp::kLegacyB] = -0.5f;
  auto r = nodea::applyCalibrationA(cur, c, true, cand);
  CHECK(r.status == kStSaved);
  CHECK(cand.co2[0].mode == 1 && cand.co2[1].mode == 1 && cand.co2[0].user == 1 && cand.co2[0].provisional == 0);
  CHECK(cand.co2_a == 12.34f && cand.co2_b == -0.5f && cand.co2_enabled == 1);  // instantanea legacy
  // Mezclar legacy y vendor => rechazo completo.
  c.present |= 1u << calp::kZero;
  c.params[calp::kZero] = 0.2f;
  r = nodea::applyCalibrationA(cur, c, true, cand);
  CHECK(r.status == kStRejectedInvalidParameters);
  CHECK(cand.co2[0].mode == 0);
  // Solo "enabled".
  c = calCmd(kCalOpSet, kTargetCo2_2);
  c.enabledPresent = 1; c.enabledValue = 0;
  r = nodea::applyCalibrationA(cur, c, true, cand);
  CHECK(r.status == kStSaved && cand.co2[1].enabled == 0 && cand.co2[0].enabled == 1);
  // Sin parametros ni enabled => invalido.
  c = calCmd(kCalOpSet, kTargetCo2_1);
  r = nodea::applyCalibrationA(cur, c, true, cand);
  CHECK(r.status == kStRejectedInvalidParameters);
}

TEST(calibration_a_reset_all_and_import_policy) {
  NodeACal cur;
  cur.ph_m = -5.9f; cur.ph_user = 1; cur.legacyVersion = 12;
  NodeACal cand;
  auto r = nodea::applyCalibrationA(cur, calCmd(kCalOpResetAll, kTargetAll), true, cand);
  CHECK(r.status == kStSaved);
  CHECK(cand.ph_m == kPhCalMDefault && !cand.ph_user && cand.legacyVersion == 12);
  // Importacion sobre datos de usuario requiere force.
  CalibrationCmd imp = calCmd(kCalOpImport, kTargetAll);
  NodeACal fromV4;
  fromV4.ph_m = -6.2f; fromV4.ph_user = 1;
  imp.blobLen = uint8_t(encodeNodeACal(fromV4, imp.blob, sizeof(imp.blob)));
  r = nodea::applyCalibrationA(cur, imp, true, cand);
  CHECK(r.status == kStRejectedImportNotAllowed);
  imp.force = 1;
  r = nodea::applyCalibrationA(cur, imp, true, cand);
  CHECK(r.status == kStImported && cand.ph_m == -6.2f && cand.importedMask == 0x3F);
  CHECK(cand.nodeRevision == cur.nodeRevision + 1);
  NodeACal fresh, cand2;
  imp.force = 0;
  r = nodea::applyCalibrationA(fresh, imp, true, cand2);  // placa nueva: se permite
  CHECK(r.status == kStImported);
}

// Exportacion v4 tal como la emite tools/BioIoT_CalibrationExport (y el gateway).
static const char* kV4Export = R"({"format":"bioiot-calibration-export","format_version":1,
 "source":"v4_integrated_nvs","device_id":"esp32-bioiot-01","exported_at_utc":null,
 "legacy":{"schema":2,"ver":7},
 "node_a":{"ph":{"m":-6.1125,"b":15.013,"user":true},"turb":{"m":-2310.5,"b":5435.4},
   "do":{"m":0.13351912,"user":false},"temp":{"m":0.9741,"b":1.0038},
   "co2_legacy":{"a":0,"b":0,"enabled":false},
   "co2_1":{"zero_point_v":0.22,"reaction_voltage_v":0.03,"a":0,"b":0,"mode":0,"user":false,"enabled":true,"provisional":true},
   "co2_2":{"zero_point_v":0.31,"reaction_voltage_v":0.025,"a":0,"b":0,"mode":0,"user":true,"enabled":true,"provisional":false}},
 "node_b":{"o2_gas_1":{"gain":1.02,"offset":-0.1,"reference":20.9,"command_utc":1790000000,"command_count":2},
   "o2_gas_2":{"gain":1,"offset":0,"reference":0,"command_utc":0,"command_count":0}}})";

TEST(calibration_import_preserves_user_values_and_flags) {
  JsonDocument doc;
  CHECK(!deserializeJson(doc, kV4Export));
  NodeACal a;
  NodeBCal b;
  const CalImportResult r = parseCalibrationExport(doc.as<JsonObjectConst>(), a, b);
  CHECK(r.formatOk && r.aValid && r.bValid);
  // Decimal sin patron _f32: ArduinoJson puede diferir 1 ulp del float exacto.
  CHECK_NEAR(a.ph_m, -6.1125, 1e-6);
  CHECK_NEAR(a.ph_b, 15.013, 2e-6);
  CHECK(a.ph_user == 1);
  CHECK(a.co2[1].zero_point_v == 0.31f && a.co2[1].user == 1 && a.co2[1].provisional == 0);
  CHECK(a.legacyVersion == 7 && a.legacySchema == 2 && a.importedMask == 0x3F);
  CHECK(b.o2[0].gain == 1.02f && b.o2[0].commandUtc == 1790000000 && b.o2[0].commandCount == 2);
  // Ida y vuelta por el exportador: ningun valor cambia (precision float32).
  JsonDocument out;
  writeCalibrationExport(out.to<JsonObject>(), "test", "dev", "", &a, &b);
  std::string text;
  serializeJson(out, text);
  JsonDocument back;
  deserializeJson(back, text);
  NodeACal a2;
  NodeBCal b2;
  const CalImportResult r2 = parseCalibrationExport(back.as<JsonObjectConst>(), a2, b2);
  CHECK(r2.aValid && r2.bValid);
  // Con _f32 la ida y vuelta es bit a bit (incluidas curvas iniciales con 8 cifras).
  CHECK(a2.ph_m == a.ph_m && a2.ph_b == a.ph_b && a2.do_m == a.do_m && a2.temp_b == a.temp_b);
  CHECK(a2.co2[1].reaction_voltage_v == a.co2[1].reaction_voltage_v);
  CHECK(b2.o2[0].offset == b.o2[0].offset && b2.o2[0].gain == b.o2[0].gain);
  NodeACal defaults, d2;
  NodeBCal unusedB;
  JsonDocument dd;
  writeCalibrationExport(dd.to<JsonObject>(), "t", "d", "", &defaults, nullptr);
  std::string dtext;
  serializeJson(dd, dtext);
  JsonDocument dback;
  deserializeJson(dback, dtext);
  CHECK(parseCalibrationExport(dback.as<JsonObjectConst>(), d2, unusedB).aValid);
  CHECK(d2.ph_m == kPhCalMDefault && d2.ph_b == kPhCalBDefault && d2.do_m == kDoCalMDefault);
  // Un patron _f32 que no corresponde al decimal se rechaza.
  dback["node_a"]["ph"]["m_f32"] = "3f800000";  // 1.0
  NodeACal d3;
  CHECK(!parseCalibrationExport(dback.as<JsonObjectConst>(), d3, unusedB).aValid);
  // Un valor invalido rechaza la seccion entera (no se importa a medias).
  doc["node_a"]["ph"]["m"] = "abc";
  NodeACal a3;
  NodeBCal b3;
  const CalImportResult r3 = parseCalibrationExport(doc.as<JsonObjectConst>(), a3, b3);
  CHECK(r3.hasA && !r3.aValid && a3.ph_m == kPhCalMDefault);
  doc["format_version"] = 2;
  CHECK(!parseCalibrationExport(doc.as<JsonObjectConst>(), a3, b3).formatOk);
}

TEST(calibration_persistence_failure_keeps_active_values) {
  // Flujo del nodo: candidata -> guardar -> activar. Si guardar falla, nada cambia.
  struct FailBackend : SlotBackend {
    size_t readSlot(uint8_t, uint8_t*, size_t) override { return 0; }
    bool writeSlot(uint8_t, const uint8_t*, size_t) override { return false; }
  } be;
  RecordStore store(be, 1);
  uint8_t tmp[8];
  store.load(tmp, sizeof(tmp));
  NodeACal active, cand;
  CalibrationCmd c = calCmd(kCalOpSet, kTargetPh);
  c.present = 1; c.params[0] = -7.0f;
  auto r = nodea::applyCalibrationA(active, c, true, cand);
  uint8_t blob[kNodeACalEncodedSize];
  encodeNodeACal(cand, blob, sizeof(blob));
  const bool saved = r.needsPersist && store.save(blob, sizeof(blob));
  if (saved) active = cand;
  CHECK(!saved);
  CHECK(active.ph_m == kPhCalMDefault);  // nunca se reporta aplicada sin guardar
}

// ---------------- Nodo B ----------------
TEST(calibration_b_rules_from_v4) {
  NodeBCal cur, cand;
  CalibrationCmd c = calCmd(kCalOpSet, kTargetO2Gas1);
  c.present = 1; c.params[calp::kGain] = 0;
  auto r = nodeb::applyCalibrationB(cur, c, true, false, false, cand);
  CHECK(r.status == kStRejectedInvalidParameters);
  c.params[calp::kGain] = 1.05f;
  r = nodeb::applyCalibrationB(cur, c, true, false, false, cand);
  CHECK(r.status == kStSaved && cand.o2[0].gain == 1.05f && r.resetFilters == 1);
  CalibrationCmd air = calCmd(kCalOpO2Air, kTargetO2Gas2);
  air.present = 1; air.params[0] = 20.8f;
  CHECK(nodeb::applyCalibrationB(cur, air, true, false, false, cand).status == kStRejectedReferenceUse209);
  air.params[0] = 20.9f;
  CHECK(nodeb::applyCalibrationB(cur, air, true, true, false, cand).status == kStRejectedWarmingUpOrBusy);
  auto ok = nodeb::applyCalibrationB(cur, air, true, false, false, cand);
  CHECK(ok.startAirCalibration && ok.airSensor == 1);
  NodeBCal rec = cur;
  nodeb::recordAirCalibration(rec, 1, 0);
  CHECK(rec.o2[1].commandCount == 1 && rec.o2[1].commandUtc == 0 && rec.o2[1].reference == 20.9f);
  // reset_all conserva referencia/fecha/conteo de la calibracion interna.
  rec.o2[1].gain = 1.3f;
  r = nodeb::applyCalibrationB(rec, calCmd(kCalOpResetAll, kTargetAll), true, false, false, cand);
  CHECK(cand.o2[1].gain == 1 && cand.o2[1].commandCount == 1 && cand.o2[1].reference == 20.9f);
}

TEST(o2_warmup_disconnection_and_invalid_are_distinct) {
  O2Calibration cal;
  O2Filter filter;
  O2Reading r;
  nodeb::O2Transport t;
  t.selected = true; t.probeOk = true; t.warming = true;
  CHECK(isnan(nodeb::evaluateO2(t, cal, filter, r, 1000)));
  CHECK_STR(r.quality, "warming_up");
  CHECK(r.connected && r.i2cDetected && !r.valid);
  t.warming = false; t.probeOk = false; t.probeError = 2;
  nodeb::evaluateO2(t, cal, filter, r, 2000);
  CHECK_STR(r.quality, "communication_fault");
  CHECK(!r.connected && r.wireError == 2);
  t.probeOk = true; t.beginOk = true; t.commOk = true; t.raw = 31;
  nodeb::evaluateO2(t, cal, filter, r, 3000);
  CHECK_STR(r.quality, "out_of_range");
  CHECK(r.connected && !r.valid && isnan(r.value));
  t.raw = 20.9f;
  for (int i = 0; i < O2_FILTER_SAMPLES - 1; ++i) {
    nodeb::evaluateO2(t, cal, filter, r, 4000 + i);
    CHECK_STR(r.quality, "stabilizing");
  }
  nodeb::evaluateO2(t, cal, filter, r, 5000);
  CHECK_STR(r.quality, "good");
  CHECK_NEAR(r.value, 20.9, 1e-4);
  t.raw = 26;
  nodeb::evaluateO2(t, cal, filter, r, 6000);
  CHECK_STR(r.quality, "above_nominal_range");
}

TEST(warmup_belongs_to_power_not_to_link) {
  nodeb::WarmupPolicy w;
  w.begin(180000, nodeb::ResetKind::PowerOn, false, 0, true, 1000);
  CHECK(w.warming(1000) && w.remainingMs(1000) == 180000);
  CHECK(!w.warming(181000));
  // Reinicio por software con RTC valida: descuenta el tiempo alimentado.
  w.begin(180000, nodeb::ResetKind::SoftOrWatchdog, true, 100000, true, 0);
  CHECK_EQ(w.remainingMs(0), 80000u);
  CHECK(!w.assumedFull());
  // Sin evidencia (RTC invalida o alimentacion independiente): warm-up completo conservador.
  w.begin(180000, nodeb::ResetKind::SoftOrWatchdog, false, 0, true, 0);
  CHECK(w.assumedFull() && w.remainingMs(0) == 180000);
  w.begin(180000, nodeb::ResetKind::SoftOrWatchdog, true, 100000, false, 0);
  CHECK(w.assumedFull() && w.remainingMs(0) == 180000);
  // Latch: tras terminar no se repite aunque millis() desborde.
  w.begin(180000, nodeb::ResetKind::PowerOn, false, 0, true, 0xFFFF0000u);
  CHECK(!w.warming(0xFFFF0000u + 180000u));
  CHECK(!w.warming(0x00001000u));
  CHECK(w.poweredLowerBoundMs(0xFFFF0000u + 1000u) == 1000u);
}

// ---------------- Recuperacion de canal ----------------
namespace {
struct ChanNet;
struct ChanRadio : RadioPort {
  ChanNet* net;
  uint8_t* channel;
  bool transmit(uint8_t dst, const uint8_t* frame, size_t len) override;
};
struct ChanNet {
  struct P { uint8_t dst, ch; std::vector<uint8_t> b; };
  std::deque<P> q;
};
bool ChanRadio::transmit(uint8_t dst, const uint8_t* frame, size_t len) {
  net->q.push_back({dst, *channel, std::vector<uint8_t>(frame, frame + len)});
  return true;
}
struct FakeChannel : ChannelControl {
  uint8_t ch = 1, persisted = 0;
  int sets = 0;
  bool setChannel(uint8_t c) override { ch = c; sets++; return true; }
  uint8_t channel() const override { return ch; }
  void persistChannel(uint8_t c) override { persisted = c; }
};
struct NullApp : NodeApp {
  int sessions = 0;
  uint8_t onGatewayMessage(const FrameHeader&, const uint8_t*, size_t) override { return kAckAccepted; }
  void onGatewaySession(uint32_t) override { sessions++; }
  void fillStatus(StatusMsg&) override {}
};
struct GwHandler : EndpointHandler {
  uint8_t onMessage(uint8_t, const FrameHeader&, const uint8_t*, size_t) override { return kAckAccepted; }
};
uint32_t lcg = 7;
uint32_t rnd() { lcg = lcg * 1664525u + 1013904223u; return lcg >> 8; }
}  // namespace

TEST(node_hunts_gateway_channel_and_persists_it) {
  uint8_t key[kAppKeySize];
  for (uint8_t i = 0; i < kAppKeySize; ++i) key[i] = uint8_t(i + 40);
  ChanNet net;
  uint8_t gwChannel = 6;
  FakeChannel nodeCh;
  ChanRadio gwRadio, nodeRadio;
  gwRadio.net = &net; gwRadio.channel = &gwChannel;
  nodeRadio.net = &net; nodeRadio.channel = &nodeCh.ch;
  GwHandler gh;
  Endpoint gw(0x77, kNodeGateway, 0x1111, gwRadio, gh, rnd);
  gw.addPeer(kNodeA, key);
  NullApp app;
  NodeLinkConfig cfg;
  alignas(Endpoint) static uint8_t epStorage[sizeof(Endpoint)];
  Endpoint* ep = reinterpret_cast<Endpoint*>(epStorage);
  NodeLink link(*ep, nodeCh, app, cfg);
  new (epStorage) Endpoint(0x77, kNodeA, 0x2222, nodeRadio, link, rnd);
  ep->addPeer(kNodeGateway, key);
  uint32_t now = 1000;
  link.begin(1, now, kResetPowerOn, true, true);
  bool everHunting = false;
  for (int i = 0; i < 4000 && !(link.online(now) && nodeCh.persisted); ++i) {
    now += 10;
    link.loop(now);
    gw.poll(now);
    everHunting |= link.channelState() == NodeLink::ChannelState::Hunting;
    std::deque<ChanNet::P> batch;
    batch.swap(net.q);
    for (auto& p : batch) {
      // Solo se recibe si ambos estan en el mismo canal.
      const uint8_t rxCh = p.dst == kNodeGateway ? gwChannel : nodeCh.ch;
      if (rxCh != p.ch) continue;
      if (p.dst == kNodeGateway) gw.receive(p.b.data(), p.b.size(), now, -40);
      else link.deliver(p.b.data(), p.b.size(), now, -40);
    }
  }
  CHECK(everHunting);
  CHECK(link.online(now));
  CHECK_EQ(nodeCh.ch, 6);
  CHECK_EQ(nodeCh.persisted, 6);
  CHECK(link.hunts() >= 1);
  CHECK(app.sessions >= 1);
  CHECK(now < 1000 + 20000 + 13 * 300 + 2000);  // timeout de enlace + un barrido acotado
}
