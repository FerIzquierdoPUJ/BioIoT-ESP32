// Pruebas 1: serializacion, tamano <= 250, rechazo de corruptos/truncados.
#include <stdlib.h>

#include "BioIoTCommon.h"
#include "test_framework.h"

using namespace bioiot;

static std::string sha256Hex(const char* s) {
  Sha256 sha;
  sha.update(reinterpret_cast<const uint8_t*>(s), strlen(s));
  uint8_t out[32];
  sha.finish(out);
  return toHex(out, 32);
}

TEST(crypto_sha256_vectors) {
  CHECK_STR(sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK_STR(sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK_STR(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(crypto_hmac_rfc4231) {
  uint8_t out[32];
  uint8_t key1[20];
  memset(key1, 0x0b, sizeof(key1));
  hmacSha256(key1, 20, reinterpret_cast<const uint8_t*>("Hi There"), 8, out);
  CHECK_STR(toHex(out, 32), "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
  const char* data = "what do ya want for nothing?";
  hmacSha256(reinterpret_cast<const uint8_t*>("Jefe"), 4, reinterpret_cast<const uint8_t*>(data),
             strlen(data), out);
  CHECK_STR(toHex(out, 32), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  // Caso 6: clave de 131 bytes (> bloque) se reduce con SHA-256.
  uint8_t key6[131];
  memset(key6, 0xaa, sizeof(key6));
  const char* d6 = "Test Using Larger Than Block-Size Key - Hash Key First";
  hmacSha256(key6, sizeof(key6), reinterpret_cast<const uint8_t*>(d6), strlen(d6), out);
  CHECK_STR(toHex(out, 32), "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

TEST(crypto_crc32_vector) {
  CHECK_EQ(crc32(reinterpret_cast<const uint8_t*>("123456789"), 9), 0xCBF43926u);
}

TEST(codec_little_endian_explicit) {
  uint8_t buf[32];
  Writer w(buf, sizeof(buf));
  w.u16(0x1234); w.u32(0xA1B2C3D4); w.f32(1.0f); w.i64(-2);
  CHECK(w.ok());
  CHECK_STR(toHex(buf, w.size()), "3412d4c3b2a10000803ffeffffffffffffff");
  Reader r(buf, w.size());
  CHECK_EQ(r.u16(), 0x1234);
  CHECK_EQ(r.u32(), 0xA1B2C3D4u);
  CHECK(r.f32() == 1.0f);
  CHECK_EQ(r.i64(), -2);
  CHECK(r.done());
  CHECK_EQ(r.u8(), 0);  // leer de mas invalida el lector
  CHECK(!r.ok());
  Writer small(buf, 3);
  CHECK(!small.u32(1));
  CHECK(!small.ok());
}

static const uint8_t* testKey() {
  static uint8_t key[kAppKeySize];
  for (uint8_t i = 0; i < kAppKeySize; ++i) key[i] = uint8_t(0xA0 + i);
  return key;
}

static size_t makeFrame(uint8_t* out, size_t payloadLen, uint8_t type = uint8_t(MsgType::Status)) {
  uint8_t payload[kMaxPayload];
  for (size_t i = 0; i < payloadLen; ++i) payload[i] = uint8_t(i * 7);
  FrameHeader h;
  h.type = MsgType(type); h.systemId = 0xCAFE0001; h.src = kNodeA; h.dst = kNodeGateway;
  h.payloadLen = uint8_t(payloadLen); h.srcBoot = 1; h.dstBoot = 2; h.seq = 3;
  return encodeFrame(h, payload, testKey(), kAppKeySize, out, kEspNowMaxFrame);
}

TEST(frame_max_size_is_250) {
  static_assert(kHeaderSize + kMaxPayload + kTagSize == 250, "limite");
  uint8_t out[300];
  CHECK_EQ(makeFrame(out, kMaxPayload), 250u);
  // Un byte mas no cabe: el codificador se niega.
  uint8_t payload[kMaxPayload + 1] = {};
  FrameHeader h;
  h.type = MsgType::Status; h.systemId = 1; h.src = kNodeA; h.dst = kNodeGateway;
  h.payloadLen = uint8_t(kMaxPayload + 1);
  CHECK_EQ(encodeFrame(h, payload, testKey(), kAppKeySize, out, sizeof(out)), 0u);
}

TEST(frame_rejects_every_single_bit_flip) {
  uint8_t frame[kEspNowMaxFrame];
  const size_t n = makeFrame(frame, 40);
  FrameHeader h;
  const uint8_t* p;
  CHECK(decodeFrame(frame, n, 0xCAFE0001, kNodeGateway, testKey(), kAppKeySize, h, p) == FrameError::None);
  int accepted = 0;
  for (size_t i = 0; i < n; ++i)
    for (int b = 0; b < 8; ++b) {
      frame[i] ^= uint8_t(1 << b);
      if (decodeFrame(frame, n, 0xCAFE0001, kNodeGateway, testKey(), kAppKeySize, h, p) == FrameError::None)
        accepted++;
      frame[i] ^= uint8_t(1 << b);
    }
  CHECK_EQ(accepted, 0);
}

TEST(frame_rejects_truncated_and_extended) {
  uint8_t frame[kEspNowMaxFrame + 4];
  const size_t n = makeFrame(frame, 60);
  FrameHeader h;
  const uint8_t* p;
  for (size_t len = 0; len < n; ++len)
    CHECK(decodeFrame(frame, len, 0xCAFE0001, kNodeGateway, testKey(), kAppKeySize, h, p) != FrameError::None);
  frame[n] = 0;
  CHECK(decodeFrame(frame, n + 1, 0xCAFE0001, kNodeGateway, testKey(), kAppKeySize, h, p) ==
        FrameError::LengthMismatch);
  CHECK(precheckFrame(frame, 251, 0xCAFE0001, kNodeGateway) == FrameError::TooLong);
}

TEST(frame_rejects_other_system_version_destination_key) {
  uint8_t frame[kEspNowMaxFrame];
  const size_t n = makeFrame(frame, 10);
  FrameHeader h;
  const uint8_t* p;
  CHECK(decodeFrame(frame, n, 0xCAFE0002, kNodeGateway, testKey(), kAppKeySize, h, p) == FrameError::WrongSystem);
  CHECK(decodeFrame(frame, n, 0xCAFE0001, kNodeB, testKey(), kAppKeySize, h, p) == FrameError::WrongDestination);
  uint8_t other[kAppKeySize] = {};
  CHECK(decodeFrame(frame, n, 0xCAFE0001, kNodeGateway, other, kAppKeySize, h, p) == FrameError::BadTag);
  frame[2] = 2;  // version futura
  CHECK(decodeFrame(frame, n, 0xCAFE0001, kNodeGateway, testKey(), kAppKeySize, h, p) == FrameError::BadVersion);
  frame[2] = 1;
  frame[3] = 99;  // tipo desconocido
  CHECK(decodeFrame(frame, n, 0xCAFE0001, kNodeGateway, testKey(), kAppKeySize, h, p) == FrameError::UnknownType);
}

TEST(selftest_golden_vector_matches) {
  uint8_t frame[kEspNowMaxFrame];
  const size_t n = selfTestFrame(frame, sizeof(frame));
  CHECK(n > 0 && n <= 250);
  const std::string hex = toHex(frame, n);
  if (getenv("BIOIOT_WRITE_GOLDEN")) {
    FILE* f = fopen(getenv("BIOIOT_WRITE_GOLDEN"), "w");
    if (f) {
      for (size_t i = 0; i < n; ++i) fprintf(f, "0x%02x,%s", frame[i], (i % 12 == 11) ? "\n" : " ");
      fprintf(f, "\n");
      fclose(f);
    }
  }
  CHECK_EQ(n, kSelfTestGoldenLen);
  CHECK(n == kSelfTestGoldenLen && memcmp(frame, kSelfTestGolden, n) == 0);
  CHECK(protocolSelfTest());
}

// ---------- Tamano real de los mensajes de cada nodo ----------
static TelemetryRecord rec(uint8_t sensor) {
  TelemetryRecord r;
  r.sensor = sensor; r.quality = kQGood; r.valueCount = sensorInfo(sensor).valueCount;
  for (uint8_t i = 0; i < r.valueCount; ++i) r.vals[i] = 1.5f * i;
  return r;
}

TEST(telemetry_parts_fit_espnow) {
  // Nodo A parte 0: analogicos + temperatura; parte 1: colores. Nodo B: una parte.
  const uint8_t partA0[] = {kSensorPh, kSensorCo2_1, kSensorCo2_2, kSensorTurbidity,
                            kSensorDissolvedOxygen, kSensorTds, kSensorTemperature};
  const uint8_t partA1[] = {kSensorColor1, kSensorColor2};
  const uint8_t partB[] = {kSensorLight1, kSensorLight2, kSensorO2Gas1, kSensorO2Gas2};
  struct P { const uint8_t* s; size_t n; } parts[] = {{partA0, 7}, {partA1, 2}, {partB, 4}};
  for (auto& part : parts) {
    TelemetryMsg t;
    t.recordCount = uint8_t(part.n);
    for (size_t i = 0; i < part.n; ++i) t.records[i] = rec(part.s[i]);
    uint8_t buf[kMaxPayload];
    const size_t n = encodeTelemetry(t, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(n + kHeaderSize + kTagSize <= 250);
    printf("    telemetria %zu registros: payload %zu B, trama %zu B\n", part.n, n, n + kHeaderSize + kTagSize);
    TelemetryMsg back;
    CHECK(decodeTelemetry(buf, n, back));
    CHECK_EQ(back.recordCount, t.recordCount);
    // Truncado en cualquier punto => rechazo.
    for (size_t len = 0; len < n; ++len) CHECK(!decodeTelemetry(buf, len, back));
  }
  // Todos los registros de A no caben en una sola trama: por eso hay 2 partes.
  TelemetryMsg all;
  all.recordCount = 8;
  for (uint8_t i = 0; i < 8; ++i) all.records[i] = rec(kSensorColor1);
  uint8_t buf[kMaxPayload];
  CHECK_EQ(encodeTelemetry(all, buf, sizeof(buf)), 0u);
}

TEST(telemetry_rejects_out_of_range_values) {
  TelemetryMsg t;
  t.recordCount = 1;
  t.records[0] = rec(kSensorPh);
  t.txUptimeMs = 1000;
  t.records[0].sampleUptimeMs = 900;
  uint8_t buf[kMaxPayload];
  size_t n = encodeTelemetry(t, buf, sizeof(buf));
  TelemetryMsg back;
  CHECK(decodeTelemetry(buf, n, back));
  t.records[0].vals[2] = INFINITY;
  n = encodeTelemetry(t, buf, sizeof(buf));
  CHECK(!decodeTelemetry(buf, n, back));
  t.records[0] = rec(kSensorPh);
  t.records[0].vals[2] = NAN;  // NaN permitido: sin valor, con quality explicita
  n = encodeTelemetry(t, buf, sizeof(buf));
  CHECK(decodeTelemetry(buf, n, back));
  CHECK(isnan(back.records[0].vals[2]));
  t.records[0].sensor = 77;
  n = encodeTelemetry(t, buf, sizeof(buf));
  CHECK(!decodeTelemetry(buf, n, back));
  t.records[0] = rec(kSensorPh);
  t.records[0].quality = kQStale;  // solo el gateway puede asignarla
  n = encodeTelemetry(t, buf, sizeof(buf));
  CHECK(!decodeTelemetry(buf, n, back));
  t.records[0] = rec(kSensorPh);
  t.records[0].sampleUptimeMs = 2000;  // muestra "del futuro" respecto a la transmision
  n = encodeTelemetry(t, buf, sizeof(buf));
  CHECK(!decodeTelemetry(buf, n, back));
  t.records[0] = rec(kSensorPh);
  t.records[0].valueCount = 2;  // conteo distinto del contrato del sensor
  n = encodeTelemetry(t, buf, sizeof(buf));
  CHECK(!decodeTelemetry(buf, n, back));
}

TEST(calibration_blobs_roundtrip_and_validate) {
  NodeACal a;
  a.ph_m = -6.0f; a.ph_user = 1; a.co2[1].mode = 1; a.co2[1].a = 12.34f; a.co2[1].b = -0.5f;
  a.co2[1].user = 1; a.rev[kCalCo2_2] = 3; a.legacyVersion = 9; a.nodeRevision = 4;
  uint8_t buf[200];
  CHECK_EQ(encodeNodeACal(a, buf, sizeof(buf)), kNodeACalEncodedSize);
  NodeACal back;
  CHECK(decodeNodeACal(buf, kNodeACalEncodedSize, back));
  CHECK(back.ph_m == -6.0f && back.ph_user == 1 && back.co2[1].a == 12.34f && back.rev[kCalCo2_2] == 3);
  CHECK(!decodeNodeACal(buf, kNodeACalEncodedSize - 1, back));
  a.co2[0].zero_point_v = 5.0f;  // fuera de rango segun validCo2Calibration
  encodeNodeACal(a, buf, sizeof(buf));
  CHECK(!decodeNodeACal(buf, kNodeACalEncodedSize, back));

  NodeBCal b;
  b.o2[0].gain = 1.02f; b.o2[0].commandUtc = 1790000000; b.o2[0].commandCount = 2;
  CHECK_EQ(encodeNodeBCal(b, buf, sizeof(buf)), kNodeBCalEncodedSize);
  NodeBCal bb;
  CHECK(decodeNodeBCal(buf, kNodeBCalEncodedSize, bb));
  CHECK(bb.o2[0].commandUtc == 1790000000 && bb.o2[0].gain == 1.02f);
  b.o2[1].gain = 0;
  encodeNodeBCal(b, buf, sizeof(buf));
  CHECK(!decodeNodeBCal(buf, kNodeBCalEncodedSize, bb));
}

TEST(all_messages_fit_and_reject_truncation) {
  uint8_t buf[kMaxPayload];
  CalibrationCmd c;
  c.op = kCalOpImport; c.target = kTargetAll; c.blobLen = uint8_t(encodeNodeACal(NodeACal(), c.blob, sizeof(c.blob)));
  size_t n = encodeCalibration(c, buf, sizeof(buf));
  CHECK(n > 0 && n <= kMaxPayload);
  CalibrationCmd cb;
  CHECK(decodeCalibration(buf, n, cb));
  for (size_t len = 0; len < n; ++len) CHECK(!decodeCalibration(buf, len, cb));
  CalibrationCmd set;
  set.op = kCalOpSet; set.target = kTargetPh; set.present = 3; set.params[0] = -6.1f; set.params[1] = NAN;
  n = encodeCalibration(set, buf, sizeof(buf));
  CHECK(!decodeCalibration(buf, n, cb));  // NaN no es un coeficiente valido

  CalStateMsg s;
  s.node = kNodeA; s.blobLen = uint8_t(encodeNodeACal(NodeACal(), s.blob, sizeof(s.blob)));
  n = encodeCalState(s, buf, sizeof(buf));
  CHECK(n > 0 && n + kHeaderSize + kTagSize <= 250);
  CalStateMsg sb;
  CHECK(decodeCalState(buf, n, sb));
  s.node = kNodeB;  // longitud de blob incoherente con el nodo
  n = encodeCalState(s, buf, sizeof(buf));
  CHECK(!decodeCalState(buf, n, sb));

  StatusMsg st;
  n = encodeStatus(st, buf, sizeof(buf));
  CHECK_EQ(n, kStatusSize);
  StatusMsg stb;
  CHECK(decodeStatus(buf, n, stb));
  CHECK(!decodeStatus(buf, n - 1, stb));

  TimeSyncMsg ts;
  ts.utcValid = 1; ts.utcMs = 1700000000000LL;
  n = encodeTimeSync(ts, buf, sizeof(buf));
  TimeSyncMsg tsb;
  CHECK(decodeTimeSync(buf, n, tsb));
  ts.utcMs = 1000;  // UTC invalido declarado valido
  n = encodeTimeSync(ts, buf, sizeof(buf));
  CHECK(!decodeTimeSync(buf, n, tsb));

  ConfigCmd cfg;
  cfg.present = 1; cfg.reportIntervalMs = 5000;
  n = encodeConfig(cfg, buf, sizeof(buf));
  ConfigCmd cfgb;
  CHECK(decodeConfig(buf, n, cfgb));
  cfg.reportIntervalMs = 10;  // fuera de rango
  n = encodeConfig(cfg, buf, sizeof(buf));
  CHECK(!decodeConfig(buf, n, cfgb));

  CommandResultMsg r;
  r.msgType = uint8_t(MsgType::Calibration); r.outcome = kOutcomeApplied; r.status = kStSaved;
  n = encodeCommandResult(r, buf, sizeof(buf));
  CHECK_EQ(n, kCommandResultSize);
  CommandResultMsg rb;
  CHECK(decodeCommandResult(buf, n, rb));
  r.status = 200;
  n = encodeCommandResult(r, buf, sizeof(buf));
  CHECK(!decodeCommandResult(buf, n, rb));
}
