#include "GatewayState.h"

#include <math.h>
#include <string.h>

namespace gw {
using namespace bioiot;

const char* sampleStateName(uint8_t s) {
  switch (s) {
    case kSampleFresh: return "fresh";
    case kSampleStale: return "stale";
    case kSampleNodeOffline: return "node_offline";
    default: return "never_received";
  }
}

const char* cmdStateName(uint8_t s) {
  switch (s) {
    case kCmdPending: return "pending";
    case kCmdDelivered: return "delivered";
    case kCmdApplied: return "applied";
    case kCmdRejected: return "rejected";
    case kCmdFailed: return "failed";
    case kCmdExpired: return "expired";
    case kCmdTimeout: return "timeout";
    case kCmdNodeRebooted: return "node_rebooted";
    case kCmdStarted: return "started";
    default: return "unknown";
  }
}

bool cmdStateFinal(uint8_t s) { return s != kCmdPending && s != kCmdDelivered; }

const char* diagPartResultName(uint8_t r) {
  switch (r) {
    case 0: return "pending";
    case 1: return "ok";
    case 2: return "timeout";
    case 3: return "node_offline";
    case 4: return "rejected";
    default: return "unknown";
  }
}

// ---------------- CommandTracker ----------------
TrackedCommand* CommandTracker::add(uint32_t cmdId, uint8_t node, uint8_t msgType, uint8_t op, uint8_t target,
                                    const char* sensor, const char* cloudId, bool hasCloudId, uint32_t groupId,
                                    int64_t nowMono, int64_t deadlineMono) {
  TrackedCommand* slot = nullptr;
  for (auto& c : items_)
    if (!c.used) { slot = &c; break; }
  if (!slot) {
    // Reutiliza el mas antiguo ya final y publicado; nunca uno pendiente.
    for (auto& c : items_)
      if (cmdStateFinal(c.state) && !c.needsPublish && (!slot || c.createdMono < slot->createdMono)) slot = &c;
    if (!slot) { dropped++; return nullptr; }
  }
  *slot = TrackedCommand();
  slot->used = true;
  slot->cmdId = cmdId; slot->node = node; slot->msgType = msgType; slot->op = op; slot->target = target;
  slot->groupId = groupId;
  strncpy(slot->sensor, sensor ? sensor : "", sizeof(slot->sensor) - 1);
  slot->hasCloudId = hasCloudId;
  if (hasCloudId) strncpy(slot->cloudId, cloudId, 64);
  slot->createdMono = nowMono;
  slot->deadlineMono = deadlineMono;
  slot->needsPublish = true;  // primer evento: pending
  return slot;
}

TrackedCommand* CommandTracker::find(uint32_t cmdId) {
  for (auto& c : items_)
    if (c.used && c.cmdId == cmdId) return &c;
  return nullptr;
}

void CommandTracker::onDelivery(uint32_t cmdId, Delivery d, int64_t nowMono) {
  TrackedCommand* c = find(cmdId);
  if (!c || cmdStateFinal(c->state)) return;
  switch (d) {
    case Delivery::Delivered:
      if (c->state == kCmdPending) { c->state = kCmdDelivered; c->deadlineMono = nowMono; }
      return;  // el resultado de ejecucion llega en COMMAND_RESULT
    case Delivery::Expired: c->state = kCmdTimeout; break;
    case Delivery::Failed: c->state = kCmdTimeout; break;
    case Delivery::PeerRebooted: c->state = kCmdNodeRebooted; break;
    case Delivery::Rejected: c->state = kCmdRejected; break;
    case Delivery::Dropped: c->state = kCmdFailed; break;
  }
  c->needsPublish = true;
}

void CommandTracker::onResult(uint8_t node, const CommandResultMsg& r) {
  TrackedCommand* c = find(r.cmdId);
  if (!c || c->node != node) return;
  if (cmdStateFinal(c->state) && c->state != kCmdStarted) return;  // duplicado: ya registrado
  c->status = r.status;
  c->nodeRevision = r.nodeRevision;
  c->sensorRevision = r.sensorRevision;
  c->storageOk = r.storageOk;
  c->extra1 = r.extra1;
  c->extra2 = r.extra2;
  switch (r.outcome) {
    case kOutcomeApplied: c->state = kCmdApplied; break;
    case kOutcomeRejected: c->state = kCmdRejected; break;
    case kOutcomeFailed: c->state = kCmdFailed; break;
    default: c->state = kCmdStarted; break;
  }
  c->needsPublish = true;
}

void CommandTracker::expire(int64_t nowMono, int64_t resultTimeoutMs) {
  for (auto& c : items_) {
    if (!c.used) continue;
    // Pendiente: el Endpoint informa el vencimiento de radio; esto cubre el caso
    // de un comando nunca encolado. Entregado sin resultado: timeout de ejecucion.
    if (c.state == kCmdPending && nowMono > c.deadlineMono + 5000) { c.state = kCmdTimeout; c.needsPublish = true; }
    else if (c.state == kCmdDelivered && nowMono - c.deadlineMono > resultTimeoutMs) {
      c.state = kCmdTimeout; c.needsPublish = true;
    }
  }
}

TrackedCommand* CommandTracker::nextToPublish() {
  TrackedCommand* best = nullptr;
  for (auto& c : items_)
    if (c.used && c.needsPublish && (!best || c.createdMono < best->createdMono)) best = &c;
  return best;
}

bool CommandTracker::groupFinal(uint32_t groupId, uint8_t& applied, uint8_t& total) const {
  applied = total = 0;
  bool final = true;
  for (const auto& c : items_) {
    if (!c.used || c.groupId != groupId || !groupId) continue;
    total++;
    if (c.state == kCmdApplied) applied++;
    if (!cmdStateFinal(c.state)) final = false;
  }
  return total > 0 && final;
}

// ---------------- GatewayState ----------------
bool GatewayState::nodeOnline(uint8_t nodeId, int64_t nowMono, int64_t linkTimeoutMs) const {
  const NodeView& n = node(nodeId);
  return n.everRx && nowMono - n.lastRxMono < linkTimeoutMs;
}

void GatewayState::touch(uint8_t nodeId, int64_t rxMono, int8_t rssi) {
  NodeView& n = node(nodeId);
  n.everRx = true;
  n.lastRxMono = rxMono;
  n.rssi = rssi;
}

uint8_t GatewayState::onTelemetry(uint8_t nodeId, const TelemetryMsg& t, int64_t rxMono) {
  NodeView& n = node(nodeId);
  n.telemetryFrames++;
  uint8_t accepted = 0;
  for (uint8_t i = 0; i < t.recordCount; ++i) {
    const TelemetryRecord& r = t.records[i];
    if (sensorInfo(r.sensor).owner != nodeId) { n.rejectedRecords++; continue; }
    if (!(r.flags & kRecObserved) && r.quality == kQNotSampled) {
      // El nodo aun no adquirio este sensor: se conserva el estado "nunca recibido".
      if (!latest_[r.sensor].present) latest_[r.sensor].node = nodeId;
      accepted++;
      continue;
    }
    SensorSample s;
    s.present = true;
    s.quality = r.quality;
    s.flags = r.flags;
    s.calRevision = r.calRevision;
    s.node = nodeId;
    s.valueCount = r.valueCount;
    s.sampleMonoMs = rxMono - int64_t(uint32_t(t.txUptimeMs - r.sampleUptimeMs));
    for (uint8_t v = 0; v < r.valueCount; ++v) s.vals[v] = r.vals[v];
    SensorSample& cur = latest_[r.sensor];
    if (!cur.present || s.sampleMonoMs >= cur.sampleMonoMs) cur = s;
    backfill(r.sensor, s);
    accepted++;
  }
  return accepted;
}

void GatewayState::backfill(uint8_t sensor, const SensorSample& s) {
  // Rellena instantaneas aun no publicadas que quedaron sin dato fresco (p. ej. un
  // barrido Wi-Fi interrumpio ESP-NOW y el nodo reenvio su cola despues).
  for (auto& snap : snapshots_) {
    if (!snap.used || snap.published || snap.state[sensor] == kSampleFresh) continue;
    if (s.sampleMonoMs > snap.takenMono || snap.takenMono - s.sampleMonoMs > maxAgeMs[sensor]) continue;
    const SensorSample& existing = snap.sensors[sensor];
    if (existing.present && existing.sampleMonoMs >= s.sampleMonoMs) continue;
    snap.sensors[sensor] = s;
    snap.state[sensor] = kSampleFresh;
    node(s.node).backfilled++;
  }
}

void GatewayState::onStatus(uint8_t nodeId, const StatusMsg& s, int64_t rxMono) {
  NodeView& n = node(nodeId);
  n.status = s;
  n.statusValid = true;
  n.statusMono = rxMono;
}

void GatewayState::onHello(uint8_t nodeId, const HelloMsg& h, uint32_t boot, int64_t rxMono) {
  NodeView& n = node(nodeId);
  n.hello = h;
  n.helloValid = true;
  if (boot) n.boot = boot;
}

bool GatewayState::onCalState(uint8_t nodeId, const CalStateMsg& c, int64_t rxMono) {
  NodeView& n = node(nodeId);
  if (c.node != nodeId) return false;
  bool ok = false;
  if (nodeId == kNodeA) ok = decodeNodeACal(c.blob, c.blobLen, n.calA);
  else if (nodeId == kNodeB) ok = decodeNodeBCal(c.blob, c.blobLen, n.calB);
  if (!ok) return false;
  n.calValid = true;  // copia confirmada por el propietario; el gateway no reaplica curvas
  n.calStorageState = c.storageState;
  n.calMono = rxMono;
  return true;
}

uint8_t GatewayState::classify(uint8_t sensor, const SensorSample& s, int64_t atMono, int64_t linkTimeoutMs,
                               int64_t nowMono) const {
  const uint8_t owner = sensorInfo(sensor).owner;
  const bool online = nodeOnline(owner, nowMono, linkTimeoutMs);
  if (!s.present) return online ? kSampleNever : kSampleNodeOffline;
  const int64_t age = atMono - s.sampleMonoMs;
  if (age <= maxAgeMs[sensor]) return kSampleFresh;
  return online ? kSampleStale : kSampleNodeOffline;
}

bool GatewayState::controlInput(uint8_t sensor, uint8_t valueIndex, int64_t nowMono, float& value) const {
  if (sensor >= kSensorCount) return false;
  const SensorSample& s = latest_[sensor];
  if (!s.present || valueIndex >= s.valueCount) return false;
  if (nowMono - s.sampleMonoMs > maxAgeMs[sensor]) return false;  // vencido: nunca entrada de control
  const uint8_t q = s.quality;
  if (q != kQGood && q != kQUserCalibrated && q != kQVendorReference && q != kQStabilizing) return false;
  if (!isfinite(s.vals[valueIndex])) return false;
  value = s.vals[valueIndex];
  return true;
}

Snapshot* GatewayState::takeSnapshot(int64_t nowMono, int64_t utcMs, const ActuatorState& act, int64_t linkTimeoutMs) {
  Snapshot* slot = nullptr;
  for (auto& s : snapshots_)
    if (!s.used) { slot = &s; break; }
  if (!slot) {
    // Desborde: se sobrescribe la mas antigua (publicada o no) y se cuenta.
    for (auto& s : snapshots_)
      if (!slot || int32_t(s.seq - slot->seq) < 0) slot = &s;
    if (!slot->published) snapshotsDropped_++;
  }
  *slot = Snapshot();
  slot->used = true;
  slot->seq = ++snapshotSeq_;
  slot->takenMono = nowMono;
  slot->takenUtcMs = utcMs;
  slot->actuators = act;
  slot->nodeOnline[0] = nodeOnline(kNodeA, nowMono, linkTimeoutMs);
  slot->nodeOnline[1] = nodeOnline(kNodeB, nowMono, linkTimeoutMs);
  for (uint8_t i = 0; i < kSensorCount; ++i) {
    slot->sensors[i] = latest_[i];
    slot->state[i] = classify(i, latest_[i], nowMono, linkTimeoutMs, nowMono);
  }
  return slot;
}

Snapshot* GatewayState::oldestUnpublished() {
  Snapshot* best = nullptr;
  for (auto& s : snapshots_)
    if (s.used && !s.published && (!best || int32_t(s.seq - best->seq) < 0)) best = &s;
  return best;
}

uint8_t GatewayState::unpublishedCount() const {
  uint8_t n = 0;
  for (const auto& s : snapshots_) n += s.used && !s.published ? 1 : 0;
  return n;
}

}  // namespace gw
