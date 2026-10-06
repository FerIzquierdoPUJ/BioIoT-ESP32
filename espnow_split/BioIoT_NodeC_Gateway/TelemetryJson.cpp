#include "TelemetryJson.h"

#include <ArduinoJson.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace gw {
using namespace bioiot;

namespace {

// Los auxiliares reciben el proxy (doc["k"], o["k"]) por plantilla: convertirlo a
// JsonVariant NO crea el miembro y la escritura se perderia en silencio.
// Valor numerico redondeado como en v4 (String(value, decimals)); NaN/inf => null.
template <typename V>
void setNum(V&& v, float value, int decimals) {
  if (!isfinite(value)) { v.set(nullptr); return; }
  double scale = 1;
  for (int i = 0; i < decimals; ++i) scale *= 10;
  v.set(round(double(value) * scale) / scale);
}
template <typename V>
void setFloat(V&& v, float value) {
  if (isfinite(value)) v.set(value); else v.set(nullptr);
}
template <typename V>
void setUtc(V&& v, int64_t utcMs) {
  char iso[25];
  if (formatIsoUtc(utcMs, iso)) v.set(iso); else v.set(nullptr);
}
template <typename V>
void setHex(V&& v, uint32_t x) {
  char b[9];
  snprintf(b, sizeof(b), "%08lx", (unsigned long)x);
  v.set(b);
}

size_t finish(JsonDocument& doc, char* out, size_t cap) {
  const size_t need = measureJson(doc);
  if (doc.overflowed() || need + 1 > cap) return 0;
  return serializeJson(doc, out, cap);
}

void header(JsonDocument& doc, const GatewayInfo& info, const char* type) {
  doc["schema_version"] = kSchemaVersion;
  doc["deviceId"] = info.deviceId;
  doc["experiment_id"] = info.experimentId;
  if (type) doc["type"] = type;
}

bool flag(const SensorSample& s, uint16_t f) { return (s.flags & f) != 0; }

void writeSensor(JsonObject o, uint8_t id, const SensorSample& s, uint8_t state, const Snapshot& snap,
                 const GatewayInfo& info) {
  const SensorInfo& si = sensorInfo(id);
  const bool fresh = state == kSampleFresh;
  o["expected"] = flag(s, kRecExpected);
  if (fresh) o["connected"] = flag(s, kRecConnected); else o["connected"] = nullptr;
  const float* v = s.vals;
  switch (id) {
    case kSensorPh: case kSensorTurbidity: case kSensorTds:
      if (fresh) {
        setNum(o["raw"], v[val::kAnalogRaw], 0);
        setNum(o["voltage"], v[val::kAnalogVoltage], 4);
        setNum(o["value"], v[val::kAnalogValue], 3);
      } else o["value"] = nullptr;
      break;
    case kSensorCo2_1: case kSensorCo2_2:
      if (fresh) {
        setNum(o["raw"], v[val::kCo2Raw], 0);
        setFloat(o["voltage"], v[val::kCo2Voltage]);
        setFloat(o["value"], v[val::kCo2Value]);
      } else o["value"] = nullptr;
      break;
    case kSensorDissolvedOxygen:
      if (fresh) {
        setNum(o["raw"], v[val::kDoRaw], 0);
        setNum(o["voltage"], v[val::kDoVoltage], 4);
        setNum(o["saturation_pct"], v[val::kDoSatPct], 2);
        setNum(o["value"], v[val::kDoValue], 3);
      } else o["value"] = nullptr;
      break;
    case kSensorTemperature:
      if (fresh) {
        setNum(o["raw"], v[val::kTempRaw], 2);
        setNum(o["value"], v[val::kTempValue], 2);
      } else o["value"] = nullptr;
      break;
    case kSensorLight1: case kSensorLight2:
      if (fresh) {
        setNum(o["value"], v[val::kLightValue], 2);
        if (isfinite(v[val::kLightDriverCode])) o["driver_code"] = v[val::kLightDriverCode];
      } else o["value"] = nullptr;
      break;
    case kSensorO2Gas1: case kSensorO2Gas2:
      if (fresh) {
        o["i2c_detected"] = flag(s, kRecI2cDetected);
        o["warming_up"] = flag(s, kRecWarming);
        o["stabilizing"] = flag(s, kRecStabilizing);
        o["measurement_valid"] = flag(s, kRecValid);
        setFloat(o["value"], v[val::kO2Value]);
      } else {
        o["measurement_valid"] = false;
        o["value"] = nullptr;
      }
      break;
    case kSensorColor1: case kSensorColor2:
      if (fresh) {
        setNum(o["rPulse"], v[val::kColorRPulse], 0); setNum(o["gPulse"], v[val::kColorGPulse], 0);
        setNum(o["bPulse"], v[val::kColorBPulse], 0);
        setNum(o["r"], v[val::kColorR], 0); setNum(o["g"], v[val::kColorG], 0); setNum(o["b"], v[val::kColorB], 0);
        setNum(o["h"], v[val::kColorH], 1); setNum(o["s"], v[val::kColorS], 3); setNum(o["l"], v[val::kColorL], 3);
      }
      break;
  }
  if (si.unit) o["unit"] = si.unit;
  if (fresh) o["quality"] = qualityName(s.quality);
  else if (state == kSampleStale) o["quality"] = qualityName(kQStale);
  else if (state == kSampleNodeOffline) o["quality"] = qualityName(kQNodeOffline);
  else o["quality"] = qualityName(kQNotSampled);
  if (id == kSensorCo2_1 || id == kSensorCo2_2) {
    o["measurement_valid"] = fresh && flag(s, kRecValid);
    o["below_reference_range"] = fresh && flag(s, kRecBelowRef);
    o["calibration_mode"] = flag(s, kRecCalLegacyMode) ? "legacy_exponential" : "sen0159_vendor";
    o["calibration_source"] = flag(s, kRecCalUser) ? "user_calibrated" : "vendor_reference";
    o["provisional"] = flag(s, kRecProvisional);
  }
  // Metadatos 1.1 (aditivos).
  o["node"] = nodeKey(si.owner);
  o["data_state"] = sampleStateName(state);
  o["stale"] = state == kSampleStale || state == kSampleNodeOffline;
  if (s.present) {
    o["age_ms"] = snap.takenMono - s.sampleMonoMs;
    setUtc(o["sampledAtUtc"], utcForMono(info, s.sampleMonoMs));
    if (id != kSensorColor1 && id != kSensorColor2 && id != kSensorLight1 && id != kSensorLight2 && id != kSensorTds)
      o["calibration_revision"] = s.calRevision;
  } else {
    o["age_ms"] = nullptr;
    o["sampledAtUtc"] = nullptr;
  }
}

const char* storageText(uint8_t s) {
  switch (s) {
    case kCalStorageOk: return "ok";
    case kCalStorageFailed: return "write_failed";
    case kCalStorageFuture: return "future_format_writes_blocked";
    default: return "defaults_not_yet_saved";
  }
}

void writeCalibration(JsonObject c, const GatewayState& st) {
  const NodeView& na = st.node(kNodeA);
  const NodeView& nb = st.node(kNodeB);
  // No existe una revision global unica: cada nodo y sensor tiene la suya.
  c["version"] = nullptr;
  c["schema_version"] = CALIBRATION_SCHEMA_VERSION;
  if (na.calValid && nb.calValid) c["nvs_ok"] = na.calStorageState == kCalStorageOk && nb.calStorageState == kCalStorageOk;
  else c["nvs_ok"] = nullptr;
  if (na.calValid) {
    const NodeACal& a = na.calA;
    JsonObject ph = c["ph"].to<JsonObject>();
    ph["m"] = a.ph_m; ph["b"] = a.ph_b;
    ph["source"] = a.ph_user ? "user_calibrated" : "experimental_3point";
    ph["input_unit"] = "V";
    if (!a.ph_user) {
      JsonArray buffers = ph["buffers"].to<JsonArray>();
      buffers.add(4.01); buffers.add(7.01); buffers.add(10.01);
      ph["r2"] = 0.9999586071;
    }
    ph["revision"] = a.rev[kCalPh]; ph["imported"] = bool(a.importedMask & (1u << kCalPh));
    JsonObject turb = c["turb"].to<JsonObject>();
    turb["m"] = a.turb_m; turb["b"] = a.turb_b; turb["revision"] = a.rev[kCalTurb];
    turb["source"] = a.turb_user ? "user_calibrated" : "initial_curve";
    JsonObject dox = c["do"].to<JsonObject>();
    dox["m"] = a.do_m; dox["input_unit"] = "mV";
    dox["source"] = a.do_user ? "user_calibrated" : "experimental_2point";
    if (!a.do_user) { dox["zero_mv"] = 0.0; dox["saturation_100_mv"] = 748.9564; dox["r2"] = 1.0; }
    dox["revision"] = a.rev[kCalDo];
    JsonObject temp = c["temp"].to<JsonObject>();
    temp["m"] = a.temp_m; temp["b"] = a.temp_b; temp["revision"] = a.rev[kCalTemp];
    temp["source"] = a.temp_user ? "user_calibrated" : "initial_curve";
    JsonObject co2 = c["co2"].to<JsonObject>();
    co2["enabled"] = bool(a.co2_enabled); co2["a"] = a.co2_a; co2["b"] = a.co2_b; co2["legacy_snapshot"] = true;
    for (uint8_t i = 0; i < 2; ++i) {
      JsonObject o = c[i == 0 ? "co2_1" : "co2_2"].to<JsonObject>();
      const Co2Calibration& k = a.co2[i];
      o["mode"] = co2ModeName(k); o["source"] = co2SourceName(k);
      o["provisional"] = bool(k.provisional); o["enabled"] = bool(k.enabled);
      o["zero_point_v"] = k.zero_point_v; o["reaction_voltage_v"] = k.reaction_voltage_v;
      o["a"] = k.a; o["b"] = k.b; o["revision"] = a.rev[kCalCo2_1 + i];
    }
  } else {
    for (const char* k : {"ph", "turb", "do", "temp", "co2", "co2_1", "co2_2"}) c[k] = nullptr;
  }
  if (nb.calValid) {
    for (uint8_t i = 0; i < 2; ++i) {
      JsonObject o = c[i == 0 ? "o2_gas_1" : "o2_gas_2"].to<JsonObject>();
      o["gain"] = nb.calB.o2[i].gain; o["offset"] = nb.calB.o2[i].offset;
      o["source"] = o2SourceName(nb.calB.o2[i]); o["revision"] = nb.calB.rev[i];
    }
  } else {
    c["o2_gas_1"] = nullptr;
    c["o2_gas_2"] = nullptr;
  }
  JsonObject nodes = c["nodes"].to<JsonObject>();
  for (uint8_t id : {uint8_t(kNodeA), uint8_t(kNodeB)}) {
    const NodeView& n = st.node(id);
    JsonObject o = nodes[nodeKey(id)].to<JsonObject>();
    o["confirmed_copy"] = n.calValid;
    if (!n.calValid) continue;
    o["revision"] = id == kNodeA ? n.calA.nodeRevision : n.calB.nodeRevision;
    o["storage"] = storageText(n.calStorageState);
    o["legacy_v4_version"] = id == kNodeA ? n.calA.legacyVersion : n.calB.legacyVersion;
    o["imported_mask"] = id == kNodeA ? n.calA.importedMask : n.calB.importedMask;
  }
}

void writeActuators(JsonObject o, const ActuatorState& a, const GatewayInfo& info) {
  o["ready"] = a.ready;
  o["mode"] = "manual_c2d";
  o["actuation"] = a.commissioning ? "commissioning_simulated" : "production";
  o["outputs_driven"] = a.outputsDriven;
  o["pin_profile"] = info.pinProfile;
  o["lastStopReason"] = a.lastStopReason;
  JsonObject c = o["compressor"].to<JsonObject>();
  c["on"] = a.compressorOn; c["expiresAt"] = a.compressorExpiresAt; c["restartAllowedInMs"] = a.restartAllowedInMs;
  c["source"] = a.compressorSource; c["locked"] = a.compressorLocked;
  JsonObject l = o["led"].to<JsonObject>();
  l["on"] = a.ledOn; l["r"] = a.rgb[0]; l["g"] = a.rgb[1]; l["b"] = a.rgb[2]; l["brightness"] = a.brightness;
  l["dutyR"] = a.duty[0]; l["dutyG"] = a.duty[1]; l["dutyB"] = a.duty[2]; l["expiresAt"] = a.ledExpiresAt;
  l["source"] = a.ledSource; l["locked"] = a.ledLocked;
  JsonObject last = o["lastCommand"].to<JsonObject>();
  last["commandId"] = static_cast<const char*>(a.lastCommandId); last["accepted"] = a.lastAccepted; last["reason"] = a.lastReason;
  JsonObject p = o["local_program"].to<JsonObject>();
  p["enabled"] = a.programEnabled; p["valid"] = a.programValid;
}

void writeNodes(JsonObject o, const GatewayState& st, const GatewayInfo& info, int64_t atMono) {
  for (uint8_t id : {uint8_t(kNodeA), uint8_t(kNodeB)}) {
    const NodeView& n = st.node(id);
    JsonObject x = o[nodeKey(id)].to<JsonObject>();
    x["online"] = st.nodeOnline(id, info.nowMono, 20000);
    if (n.boot) setHex(x["boot_id"], n.boot); else x["boot_id"] = nullptr;
    if (n.everRx) x["link_age_ms"] = atMono - n.lastRxMono; else x["link_age_ms"] = nullptr;
    x["rssi"] = n.rssi;
    x["telemetry_frames"] = n.telemetryFrames;
    x["backfilled_samples"] = n.backfilled;
    x["rejected_records"] = n.rejectedRecords;
    if (n.helloValid) {
      char fw[8];
      snprintf(fw, sizeof(fw), "%u.%u", unsigned(n.hello.fwVersion >> 8), unsigned(n.hello.fwVersion & 0xFF));
      x["fw_version"] = fw;
      x["self_test_ok"] = bool(n.hello.flags & kHelloSelfTestOk);
      x["placeholder_keys"] = bool(n.hello.flags & kHelloPlaceholderKeys);
    }
    if (!n.statusValid) { x["status"] = nullptr; continue; }
    const StatusMsg& s = n.status;
    JsonObject y = x["status"].to<JsonObject>();
    y["age_ms"] = atMono - n.statusMono;
    y["uptime_ms"] = s.uptimeMs;
    y["free_heap"] = s.freeHeap; y["min_free_heap"] = s.minFreeHeap; y["max_alloc_heap"] = s.maxAllocHeap;
    y["reset_reason"] = resetReasonText(s.resetReason);
    y["channel"] = s.channel;
    y["storage_ok"] = bool(s.flags & kSfStorageOk);
    y["time_synced"] = bool(s.flags & kSfTimeSynced);
    y["channel_hunting"] = bool(s.flags & kSfChannelHunting);
    y["queue_depth"] = s.queueDepth;
    y["retries"] = s.retries; y["tx_failures"] = s.txFailures; y["telemetry_dropped"] = s.telemetryDropped;
    y["rx_rejected"] = s.rxRejected; y["channel_hunts"] = s.channelHunts;
    y["report_interval_ms"] = s.reportIntervalMs;
    if (id == kNodeB) {
      y["o2_warmup_remaining_ms"] = s.warmupRemainingMs;
      y["o2_warmup_assumed_full"] = bool(s.flags & kSfWarmupAssumed);
    }
  }
}

// Resumen breve compatible con "diagnostics" de v4, con datos del STATUS de cada nodo.
void writeQuickDiagnostics(JsonObject o, const GatewayState& st, const GatewayInfo& info) {
  const NodeView& a = st.node(kNodeA);
  const NodeView& b = st.node(kNodeB);
  bool warning = !info.mqttConnected || !info.wifiConnected || !info.utcValid;
  bool incomplete = !a.statusValid || !b.statusValid;
  JsonObject bus = o["i2c"].to<JsonObject>();
  char addr[5];
  if (b.statusValid) {
    const uint16_t d = b.status.diagFlags;
    bus["tca_detected"] = bool(d & kDiagBTcaDetected);
    bus["last_error"] = b.status.diagError;
    bus["consecutive_failures"] = b.status.diagCounter;
    bus["readings_ok"] = (d & (kDiagBBh1Ok | kDiagBBh2Ok | kDiagBO2_1Ok | kDiagBO2_2Ok)) ==
                         (kDiagBBh1Ok | kDiagBBh2Ok | kDiagBO2_1Ok | kDiagBO2_2Ok);
    if (!(d & kDiagBTcaDetected) || !bus["readings_ok"].as<bool>()) warning = true;
  } else {
    bus["tca_detected"] = nullptr;
  }
  (void)addr;
  bus["source"] = "node_b_status";
  JsonObject analog = o["analog_mux"].to<JsonObject>();
  JsonObject color = o["color"].to<JsonObject>();
  if (a.statusValid) {
    const uint16_t d = a.status.diagFlags;
    analog["sampled"] = bool(d & kDiagAAnalogSampled);
    analog["all_near_zero"] = bool(d & kDiagAAllNearZero);
    analog["channels_too_similar"] = bool(d & kDiagATooSimilar);
    analog["all_zero_count"] = a.status.diagCounter;
    analog["sampled_age_ms"] = a.status.diagSampledAgeMs;
    if (d & kDiagAColor1Sampled) color["color_1_pulse_ok"] = bool(d & kDiagAColor1PulseOk); else color["color_1_pulse_ok"] = nullptr;
    if (d & kDiagAColor2Sampled) color["color_2_pulse_ok"] = bool(d & kDiagAColor2PulseOk); else color["color_2_pulse_ok"] = nullptr;
    if ((d & (kDiagAAllNearZero | kDiagATooSimilar)) ||
        ((d & kDiagAColor1Sampled) && !(d & kDiagAColor1PulseOk)) ||
        ((d & kDiagAColor2Sampled) && !(d & kDiagAColor2PulseOk)) ||
        ((d & kDiagATempSampled) && !(d & kDiagATempDetected)))
      warning = true;
  } else {
    analog["sampled"] = nullptr;
  }
  JsonObject sys = o["system"].to<JsonObject>();
  sys["free_heap"] = info.freeHeap; sys["min_free_heap"] = info.minFreeHeap; sys["max_alloc_heap"] = info.maxAllocHeap;
  sys["reset_reason"] = info.resetReason; sys["mqtt_connected"] = info.mqttConnected;
  o["health"] = warning ? "warning" : incomplete ? "unknown" : "ok";
}

void appendAlert(JsonArray alerts, const char* name, const char* suffix) {
  char buf[48];
  snprintf(buf, sizeof(buf), "%s%s", name, suffix);
  alerts.add(buf);
}

}  // namespace

bool formatIsoUtc(int64_t utcMs, char out[25]) {
  if (utcMs < 1600000000000LL) { out[0] = 0; return false; }
  int64_t secs = utcMs / 1000;
  int64_t days = secs / 86400;
  int64_t rem = secs % 86400;
  // Algoritmo civil_from_days (H. Hinnant), portable sin gmtime_r.
  days += 719468;
  const int64_t era = days / 146097;
  const unsigned doe = unsigned(days - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = int64_t(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  if (m <= 2) y++;
  snprintf(out, 25, "%04d-%02u-%02uT%02u:%02u:%02uZ", int(y), m, d, unsigned(rem / 3600), unsigned((rem % 3600) / 60),
           unsigned(rem % 60));
  return true;
}

int64_t utcForMono(const GatewayInfo& info, int64_t mono) {
  if (!info.utcValid) return 0;
  return info.utcMs - (info.nowMono - mono);
}

void fillTelemetryJson(JsonDocument& doc, const Snapshot& snap, const GatewayState& st, const GatewayInfo& info) {
  header(doc, info, nullptr);
  // timestampUtc = instante de la instantanea (v4: inicio del ciclo de lectura).
  int64_t takenUtc = snap.takenUtcMs;
  const char* timeStatus = "synchronized";
  if (!takenUtc) {
    takenUtc = utcForMono(info, snap.takenMono);
    timeStatus = takenUtc ? "derived_after_sync" : "unsynchronized";
  }
  setUtc(doc["timestampUtc"], takenUtc);
  doc["uptimeMs"] = snap.takenMono;
  doc["wifiRssi"] = info.wifiRssi;
  JsonArray alerts = doc["alerts"].to<JsonArray>();
  for (uint8_t i = 0; i < 2; ++i)
    if (!snap.nodeOnline[i]) alerts.add(i == 0 ? "node_a_offline" : "node_b_offline");
  for (uint8_t id = 0; id < kSensorCount; ++id) {
    const SensorSample& s = snap.sensors[id];
    const uint8_t state = snap.state[id];
    const bool expected = flag(s, kRecExpected);
    const char* name = sensorInfo(id).key;
    if (state == kSampleFresh) {
      if (expected && !flag(s, kRecConnected) && !(flag(s, kRecWarming))) appendAlert(alerts, name, "_disconnected");
      if ((id == kSensorO2Gas1 || id == kSensorO2Gas2) && expected && flag(s, kRecConnected) && !flag(s, kRecWarming) &&
          !flag(s, kRecValid))
        appendAlert(alerts, name, "_invalid_measurement");
    } else if (state == kSampleStale && expected) {
      appendAlert(alerts, name, "_stale");
    }
  }
  doc["status"] = alerts.size() == 0 ? "ok" : "warning";
  writeCalibration(doc["calibration"].to<JsonObject>(), st);
  writeActuators(doc["actuators"].to<JsonObject>(), snap.actuators, info);
  writeQuickDiagnostics(doc["diagnostics"].to<JsonObject>(), st, info);
  JsonObject sensors = doc["sensors"].to<JsonObject>();
  for (uint8_t id = 0; id < kSensorCount; ++id)
    writeSensor(sensors[sensorInfo(id).key].to<JsonObject>(), id, snap.sensors[id], snap.state[id], snap, info);
  // Metadatos 1.1.
  char msgId[80];
  snprintf(msgId, sizeof(msgId), "%s:%08lx:%lu", info.deviceId, (unsigned long)info.bootId, (unsigned long)snap.seq);
  doc["messageId"] = msgId;  // estable entre reintentos del mismo arranque: deduplicable
  JsonObject g = doc["gateway"].to<JsonObject>();
  setHex(g["boot_id"], info.bootId);
  g["snapshot_seq"] = snap.seq;
  g["time_status"] = timeStatus;
  g["replayed"] = info.nowMono - snap.takenMono > int64_t(info.telemetryIntervalMs);
  g["published_delay_ms"] = info.nowMono - snap.takenMono;
  g["buffered_unpublished"] = st.unpublishedCount();
  g["snapshots_dropped"] = st.snapshotsDropped();
  g["wifi_channel"] = info.wifiChannel;
  g["espnow_channel"] = info.espnowChannel;
  g["espnow_enabled"] = info.espnowEnabled;
  g["wifi_search_state"] = info.wifiSearchState;
  if (info.wifiEverConnected) g["wifi_ms_since_connected"] = info.wifiMsSinceConnected;
  else g["wifi_ms_since_connected"] = nullptr;
  g["espnow_channel_changes"] = info.espnowChannelChanges;
  g["mqtt_qos"] = 0;
  writeNodes(doc["nodes"].to<JsonObject>(), st, info, snap.takenMono);
}

size_t buildTelemetryJson(const Snapshot& snap, const GatewayState& st, const GatewayInfo& info, char* out, size_t cap) {
  JsonDocument doc;
  fillTelemetryJson(doc, snap, st, info);
  return finish(doc, out, cap);
}

void fillActuatorEvent(JsonDocument& doc, const ActuatorState& a, const GatewayInfo& info) {
  header(doc, info, "actuator_state");
  setUtc(doc["timestampUtc"], info.utcValid ? info.utcMs : 0);
  writeActuators(doc["actuators"].to<JsonObject>(), a, info);
}

size_t buildActuatorEvent(const ActuatorState& a, const GatewayInfo& info, char* out, size_t cap) {
  JsonDocument doc;
  fillActuatorEvent(doc, a, info);
  return finish(doc, out, cap);
}

void fillCalibrationAck(JsonDocument& doc, const TrackedCommand& c, const GatewayState& st, const GatewayInfo& info) {
  header(doc, info, c.msgType == uint8_t(MsgType::Calibration) ? "calibration_ack" : "command_result");
  doc["sensor"] = static_cast<const char*>(c.sensor);  // copia: se serializa fuera del mutex
  doc["node"] = nodeKey(c.node);
  doc["state"] = cmdStateName(c.state);  // pending/delivered/applied/rejected/failed/expired/timeout/node_rebooted
  // status v4 cuando el nodo respondio; si no, el estado del gateway.
  doc["status"] = c.status <= kMaxStatusCode ? statusName(c.status) : cmdStateName(c.state);
  if (c.hasCloudId) doc["commandId"] = static_cast<const char*>(c.cloudId); else doc["commandId"] = nullptr;
  if (c.status <= kMaxStatusCode) {
    doc["calibration_version"] = c.nodeRevision;  // revision del nodo propietario
    doc["sensor_revision"] = c.sensorRevision;
    doc["nvs_ok"] = c.storageOk;
  } else {
    doc["calibration_version"] = nullptr;
    doc["nvs_ok"] = nullptr;
  }
  if (c.op == kCalOpO2Air && c.status <= kMaxStatusCode) {
    doc["channels_disabled"] = bool(c.extra1);
    doc["disable_error"] = c.extra2;
    doc["reference_vol"] = 20.9;
  }
  if (c.groupId) doc["group_id"] = c.groupId;
  setUtc(doc["timestampUtc"], info.utcValid ? info.utcMs : 0);
  doc["physical_calibration_confirmed"] = false;
}

size_t buildCalibrationAck(const TrackedCommand& c, const GatewayState& st, const GatewayInfo& info, char* out, size_t cap) {
  JsonDocument doc;
  fillCalibrationAck(doc, c, st, info);
  return finish(doc, out, cap);
}

void fillGroupSummary(JsonDocument& doc, uint32_t groupId, const char* kind, const char* commandId, const GatewayState& st, const GatewayInfo& info, uint8_t applied, uint8_t total) {
  header(doc, info, "calibration_ack");
  doc["sensor"] = "all";
  doc["group"] = kind;
  doc["group_id"] = groupId;
  // Sin transaccion atomica entre nodos: se informa cada nodo y la aplicacion parcial.
  doc["state"] = applied == total ? "applied" : applied == 0 ? "rejected" : "partial";
  doc["nodes_applied"] = applied;
  doc["nodes_total"] = total;
  if (commandId && *commandId) doc["commandId"] = commandId; else doc["commandId"] = nullptr;
  setUtc(doc["timestampUtc"], info.utcValid ? info.utcMs : 0);
  doc["physical_calibration_confirmed"] = false;
}

size_t buildGroupSummary(uint32_t groupId, const char* kind, const char* commandId, const GatewayState& st, const GatewayInfo& info, uint8_t applied, uint8_t total, char* out, size_t cap) {
  JsonDocument doc;
  fillGroupSummary(doc, groupId, kind, commandId, st, info, applied, total);
  return finish(doc, out, cap);
}

void fillCalibrationExport(JsonDocument& doc, const GatewayState& st, const GatewayInfo& info, const char* commandId) {
  header(doc, info, "calibration_export");
  if (commandId && *commandId) doc["commandId"] = commandId; else doc["commandId"] = nullptr;
  char iso[25] = "";
  formatIsoUtc(info.utcValid ? info.utcMs : 0, iso);
  const NodeView& a = st.node(kNodeA);
  const NodeView& b = st.node(kNodeB);
  writeCalibrationExport(doc["export"].to<JsonObject>(), "gateway_confirmed_copies", info.deviceId, iso,
                         a.calValid ? &a.calA : nullptr, b.calValid ? &b.calB : nullptr);
  doc["node_a_available"] = a.calValid;
  doc["node_b_available"] = b.calValid;
}

size_t buildCalibrationExport(const GatewayState& st, const GatewayInfo& info, const char* commandId, char* out, size_t cap) {
  JsonDocument doc;
  fillCalibrationExport(doc, st, info, commandId);
  return finish(doc, out, cap);
}

void fillDiagnosticReport(JsonDocument& doc, const GatewayState& st, const GatewayInfo& info) {
  const DiagSession& d = st.diag;
  header(doc, info, "diagnostic_report");
  doc["scope"] = diagScopeName(d.scope);
  doc["report_id"] = d.id;
  setUtc(doc["timestampUtc"], info.utcValid ? info.utcMs : 0);
  doc["uptimeMs"] = info.nowMono;
  doc["started_uptime_ms"] = d.startMono;
  JsonObject assessment = doc["assessment"].to<JsonObject>();
  JsonArray findings = assessment["findings"].to<JsonArray>();
  JsonObject sys = doc["gateway"].to<JsonObject>();
  sys["free_heap"] = info.freeHeap; sys["min_free_heap"] = info.minFreeHeap; sys["max_alloc_heap"] = info.maxAllocHeap;
  sys["reset_reason"] = info.resetReason;
  sys["wifi_connected"] = info.wifiConnected; sys["wifi_rssi"] = info.wifiRssi;
  sys["wifi_channel"] = info.wifiChannel; sys["espnow_channel"] = info.espnowChannel;
  sys["wifi_search_state"] = info.wifiSearchState; sys["espnow_channel_changes"] = info.espnowChannelChanges;
  sys["mqtt_connected"] = info.mqttConnected; sys["time_synced"] = info.utcValid;
  sys["espnow_enabled"] = info.espnowEnabled;
  sys["mqtt_publish_ok"] = info.mqttPublishOk; sys["mqtt_publish_fail"] = info.mqttPublishFail;
  if (!info.wifiConnected) findings.add("wifi_disconnected");
  if (!info.mqttConnected) findings.add("mqtt_disconnected");
  if (!info.utcValid) findings.add("utc_not_synchronized");
  // quick (v4): resumen breve sin barridos, con el ultimo STATUS de cada nodo.
  if (d.scope == kDiagQuick) writeQuickDiagnostics(doc["diagnostics"].to<JsonObject>(), st, info);
  JsonObject nodes = doc["nodes"].to<JsonObject>();
  for (uint8_t i = 0; i < 2; ++i) {
    const DiagNodePart& p = d.part[i];
    if (!p.requested) continue;
    const char* key = i == 0 ? "node_a" : "node_b";
    JsonObject n = nodes[key].to<JsonObject>();
    n["result"] = diagPartResultName(p.result);
    if (p.result == 1 && p.reasm.complete()) {
      JsonDocument part;
      if (deserializeJson(part, reinterpret_cast<const char*>(p.reasm.data()), p.reasm.length())) {
        n["result"] = "invalid_json";
      } else {
        n["report"] = part.as<JsonObjectConst>();
        for (JsonVariantConst f : part["findings"].as<JsonArrayConst>()) {
          char buf[96];
          snprintf(buf, sizeof(buf), "%s:%s", key, f.as<const char*>() ? f.as<const char*>() : "");
          findings.add(buf);
        }
      }
    } else {
      n["fragments_received"] = p.reasm.received();
      n["fragments_expected"] = p.reasm.expectedCount();
      char buf[48];
      snprintf(buf, sizeof(buf), "%s_diagnostic_%s", key, diagPartResultName(p.result));
      findings.add(buf);
    }
  }
  assessment["severity"] = findings.size() ? "warning" : "ok";
  assessment["physical_fault_confirmed"] = false;
}

size_t buildDiagnosticReport(const GatewayState& st, const GatewayInfo& info, char* out, size_t cap) {
  JsonDocument doc;
  fillDiagnosticReport(doc, st, info);
  return finish(doc, out, cap);
}

void fillRejection(JsonDocument& doc, const char* type, const char* action, const char* status, const char* commandId, const GatewayInfo& info) {
  header(doc, info, type);
  doc["action"] = action;
  doc["state"] = "rejected";
  doc["status"] = status;
  if (commandId && *commandId) doc["commandId"] = commandId; else doc["commandId"] = nullptr;
  setUtc(doc["timestampUtc"], info.utcValid ? info.utcMs : 0);
}

size_t buildRejection(const char* type, const char* action, const char* status, const char* commandId, const GatewayInfo& info, char* out, size_t cap) {
  JsonDocument doc;
  fillRejection(doc, type, action, status, commandId, info);
  return finish(doc, out, cap);
}

}  // namespace gw
