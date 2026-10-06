#include "CalibrationLogicA.h"

#include <math.h>

namespace nodea {
using namespace bioiot;

namespace {
bool has(const CalibrationCmd& c, uint8_t p) { return (c.present >> p) & 1u; }

// setCo2Profile() de v4. Los parametros ya llegan tipados y finitos.
bool setCo2Profile(const CalibrationCmd& cmd, Co2Calibration& target) {
  Co2Calibration candidate = target;
  const bool legacy = has(cmd, calp::kA) || has(cmd, calp::kLegacyB);
  const bool vendor = has(cmd, calp::kZero) || has(cmd, calp::kReaction);
  if (legacy && vendor) return false;
  if (legacy) {
    if (has(cmd, calp::kA)) candidate.a = cmd.params[calp::kA];
    if (has(cmd, calp::kLegacyB)) candidate.b = cmd.params[calp::kLegacyB];
    candidate.mode = 1; candidate.user = 1; candidate.provisional = 0;
  } else if (vendor) {
    if (has(cmd, calp::kZero)) candidate.zero_point_v = cmd.params[calp::kZero];
    if (has(cmd, calp::kReaction)) candidate.reaction_voltage_v = cmd.params[calp::kReaction];
    candidate.mode = 0; candidate.user = 1; candidate.provisional = 0;
  } else if (!cmd.enabledPresent) {
    return false;
  }
  candidate.enabled = 1;
  if (cmd.enabledPresent) candidate.enabled = cmd.enabledValue ? 1 : 0;
  if (!validCo2Calibration(candidate)) return false;
  target = candidate;
  return true;
}
}  // namespace

void loadDefaultsA(NodeACal& c) {
  const uint32_t legacyVersion = c.legacyVersion, legacySchema = c.legacySchema, nodeRev = c.nodeRevision;
  uint16_t rev[kCalACount];
  for (uint8_t i = 0; i < kCalACount; ++i) rev[i] = c.rev[i];
  c = NodeACal();  // curvas iniciales vigentes de v4
  c.legacyVersion = legacyVersion;
  c.legacySchema = legacySchema;
  c.nodeRevision = nodeRev;
  for (uint8_t i = 0; i < kCalACount; ++i) c.rev[i] = rev[i];
}

void bumpRevisionsA(NodeACal& c, uint8_t changedMask) {
  if (!changedMask) return;
  for (uint8_t i = 0; i < kCalACount; ++i)
    if (changedMask & (1u << i)) c.rev[i]++;
  c.nodeRevision++;
}

uint16_t sensorRevisionForTarget(const NodeACal& c, uint8_t target) {
  switch (target) {
    case kTargetPh: return c.rev[kCalPh];
    case kTargetTurb: return c.rev[kCalTurb];
    case kTargetDo: return c.rev[kCalDo];
    case kTargetTemp: return c.rev[kCalTemp];
    case kTargetCo2_1: case kTargetCo2Both: return c.rev[kCalCo2_1];
    case kTargetCo2_2: return c.rev[kCalCo2_2];
    default: return 0;
  }
}

CalApplyResult applyCalibrationA(const NodeACal& current, const CalibrationCmd& cmd, bool storageWritable,
                                 NodeACal& candidate) {
  CalApplyResult r;
  candidate = current;
  if (cmd.op == kCalOpO2Air) { r.status = kStRejectedUnsupported; return r; }
  if (!storageWritable) { r.status = kStRejectedBusyOrFutureSchema; return r; }

  if (cmd.op == kCalOpImport) {
    NodeACal imported;
    if (cmd.blobLen != kNodeACalEncodedSize || !decodeNodeACal(cmd.blob, cmd.blobLen, imported)) {
      r.status = kStRejectedInvalidParameters;
      return r;
    }
    if (nodeACalHasUserData(current) && !cmd.force) { r.status = kStRejectedImportNotAllowed; return r; }
    candidate = imported;
    candidate.importedMask = (1u << kCalACount) - 1;
    for (uint8_t i = 0; i < kCalACount; ++i) candidate.rev[i] = current.rev[i];
    candidate.nodeRevision = current.nodeRevision;
    bumpRevisionsA(candidate, (1u << kCalACount) - 1);
    r.changedMask = (1u << kCalACount) - 1;
    r.outcome = kOutcomeApplied; r.status = kStImported; r.needsPersist = true;
    return r;
  }

  const bool set = cmd.op == kCalOpSet;
  const bool reset = cmd.op == kCalOpReset;
  bool ok = true;
  uint8_t changed = 0;
  if (cmd.op == kCalOpResetAll) {
    if (cmd.target != kTargetAll) { r.status = kStRejectedInvalidParameters; return r; }
    loadDefaultsA(candidate);
    changed = (1u << kCalACount) - 1;
  } else if (cmd.target == kTargetCo2Both || cmd.target == kTargetCo2_1 || cmd.target == kTargetCo2_2) {
    const uint8_t first = cmd.target == kTargetCo2_2 ? 1 : 0;
    const uint8_t last = cmd.target == kTargetCo2_1 ? 0 : 1;
    for (uint8_t i = first; i <= last; ++i) {
      if (reset) candidate.co2[i] = Co2Calibration();
      else if (!setCo2Profile(cmd, candidate.co2[i])) ok = false;
      changed |= 1u << (kCalCo2_1 + i);
    }
    if (cmd.target == kTargetCo2Both && ok) {
      // Instantanea legacy v4: co2_a/co2_b/co2_en siguen al perfil #1.
      candidate.co2_a = candidate.co2[0].a;
      candidate.co2_b = candidate.co2[0].b;
      candidate.co2_enabled = candidate.co2[0].mode == 1 && candidate.co2[0].enabled;
    }
  } else {
    float* m = nullptr;
    float* b = nullptr;
    float defaultM = 0, defaultB = 0;
    uint8_t* userFlag = nullptr;
    uint8_t entry = 0;
    switch (cmd.target) {
      case kTargetPh:
        m = &candidate.ph_m; b = &candidate.ph_b; defaultM = kPhCalMDefault; defaultB = kPhCalBDefault;
        userFlag = &candidate.ph_user; entry = kCalPh;
        break;
      case kTargetTurb:
        m = &candidate.turb_m; b = &candidate.turb_b; defaultM = kTurbCalMDefault; defaultB = kTurbCalBDefault;
        userFlag = &candidate.turb_user; entry = kCalTurb;
        break;
      case kTargetDo:
        m = &candidate.do_m; defaultM = kDoCalMDefault; userFlag = &candidate.do_user; entry = kCalDo;
        break;
      case kTargetTemp:
        m = &candidate.temp_m; b = &candidate.temp_b; defaultM = kTempCalMDefault; defaultB = kTempCalBDefault;
        userFlag = &candidate.temp_user; entry = kCalTemp;
        break;
      default:
        r.status = kStRejectedUnknownTarget;
        return r;
    }
    if (reset) { *m = defaultM; if (b) *b = defaultB; }
    if (set) {
      if (!has(cmd, calp::kM) && (!b || !has(cmd, calp::kB))) ok = false;
      if (has(cmd, calp::kM)) *m = cmd.params[calp::kM];
      if (b && has(cmd, calp::kB)) *b = cmd.params[calp::kB];
      if (!isfinite(*m) || (b && !isfinite(*b))) ok = false;
    }
    if (!set && !reset) ok = false;
    if (ok) {
      *userFlag = set ? 1 : 0;  // ph_user/do_user de v4; turb/temp: procedencia nueva
      candidate.importedMask &= uint8_t(~(1u << entry));
    }
    changed = uint8_t(1u << entry);
  }
  if (!ok) {
    candidate = current;  // se revierte el comando completo
    r.status = kStRejectedInvalidParameters;
    return r;
  }
  if (cmd.op == kCalOpResetAll) candidate.importedMask = 0;
  else if (cmd.target == kTargetCo2Both || cmd.target == kTargetCo2_1 || cmd.target == kTargetCo2_2)
    candidate.importedMask &= uint8_t(~changed);
  bumpRevisionsA(candidate, changed);
  r.changedMask = changed;
  r.outcome = kOutcomeApplied;
  r.status = kStSaved;
  r.needsPersist = true;
  return r;
}

}  // namespace nodea
