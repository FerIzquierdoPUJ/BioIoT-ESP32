#include "bioiot_selftest.h"

#include <math.h>
#include <string.h>

#include "bioiot_calibration.h"
#include "bioiot_messages.h"
#include "bioiot_protocol.h"

namespace bioiot {

size_t selfTestFrame(uint8_t* out, size_t cap) {
  uint8_t key[kAppKeySize];
  for (uint8_t i = 0; i < kAppKeySize; ++i) key[i] = i;
  TelemetryMsg t;
  t.snapshotId = 0x01020304; t.part = 0; t.parts = 1; t.txUptimeMs = 123456; t.recordCount = 2;
  TelemetryRecord& a = t.records[0];
  a.sensor = kSensorPh; a.quality = kQGood; a.flags = kRecConnected | kRecExpected; a.calRevision = 7;
  a.sampleUptimeMs = 120000; a.valueCount = val::kAnalogCount;
  a.vals[0] = 1365.0f; a.vals[1] = 1.1f; a.vals[2] = kPhCalMDefault * 1.1f + kPhCalBDefault;
  TelemetryRecord& b = t.records[1];
  b.sensor = kSensorTemperature; b.quality = kQDisconnected; b.flags = kRecExpected;
  b.sampleUptimeMs = 123000; b.valueCount = val::kTempCount;
  b.vals[0] = -127.0f; b.vals[1] = NAN;
  uint8_t payload[kMaxPayload];
  const size_t plen = encodeTelemetry(t, payload, sizeof(payload));
  if (!plen) return 0;
  FrameHeader h;
  h.type = MsgType::Telemetry; h.systemId = 0xB10107A1; h.src = kNodeA; h.dst = kNodeGateway;
  h.flags = kFlagAckRequested; h.payloadLen = uint8_t(plen); h.srcBoot = 0x11223344;
  h.dstBoot = 0x55667788; h.seq = 42;
  return encodeFrame(h, payload, key, sizeof(key), out, cap);
}

// Vector dorado generado por tests/host (zig c++ x86-64). Ver PROTOCOLO.md.
const uint8_t kSelfTestGolden[] = {
#include "bioiot_selftest_golden.inc"
};
const size_t kSelfTestGoldenLen = sizeof(kSelfTestGolden);

bool protocolSelfTest() {
  uint8_t frame[kEspNowMaxFrame];
  const size_t n = selfTestFrame(frame, sizeof(frame));
  if (n != kSelfTestGoldenLen || memcmp(frame, kSelfTestGolden, n) != 0) return false;
  // Ida y vuelta: el decodificador debe aceptar y reproducir los campos.
  uint8_t key[kAppKeySize];
  for (uint8_t i = 0; i < kAppKeySize; ++i) key[i] = i;
  FrameHeader h;
  const uint8_t* payload = nullptr;
  if (decodeFrame(frame, n, 0xB10107A1, kNodeGateway, key, sizeof(key), h, payload) != FrameError::None)
    return false;
  TelemetryMsg t;
  if (!decodeTelemetry(payload, h.payloadLen, t) || t.recordCount != 2) return false;
  if (t.records[0].vals[1] != 1.1f || !isnan(t.records[1].vals[1])) return false;
  // Una trama corrupta debe rechazarse.
  frame[30] ^= 0x01;
  return decodeFrame(frame, n, 0xB10107A1, kNodeGateway, key, sizeof(key), h, payload) == FrameError::BadTag;
}

}  // namespace bioiot
