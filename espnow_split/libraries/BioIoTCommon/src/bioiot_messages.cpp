#include "bioiot_messages.h"

#include <math.h>
#include <string.h>

#include "bioiot_codec.h"

namespace bioiot {

namespace {
size_t done(const Writer& w) { return w.ok() ? w.size() : 0; }
}  // namespace

// ---------------- HELLO ----------------
size_t encodeHello(const HelloMsg& m, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u8(m.role); w.u16(m.fwVersion); w.u16(m.flags); w.u8(m.channel); w.u32(m.uptimeMs);
  w.u8(m.resetReason);
  return done(w);
}
bool decodeHello(const uint8_t* in, size_t len, HelloMsg& m) {
  Reader r(in, len);
  HelloMsg h;
  h.role = r.u8(); h.fwVersion = r.u16(); h.flags = r.u16(); h.channel = r.u8(); h.uptimeMs = r.u32();
  h.resetReason = r.u8();
  if (!r.done() || h.role == kNodeNone || h.role > kMaxNodeId || h.channel > 14) return false;
  m = h;
  return true;
}

// ---------------- TIME_SYNC ----------------
size_t encodeTimeSync(const TimeSyncMsg& m, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u8(m.utcValid); w.i64(m.utcMs); w.u32(m.gatewayUptimeMs); w.u32(m.utcAgeMs);
  return done(w);
}
bool decodeTimeSync(const uint8_t* in, size_t len, TimeSyncMsg& m) {
  Reader r(in, len);
  TimeSyncMsg t;
  t.utcValid = r.u8(); t.utcMs = r.i64(); t.gatewayUptimeMs = r.u32(); t.utcAgeMs = r.u32();
  if (!r.done() || t.utcValid > 1) return false;
  // 2020-09-13 .. 2100: fuera de ese rango no es una hora valida.
  if (t.utcValid && (t.utcMs < 1600000000000LL || t.utcMs > 4102444800000LL)) return false;
  m = t;
  return true;
}

// ---------------- ACK ----------------
size_t encodeAck(const AckMsg& m, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u32(m.ackSeq); w.u8(m.ackType); w.u8(m.status);
  return done(w);
}
bool decodeAck(const uint8_t* in, size_t len, AckMsg& m) {
  Reader r(in, len);
  AckMsg a;
  a.ackSeq = r.u32(); a.ackType = r.u8(); a.status = r.u8();
  if (!r.done() || a.ackType == 0 || a.ackType > kMaxMsgType || a.status > kAckUnsupported) return false;
  m = a;
  return true;
}

// ---------------- STATUS ----------------
size_t encodeStatus(const StatusMsg& m, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u32(m.uptimeMs); w.u32(m.freeHeap); w.u32(m.minFreeHeap); w.u32(m.maxAllocHeap);
  w.u8(m.resetReason); w.u16(m.flags); w.u32(m.warmupRemainingMs); w.u8(m.queueDepth);
  w.u16(m.telemetryDropped); w.u16(m.txFailures); w.u16(m.retries); w.u16(m.rxRejected);
  w.u16(m.channelHunts); w.u8(m.channel); w.i8(m.rssi); w.u32(m.reportIntervalMs);
  w.u16(m.diagFlags); w.u32(m.diagCounter); w.u8(m.diagError); w.u32(m.diagSampledAgeMs);
  return done(w);
}
bool decodeStatus(const uint8_t* in, size_t len, StatusMsg& m) {
  Reader r(in, len);
  StatusMsg s;
  s.uptimeMs = r.u32(); s.freeHeap = r.u32(); s.minFreeHeap = r.u32(); s.maxAllocHeap = r.u32();
  s.resetReason = r.u8(); s.flags = r.u16(); s.warmupRemainingMs = r.u32(); s.queueDepth = r.u8();
  s.telemetryDropped = r.u16(); s.txFailures = r.u16(); s.retries = r.u16(); s.rxRejected = r.u16();
  s.channelHunts = r.u16(); s.channel = r.u8(); s.rssi = r.i8(); s.reportIntervalMs = r.u32();
  s.diagFlags = r.u16(); s.diagCounter = r.u32(); s.diagError = r.u8(); s.diagSampledAgeMs = r.u32();
  if (!r.done() || s.channel > 14) return false;
  m = s;
  return true;
}

// ---------------- TELEMETRY ----------------
size_t telemetryRecordSize(const TelemetryRecord& r) { return kRecordHeaderSize + 4u * r.valueCount; }

size_t encodeTelemetry(const TelemetryMsg& m, uint8_t* out, size_t cap) {
  if (m.recordCount > kMaxRecordsPerFrame) return 0;
  Writer w(out, cap);
  w.u32(m.snapshotId); w.u8(m.part); w.u8(m.parts); w.u32(m.txUptimeMs); w.u8(m.recordCount);
  for (uint8_t i = 0; i < m.recordCount; ++i) {
    const TelemetryRecord& rec = m.records[i];
    if (rec.valueCount > kMaxRecordValues) return 0;
    w.u8(rec.sensor); w.u8(rec.quality); w.u16(rec.flags); w.u16(rec.calRevision);
    w.u32(rec.sampleUptimeMs); w.u8(rec.valueCount);
    for (uint8_t v = 0; v < rec.valueCount; ++v) w.f32(rec.vals[v]);
  }
  return done(w);
}

bool decodeTelemetry(const uint8_t* in, size_t len, TelemetryMsg& m) {
  Reader r(in, len);
  m = TelemetryMsg();
  m.snapshotId = r.u32(); m.part = r.u8(); m.parts = r.u8(); m.txUptimeMs = r.u32();
  m.recordCount = r.u8();
  if (!r.ok() || m.parts == 0 || m.parts > 4 || m.part >= m.parts || m.recordCount == 0 ||
      m.recordCount > kMaxRecordsPerFrame)
    return false;
  for (uint8_t i = 0; i < m.recordCount; ++i) {
    TelemetryRecord& rec = m.records[i];
    rec.sensor = r.u8(); rec.quality = r.u8(); rec.flags = r.u16(); rec.calRevision = r.u16();
    rec.sampleUptimeMs = r.u32(); rec.valueCount = r.u8();
    if (!r.ok() || rec.sensor >= kSensorCount || rec.quality > kQStabilizing ||
        rec.valueCount != sensorInfo(rec.sensor).valueCount)
      return false;
    for (uint8_t v = 0; v < rec.valueCount; ++v) {
      rec.vals[v] = r.f32();
      // NaN = sin valor (con quality explicita); infinito nunca es una medicion.
      if (isinf(rec.vals[v])) return false;
    }
    // Una muestra no puede ser posterior a su transmision (mismo reloj del nodo).
    if (int32_t(m.txUptimeMs - rec.sampleUptimeMs) < 0) return false;
  }
  return r.done();
}

// ---------------- CALIBRATION ----------------
size_t encodeCalibration(const CalibrationCmd& m, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u32(m.cmdId); w.u8(m.op); w.u8(m.target); w.u8(m.present);
  w.u8(uint8_t((m.enabledPresent ? 1 : 0) | (m.enabledValue ? 2 : 0))); w.u8(m.force);
  for (uint8_t i = 0; i < calp::kMaxParams; ++i)
    if (m.present & (1u << i)) w.f32(m.params[i]);
  if (m.op == kCalOpImport) {
    w.u8(m.blobLen);
    w.bytes(m.blob, m.blobLen);
  }
  return done(w);
}

bool decodeCalibration(const uint8_t* in, size_t len, CalibrationCmd& m) {
  Reader r(in, len);
  CalibrationCmd c;
  c.cmdId = r.u32(); c.op = r.u8(); c.target = r.u8(); c.present = r.u8();
  const uint8_t en = r.u8();
  c.force = r.u8();
  if (!r.ok() || c.op < kCalOpSet || c.op > kCalOpImport || c.target == kTargetNone ||
      c.target > kTargetAll || c.present >= (1u << calp::kMaxParams) || en > 3 || c.force > 1)
    return false;
  c.enabledPresent = en & 1;
  c.enabledValue = (en >> 1) & 1;
  for (uint8_t i = 0; i < calp::kMaxParams; ++i)
    if (c.present & (1u << i)) {
      c.params[i] = r.f32();
      if (!isfinite(c.params[i])) return false;
    }
  if (c.op == kCalOpImport) {
    c.blobLen = r.u8();
    if (c.blobLen != kNodeACalEncodedSize && c.blobLen != kNodeBCalEncodedSize) return false;
    if (!r.bytes(c.blob, c.blobLen)) return false;
  }
  if (!r.done()) return false;
  m = c;
  return true;
}

// ---------------- CONFIG ----------------
size_t encodeConfig(const ConfigCmd& m, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u32(m.cmdId); w.u8(m.present); w.u32(m.reportIntervalMs); w.u16(m.expectedMask);
  return done(w);
}
bool decodeConfig(const uint8_t* in, size_t len, ConfigCmd& m) {
  Reader r(in, len);
  ConfigCmd c;
  c.cmdId = r.u32(); c.present = r.u8(); c.reportIntervalMs = r.u32(); c.expectedMask = r.u16();
  if (!r.done() || c.present == 0 || c.present > 3) return false;
  if ((c.present & 1) &&
      (c.reportIntervalMs < kMinReportIntervalMs || c.reportIntervalMs > kMaxReportIntervalMs))
    return false;
  if ((c.present & 2) && c.expectedMask >= (1u << kSensorCount)) return false;
  m = c;
  return true;
}

// ---------------- COMMAND ----------------
size_t encodeGeneric(const GenericCmd& m, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u32(m.cmdId); w.u8(m.op);
  return done(w);
}
bool decodeGeneric(const uint8_t* in, size_t len, GenericCmd& m) {
  Reader r(in, len);
  GenericCmd g;
  g.cmdId = r.u32(); g.op = r.u8();
  if (!r.done() || g.op < kOpReboot || g.op > kOpSendState) return false;
  m = g;
  return true;
}

// ---------------- COMMAND_RESULT ----------------
size_t encodeCommandResult(const CommandResultMsg& m, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u32(m.cmdId); w.u8(m.msgType); w.u8(m.op); w.u8(m.target); w.u8(m.outcome); w.u8(m.status);
  w.u32(m.nodeRevision); w.u16(m.sensorRevision); w.u8(m.storageOk); w.u8(m.extra1); w.u8(m.extra2);
  return done(w);
}
bool decodeCommandResult(const uint8_t* in, size_t len, CommandResultMsg& m) {
  Reader r(in, len);
  CommandResultMsg c;
  c.cmdId = r.u32(); c.msgType = r.u8(); c.op = r.u8(); c.target = r.u8(); c.outcome = r.u8();
  c.status = r.u8(); c.nodeRevision = r.u32(); c.sensorRevision = r.u16(); c.storageOk = r.u8();
  c.extra1 = r.u8(); c.extra2 = r.u8();
  if (!r.done() || c.outcome < kOutcomeApplied || c.outcome > kOutcomeStarted ||
      c.status > kMaxStatusCode || c.storageOk > 1 || c.msgType == 0 || c.msgType > kMaxMsgType)
    return false;
  m = c;
  return true;
}

// ---------------- DIAGNOSTICS ----------------
const char* diagScopeName(uint8_t scope) {
  switch (scope) {
    case kDiagFull: return "full";
    case kDiagI2c: return "i2c";
    case kDiagAnalog: return "analog";
    case kDiagColor: return "color";
    case kDiagSystem: return "system";
    case kDiagQuick: return "quick";
    case kDiagRecover: return "i2c_recover";
    case kDiagTemperature: return "temperature";
    case kDiagO2: return "o2";
    default: return "unknown";
  }
}
uint8_t diagScopeFromName(const char* name) {
  if (!name) return 255;
  for (uint8_t i = 0; i <= kMaxDiagScope; ++i)
    if (strcmp(name, diagScopeName(i)) == 0) return i;
  return 255;
}

size_t encodeDiagRequest(const DiagRequestMsg& m, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u32(m.requestId); w.u8(m.scope);
  return done(w);
}
bool decodeDiagRequest(const uint8_t* in, size_t len, DiagRequestMsg& m) {
  Reader r(in, len);
  DiagRequestMsg d;
  d.requestId = r.u32(); d.scope = r.u8();
  if (!r.done() || d.scope > kMaxDiagScope) return false;
  m = d;
  return true;
}

size_t encodeDiagFragment(const DiagFragmentMsg& m, uint8_t* out, size_t cap) {
  if (m.chunkLen > kDiagChunkSize || (m.chunkLen && !m.chunk)) return 0;
  Writer w(out, cap);
  w.u32(m.requestId); w.u8(m.index); w.u8(m.count); w.u16(m.totalLen);
  w.bytes(m.chunk, m.chunkLen);
  return done(w);
}
bool decodeDiagFragment(const uint8_t* in, size_t len, DiagFragmentMsg& m) {
  if (len < kDiagFragmentHeader) return false;
  Reader r(in, len);
  DiagFragmentMsg f;
  f.requestId = r.u32(); f.index = r.u8(); f.count = r.u8(); f.totalLen = r.u16();
  f.chunkLen = uint8_t(len - kDiagFragmentHeader);
  f.chunk = in + kDiagFragmentHeader;
  if (!r.ok() || f.count == 0 || f.count > kDiagMaxFragments || f.index >= f.count ||
      f.totalLen == 0 || f.totalLen > kDiagMaxBytes)
    return false;
  // Todos los fragmentos son completos salvo el ultimo; el total debe cuadrar.
  const size_t expectedCount = (f.totalLen + kDiagChunkSize - 1) / kDiagChunkSize;
  if (expectedCount != f.count) return false;
  const size_t expectedLen = f.index + 1 < f.count ? kDiagChunkSize : f.totalLen - kDiagChunkSize * (f.count - 1);
  if (f.chunkLen != expectedLen) return false;
  m = f;
  return true;
}

// ---------------- CALIBRATION_STATE ----------------
size_t encodeCalState(const CalStateMsg& m, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u8(m.node); w.u8(m.storageState); w.u8(m.formatVersion); w.u8(m.blobLen);
  w.bytes(m.blob, m.blobLen);
  return done(w);
}
bool decodeCalState(const uint8_t* in, size_t len, CalStateMsg& m) {
  Reader r(in, len);
  CalStateMsg c;
  c.node = r.u8(); c.storageState = r.u8(); c.formatVersion = r.u8(); c.blobLen = r.u8();
  if (!r.ok() || c.formatVersion != kCalFormatVersion || c.storageState > kCalStorageDefaults) return false;
  const size_t expected = c.node == kNodeA ? kNodeACalEncodedSize : c.node == kNodeB ? kNodeBCalEncodedSize : 0;
  if (!expected || c.blobLen != expected || !r.bytes(c.blob, c.blobLen) || !r.done()) return false;
  m = c;
  return true;
}

}  // namespace bioiot
