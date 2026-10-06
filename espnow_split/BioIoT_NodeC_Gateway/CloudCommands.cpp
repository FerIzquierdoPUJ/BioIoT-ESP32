#include "CloudCommands.h"

#include <math.h>
#include <string.h>

namespace gw {
using namespace bioiot;

namespace {
// Presencia de una clave aunque su valor sea null (containsKey esta obsoleto en 7.x).
bool hasKey(JsonObjectConst o, const char* key) {
  for (JsonPairConst kv : o)
    if (strcmp(kv.key().c_str(), key) == 0) return true;
  return false;
}

// v4 calibrationNumber(): tipado y finito; nunca convertir null/texto en cero.
bool number(JsonVariantConst v) { return v.is<double>() && isfinite(v.as<double>()); }

// Parametro opcional: ausente => ok sin marcar; presente invalido => error.
bool takeParam(JsonObjectConst doc, const char* key, CalibrationCmd& c, uint8_t index) {
  if (!hasKey(doc, key)) return true;
  if (!number(doc[key])) return false;  // null explicito o texto = invalido (v4)
  c.params[index] = doc[key].as<float>();
  c.present |= uint8_t(1u << index);
  return true;
}

void copyId(CloudCommand& c, JsonObjectConst doc, bool strict) {
  JsonVariantConst id = doc["commandId"];
  if (id.isNull()) return;
  if (!id.is<const char*>()) {
    if (strict) c.act.parseError = "invalid_command_id";
    return;
  }
  const char* s = id.as<const char*>();
  if (strict && strlen(s) > 64) { c.act.parseError = "invalid_command_id"; return; }
  c.hasCommandId = true;
  strncpy(c.commandId, s, 64);  // calibraciones v4: se trunca a 64
  c.commandId[64] = 0;
}

void parseActuator(CloudCommand& c, JsonObjectConst doc, const char* action) {
  c.kind = CloudKind::Actuator;
  copyId(c, doc, true);
  c.act.hasCommandId = c.hasCommandId;
  strncpy(c.act.commandId, c.commandId, 64);
  if (strcmp(action, "all_off") == 0) { c.act.allOff = true; return; }
  const char* target = doc["target"] | "";
  if (doc["expiresAt"].is<uint64_t>()) {
    c.act.hasExpiresAt = true;
    c.act.expiresAt = doc["expiresAt"].as<uint64_t>();
  }
  if (strcmp(target, "compressor") == 0) {
    c.act.target = kActCompressor;
    if (!doc["on"].is<bool>()) { c.act.fieldError = "on_must_be_boolean"; return; }
    c.act.on = doc["on"].as<bool>();
  } else if (strcmp(target, "led") == 0) {
    c.act.target = kActLed;
    const char* keys[] = {"r", "g", "b", "brightness"};
    uint8_t values[4] = {};
    for (uint8_t i = 0; i < 4; ++i) {
      if (!doc[keys[i]].is<int>() || doc[keys[i]].as<int>() < 0 || doc[keys[i]].as<int>() > 255) {
        c.act.fieldError = "rgb_and_brightness_must_be_0_to_255";
        return;
      }
      values[i] = uint8_t(doc[keys[i]].as<int>());
    }
    c.act.r = values[0]; c.act.g = values[1]; c.act.b = values[2]; c.act.brightness = values[3];
  } else {
    c.act.fieldError = "unknown_target";
  }
}

void parseCalibration(CloudCommand& c, JsonObjectConst doc, const char* action) {
  copyId(c, doc, false);
  const char* sensor = doc["sensor"] | "";
  strncpy(c.sensor, sensor, sizeof(c.sensor) - 1);
  if (doc["expiresAt"].is<uint64_t>()) { c.hasExpiresAt = true; c.expiresAt = doc["expiresAt"].as<uint64_t>(); }
  c.cal.target = calTargetFromName(sensor);
  c.node = calTargetOwner(c.cal.target);
  if (strcmp(action, "reset_all") == 0) {
    c.kind = CloudKind::ResetAll;
    c.cal.op = kCalOpResetAll;
    c.cal.target = kTargetAll;
    strncpy(c.sensor, "all", sizeof(c.sensor) - 1);
    return;
  }
  c.kind = CloudKind::Calibration;
  if (strcmp(action, "calibrate") == 0) {
    c.cal.op = kCalOpO2Air;
    if ((c.cal.target != kTargetO2Gas1 && c.cal.target != kTargetO2Gas2) || !number(doc["reference_vol"]) ||
        fabsf(doc["reference_vol"].as<float>() - 20.9f) > 0.0001f) {
      c.rejectStatus = "rejected_reference_use_20.9";
      return;
    }
    c.cal.params[calp::kReference] = doc["reference_vol"].as<float>();
    c.cal.present = 1;
    return;
  }
  c.cal.op = strcmp(action, "set") == 0 ? kCalOpSet : kCalOpReset;
  if (c.cal.target == kTargetNone) { c.rejectStatus = "rejected_invalid_parameters"; return; }
  if (c.cal.op == kCalOpReset) return;
  bool ok = true;
  switch (c.cal.target) {
    case kTargetPh: case kTargetTurb: case kTargetTemp:
      ok = takeParam(doc, "m", c.cal, calp::kM) && takeParam(doc, "b", c.cal, calp::kB);
      break;
    case kTargetDo:
      ok = takeParam(doc, "m", c.cal, calp::kM);  // v4 ignora "b" en DO
      break;
    case kTargetCo2Both: case kTargetCo2_1: case kTargetCo2_2:
      ok = takeParam(doc, "a", c.cal, calp::kA) && takeParam(doc, "b", c.cal, calp::kLegacyB) &&
           takeParam(doc, "zero_point_v", c.cal, calp::kZero) &&
           takeParam(doc, "reaction_voltage_v", c.cal, calp::kReaction);
      if (ok && hasKey(doc, "enabled")) {
        if (!doc["enabled"].is<bool>()) ok = false;
        else { c.cal.enabledPresent = 1; c.cal.enabledValue = doc["enabled"].as<bool>() ? 1 : 0; }
      }
      break;
    case kTargetO2Gas1: case kTargetO2Gas2:
      ok = takeParam(doc, "gain", c.cal, calp::kGain) && takeParam(doc, "offset", c.cal, calp::kOffset);
      break;
    default:
      ok = false;
  }
  if (!ok) c.rejectStatus = "rejected_invalid_parameters";
}

const struct {
  const char* key;
  uint8_t sensor;
} kExpectedKeys[] = {
    {"ph", kSensorPh}, {"co2_1", kSensorCo2_1}, {"co2_2", kSensorCo2_2}, {"turbidity", kSensorTurbidity},
    {"dissolved_oxygen", kSensorDissolvedOxygen}, {"tds", kSensorTds}, {"temperature", kSensorTemperature},
    {"color_1", kSensorColor1}, {"color_2", kSensorColor2}, {"light_1", kSensorLight1},
    {"light_2", kSensorLight2}, {"o2_gas_1", kSensorO2Gas1}, {"o2_gas_2", kSensorO2Gas2},
};
}  // namespace

uint8_t nodeFromName(const char* name) {
  if (!name) return kNodeNone;
  if (!strcmp(name, "node_a") || !strcmp(name, "a")) return kNodeA;
  if (!strcmp(name, "node_b") || !strcmp(name, "b")) return kNodeB;
  if (!strcmp(name, "gateway") || !strcmp(name, "c") || !strcmp(name, "node_c")) return kNodeGateway;
  return kNodeNone;
}

CloudCommand parseCloudCommand(JsonObjectConst doc) {
  CloudCommand c;
  if (doc.isNull()) return c;
  const char* action = doc["action"] | "";
  if (!strcmp(action, "diagnostics")) {
    c.kind = CloudKind::Diagnostics;
    copyId(c, doc, false);
    c.scope = diagScopeFromName(doc["scope"] | "full");
    if (c.scope == 255) c.rejectStatus = "unknown_scope";
  } else if (!strcmp(action, "control") || !strcmp(action, "all_off")) {
    parseActuator(c, doc, action);
  } else if (!strcmp(action, "set") || !strcmp(action, "reset") || !strcmp(action, "reset_all") ||
             !strcmp(action, "calibrate")) {
    parseCalibration(c, doc, action);
  } else if (!strcmp(action, "reboot")) {
    c.kind = CloudKind::Reboot;
    copyId(c, doc, false);
    const char* n = doc["node"] | "gateway";
    if (!strcmp(n, "all")) c.rebootTarget = kRebootAll;
    else {
      const uint8_t id = nodeFromName(n);
      if (id == kNodeA) c.rebootTarget = kRebootNodeA;
      else if (id == kNodeB) c.rebootTarget = kRebootNodeB;
      else if (id == kNodeGateway) c.rebootTarget = kRebootGateway;
      else c.rejectStatus = "unknown_node";
    }
  } else if (!strcmp(action, "config")) {
    c.kind = CloudKind::Config;
    copyId(c, doc, false);
    c.node = nodeFromName(doc["node"] | "");
    if (c.node != kNodeA && c.node != kNodeB) { c.rejectStatus = "unknown_node"; return c; }
    if (hasKey(doc, "report_interval_s")) {
      if (!doc["report_interval_s"].is<uint32_t>()) { c.rejectStatus = "rejected_invalid_parameters"; return c; }
      const uint32_t ms = doc["report_interval_s"].as<uint32_t>() * 1000UL;
      if (ms < kMinReportIntervalMs || ms > kMaxReportIntervalMs) { c.rejectStatus = "rejected_invalid_parameters"; return c; }
      c.cfg.present |= 1;
      c.cfg.reportIntervalMs = ms;
    }
    JsonObjectConst expected = doc["expected"];
    if (!expected.isNull()) {
      uint16_t mask = 0, touched = 0;
      for (const auto& k : kExpectedKeys) {
        JsonVariantConst v = expected[k.key];
        if (v.isNull()) continue;
        if (!v.is<bool>() || sensorInfo(k.sensor).owner != c.node) { c.rejectStatus = "rejected_invalid_parameters"; return c; }
        touched |= uint16_t(1u << k.sensor);
        if (v.as<bool>()) mask |= uint16_t(1u << k.sensor);
      }
      // El gateway combina con la mascara vigente del nodo (STATUS/telemetria).
      c.cfg.present |= 2;
      c.cfg.expectedMask = mask;
      c.expectedTouched = touched;
    }
    if (!c.cfg.present) c.rejectStatus = "rejected_invalid_parameters";
  } else if (!strcmp(action, "calibration_import")) {
    c.kind = CloudKind::CalImport;
    copyId(c, doc, false);
    c.force = doc["force"] | false;
    const CalImportResult r = parseCalibrationExport(doc["export"].as<JsonObjectConst>(), c.importA, c.importB);
    c.importHasA = r.hasA && r.aValid;
    c.importHasB = r.hasB && r.bValid;
    if (!r.formatOk || (r.hasA && !r.aValid) || (r.hasB && !r.bValid) || (!c.importHasA && !c.importHasB))
      c.rejectStatus = "rejected_invalid_parameters";
  } else if (!strcmp(action, "calibration_export")) {
    c.kind = CloudKind::CalExport;
    copyId(c, doc, false);
  } else {
    c.kind = CloudKind::Unknown;
  }
  return c;
}

}  // namespace gw
