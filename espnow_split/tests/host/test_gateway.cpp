// Pruebas del gateway: actuadores sin cargas reales (6), datos vencidos/reloj (5),
// seguimiento de comandos (2), comandos cloud v4, JSON 1.1 y colas acotadas (7).
#include <ArduinoJson.h>

#include "ActuatorCore.h"
#include "CloudCommands.h"
#include "GatewayState.h"
#include "TelemetryJson.h"
#include "test_framework.h"

using namespace bioiot;
using namespace gw;

namespace {
struct RecordingDriver : OutputDriver {
  int compressorWrites = 0, ledWrites = 0;
  bool compressor = false;
  uint8_t duty[3] = {};
  void writeCompressor(bool on) override { compressorWrites++; compressor = on; }
  void writeLed(uint8_t r, uint8_t g, uint8_t b) override { ledWrites++; duty[0] = r; duty[1] = g; duty[2] = b; }
};
const int64_t kUtc = 1790000000;  // s

ActuatorCommand compressorOn(uint64_t expiresAt, const char* id = nullptr) {
  ActuatorCommand c;
  c.target = kActCompressor; c.on = true; c.hasExpiresAt = true; c.expiresAt = expiresAt;
  if (id) { c.hasCommandId = true; strncpy(c.commandId, id, 64); }
  return c;
}
}  // namespace

TEST(actuators_commissioning_never_drives_outputs) {
  RecordingDriver d;
  ActuatorCore core;
  ActuatorConfig cfg;  // commissioning=true por defecto
  core.begin(cfg, LocalProgram(), &d, 0);
  core.handle(compressorOn(kUtc + 300), 200000, true, kUtc);
  const ActuatorState s = core.state(200000);
  CHECK(s.compressorOn && s.commissioning && !s.outputsDriven);
  CHECK_EQ(d.compressorWrites, 0);
  CHECK_EQ(d.ledWrites, 0);
}

TEST(actuators_unconfirmed_electrical_locks_only_that_actuator) {
  RecordingDriver d;
  ActuatorCore core;
  ActuatorConfig cfg;
  cfg.commissioning = false; cfg.compressorConfirmed = false; cfg.ledConfirmed = true;
  core.begin(cfg, LocalProgram(), &d, 0);
  core.handle(compressorOn(kUtc + 300), 200000, true, kUtc);
  CHECK_STR(core.state(200000).lastReason, "electrical_config_unconfirmed");
  CHECK(!core.state(200000).compressorOn);
  ActuatorCommand off;
  off.target = kActCompressor; off.on = false;
  core.handle(off, 200001, true, kUtc);
  CHECK(core.state(200001).lastAccepted);  // OFF siempre permitido
  ActuatorCommand led;
  led.target = kActLed; led.r = 255; led.brightness = 128; led.hasExpiresAt = true; led.expiresAt = kUtc + 60;
  core.handle(led, 200002, true, kUtc);
  CHECK(core.state(200002).ledOn && d.duty[0] == 128);
  CHECK(core.state(200002).compressorLocked && !core.state(200002).ledLocked);
}

TEST(actuators_v4_protections_preserved) {
  RecordingDriver d;
  ActuatorCore core;
  ActuatorConfig cfg;
  cfg.commissioning = false; cfg.compressorConfirmed = cfg.ledConfirmed = true;
  core.begin(cfg, LocalProgram(), &d, 0);
  // Reposo minimo tambien desde el arranque.
  core.handle(compressorOn(kUtc + 300), 1000, true, kUtc);
  CHECK_STR(core.state(1000).lastReason, "compressor_min_off_time");
  // Sin hora valida o expiresAt vencido/excesivo => rechazo.
  core.handle(compressorOn(kUtc + 300), 200000, false, 0);
  CHECK_STR(core.state(200000).lastReason, "invalid_or_expired_lease");
  core.handle(compressorOn(kUtc - 1), 200000, true, kUtc);
  CHECK_STR(core.state(200000).lastReason, "invalid_or_expired_lease");
  core.handle(compressorOn(kUtc + 901), 200000, true, kUtc);
  CHECK_STR(core.state(200000).lastReason, "invalid_or_expired_lease");
  ActuatorCommand bad;
  bad.target = kActLed; bad.fieldError = "rgb_and_brightness_must_be_0_to_255";
  core.handle(bad, 200000, true, kUtc);
  CHECK_STR(core.state(200000).lastReason, "rgb_and_brightness_must_be_0_to_255");
  core.handle(compressorOn(kUtc + 300), 200000, true, kUtc);
  CHECK(core.state(200000).compressorOn && d.compressor);
  ActuatorCommand allOff;
  allOff.allOff = true;
  core.handle(allOff, 210000, true, kUtc);
  CHECK(!core.state(210000).compressorOn && !d.compressor);
  CHECK_STR(core.state(210000).lastStopReason, "all_off");
}

TEST(actuators_internet_loss_does_not_stop_lease_expires_monotonic) {
  RecordingDriver d;
  ActuatorCore core;
  ActuatorConfig cfg;
  cfg.commissioning = false; cfg.compressorConfirmed = true;
  core.begin(cfg, LocalProgram(), &d, 0);
  core.handle(compressorOn(kUtc + 300), 200000, true, kUtc);
  // Sin UTC ni nube durante 299 s (SAS, Wi-Fi caido): sigue encendido.
  for (uint32_t t = 200000; t < 200000 + 299000; t += 1000) core.service(t, false, 0);
  CHECK(core.state(200000 + 299000).compressorOn);
  core.service(200000 + 300000, false, 0);
  CHECK(!core.state(200000 + 300000).compressorOn);
  CHECK_STR(core.state(200000 + 300000).lastStopReason, "compressor_lease_expired");
}

TEST(actuators_duplicate_command_cannot_extend_or_revive) {
  RecordingDriver d;
  ActuatorCore core;
  ActuatorConfig cfg;
  cfg.commissioning = false; cfg.compressorConfirmed = true;
  core.begin(cfg, LocalProgram(), &d, 0);
  core.handle(compressorOn(kUtc + 60, "c-1"), 200000, true, kUtc);
  CHECK(core.state(200000).compressorOn);
  core.service(260000, true, kUtc + 60);
  CHECK(!core.state(260000).compressorOn);
  // Reentrega QoS1 tras reconectar: mismo commandId => ignorado.
  core.handle(compressorOn(kUtc + 200, "c-1"), 500000, true, kUtc + 100);
  CHECK(!core.state(500000).compressorOn);
  CHECK_STR(core.state(500000).lastReason, "duplicate_command_ignored");
}

TEST(actuators_local_program_disabled_without_parameters_manual_has_priority) {
  RecordingDriver d;
  ActuatorCore core;
  ActuatorConfig cfg;
  cfg.commissioning = false; cfg.ledConfirmed = true;
  LocalProgram p;
  p.enabled = true;  // habilitado pero sin parametros => invalido
  core.begin(cfg, p, &d, 0);
  core.service(1000, true, kUtc);
  CHECK(!core.state(1000).programValid && !core.state(1000).ledOn);
  // Programa sintetico solo para probar el arbitraje (no es una consigna real).
  p.lightOnMinuteUtc = 0; p.lightOffMinuteUtc = 1439; p.r = 255; p.brightness = 255;
  core.begin(cfg, p, &d, 0);
  core.service(1000, true, kUtc);
  CHECK(core.state(1000).ledOn);
  CHECK_STR(core.state(1000).ledSource, "local_program");
  ActuatorCommand off;
  off.target = kActLed;
  core.handle(off, 2000, true, kUtc);
  core.service(3000, true, kUtc);
  CHECK(!core.state(3000).ledOn);  // OFF manual suspende el programa
  CHECK_STR(core.state(3000).lastReason, "led_off");
  core.service(2000 + 900000, true, kUtc + 900);
  CHECK(core.state(2000 + 900000).ledOn);  // tras el maximo lease vuelve el programa
  // Sin UTC valida el programa no puede evaluar el fotoperiodo: apaga (documentado).
  core.service(2000 + 900001, false, 0);
  CHECK(!core.state(2000 + 900001).ledOn);
}

// ---------------- Estado del gateway ----------------
static TelemetryMsg phTelemetry(uint32_t txUptime, uint32_t sampleUptime, float ph) {
  TelemetryMsg t;
  t.snapshotId = 1; t.parts = 1; t.txUptimeMs = txUptime; t.recordCount = 1;
  TelemetryRecord& r = t.records[0];
  r.sensor = kSensorPh; r.quality = kQGood; r.flags = kRecConnected | kRecExpected | kRecObserved;
  r.sampleUptimeMs = sampleUptime; r.valueCount = 3;
  r.vals[0] = 1365; r.vals[1] = 1.1f; r.vals[2] = ph;
  return t;
}

static void initAges(GatewayState& st) {
  for (uint8_t i = 0; i < kSensorCount; ++i) st.maxAgeMs[i] = 20000;
}

TEST(gateway_sample_age_uses_node_clock_and_retransmission_is_not_fresh) {
  static GatewayState st;
  st = GatewayState();
  initAges(st);
  // Muestra tomada 15 s antes de su (re)transmision.
  st.touch(kNodeA, 100000, -50);
  CHECK_EQ(st.onTelemetry(kNodeA, phTelemetry(50000, 35000, 7.0f), 100000), 1);
  CHECK_EQ(st.latest(kSensorPh).sampleMonoMs, 85000);
  float v = 0;
  CHECK(st.controlInput(kSensorPh, 2, 100000, v) && v == 7.0f);
  // 10 s despues ya tiene 25 s: vencida, nunca entrada de control.
  CHECK(!st.controlInput(kSensorPh, 2, 110000, v));
  CHECK_EQ(st.classify(kSensorPh, st.latest(kSensorPh), 110000, 20000, 110000), kSampleStale);
  // Nodo sin enlace => node_offline.
  CHECK_EQ(st.classify(kSensorPh, st.latest(kSensorPh), 130000, 20000, 130000), kSampleNodeOffline);
  // Una muestra mas vieja no reemplaza a la mas nueva.
  st.onTelemetry(kNodeA, phTelemetry(60000, 30000, 6.0f), 101000);
  CHECK(st.latest(kSensorPh).vals[2] == 7.0f);
  // Un nodo no puede publicar sensores ajenos.
  TelemetryMsg foreign = phTelemetry(1, 1, 7);
  foreign.records[0].sensor = kSensorLight1;
  foreign.records[0].valueCount = 2;
  CHECK_EQ(st.onTelemetry(kNodeA, foreign, 101000), 0);
}

TEST(gateway_snapshots_bounded_backfill_and_overflow) {
  static GatewayState st;
  st = GatewayState();
  initAges(st);
  ActuatorState a;
  st.touch(kNodeA, 0, 0);
  Snapshot* s = st.takeSnapshot(1000, 0, a, 20000);
  CHECK(s && s->state[kSensorPh] == kSampleNever);
  const uint32_t seq = s->seq;
  // Llega despues una muestra de antes de la instantanea (cola del nodo): se rellena.
  st.onTelemetry(kNodeA, phTelemetry(5000, 4500, 7.2f), 1500);  // sampleMono = 1000
  CHECK_EQ(st.oldestUnpublished()->seq, seq);
  CHECK_EQ(st.oldestUnpublished()->state[kSensorPh], kSampleFresh);
  CHECK(st.oldestUnpublished()->sensors[kSensorPh].vals[2] == 7.2f);
  CHECK_EQ(st.node(kNodeA).backfilled, 1u);
  for (int i = 0; i < 100; ++i) st.takeSnapshot(2000 + i, 0, a, 20000);
  CHECK_EQ(st.unpublishedCount(), GatewayState::kSnapshots);
  CHECK_EQ(st.snapshotsDropped(), 101u - GatewayState::kSnapshots);
  CHECK(int32_t(st.oldestUnpublished()->seq - seq) > 0);  // se descarta lo mas antiguo
}

TEST(command_tracker_states_and_partial_group) {
  CommandTracker t;
  TrackedCommand* a = t.add(1, kNodeA, uint8_t(MsgType::Calibration), kCalOpResetAll, kTargetAll, "all", "x", true, 9, 0, 30000);
  TrackedCommand* b = t.add(2, kNodeB, uint8_t(MsgType::Calibration), kCalOpResetAll, kTargetAll, "all", "x", true, 9, 0, 30000);
  CHECK(a && b);
  t.onDelivery(1, Delivery::Delivered, 100);
  CHECK_EQ(t.find(1)->state, kCmdDelivered);
  CommandResultMsg r;
  r.cmdId = 1; r.outcome = kOutcomeApplied; r.status = kStSaved; r.storageOk = 1;
  t.onResult(kNodeA, r);
  CHECK_EQ(t.find(1)->state, kCmdApplied);
  t.onDelivery(2, Delivery::Failed, 30000);
  CHECK_EQ(t.find(2)->state, kCmdTimeout);
  uint8_t applied, total;
  CHECK(t.groupFinal(9, applied, total));
  CHECK(applied == 1 && total == 2);  // aplicacion parcial, reportada como tal
  // Entregado sin resultado => timeout de ejecucion.
  t.add(3, kNodeA, uint8_t(MsgType::Calibration), kCalOpSet, kTargetPh, "ph", "", false, 0, 0, 30000);
  t.onDelivery(3, Delivery::Delivered, 1000);
  t.expire(1000 + 30001, 30000);
  CHECK_EQ(t.find(3)->state, kCmdTimeout);
  t.onDelivery(3, Delivery::Delivered, 1);  // eventos tardios no reabren un estado final
  CHECK_EQ(t.find(3)->state, kCmdTimeout);
  // Nodo reiniciado: resultado desconocido.
  t.add(4, kNodeB, uint8_t(MsgType::Calibration), kCalOpSet, kTargetO2Gas1, "o2_gas_1", "", false, 0, 0, 30000);
  t.onDelivery(4, Delivery::PeerRebooted, 10);
  CHECK_EQ(t.find(4)->state, kCmdNodeRebooted);
  // Tabla acotada: no crece.
  for (uint32_t i = 10; i < 100; ++i) t.add(i, kNodeA, 6, 1, 1, "ph", "", false, 0, i, i + 1);
  CHECK(t.dropped > 0);
}

// ---------------- Comandos cloud (ejemplos de README v4) ----------------
static CloudCommand parse(const char* json) {
  static JsonDocument doc;
  doc.clear();
  deserializeJson(doc, json);
  return parseCloudCommand(doc.as<JsonObjectConst>());
}

TEST(cloud_commands_v4_examples_route_to_owner) {
  CloudCommand c = parse(R"({"action":"set","sensor":"ph","m":-6.1125,"b":15.013,"commandId":"cal-1"})");
  CHECK(c.kind == CloudKind::Calibration && c.node == kNodeA && !c.rejectStatus);
  CHECK(c.cal.op == kCalOpSet && c.cal.present == 3);
  CHECK_NEAR(c.cal.params[0], -6.1125, 1e-6);  // ArduinoJson: hasta 1 ulp (igual que v4)
  CHECK_STR(c.commandId, "cal-1");
  c = parse(R"({"action":"set","sensor":"co2","a":12.34,"b":-0.5})");
  CHECK(c.node == kNodeA && c.cal.target == kTargetCo2Both && c.cal.present == 3);
  c = parse(R"({"action":"set","sensor":"o2_gas_2","gain":1.01})");
  CHECK(c.node == kNodeB && c.cal.target == kTargetO2Gas2);
  c = parse(R"({"action":"set","sensor":"ph","m":"abc"})");
  CHECK_STR(c.rejectStatus ? c.rejectStatus : "", "rejected_invalid_parameters");
  c = parse(R"({"action":"set","sensor":"ph","m":null})");
  CHECK(c.rejectStatus != nullptr);
  c = parse(R"({"action":"calibrate","sensor":"o2_gas_1","reference_vol":20.8})");
  CHECK_STR(c.rejectStatus ? c.rejectStatus : "", "rejected_reference_use_20.9");
  c = parse(R"({"action":"reset_all"})");
  CHECK(c.kind == CloudKind::ResetAll && c.cal.op == kCalOpResetAll);
  c = parse(R"({"action":"reboot"})");
  CHECK(c.kind == CloudKind::Reboot && c.rebootTarget == kRebootGateway);
  c = parse(R"({"action":"reboot","node":"node_b"})");
  CHECK(c.rebootTarget == kRebootNodeB);
  c = parse(R"({"action":"control","target":"compressor","on":true,"expiresAt":1790000300,"commandId":"compresor-001"})");
  CHECK(c.kind == CloudKind::Actuator && c.act.on && c.act.expiresAt == 1790000300ULL && !c.act.fieldError);
  c = parse(R"({"action":"control","target":"led","r":256,"g":0,"b":0,"brightness":1})");
  CHECK_STR(c.act.fieldError ? c.act.fieldError : "", "rgb_and_brightness_must_be_0_to_255");
  c = parse(R"({"action":"control","target":"compressor","on":true,"commandId":"0123456789012345678901234567890123456789012345678901234567890123456789"})");
  CHECK_STR(c.act.parseError ? c.act.parseError : "", "invalid_command_id");
  c = parse(R"({"action":"diagnostics","scope":"i2c_recover"})");
  CHECK(c.kind == CloudKind::Diagnostics && c.scope == kDiagRecover);
  c = parse(R"({"action":"config","node":"node_a","report_interval_s":5,"expected":{"turbidity":true}})");
  CHECK(c.kind == CloudKind::Config && c.cfg.reportIntervalMs == 5000 && (c.expectedTouched >> kSensorTurbidity) & 1);
  c = parse(R"({"action":"config","node":"node_a","expected":{"light_1":true}})");  // sensor de otro nodo
  CHECK(c.rejectStatus != nullptr);
}

// ---------------- JSON 1.1 ----------------
static GatewayInfo info(int64_t nowMono, bool utc) {
  GatewayInfo i;
  i.deviceId = "esp32-bioiot-01"; i.experimentId = "EXP-\"1\""; i.bootId = 0xABCD;
  i.nowMono = nowMono; i.utcValid = utc; i.utcMs = utc ? 1790000000000LL : 0;
  i.mqttConnected = utc; i.wifiConnected = utc; i.pinProfile = "target_c_13_32_33_4";
  return i;
}

TEST(iso_utc_format_is_v4_compatible) {
  char iso[25];
  CHECK(formatIsoUtc(1790000000000LL, iso));
  CHECK_STR(iso, "2026-09-21T14:13:20Z");
  CHECK(formatIsoUtc(1709251199000LL, iso));
  CHECK_STR(iso, "2024-02-29T23:59:59Z");
  CHECK(!formatIsoUtc(0, iso));
}

TEST(telemetry_json_contract_11) {
  static GatewayState st;
  st = GatewayState();
  initAges(st);
  st.touch(kNodeA, 100000, -40);
  st.onTelemetry(kNodeA, phTelemetry(50000, 49000, 7.123456f), 100000);
  CalStateMsg cs;
  cs.node = kNodeA;
  NodeACal a;
  a.ph_user = 1; a.ph_m = -6.0f; a.rev[kCalPh] = 3;
  cs.blobLen = uint8_t(encodeNodeACal(a, cs.blob, sizeof(cs.blob)));
  CHECK(st.onCalState(kNodeA, cs, 100000));
  ActuatorState act;
  Snapshot* snap = st.takeSnapshot(100500, 0, act, 20000);
  static char out[24576];
  const size_t n = buildTelemetryJson(*snap, st, info(130000, true), out, sizeof(out));
  CHECK(n > 0 && n < 12000);
  printf("    telemetria JSON: %zu bytes\n", n);
  JsonDocument doc;
  CHECK(!deserializeJson(doc, out));
  CHECK_STR(doc["schema_version"].as<const char*>(), "1.1");
  CHECK_STR(doc["experiment_id"].as<const char*>(), "EXP-\"1\"");
  // Snapshot sin UTC al tomarla: se deriva de forma monotona tras sincronizar.
  CHECK_STR(doc["gateway"]["time_status"].as<const char*>(), "derived_after_sync");
  CHECK_STR(doc["timestampUtc"].as<const char*>(), "2026-09-21T14:12:50Z");
  CHECK_NEAR(doc["sensors"]["ph"]["value"].as<double>(), 7.123, 1e-6);  // 3 decimales como v4
  CHECK_STR(doc["sensors"]["ph"]["quality"].as<const char*>(), "good");
  CHECK_EQ(doc["sensors"]["ph"]["age_ms"].as<long>(), 1500);
  CHECK_EQ(doc["sensors"]["ph"]["calibration_revision"].as<int>(), 0);
  // Sensor de B sin datos y B fuera de linea.
  CHECK_STR(doc["sensors"]["light_1"]["quality"].as<const char*>(), "node_offline");
  CHECK(doc["sensors"]["light_1"]["connected"].isNull());
  CHECK(doc["sensors"]["light_1"]["value"].isNull());
  CHECK(doc["calibration"]["version"].isNull());
  CHECK_STR(doc["calibration"]["ph"]["source"].as<const char*>(), "user_calibrated");
  CHECK_EQ(doc["calibration"]["ph"]["revision"].as<int>(), 3);
  CHECK(doc["calibration"]["o2_gas_1"].isNull());
  bool offlineAlert = false;
  for (JsonVariant v : doc["alerts"].as<JsonArray>()) offlineAlert |= strcmp(v.as<const char*>(), "node_b_offline") == 0;
  CHECK(offlineAlert);
  CHECK_STR(doc["status"].as<const char*>(), "warning");
  CHECK_STR(doc["actuators"]["mode"].as<const char*>(), "manual_c2d");
  CHECK(doc["messageId"].as<std::string>().find("esp32-bioiot-01:0000abcd:") == 0);
  // Sin UTC en ningun momento: timestampUtc null, nunca inventado.
  const size_t m = buildTelemetryJson(*snap, st, info(130000, false), out, sizeof(out));
  JsonDocument d2;
  deserializeJson(d2, out, m);
  CHECK(d2["timestampUtc"].isNull());
  CHECK_STR(d2["gateway"]["time_status"].as<const char*>(), "unsynchronized");
  // Buffer insuficiente: no se emite JSON truncado.
  CHECK_EQ(buildTelemetryJson(*snap, st, info(130000, true), out, 100), 0u);
}

TEST(calibration_ack_and_group_summary_json) {
  static GatewayState st;
  st = GatewayState();
  TrackedCommand c;
  c.used = true; c.cmdId = 5; c.node = kNodeB; c.msgType = uint8_t(MsgType::Calibration);
  c.op = kCalOpO2Air; c.target = kTargetO2Gas1; strcpy(c.sensor, "o2_gas_1");
  c.state = kCmdApplied; c.status = kStCommandSent; c.storageOk = true; c.extra1 = 1;
  c.hasCloudId = true; strcpy(c.cloudId, "o2-cal-1");
  char out[2048];
  const size_t n = buildCalibrationAck(c, st, info(1, true), out, sizeof(out));
  JsonDocument doc;
  CHECK(n && !deserializeJson(doc, out));
  CHECK_STR(doc["type"].as<const char*>(), "calibration_ack");
  CHECK_STR(doc["status"].as<const char*>(), "command_sent");
  CHECK_STR(doc["state"].as<const char*>(), "applied");
  CHECK(doc["physical_calibration_confirmed"] == false);
  CHECK(doc["channels_disabled"] == true);
  const size_t g = buildGroupSummary(9, "reset_all", "x", st, info(1, true), 1, 2, out, sizeof(out));
  JsonDocument d2;
  CHECK(g && !deserializeJson(d2, out));
  CHECK_STR(d2["state"].as<const char*>(), "partial");
}
