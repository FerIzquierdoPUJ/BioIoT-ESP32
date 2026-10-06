// Publicacion por trozos del gateway: sin buffer del tamano del mensaje, la falta de
// heap no pierde muestras y los textos del estado se copian al documento.
#include <ArduinoJson.h>

#include <memory>
#include <string>
#include <vector>

#include "PublishStream.h"
#include "TelemetryJson.h"
#include "test_framework.h"

using namespace bioiot;
using namespace gw;

namespace {
struct Collect {
  std::string data;
  std::vector<size_t> chunks;
  size_t failAfter = size_t(-1);  // numero de trozos aceptados antes de fallar
};
bool collectSink(void* ctx, const uint8_t* d, size_t n) {
  Collect* c = static_cast<Collect*>(ctx);
  if (c->chunks.size() >= c->failAfter) return false;
  c->data.append(reinterpret_cast<const char*>(d), n);
  c->chunks.push_back(n);
  return true;
}
// Asignador que se queda sin memoria tras `budget` bytes (heap agotado durante TLS).
struct TightAllocator : ArduinoJson::Allocator {
  size_t budget, used = 0;
  explicit TightAllocator(size_t b) : budget(b) {}
  void* allocate(size_t n) override {
    if (used + n > budget) return nullptr;
    used += n;
    return malloc(n);
  }
  void deallocate(void* p) override { free(p); }
  void* reallocate(void* p, size_t n) override {
    if (used + n > budget) return nullptr;
    used += n;
    return realloc(p, n);
  }
};
std::string readFile(const char* path) {
  FILE* f = fopen(path, "rb");
  std::string s;
  if (!f) return s;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
  fclose(f);
  return s;
}
}  // namespace

TEST(publish_stream_chunks_whole_telemetry_with_1kb) {
  JsonDocument doc;
  const std::string src = readFile("docs/examples/telemetry-v1.1.json");
  CHECK(!src.empty());
  CHECK(!deserializeJson(doc, src));
  std::string expected;
  serializeJson(doc, expected);
  uint8_t chunk[1024];
  Collect c;
  ChunkWriter w(chunk, sizeof(chunk), collectSink, &c);
  const size_t written = serializeJson(doc, w);
  CHECK(w.flush() && !w.failed());
  CHECK_EQ(written, measureJson(doc));
  CHECK(c.data == expected);  // byte a byte, mismo JSON que con buffer completo
  CHECK(expected.size() > 5000);
  CHECK_EQ(c.chunks.size(), (expected.size() + 1023) / 1024);  // un envio por KB, no por caracter
  for (size_t n : c.chunks) CHECK(n <= 1024);
}

TEST(publish_stream_sink_failure_is_reported) {
  JsonDocument doc;
  for (int i = 0; i < 400; ++i) doc["k"].add("valor_de_relleno");
  uint8_t chunk[256];
  Collect c;
  c.failAfter = 2;
  ChunkWriter w(chunk, sizeof(chunk), collectSink, &c);
  const size_t written = serializeJson(doc, w);
  w.flush();
  CHECK(w.failed());
  CHECK(written < measureJson(doc));  // el publicador cierra MQTT: paquete incompleto
}

TEST(publish_verdict_memory_is_retried_size_is_dropped) {
  CHECK(jsonVerdict(true, 0, 24576) == JsonVerdict::RetryNoMemory);
  CHECK(jsonVerdict(true, 9000, 24576) == JsonVerdict::RetryNoMemory);
  CHECK(jsonVerdict(false, 7000, 24576) == JsonVerdict::Publish);
  CHECK(jsonVerdict(false, 24576, 24576) == JsonVerdict::Publish);
  CHECK(jsonVerdict(false, 24577, 24576) == JsonVerdict::DropTooLarge);
  CHECK(jsonVerdict(false, 0, 24576) == JsonVerdict::DropTooLarge);
  CHECK_STR(jsonVerdictName(JsonVerdict::RetryNoMemory), "retry_no_memory");
}

TEST(publish_no_heap_overflows_document_and_keeps_sample) {
  GatewayInfo info;
  info.deviceId = "esp32-bioiot-01";
  info.experimentId = "EXP-001";
  TrackedCommand t;
  t.hasCloudId = true;
  strcpy(t.cloudId, "cmd-123");
  strcpy(t.sensor, "o2_gas_1");
  GatewayState* st = new GatewayState();
  TightAllocator tight(64);  // casi sin heap
  JsonDocument starving(&tight);
  fillCalibrationAck(starving, t, *st, info);
  CHECK(starving.overflowed());
  CHECK(jsonVerdict(starving.overflowed(), 0, 24576) == JsonVerdict::RetryNoMemory);  // no se descarta
  // Con heap suficiente, el mismo elemento se genera completo en el reintento.
  JsonDocument ok;
  fillCalibrationAck(ok, t, *st, info);
  CHECK(!ok.overflowed());
  CHECK(jsonVerdict(false, measureJson(ok), 24576) == JsonVerdict::Publish);
  delete st;
}

TEST(publish_document_copies_state_strings_before_unlock) {
  GatewayInfo info;
  info.deviceId = "esp32-bioiot-01";
  TrackedCommand t;
  t.hasCloudId = true;
  strcpy(t.cloudId, "cmd-original");
  strcpy(t.sensor, "o2_gas_1");
  GatewayState* st = new GatewayState();
  JsonDocument doc;
  fillCalibrationAck(doc, t, *st, info);
  // La tarea local reutiliza la ranura mientras loop() publica sin el mutex.
  strcpy(t.cloudId, "cmd-OTRO");
  strcpy(t.sensor, "ph");
  CHECK_STR(doc["commandId"].as<const char*>(), "cmd-original");
  CHECK_STR(doc["sensor"].as<const char*>(), "o2_gas_1");
  ActuatorState a;
  strcpy(a.lastCommandId, "act-1");
  JsonDocument ev;
  fillActuatorEvent(ev, a, info);
  strcpy(a.lastCommandId, "act-2");
  std::string out;
  serializeJson(ev, out);
  CHECK(out.find("act-1") != std::string::npos && out.find("act-2") == std::string::npos);
  delete st;
}
