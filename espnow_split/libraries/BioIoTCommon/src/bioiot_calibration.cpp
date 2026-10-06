#include "bioiot_calibration.h"

#include <math.h>
#include <string.h>

#include "bioiot_codec.h"
#include "bioiot_protocol.h"

namespace bioiot {

namespace {
void putCo2(Writer& w, const Co2Calibration& c) {
  w.f32(c.zero_point_v); w.f32(c.reaction_voltage_v); w.f32(c.a); w.f32(c.b);
  w.u8(c.mode); w.u8(c.user); w.u8(c.enabled); w.u8(c.provisional);
}
void getCo2(Reader& r, Co2Calibration& c) {
  c.zero_point_v = r.f32(); c.reaction_voltage_v = r.f32(); c.a = r.f32(); c.b = r.f32();
  c.mode = r.u8(); c.user = r.u8(); c.enabled = r.u8(); c.provisional = r.u8();
}
bool flag01(uint8_t v) { return v <= 1; }
}  // namespace

size_t encodeNodeACal(const NodeACal& c, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u32(c.legacyVersion); w.u32(c.legacySchema); w.u32(c.nodeRevision);
  w.f32(c.ph_m); w.f32(c.ph_b); w.f32(c.turb_m); w.f32(c.turb_b); w.f32(c.do_m);
  w.f32(c.temp_m); w.f32(c.temp_b); w.f32(c.co2_a); w.f32(c.co2_b);
  w.u32(0);  // reservado (alineacion de formato v1, siempre 0)
  w.u8(c.ph_user); w.u8(c.turb_user); w.u8(c.do_user); w.u8(c.temp_user);
  w.u8(c.co2_enabled); w.u8(c.importedMask);
  putCo2(w, c.co2[0]); putCo2(w, c.co2[1]);
  for (uint8_t i = 0; i < kCalACount; ++i) w.u16(c.rev[i]);
  return w.ok() && w.size() == kNodeACalEncodedSize ? w.size() : 0;
}

bool decodeNodeACal(const uint8_t* in, size_t len, NodeACal& out) {
  if (len != kNodeACalEncodedSize) return false;
  Reader r(in, len);
  NodeACal c;
  c.legacyVersion = r.u32(); c.legacySchema = r.u32(); c.nodeRevision = r.u32();
  c.ph_m = r.f32(); c.ph_b = r.f32(); c.turb_m = r.f32(); c.turb_b = r.f32(); c.do_m = r.f32();
  c.temp_m = r.f32(); c.temp_b = r.f32(); c.co2_a = r.f32(); c.co2_b = r.f32();
  if (r.u32() != 0) return false;
  c.ph_user = r.u8(); c.turb_user = r.u8(); c.do_user = r.u8(); c.temp_user = r.u8();
  c.co2_enabled = r.u8(); c.importedMask = r.u8();
  getCo2(r, c.co2[0]); getCo2(r, c.co2[1]);
  for (uint8_t i = 0; i < kCalACount; ++i) c.rev[i] = r.u16();
  if (!r.done() || !validNodeACal(c)) return false;
  out = c;
  return true;
}

size_t encodeNodeBCal(const NodeBCal& c, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u32(c.legacyVersion); w.u32(c.nodeRevision); w.u8(c.importedMask);
  for (uint8_t i = 0; i < 2; ++i) {
    w.f32(c.o2[i].gain); w.f32(c.o2[i].offset); w.f32(c.o2[i].reference);
    w.i64(c.o2[i].commandUtc); w.u32(c.o2[i].commandCount);
  }
  for (uint8_t i = 0; i < kCalBCount; ++i) w.u16(c.rev[i]);
  return w.ok() && w.size() == kNodeBCalEncodedSize ? w.size() : 0;
}

bool decodeNodeBCal(const uint8_t* in, size_t len, NodeBCal& out) {
  if (len != kNodeBCalEncodedSize) return false;
  Reader r(in, len);
  NodeBCal c;
  c.legacyVersion = r.u32(); c.nodeRevision = r.u32(); c.importedMask = r.u8();
  for (uint8_t i = 0; i < 2; ++i) {
    c.o2[i].gain = r.f32(); c.o2[i].offset = r.f32(); c.o2[i].reference = r.f32();
    c.o2[i].commandUtc = r.i64(); c.o2[i].commandCount = r.u32();
  }
  for (uint8_t i = 0; i < kCalBCount; ++i) c.rev[i] = r.u16();
  if (!r.done() || !validNodeBCal(c)) return false;
  out = c;
  return true;
}

bool validNodeACal(const NodeACal& c) {
  const float f[] = {c.ph_m, c.ph_b, c.turb_m, c.turb_b, c.do_m, c.temp_m, c.temp_b, c.co2_a, c.co2_b};
  for (float v : f)
    if (!isfinite(v)) return false;
  if (!flag01(c.ph_user) || !flag01(c.turb_user) || !flag01(c.do_user) || !flag01(c.temp_user) ||
      !flag01(c.co2_enabled) || c.importedMask >= (1u << kCalACount))
    return false;
  return validCo2Calibration(c.co2[0]) && validCo2Calibration(c.co2[1]);
}

bool validNodeBCal(const NodeBCal& c) {
  if (c.importedMask >= (1u << kCalBCount)) return false;
  for (uint8_t i = 0; i < 2; ++i) {
    const O2Calibration& o = c.o2[i];
    if (!isfinite(o.gain) || o.gain <= 0 || !isfinite(o.offset) || !isfinite(o.reference) ||
        o.commandUtc < 0)
      return false;
  }
  return true;
}

bool nodeACalHasUserData(const NodeACal& c) {
  return c.ph_user || c.turb_user || c.do_user || c.temp_user || c.co2[0].user || c.co2[1].user ||
         c.importedMask || c.nodeRevision;
}

bool nodeBCalHasUserData(const NodeBCal& c) {
  for (uint8_t i = 0; i < 2; ++i)
    if (c.o2[i].gain != 1 || c.o2[i].offset != 0 || c.o2[i].commandCount) return true;
  return c.importedMask || c.nodeRevision;
}

namespace {
struct TargetName {
  uint8_t target;
  const char* name;
  uint8_t owner;
};
const TargetName kTargets[] = {
    {kTargetPh, "ph", kNodeA},          {kTargetTurb, "turb", kNodeA},
    {kTargetDo, "do", kNodeA},          {kTargetTemp, "temp", kNodeA},
    {kTargetCo2Both, "co2", kNodeA},    {kTargetCo2_1, "co2_1", kNodeA},
    {kTargetCo2_2, "co2_2", kNodeA},    {kTargetO2Gas1, "o2_gas_1", kNodeB},
    {kTargetO2Gas2, "o2_gas_2", kNodeB}, {kTargetAll, "all", kNodeNone},
};
}  // namespace

uint8_t calTargetFromName(const char* sensor) {
  if (!sensor) return kTargetNone;
  for (const auto& t : kTargets)
    if (t.target != kTargetAll && strcmp(sensor, t.name) == 0) return t.target;
  return kTargetNone;
}

const char* calTargetName(uint8_t target) {
  for (const auto& t : kTargets)
    if (t.target == target) return t.name;
  return "";
}

uint8_t calTargetOwner(uint8_t target) {
  for (const auto& t : kTargets)
    if (t.target == target) return t.owner;
  return kNodeNone;
}

const char* co2ModeName(const Co2Calibration& c) { return c.mode ? "legacy_exponential" : "sen0159_vendor"; }
const char* co2SourceName(const Co2Calibration& c) { return c.user ? "user_calibrated" : "vendor_reference"; }
const char* o2SourceName(const O2Calibration& c) {
  return c.gain == 1 && c.offset == 0 ? "sensor_internal_calibration" : "user_software_correction";
}

}  // namespace bioiot
