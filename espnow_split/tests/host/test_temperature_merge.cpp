// Temperatura del sistema: nodo A (temperature, GPIO23) y nodo B ESP32
// (temperature_b, GPIO18). El gateway publica en sensors.temperature el promedio si
// ambas son utilizables, si no la disponible, y nunca mezcla una lectura vencida.
#include <ArduinoJson.h>

#include <initializer_list>
#include <string>

#include "BioIoTCommon.h"
#include "CloudCommands.h"
#include "TelemetryJson.h"
#include "TemperatureMerge.h"
#include "test_framework.h"

using namespace bioiot;
using namespace gw;

namespace {
SensorSample tempSample(float value, uint8_t quality = kQGood) {
  SensorSample s;
  s.present = true;
  s.quality = quality;
  s.valueCount = val::kTempCount;
  s.vals[val::kTempRaw] = value;
  s.vals[val::kTempValue] = value;
  return s;
}

TelemetryRecord tempRec(uint8_t sensor, float value, uint8_t quality, uint32_t sampleUptime) {
  TelemetryRecord r;
  r.sensor = sensor;
  r.quality = quality;
  r.flags = uint16_t((quality == kQDisconnected ? 0 : kRecConnected) | kRecExpected | kRecObserved);
  r.sampleUptimeMs = sampleUptime;
  r.valueCount = val::kTempCount;
  r.vals[val::kTempRaw] = value;
  r.vals[val::kTempValue] = quality == kQGood ? value : NAN;
  return r;
}

TelemetryMsg single(const TelemetryRecord& r, uint32_t tx) {
  TelemetryMsg t;
  t.snapshotId = 1; t.parts = 1; t.txUptimeMs = tx;
  t.records[t.recordCount++] = r;
  return t;
}

// Telemetria JSON real con las temperaturas dadas (NAN = el nodo no la envio).
JsonDocument telemetryWith(float tempA, float tempB, uint8_t qualityB = kQGood, bool bStale = false) {
  static GatewayState st;
  st = GatewayState();
  for (uint8_t i = 0; i < kSensorCount; ++i) st.maxAgeMs[i] = 20000;
  const int64_t rx = 600000;
  st.touch(kNodeA, rx, -50);
  st.touch(kNodeB, rx, -55);
  if (!isnan(tempA)) st.onTelemetry(kNodeA, single(tempRec(kSensorTemperature, tempA, kQGood, 100000), 100500), rx);
  if (!isnan(tempB)) {
    const uint32_t sample = bStale ? 60000 : 100000;  // 40 s antes: vencida (maximo 20 s)
    st.onTelemetry(kNodeB, single(tempRec(kSensorTemperatureB, tempB, qualityB, sample), 100500), rx);
  }
  Snapshot* snap = st.takeSnapshot(rx + 100, 0, ActuatorState(), 60000);
  GatewayInfo info;
  info.deviceId = "esp32-bioiot-01";
  info.nowMono = rx + 200;
  static char out[24576];
  const size_t n = buildTelemetryJson(*snap, st, info, out, sizeof(out));
  JsonDocument doc;
  CHECK(n > 0 && !deserializeJson(doc, out, n));
  return doc;
}
}  // namespace

TEST(temperature_merge_rules) {
  const SensorSample none;
  TempCombined t = combineTemperature(tempSample(24.0f), kSampleFresh, tempSample(26.0f), kSampleFresh);
  CHECK(t.source == TempSource::Average);
  CHECK_NEAR(t.value, 25.0, 1e-6);
  t = combineTemperature(tempSample(24.0f), kSampleFresh, none, kSampleNever);
  CHECK(t.source == TempSource::NodeA && t.value == 24.0f);
  t = combineTemperature(none, kSampleNodeOffline, tempSample(26.5f), kSampleFresh);
  CHECK(t.source == TempSource::NodeB && t.value == 26.5f);
  // Vencida, nodo sin enlace, desconectada o fuera de rango: no entra en el promedio.
  t = combineTemperature(tempSample(24.0f), kSampleStale, tempSample(26.0f), kSampleFresh);
  CHECK(t.source == TempSource::NodeB && t.value == 26.0f);
  t = combineTemperature(tempSample(24.0f), kSampleFresh, tempSample(-127.0f, kQDisconnected), kSampleFresh);
  CHECK(t.source == TempSource::NodeA);
  t = combineTemperature(tempSample(NAN, kQOutOfRange), kSampleFresh, tempSample(26.0f), kSampleFresh);
  CHECK(t.source == TempSource::NodeB);
  t = combineTemperature(tempSample(24.0f, kQUserCalibrated), kSampleFresh, tempSample(26.0f), kSampleFresh);
  CHECK(t.source == TempSource::Average);  // calibracion de usuario en A sigue siendo valida
  t = combineTemperature(none, kSampleNever, none, kSampleNever);
  CHECK(t.source == TempSource::None && isnan(t.value));
  CHECK(tempSourceName(TempSource::None) == nullptr);
}

TEST(telemetry_temperature_average_of_both_nodes) {
  JsonDocument doc = telemetryWith(24.0f, 26.0f);
  JsonObject t = doc["sensors"]["temperature"];
  CHECK_NEAR(t["value"].as<float>(), 25.0, 1e-6);
  CHECK_STR(t["source"].as<const char*>(), "average");
  CHECK_STR(t["node"].as<const char*>(), "node_a+node_b");
  CHECK_NEAR(t["sources"]["node_a"].as<float>(), 24.0, 1e-6);
  CHECK_NEAR(t["sources"]["node_b"].as<float>(), 26.0, 1e-6);
  CHECK_STR(t["quality"].as<const char*>(), "good");
  CHECK_STR(t["unit"].as<const char*>(), "C");
  CHECK(t["raw"].isNull());  // dos sondas: sin raw unico
  // temperature_b se publica tambien por separado.
  JsonObject tb = doc["sensors"]["temperature_b"];
  CHECK_NEAR(tb["value"].as<float>(), 26.0, 1e-6);
  CHECK_STR(tb["node"].as<const char*>(), "node_b");
}

TEST(telemetry_temperature_from_either_node) {
  JsonDocument onlyA = telemetryWith(24.5f, NAN);
  CHECK_NEAR(onlyA["sensors"]["temperature"]["value"].as<float>(), 24.5, 1e-6);
  CHECK_STR(onlyA["sensors"]["temperature"]["source"].as<const char*>(), "node_a");
  CHECK(onlyA["sensors"]["temperature"]["sources"]["node_b"].isNull());

  JsonDocument onlyB = telemetryWith(NAN, 23.25f);
  JsonObject t = onlyB["sensors"]["temperature"];
  CHECK_NEAR(t["value"].as<float>(), 23.25, 1e-6);
  CHECK_STR(t["source"].as<const char*>(), "node_b");
  CHECK_STR(t["node"].as<const char*>(), "node_b");
  CHECK(t["sources"]["node_a"].isNull());

  // B desconectado (-127): la temperatura sale solo de A, con alerta de B.
  JsonDocument bDisc = telemetryWith(24.0f, -127.0f, kQDisconnected);
  CHECK_STR(bDisc["sensors"]["temperature"]["source"].as<const char*>(), "node_a");
  CHECK_NEAR(bDisc["sensors"]["temperature"]["value"].as<float>(), 24.0, 1e-6);
  bool alert = false;
  for (JsonVariant a : bDisc["alerts"].as<JsonArray>()) alert |= std::string(a.as<const char*>()) == "temperature_b_disconnected";
  CHECK(alert);

  // B vencida: no se promedia un valor viejo.
  JsonDocument bStale = telemetryWith(24.0f, 30.0f, kQGood, true);
  CHECK_STR(bStale["sensors"]["temperature"]["source"].as<const char*>(), "node_a");
  CHECK_NEAR(bStale["sensors"]["temperature"]["value"].as<float>(), 24.0, 1e-6);

  // Ninguna: value null y source null, como antes.
  JsonDocument none = telemetryWith(NAN, NAN);
  CHECK(none["sensors"]["temperature"]["value"].isNull());
  CHECK(none["sensors"]["temperature"]["source"].isNull());
}

TEST(temperature_b_config_and_record_belong_to_node_b) {
  CHECK_EQ(sensorInfo(kSensorTemperatureB).owner, kNodeB);
  CHECK_STR(sensorInfo(kSensorTemperatureB).key, "temperature_b");
  JsonDocument d;
  deserializeJson(d, R"({"action":"config","node":"node_b","expected":{"temperature_b":false}})");
  CloudCommand c = parseCloudCommand(d.as<JsonObjectConst>());
  CHECK(c.kind == CloudKind::Config && !c.rejectStatus);
  CHECK(c.expectedTouched == (1u << kSensorTemperatureB));
  // temperature_b no se puede configurar en el nodo A.
  deserializeJson(d, R"({"action":"config","node":"node_a","expected":{"temperature_b":true}})");
  c = parseCloudCommand(d.as<JsonObjectConst>());
  CHECK(c.rejectStatus != nullptr);
  // Un registro temperature_b del nodo A se rechaza (dueno equivocado).
  GatewayState* st = new GatewayState();
  st->touch(kNodeA, 1000, -50);
  CHECK_EQ(st->onTelemetry(kNodeA, single(tempRec(kSensorTemperatureB, 25.0f, kQGood, 900), 1000), 1000), 0);
  delete st;
}
