#pragma once
// Payloads del contrato v1. Cada encode/decode es explicito; decode exige
// consumo exacto y rangos validos (rechaza truncados y valores fuera de rango).
#include <stddef.h>
#include <stdint.h>

#include "bioiot_calibration.h"
#include "bioiot_protocol.h"
#include "bioiot_types.h"

namespace bioiot {

constexpr uint16_t kFirmwareVersion = 0x0100;  // 1.0

// ---------- HELLO (ambos sentidos) ----------
enum HelloFlags : uint16_t {
  kHelloReply = 1u << 0,
  kHelloSelfTestOk = 1u << 1,
  kHelloStorageOk = 1u << 2,
  kHelloTimeSynced = 1u << 3,
  kHelloPlaceholderKeys = 1u << 4,
};
struct HelloMsg {
  uint8_t role = 0;  // NodeId del emisor
  uint16_t fwVersion = kFirmwareVersion;
  uint16_t flags = 0;
  uint8_t channel = 0;
  uint32_t uptimeMs = 0;
  uint8_t resetReason = 0;
};
constexpr size_t kHelloSize = 1 + 2 + 2 + 1 + 4 + 1;

// ---------- TIME_SYNC (gateway -> nodo) ----------
struct TimeSyncMsg {
  uint8_t utcValid = 0;
  int64_t utcMs = 0;             // UTC en ms al construir el mensaje (0 si invalido)
  uint32_t gatewayUptimeMs = 0;  // reloj monotono del gateway (solo diagnostico)
  uint32_t utcAgeMs = 0;         // tiempo desde la ultima sincronizacion NTP del gateway
};
constexpr size_t kTimeSyncSize = 1 + 8 + 4 + 4;

// ---------- ACK ----------
struct AckMsg {
  uint32_t ackSeq = 0;
  uint8_t ackType = 0;
  uint8_t status = kAckAccepted;
};
constexpr size_t kAckSize = 6;

// ---------- STATUS (nodo -> gateway, sin ACK) ----------
enum StatusFlags : uint16_t {
  kSfStorageOk = 1u << 0,
  kSfStorageFuture = 1u << 1,
  kSfTimeSynced = 1u << 2,
  kSfWarmupActive = 1u << 3,
  kSfDiagBusy = 1u << 4,
  kSfSelfTestOk = 1u << 5,
  kSfChannelHunting = 1u << 6,
  kSfPlaceholderKeys = 1u << 7,
  kSfWarmupAssumed = 1u << 8,  // tiempo alimentado desconocido: warm-up completo conservador
};
// Bits diag del nodo A
enum DiagFlagsA : uint16_t {
  kDiagAAnalogSampled = 1u << 0,
  kDiagAAllNearZero = 1u << 1,
  kDiagATooSimilar = 1u << 2,
  kDiagAColor1Sampled = 1u << 3,
  kDiagAColor1PulseOk = 1u << 4,
  kDiagAColor2Sampled = 1u << 5,
  kDiagAColor2PulseOk = 1u << 6,
  kDiagATempSampled = 1u << 7,
  kDiagATempDetected = 1u << 8,
};
// Bits diag del nodo B
enum DiagFlagsB : uint16_t {
  kDiagBTcaObserved = 1u << 0,
  kDiagBTcaDetected = 1u << 1,
  kDiagBIsolated = 1u << 2,
  kDiagBBh1Ok = 1u << 3,
  kDiagBBh2Ok = 1u << 4,
  kDiagBO2_1Ok = 1u << 5,
  kDiagBO2_2Ok = 1u << 6,
  kDiagBReadingsObserved = 1u << 7,
};
struct StatusMsg {
  uint32_t uptimeMs = 0, freeHeap = 0, minFreeHeap = 0, maxAllocHeap = 0;
  uint8_t resetReason = 0;
  uint16_t flags = 0;
  uint32_t warmupRemainingMs = 0;
  uint8_t queueDepth = 0;
  uint16_t telemetryDropped = 0, txFailures = 0, retries = 0, rxRejected = 0, channelHunts = 0;
  uint8_t channel = 0;
  int8_t rssi = 0;
  uint32_t reportIntervalMs = 0;
  uint16_t diagFlags = 0;
  uint32_t diagCounter = 0;      // A: analog all_zero_count; B: fallos consecutivos TCA
  uint8_t diagError = 0;         // B: ultimo codigo Wire del TCA
  uint32_t diagSampledAgeMs = 0; // edad de la ultima muestra diagnostica
};
constexpr size_t kStatusSize = 4 * 4 + 1 + 2 + 4 + 1 + 2 * 5 + 1 + 1 + 4 + 2 + 4 + 1 + 4;  // 51

// ---------- TELEMETRY (nodo -> gateway, con ACK) ----------
struct TelemetryRecord {
  uint8_t sensor = 0;
  uint8_t quality = kQNotSampled;
  uint16_t flags = 0;
  uint16_t calRevision = 0;
  uint32_t sampleUptimeMs = 0;  // reloj del NODO al adquirir
  uint8_t valueCount = 0;
  float vals[kMaxRecordValues] = {};
};
constexpr size_t kRecordHeaderSize = 1 + 1 + 2 + 2 + 4 + 1;  // 11
constexpr uint8_t kMaxRecordsPerFrame = 8;
struct TelemetryMsg {
  uint32_t snapshotId = 0;
  uint8_t part = 0, parts = 1;
  uint32_t txUptimeMs = 0;  // reloj del nodo en cada (re)transmision
  uint8_t recordCount = 0;
  TelemetryRecord records[kMaxRecordsPerFrame];
};
constexpr size_t kTelemetryPrefixSize = 4 + 1 + 1 + 4 + 1;  // 11
constexpr size_t kTelemetryTxTimeOffset = 6;              // offset de txUptimeMs en el payload
size_t telemetryRecordSize(const TelemetryRecord& r);

// ---------- CALIBRATION (gateway -> nodo) ----------
enum CalOp : uint8_t { kCalOpSet = 1, kCalOpReset = 2, kCalOpResetAll = 3, kCalOpO2Air = 4, kCalOpImport = 5 };
// Indices de parametros
namespace calp {
constexpr uint8_t kM = 0, kB = 1;                                // lineales (ph, turb, do, temp)
constexpr uint8_t kA = 0, kLegacyB = 1, kZero = 2, kReaction = 3;  // co2
constexpr uint8_t kGain = 0, kOffset = 1;                        // o2 set
constexpr uint8_t kReference = 0;                                // o2 aire
constexpr uint8_t kMaxParams = 4;
}  // namespace calp
struct CalibrationCmd {
  uint32_t cmdId = 0;
  uint8_t op = 0, target = 0;
  uint8_t present = 0;  // bit por parametro
  float params[calp::kMaxParams] = {};
  uint8_t enabledPresent = 0, enabledValue = 0;
  uint8_t force = 0;  // importacion
  uint8_t blobLen = 0;
  uint8_t blob[kNodeACalEncodedSize] = {};
};

// ---------- CONFIG (gateway -> nodo) ----------
struct ConfigCmd {
  uint32_t cmdId = 0;
  uint8_t present = 0;  // bit0 intervalo, bit1 expected
  uint32_t reportIntervalMs = 0;
  uint16_t expectedMask = 0;  // bit por SensorId del nodo
};
constexpr size_t kConfigSize = 4 + 1 + 4 + 2;
constexpr uint32_t kMinReportIntervalMs = 1000, kMaxReportIntervalMs = 600000;

// ---------- COMMAND (gateway -> nodo) ----------
enum GenericOp : uint8_t { kOpReboot = 1, kOpSendState = 2 };
struct GenericCmd {
  uint32_t cmdId = 0;
  uint8_t op = 0;
};
constexpr size_t kGenericCmdSize = 5;

// ---------- COMMAND_RESULT (nodo -> gateway, con ACK) ----------
struct CommandResultMsg {
  uint32_t cmdId = 0;
  uint8_t msgType = 0;  // tipo del comando original
  uint8_t op = 0, target = 0;
  uint8_t outcome = 0, status = 0;
  uint32_t nodeRevision = 0;
  uint16_t sensorRevision = 0;
  uint8_t storageOk = 0;
  uint8_t extra1 = 0, extra2 = 0;  // o2 aire: channels_disabled, disable_error
};
constexpr size_t kCommandResultSize = 4 + 1 + 1 + 1 + 1 + 1 + 4 + 2 + 1 + 1 + 1;  // 18

// ---------- DIAGNOSTIC_REQUEST / RESULT ----------
enum DiagScope : uint8_t {
  kDiagFull = 0, kDiagI2c = 1, kDiagAnalog = 2, kDiagColor = 3, kDiagSystem = 4, kDiagQuick = 5,
  kDiagRecover = 6, kDiagTemperature = 7, kDiagO2 = 8,
};
constexpr uint8_t kMaxDiagScope = 8;
const char* diagScopeName(uint8_t scope);
uint8_t diagScopeFromName(const char* name);  // 255 si desconocido
struct DiagRequestMsg {
  uint32_t requestId = 0;
  uint8_t scope = 0;
};
constexpr size_t kDiagRequestSize = 5;
constexpr size_t kDiagFragmentHeader = 4 + 1 + 1 + 2;
constexpr size_t kDiagChunkSize = kMaxPayload - kDiagFragmentHeader;  // 210
constexpr size_t kDiagMaxBytes = 4096;
constexpr uint8_t kDiagMaxFragments = (kDiagMaxBytes + kDiagChunkSize - 1) / kDiagChunkSize;  // 20
static_assert(kDiagMaxFragments <= 32, "bitmap de 32 fragmentos");
struct DiagFragmentMsg {
  uint32_t requestId = 0;
  uint8_t index = 0, count = 0;
  uint16_t totalLen = 0;
  uint8_t chunkLen = 0;
  const uint8_t* chunk = nullptr;  // apunta al buffer de origen
};

// ---------- CALIBRATION_STATE (nodo -> gateway, con ACK) ----------
enum CalStorageState : uint8_t { kCalStorageOk = 0, kCalStorageFailed = 1, kCalStorageFuture = 2, kCalStorageDefaults = 3 };
struct CalStateMsg {
  uint8_t node = 0;
  uint8_t storageState = kCalStorageOk;
  uint8_t formatVersion = kCalFormatVersion;
  uint8_t blobLen = 0;
  uint8_t blob[kNodeACalEncodedSize] = {};
};

// Encoders devuelven longitud (0 si no cabe); decoders validan todo.
size_t encodeHello(const HelloMsg& m, uint8_t* out, size_t cap);
bool decodeHello(const uint8_t* in, size_t len, HelloMsg& m);
size_t encodeTimeSync(const TimeSyncMsg& m, uint8_t* out, size_t cap);
bool decodeTimeSync(const uint8_t* in, size_t len, TimeSyncMsg& m);
size_t encodeAck(const AckMsg& m, uint8_t* out, size_t cap);
bool decodeAck(const uint8_t* in, size_t len, AckMsg& m);
size_t encodeStatus(const StatusMsg& m, uint8_t* out, size_t cap);
bool decodeStatus(const uint8_t* in, size_t len, StatusMsg& m);
size_t encodeTelemetry(const TelemetryMsg& m, uint8_t* out, size_t cap);
bool decodeTelemetry(const uint8_t* in, size_t len, TelemetryMsg& m);
size_t encodeCalibration(const CalibrationCmd& m, uint8_t* out, size_t cap);
bool decodeCalibration(const uint8_t* in, size_t len, CalibrationCmd& m);
size_t encodeConfig(const ConfigCmd& m, uint8_t* out, size_t cap);
bool decodeConfig(const uint8_t* in, size_t len, ConfigCmd& m);
size_t encodeGeneric(const GenericCmd& m, uint8_t* out, size_t cap);
bool decodeGeneric(const uint8_t* in, size_t len, GenericCmd& m);
size_t encodeCommandResult(const CommandResultMsg& m, uint8_t* out, size_t cap);
bool decodeCommandResult(const uint8_t* in, size_t len, CommandResultMsg& m);
size_t encodeDiagRequest(const DiagRequestMsg& m, uint8_t* out, size_t cap);
bool decodeDiagRequest(const uint8_t* in, size_t len, DiagRequestMsg& m);
size_t encodeDiagFragment(const DiagFragmentMsg& m, uint8_t* out, size_t cap);
bool decodeDiagFragment(const uint8_t* in, size_t len, DiagFragmentMsg& m);
size_t encodeCalState(const CalStateMsg& m, uint8_t* out, size_t cap);
bool decodeCalState(const uint8_t* in, size_t len, CalStateMsg& m);

}  // namespace bioiot
