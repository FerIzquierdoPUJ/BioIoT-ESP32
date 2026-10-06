// Genera los ejemplos de docs/examples con el codigo real del gateway (datos
// SINTETICOS, no capturas de hardware). Con BIOIOT_WRITE_EXAMPLES=<dir>.
#include <ArduinoJson.h>
#include <stdlib.h>

#include <string>

#include "GatewayState.h"
#include "TelemetryJson.h"
#include "test_framework.h"

using namespace bioiot;
using namespace gw;

namespace {
void writePretty(const char* dir, const char* name, const char* json) {
  if (!dir) return;
  JsonDocument doc;
  if (deserializeJson(doc, json)) return;
  std::string path = std::string(dir) + "/" + name;
  FILE* f = fopen(path.c_str(), "w");
  if (!f) return;
  std::string text;
  serializeJsonPretty(doc, text);
  fputs(text.c_str(), f);
  fputs("\n", f);
  fclose(f);
}

TelemetryRecord rec(uint8_t sensor, uint8_t quality, uint16_t flags, uint32_t sampleUptime, std::initializer_list<float> v,
                    uint16_t rev = 0) {
  TelemetryRecord r;
  r.sensor = sensor; r.quality = quality; r.flags = uint16_t(flags | kRecObserved); r.sampleUptimeMs = sampleUptime;
  r.calRevision = rev; r.valueCount = uint8_t(v.size());
  uint8_t i = 0;
  for (float x : v) r.vals[i++] = x;
  return r;
}
}  // namespace

TEST(generate_documentation_examples) {
  const char* dir = getenv("BIOIOT_WRITE_EXAMPLES");
  static GatewayState st;
  st = GatewayState();
  for (uint8_t i = 0; i < kSensorCount; ++i) st.maxAgeMs[i] = (i >= kSensorO2Gas1) ? 270000 : 20000;
  const uint16_t CE = kRecConnected | kRecExpected;
  // Nodo A: valores de sensores tomados del ejemplo v4 (examples/telemetry-v1.json), sinteticos.
  TelemetryMsg a0;
  a0.snapshotId = 24; a0.parts = 2; a0.txUptimeMs = 240100;
  a0.records[0] = rec(kSensorPh, kQGood, CE, 239000, {1365, 1.1001f, 8.288f}, 0);
  a0.records[1] = rec(kSensorCo2_1, kQBelowReferenceRange, CE | kRecBelowRef | kRecProvisional | kRecCalEnabled, 239100,
                      {2483, 2.0011f, NAN, 3.2032f, 0.37685f});
  a0.records[2] = rec(kSensorCo2_2, kQBelowReferenceRange, CE | kRecBelowRef | kRecProvisional | kRecCalEnabled, 239150,
                      {2401, 1.9349f, NAN, 3.0973f, 0.36439f});
  a0.records[3] = rec(kSensorTurbidity, kQOutOfRange, kRecConnected, 239200, {3110, 2.5062f, NAN});
  a0.records[4] = rec(kSensorDissolvedOxygen, kQGood, CE, 239250, {612, 0.4932f, 65.85f, 5.459f});
  a0.records[5] = rec(kSensorTds, kQGood, CE, 239300, {455, 0.3667f, 145.7f});
  a0.records[6] = rec(kSensorTemperature, kQGood, CE, 239400, {24.5f, 24.869f});
  a0.recordCount = 7;
  TelemetryMsg a1;
  a1.snapshotId = 24; a1.part = 1; a1.parts = 2; a1.txUptimeMs = 240110;
  a1.records[0] = rec(kSensorColor1, kQDisconnected, kRecExpected, 239500, {0, 0, 0, 0, 0, 0, 0, 0, 0});
  a1.records[1] = rec(kSensorColor2, kQGood, CE, 239600, {512, 498, 640, 251, 251, 250, 240, 0.0118f, 0.982f});
  a1.recordCount = 2;
  // Nodo B: calentamiento de O2 en curso.
  TelemetryMsg b0;
  b0.snapshotId = 31; b0.txUptimeMs = 150020; b0.recordCount = 5;
  b0.records[0] = rec(kSensorLight1, kQGood, CE, 149000, {812.5f, NAN});
  b0.records[1] = rec(kSensorLight2, kQGood, CE, 149200, {790.0f, NAN});
  b0.records[2] = rec(kSensorO2Gas1, kQWarmingUp, CE | kRecI2cDetected | kRecWarming, 145000, {NAN, NAN, NAN, 0, 0, 0});
  b0.records[3] = rec(kSensorO2Gas2, kQWarmingUp, CE | kRecI2cDetected | kRecWarming, 145100, {NAN, NAN, NAN, 0, 0, 0});
  b0.records[4] = rec(kSensorTemperatureB, kQGood, CE | kRecObserved, 149400, {25.1f, 25.1f});  // DS18B20 de B (ESP32)
  const int64_t rx = 600000;
  st.touch(kNodeA, rx, -48);
  st.touch(kNodeB, rx, -61);
  st.onTelemetry(kNodeA, a0, rx);
  st.onTelemetry(kNodeA, a1, rx);
  st.onTelemetry(kNodeB, b0, rx);
  CalStateMsg ca;
  ca.node = kNodeA;
  NodeACal cal;
  cal.legacyVersion = 7; cal.legacySchema = 2; cal.importedMask = 0x3F; cal.nodeRevision = 1;
  for (auto& r : cal.rev) r = 1;
  ca.blobLen = uint8_t(encodeNodeACal(cal, ca.blob, sizeof(ca.blob)));
  st.onCalState(kNodeA, ca, rx);
  CalStateMsg cb;
  cb.node = kNodeB;
  NodeBCal calb;
  cb.blobLen = uint8_t(encodeNodeBCal(calb, cb.blob, sizeof(cb.blob)));
  st.onCalState(kNodeB, cb, rx);
  StatusMsg sa;
  sa.uptimeMs = 240000; sa.freeHeap = 231000; sa.minFreeHeap = 219500; sa.maxAllocHeap = 110580;
  sa.resetReason = kResetPowerOn; sa.flags = kSfStorageOk | kSfTimeSynced | kSfSelfTestOk; sa.channel = 6;
  sa.reportIntervalMs = 5000; sa.diagFlags = kDiagAAnalogSampled | kDiagAColor1Sampled | kDiagAColor2Sampled |
                                              kDiagAColor2PulseOk | kDiagATempSampled | kDiagATempDetected;
  st.onStatus(kNodeA, sa, rx);
  StatusMsg sb = sa;
  sb.uptimeMs = 150000; sb.freeHeap = 23800; sb.minFreeHeap = 21000; sb.maxAllocHeap = 14800; sb.warmupRemainingMs = 30000;
  sb.flags = kSfStorageOk | kSfTimeSynced | kSfSelfTestOk | kSfWarmupActive;
  sb.diagFlags = kDiagBTcaObserved | kDiagBTcaDetected | kDiagBBh1Ok | kDiagBBh2Ok | kDiagBO2_1Ok | kDiagBO2_2Ok |
                 kDiagBReadingsObserved;
  st.onStatus(kNodeB, sb, rx);
  st.node(kNodeA).boot = 0x5A11C0DE;
  st.node(kNodeB).boot = 0x0B0E8266;
  ActuatorState act;
  act.commissioning = true; act.ready = true;
  Snapshot* snap = st.takeSnapshot(rx + 400, 1790000000000LL, act, 20000);
  GatewayInfo info;
  info.deviceId = "esp32-bioiot-01"; info.experimentId = "EXP-001"; info.pinProfile = "target_c_13_32_33_4";
  info.bootId = 0xC0FFEE01; info.nowMono = rx + 900; info.utcValid = true; info.utcMs = 1790000000500LL;
  info.wifiConnected = info.mqttConnected = true; info.wifiRssi = -58; info.wifiChannel = 6; info.espnowChannel = 6;
  info.espnowEnabled = true; info.freeHeap = 151200; info.minFreeHeap = 98400; info.maxAllocHeap = 69620;
  info.wifiSearchState = "connected"; info.wifiEverConnected = true; info.wifiMsSinceConnected = 0;
  info.espnowChannelChanges = 1;  // hotspot adoptado en el canal 6
  info.resetReason = "power_on";
  static char out[24576];
  size_t n = buildTelemetryJson(*snap, st, info, out, sizeof(out));
  CHECK(n > 0);
  printf("    ejemplo telemetria 1.1: %zu bytes\n", n);
  writePretty(dir, "telemetry-v1.1.json", out);
  TrackedCommand c;
  c.used = true; c.cmdId = 3; c.node = kNodeA; c.msgType = uint8_t(MsgType::Calibration); c.op = kCalOpSet;
  c.target = kTargetPh; strcpy(c.sensor, "ph"); c.hasCloudId = true; strcpy(c.cloudId, "cal-ph-001");
  c.state = kCmdPending;
  n = buildCalibrationAck(c, st, info, out, sizeof(out));
  writePretty(dir, "calibration-ack-pending.json", out);
  c.state = kCmdApplied; c.status = kStSaved; c.nodeRevision = 2; c.sensorRevision = 2; c.storageOk = true;
  n = buildCalibrationAck(c, st, info, out, sizeof(out));
  writePretty(dir, "calibration-ack-applied.json", out);
  c.state = kCmdTimeout; c.status = 0xFF; c.node = kNodeB; strcpy(c.sensor, "o2_gas_1"); c.target = kTargetO2Gas1;
  strcpy(c.cloudId, "cal-o2-002"); c.op = kCalOpSet;
  n = buildCalibrationAck(c, st, info, out, sizeof(out));
  writePretty(dir, "calibration-ack-timeout.json", out);
  n = buildGroupSummary(4, "reset_all", "reset-all-001", st, info, 1, 2, out, sizeof(out));
  writePretty(dir, "reset-all-partial.json", out);
  act.compressorOn = true; act.compressorExpiresAt = 1790000300; act.compressorSource = "manual";
  strcpy(act.lastCommandId, "compresor-001"); act.lastAccepted = true; act.lastReason = "compressor_on";
  n = buildActuatorEvent(act, info, out, sizeof(out));
  writePretty(dir, "actuator-state-commissioning.json", out);
  n = buildCalibrationExport(st, info, "export-001", out, sizeof(out));
  writePretty(dir, "calibration-export.json", out);
  // Diagnostico agregado: A responde, B sin enlace.
  st.diag.active = true; st.diag.ready = true; st.diag.id = 9; st.diag.scope = kDiagFull; st.diag.startMono = rx;
  st.diag.part[0].requested = true; st.diag.part[0].done = true; st.diag.part[0].result = 1;
  st.diag.part[1].requested = true; st.diag.part[1].done = true; st.diag.part[1].result = 3;
  const char* nodeA = R"({"node":"node_a","report_id":9,"scope":"full","findings":["color_1_pulse_failure"],)"
                      R"("color":{"color_1":{"out_gpio":34,"pulse_ok":false,"all_pulses_zero":true}},"physical_fault_confirmed":false})";
  st.diag.part[0].reasm.expect(9, 0);
  DiagFragmentMsg f;
  f.requestId = 9; f.index = 0; f.count = 1; f.totalLen = uint16_t(strlen(nodeA)); f.chunkLen = uint8_t(strlen(nodeA));
  f.chunk = reinterpret_cast<const uint8_t*>(nodeA);
  CHECK(st.diag.part[0].reasm.add(f, 1) == Reassembler::kComplete);
  n = buildDiagnosticReport(st, info, out, sizeof(out));
  CHECK(n > 0);
  writePretty(dir, "diagnostic-report-aggregated.json", out);
}
