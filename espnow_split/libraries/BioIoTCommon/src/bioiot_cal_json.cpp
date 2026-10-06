#include "bioiot_cal_json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace bioiot {

namespace {
// ArduinoJson 7.4.3 serializa float con ~7 cifras y su parser no redondea al
// float exacto: para migrar curvas bit a bit cada coeficiente lleva tambien su
// patron IEEE-754 ("<campo>_f32": "c0c39a5d"). El decimal sigue siendo legible.
void putF(JsonObject o, const char* key, float v) {
  o[key] = v;
  uint32_t bits;
  memcpy(&bits, &v, 4);
  char k[32], hex[9];
  snprintf(k, sizeof(k), "%s_f32", key);
  snprintf(hex, sizeof(hex), "%08lx", (unsigned long)bits);
  o[k] = hex;
}

// Lectura estricta: el campo debe existir y ser del tipo correcto.
struct Strict {
  bool ok = true;
  const char* error = "";
  float num(JsonObjectConst o, const char* key) {
    JsonVariantConst v = o[key];
    if (!v.is<double>() || !isfinite(v.as<double>())) { fail(key); return NAN; }
    const double decimal = v.as<double>();
    char k[32];
    snprintf(k, sizeof(k), "%s_f32", key);
    const char* hex = o[k] | (const char*)nullptr;
    if (!hex) return float(decimal);  // sin patron: a lo sumo 1 ulp de diferencia
    char* end = nullptr;
    const unsigned long bits = strtoul(hex, &end, 16);
    if (strlen(hex) != 8 || !end || *end) { fail(k); return NAN; }
    const uint32_t b32 = uint32_t(bits);
    float exact;
    memcpy(&exact, &b32, 4);
    // El patron debe corresponder al decimal (protege de copias mezcladas).
    if (!isfinite(exact) || fabs(double(exact) - decimal) > 1e-4 * (fabs(decimal) > 1 ? fabs(decimal) : 1)) {
      fail(k);
      return NAN;
    }
    return exact;
  }
  uint8_t flag(JsonObjectConst o, const char* key) {
    JsonVariantConst v = o[key];
    if (v.is<bool>()) return v.as<bool>() ? 1 : 0;
    if (v.is<int>() && (v.as<int>() == 0 || v.as<int>() == 1)) return uint8_t(v.as<int>());
    fail(key);
    return 0;
  }
  uint32_t u32(JsonObjectConst o, const char* key, bool optional = false) {
    JsonVariantConst v = o[key];
    if (v.isNull() && optional) return 0;
    if (!v.is<uint32_t>()) { fail(key); return 0; }
    return v.as<uint32_t>();
  }
  int64_t i64(JsonObjectConst o, const char* key) {
    JsonVariantConst v = o[key];
    if (v.isNull()) return 0;
    if (!v.is<int64_t>() || v.as<int64_t>() < 0) { fail(key); return 0; }
    return v.as<int64_t>();
  }
  JsonObjectConst obj(JsonObjectConst o, const char* key) {
    JsonObjectConst c = o[key];
    if (c.isNull()) fail(key);
    return c;
  }
  void fail(const char* key) {
    if (ok) error = key;
    ok = false;
  }
};

void readCo2(Strict& s, JsonObjectConst o, Co2Calibration& c) {
  c.zero_point_v = s.num(o, "zero_point_v");
  c.reaction_voltage_v = s.num(o, "reaction_voltage_v");
  c.a = s.num(o, "a");
  c.b = s.num(o, "b");
  c.mode = s.flag(o, "mode");
  c.user = s.flag(o, "user");
  c.enabled = s.flag(o, "enabled");
  c.provisional = s.flag(o, "provisional");
}

void writeCo2(JsonObject o, const Co2Calibration& c) {
  putF(o, "zero_point_v", c.zero_point_v); putF(o, "reaction_voltage_v", c.reaction_voltage_v);
  putF(o, "a", c.a); putF(o, "b", c.b); o["mode"] = c.mode; o["user"] = bool(c.user);
  o["enabled"] = bool(c.enabled); o["provisional"] = bool(c.provisional);
}
}  // namespace

CalImportResult parseCalibrationExport(JsonObjectConst root, NodeACal& outA, NodeBCal& outB) {
  CalImportResult r;
  const char* format = root["format"] | "";
  if (strcmp(format, kCalExportFormat) != 0) { r.error = "format"; return r; }
  if (!root["format_version"].is<int>() || root["format_version"].as<int>() != kCalExportFormatVersion) {
    r.error = "format_version";
    return r;
  }
  r.formatOk = true;
  uint32_t legacyVer = 0, legacySchema = 0;
  JsonObjectConst legacy = root["legacy"];
  if (!legacy.isNull()) {
    Strict s;
    legacyVer = s.u32(legacy, "ver", true);
    legacySchema = s.u32(legacy, "schema", true);
    if (!s.ok) { r.error = "legacy"; return r; }
  }
  JsonObjectConst a = root["node_a"];
  if (!a.isNull()) {
    r.hasA = true;
    Strict s;
    NodeACal c = outA;
    JsonObjectConst ph = s.obj(a, "ph"), turb = s.obj(a, "turb"), dox = s.obj(a, "do"), temp = s.obj(a, "temp");
    JsonObjectConst co2l = s.obj(a, "co2_legacy"), c1 = s.obj(a, "co2_1"), c2 = s.obj(a, "co2_2");
    if (s.ok) {
      c.ph_m = s.num(ph, "m"); c.ph_b = s.num(ph, "b"); c.ph_user = s.flag(ph, "user");
      c.turb_m = s.num(turb, "m"); c.turb_b = s.num(turb, "b");
      c.turb_user = turb["user"].isNull() ? 0 : s.flag(turb, "user");
      c.do_m = s.num(dox, "m"); c.do_user = s.flag(dox, "user");
      c.temp_m = s.num(temp, "m"); c.temp_b = s.num(temp, "b");
      c.temp_user = temp["user"].isNull() ? 0 : s.flag(temp, "user");
      c.co2_a = s.num(co2l, "a"); c.co2_b = s.num(co2l, "b"); c.co2_enabled = s.flag(co2l, "enabled");
      readCo2(s, c1, c.co2[0]);
      readCo2(s, c2, c.co2[1]);
    }
    if (s.ok && validNodeACal(c)) {
      c.legacyVersion = legacyVer;
      c.legacySchema = legacySchema;
      c.importedMask = (1u << kCalACount) - 1;
      for (uint8_t i = 0; i < kCalACount; ++i) c.rev[i]++;
      c.nodeRevision++;
      outA = c;
      r.aValid = true;
    } else if (!*r.error) {
      r.error = s.ok ? "node_a_out_of_range" : s.error;
    }
  }
  JsonObjectConst b = root["node_b"];
  if (!b.isNull()) {
    r.hasB = true;
    Strict s;
    NodeBCal c = outB;
    const char* keys[2] = {"o2_gas_1", "o2_gas_2"};
    for (uint8_t i = 0; i < 2 && s.ok; ++i) {
      JsonObjectConst o = s.obj(b, keys[i]);
      if (!s.ok) break;
      c.o2[i].gain = s.num(o, "gain");
      c.o2[i].offset = s.num(o, "offset");
      c.o2[i].reference = s.num(o, "reference");
      c.o2[i].commandUtc = s.i64(o, "command_utc");
      c.o2[i].commandCount = s.u32(o, "command_count", true);
    }
    if (s.ok && validNodeBCal(c)) {
      c.legacyVersion = legacyVer;
      c.importedMask = (1u << kCalBCount) - 1;
      for (uint8_t i = 0; i < kCalBCount; ++i) c.rev[i]++;
      c.nodeRevision++;
      outB = c;
      r.bValid = true;
    } else if (!*r.error) {
      r.error = s.ok ? "node_b_out_of_range" : s.error;
    }
  }
  if (!r.hasA && !r.hasB) r.error = "no_sections";
  return r;
}

void writeNodeACalExport(JsonObject o, const NodeACal& a) {
  JsonObject ph = o["ph"].to<JsonObject>();
  putF(ph, "m", a.ph_m); putF(ph, "b", a.ph_b); ph["user"] = bool(a.ph_user);
  JsonObject turb = o["turb"].to<JsonObject>();
  putF(turb, "m", a.turb_m); putF(turb, "b", a.turb_b); turb["user"] = bool(a.turb_user);
  JsonObject dox = o["do"].to<JsonObject>();
  putF(dox, "m", a.do_m); dox["user"] = bool(a.do_user);
  JsonObject temp = o["temp"].to<JsonObject>();
  putF(temp, "m", a.temp_m); putF(temp, "b", a.temp_b); temp["user"] = bool(a.temp_user);
  JsonObject l = o["co2_legacy"].to<JsonObject>();
  putF(l, "a", a.co2_a); putF(l, "b", a.co2_b); l["enabled"] = bool(a.co2_enabled);
  writeCo2(o["co2_1"].to<JsonObject>(), a.co2[0]);
  writeCo2(o["co2_2"].to<JsonObject>(), a.co2[1]);
  JsonObject rev = o["revisions"].to<JsonObject>();
  const char* names[kCalACount] = {"ph", "turb", "do", "temp", "co2_1", "co2_2"};
  for (uint8_t i = 0; i < kCalACount; ++i) rev[names[i]] = a.rev[i];
  o["node_revision"] = a.nodeRevision;
  o["imported_mask"] = a.importedMask;
}

void writeNodeBCalExport(JsonObject o, const NodeBCal& b) {
  const char* keys[2] = {"o2_gas_1", "o2_gas_2"};
  for (uint8_t i = 0; i < 2; ++i) {
    JsonObject x = o[keys[i]].to<JsonObject>();
    putF(x, "gain", b.o2[i].gain); putF(x, "offset", b.o2[i].offset); putF(x, "reference", b.o2[i].reference);
    x["command_utc"] = b.o2[i].commandUtc; x["command_count"] = b.o2[i].commandCount;
  }
  JsonObject rev = o["revisions"].to<JsonObject>();
  rev["o2_gas_1"] = b.rev[0]; rev["o2_gas_2"] = b.rev[1];
  o["node_revision"] = b.nodeRevision;
  o["imported_mask"] = b.importedMask;
}

void writeCalibrationExport(JsonObject root, const char* source, const char* deviceId, const char* utcIso,
                            const NodeACal* a, const NodeBCal* b) {
  root["format"] = kCalExportFormat;
  root["format_version"] = kCalExportFormatVersion;
  root["source"] = source;
  root["device_id"] = deviceId;
  if (utcIso && *utcIso) root["exported_at_utc"] = utcIso; else root["exported_at_utc"] = nullptr;
  JsonObject legacy = root["legacy"].to<JsonObject>();
  legacy["ver"] = a ? a->legacyVersion : b ? b->legacyVersion : 0;
  legacy["schema"] = a ? a->legacySchema : 0;
  if (a) writeNodeACalExport(root["node_a"].to<JsonObject>(), *a);
  if (b) writeNodeBCalExport(root["node_b"].to<JsonObject>(), *b);
}

}  // namespace bioiot
