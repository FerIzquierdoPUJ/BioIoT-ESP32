#include "CalibrationLogicB.h"

#include <math.h>

namespace nodeb {
using namespace bioiot;

void bumpRevisionsB(NodeBCal& c, uint8_t changedMask) {
  if (!changedMask) return;
  for (uint8_t i = 0; i < kCalBCount; ++i)
    if (changedMask & (1u << i)) c.rev[i]++;
  c.nodeRevision++;
}

void recordAirCalibration(NodeBCal& c, uint8_t sensor, int64_t utcSeconds) {
  c.o2[sensor].reference = 20.9f;
  c.o2[sensor].commandUtc = utcSeconds > 0 ? utcSeconds : 0;  // 0 = sin UTC valido; nunca inventado
  c.o2[sensor].commandCount++;
  bumpRevisionsB(c, uint8_t(1u << sensor));
}

uint16_t sensorRevisionForTargetB(const NodeBCal& c, uint8_t target) {
  if (target == kTargetO2Gas1) return c.rev[kCalO2_1];
  if (target == kTargetO2Gas2) return c.rev[kCalO2_2];
  return 0;
}

void reportStorageUnavailableB(CalApplyResultB& r, bool storageMounted) {
  if (storageMounted || r.outcome != kOutcomeRejected) return;
  if (r.status == kStRejectedBusyOrFutureSchema || r.status == kStRejectedWarmingUpOrBusy)
    r.status = kStRejectedStorageFailed;
}

CalApplyResultB applyCalibrationB(const NodeBCal& current, const CalibrationCmd& cmd, bool storageWritable,
                                  bool warming, bool busy, NodeBCal& candidate) {
  CalApplyResultB r;
  candidate = current;
  const bool isO2Target = cmd.target == kTargetO2Gas1 || cmd.target == kTargetO2Gas2;
  const uint8_t i = cmd.target == kTargetO2Gas2 ? 1 : 0;

  if (cmd.op == kCalOpO2Air) {
    if (!isO2Target || !(cmd.present & 1u) || fabsf(cmd.params[calp::kReference] - 20.9f) > 0.0001f) {
      r.status = kStRejectedReferenceUse209;
      return r;
    }
    if (warming || busy || !storageWritable) { r.status = kStRejectedWarmingUpOrBusy; return r; }
    r.outcome = kOutcomeStarted;
    r.status = kStCommandSent;
    r.startAirCalibration = true;
    r.airSensor = i;
    return r;
  }
  if (!storageWritable || busy) { r.status = kStRejectedBusyOrFutureSchema; return r; }

  if (cmd.op == kCalOpImport) {
    NodeBCal imported;
    if (cmd.blobLen != kNodeBCalEncodedSize || !decodeNodeBCal(cmd.blob, cmd.blobLen, imported)) {
      r.status = kStRejectedInvalidParameters;
      return r;
    }
    if (nodeBCalHasUserData(current) && !cmd.force) { r.status = kStRejectedImportNotAllowed; return r; }
    candidate = imported;
    candidate.importedMask = (1u << kCalBCount) - 1;
    for (uint8_t k = 0; k < kCalBCount; ++k) candidate.rev[k] = current.rev[k];
    candidate.nodeRevision = current.nodeRevision;
    bumpRevisionsB(candidate, (1u << kCalBCount) - 1);
    r.changedMask = (1u << kCalBCount) - 1;
    r.resetFilters = 3;
    r.outcome = kOutcomeApplied; r.status = kStImported; r.needsPersist = true;
    return r;
  }

  if (cmd.op == kCalOpResetAll) {
    if (cmd.target != kTargetAll) { r.status = kStRejectedInvalidParameters; return r; }
    // Resetea correcciones del host, no la calibracion interna del modulo.
    for (uint8_t k = 0; k < 2; ++k) { candidate.o2[k].gain = 1; candidate.o2[k].offset = 0; }
    candidate.importedMask = 0;
    r.changedMask = 3;
    r.resetFilters = 3;
  } else if (isO2Target) {
    if (cmd.op == kCalOpReset) {
      candidate.o2[i].gain = 1;
      candidate.o2[i].offset = 0;
    } else if (cmd.op == kCalOpSet) {
      const bool hasGain = cmd.present & (1u << calp::kGain);
      const bool hasOffset = cmd.present & (1u << calp::kOffset);
      bool ok = hasGain || hasOffset;
      if (hasGain) {
        if (!isfinite(cmd.params[calp::kGain]) || cmd.params[calp::kGain] <= 0) ok = false;
        else candidate.o2[i].gain = cmd.params[calp::kGain];
      }
      if (hasOffset) {
        if (!isfinite(cmd.params[calp::kOffset])) ok = false;
        else candidate.o2[i].offset = cmd.params[calp::kOffset];
      }
      if (!ok) {
        candidate = current;
        r.status = kStRejectedInvalidParameters;
        return r;
      }
    } else {
      r.status = kStRejectedInvalidParameters;
      return r;
    }
    candidate.importedMask &= uint8_t(~(1u << i));
    r.changedMask = uint8_t(1u << i);
    r.resetFilters = uint8_t(1u << i);
  } else {
    r.status = kStRejectedUnknownTarget;
    return r;
  }
  bumpRevisionsB(candidate, r.changedMask);
  r.outcome = kOutcomeApplied;
  r.status = kStSaved;
  r.needsPersist = true;
  return r;
}

}  // namespace nodeb
