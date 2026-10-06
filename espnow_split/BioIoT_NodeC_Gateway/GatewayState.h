#pragma once
// Estado del gateway, portable (sin Arduino). Todos los tiempos usan el reloj
// monotono del GATEWAY (ms, 64 bits). La edad de una muestra se traduce al
// recibirla: sampleMono = rxMono - (txUptime - sampleUptime), ambos del reloj del
// nodo; nunca se mezclan millis() de placas distintas. Una retransmision no
// rejuvenece la muestra porque txUptime se reescribe en cada envio.
#include <stdint.h>

#include "ActuatorCore.h"
#include "BioIoTCommon.h"

namespace gw {

struct SensorSample {
  bool present = false;
  uint8_t quality = bioiot::kQNotSampled;
  uint16_t flags = 0;
  uint16_t calRevision = 0;
  uint8_t node = 0;
  uint8_t valueCount = 0;
  int64_t sampleMonoMs = 0;
  float vals[bioiot::kMaxRecordValues] = {};
};

enum SampleState : uint8_t { kSampleFresh = 0, kSampleStale = 1, kSampleNodeOffline = 2, kSampleNever = 3 };
const char* sampleStateName(uint8_t s);

struct NodeView {
  uint32_t boot = 0;
  bool everRx = false;
  int64_t lastRxMono = 0;
  int8_t rssi = 0;
  bool helloValid = false;
  bioiot::HelloMsg hello;
  bool statusValid = false;
  bioiot::StatusMsg status;
  int64_t statusMono = 0;
  bool calValid = false;
  uint8_t calStorageState = 0;
  bioiot::NodeACal calA;
  bioiot::NodeBCal calB;
  int64_t calMono = 0;
  uint32_t telemetryFrames = 0, backfilled = 0, rejectedRecords = 0;
};

struct Snapshot {
  bool used = false, published = false;
  uint32_t seq = 0;
  int64_t takenMono = 0;
  int64_t takenUtcMs = 0;  // 0 = sin UTC al tomarla
  bool nodeOnline[2] = {false, false};
  uint8_t state[bioiot::kSensorCount] = {};
  SensorSample sensors[bioiot::kSensorCount];
  ActuatorState actuators;
};

enum CmdState : uint8_t {
  kCmdPending = 0,     // aceptado por el gateway, aun no confirmado por el nodo
  kCmdDelivered = 1,   // ACK de aplicacion recibido (recepcion, no ejecucion)
  kCmdApplied = 2,
  kCmdRejected = 3,
  kCmdFailed = 4,
  kCmdExpired = 5,     // vencio antes de entregarse
  kCmdTimeout = 6,     // sin ACK o sin resultado a tiempo
  kCmdNodeRebooted = 7,  // el nodo reinicio: resultado desconocido, comparar revision
  kCmdStarted = 8,
};
const char* cmdStateName(uint8_t s);
bool cmdStateFinal(uint8_t s);

struct TrackedCommand {
  bool used = false;
  bool hasCloudId = false;
  char cloudId[65] = "";
  char sensor[16] = "";
  uint32_t cmdId = 0, groupId = 0;
  uint8_t node = 0, msgType = 0, op = 0, target = 0;
  uint8_t state = kCmdPending, status = 0xFF;
  uint32_t nodeRevision = 0;
  uint16_t sensorRevision = 0;
  bool storageOk = false;
  uint8_t extra1 = 0, extra2 = 0;
  int64_t createdMono = 0, deadlineMono = 0;
  bool needsPublish = false;
};

class CommandTracker {
 public:
  static constexpr uint8_t kSize = 12;
  TrackedCommand* add(uint32_t cmdId, uint8_t node, uint8_t msgType, uint8_t op, uint8_t target, const char* sensor,
                      const char* cloudId, bool hasCloudId, uint32_t groupId, int64_t nowMono, int64_t deadlineMono);
  TrackedCommand* find(uint32_t cmdId);
  void onDelivery(uint32_t cmdId, bioiot::Delivery d, int64_t nowMono);
  void onResult(uint8_t node, const bioiot::CommandResultMsg& r);
  void expire(int64_t nowMono, int64_t resultTimeoutMs);
  TrackedCommand* nextToPublish();
  // Resumen de un grupo (reset_all/importacion): true si todos los miembros son finales.
  bool groupFinal(uint32_t groupId, uint8_t& applied, uint8_t& total) const;
  uint32_t dropped = 0;

 private:
  TrackedCommand items_[kSize];
};

struct DiagNodePart {
  bool requested = false, done = false;
  uint8_t result = 0;  // 0 pendiente, 1 ok, 2 timeout, 3 node_offline, 4 rechazado
  bioiot::Reassembler reasm;
};
const char* diagPartResultName(uint8_t r);

struct DiagSession {
  bool active = false, ready = false, fromSerial = false;
  uint32_t id = 0;
  uint8_t scope = 0;
  int64_t startMono = 0;
  DiagNodePart part[2];  // 0 = nodo A, 1 = nodo B
};

class GatewayState {
 public:
  static constexpr uint8_t kSnapshots = 40;

  NodeView& node(uint8_t nodeId) { return nodes_[nodeId == bioiot::kNodeB ? 1 : 0]; }
  const NodeView& node(uint8_t nodeId) const { return nodes_[nodeId == bioiot::kNodeB ? 1 : 0]; }
  bool nodeOnline(uint8_t nodeId, int64_t nowMono, int64_t linkTimeoutMs) const;

  // Devuelve registros aceptados (los de sensores ajenos al nodo se rechazan).
  uint8_t onTelemetry(uint8_t nodeId, const bioiot::TelemetryMsg& t, int64_t rxMono);
  void onStatus(uint8_t nodeId, const bioiot::StatusMsg& s, int64_t rxMono);
  void onHello(uint8_t nodeId, const bioiot::HelloMsg& h, uint32_t boot, int64_t rxMono);
  bool onCalState(uint8_t nodeId, const bioiot::CalStateMsg& c, int64_t rxMono);
  void touch(uint8_t nodeId, int64_t rxMono, int8_t rssi);

  const SensorSample& latest(uint8_t sensor) const { return latest_[sensor]; }
  uint8_t classify(uint8_t sensor, const SensorSample& s, int64_t atMono, int64_t linkTimeoutMs, int64_t nowMono) const;
  // Entrada de control: falso si no hay valor, vencido o con calidad no valida.
  bool controlInput(uint8_t sensor, uint8_t valueIndex, int64_t nowMono, float& value) const;

  Snapshot* takeSnapshot(int64_t nowMono, int64_t utcMs, const ActuatorState& act, int64_t linkTimeoutMs);
  Snapshot* oldestUnpublished();
  uint32_t snapshotsDropped() const { return snapshotsDropped_; }
  uint8_t unpublishedCount() const;

  CommandTracker commands;
  DiagSession diag;
  int64_t maxAgeMs[bioiot::kSensorCount] = {};

 private:
  void backfill(uint8_t sensor, const SensorSample& s);
  NodeView nodes_[2];
  SensorSample latest_[bioiot::kSensorCount];
  Snapshot snapshots_[kSnapshots];
  uint32_t snapshotSeq_ = 0, snapshotsDropped_ = 0;
  int64_t linkTimeoutMs_ = 20000;
};

}  // namespace gw
