// Pruebas 3 (almacenamiento): doble ranura, escritura interrumpida, CRC, formato futuro.
#include "BioIoTCommon.h"
#include "test_framework.h"

using namespace bioiot;

namespace {
struct RamBackend : SlotBackend {
  std::vector<uint8_t> slot[2];
  bool failWrite = false;        // write devuelve false sin tocar datos
  int truncateNextWrite = -1;    // corte de energia: solo se escriben N bytes
  bool corruptReadback = false;  // la relectura no coincide
  int writes = 0;
  size_t readSlot(uint8_t s, uint8_t* buf, size_t cap) override {
    if (slot[s].empty() || slot[s].size() > cap) return 0;
    memcpy(buf, slot[s].data(), slot[s].size());
    if (corruptReadback) buf[0] ^= 1;
    return slot[s].size();
  }
  bool writeSlot(uint8_t s, const uint8_t* buf, size_t len) override {
    writes++;
    if (failWrite) return false;
    if (truncateNextWrite >= 0) {
      slot[s].assign(buf, buf + truncateNextWrite);
      truncateNextWrite = -1;
      return true;  // el driver crey'o terminar; la relectura lo detecta
    }
    slot[s].assign(buf, buf + len);
    return true;
  }
};
}  // namespace

TEST(store_first_boot_empty_then_saves) {
  RamBackend be;
  RecordStore st(be, 1);
  uint8_t out[64];
  CHECK_EQ(st.load(out, sizeof(out)), 0u);
  CHECK(st.state() == StoreState::Empty);
  const uint8_t a[] = {1, 2, 3};
  CHECK(st.save(a, 3));
  RecordStore st2(be, 1);
  CHECK_EQ(st2.load(out, sizeof(out)), 3u);
  CHECK(st2.state() == StoreState::Ok && out[2] == 3);
}

TEST(store_alternates_slots_and_keeps_previous_on_interrupted_write) {
  RamBackend be;
  RecordStore st(be, 1);
  uint8_t out[64];
  st.load(out, sizeof(out));
  const uint8_t v1[] = {11}, v2[] = {22}, v3[] = {33};
  CHECK(st.save(v1, 1));
  CHECK(st.save(v2, 1));
  CHECK(!be.slot[0].empty() && !be.slot[1].empty());
  // Corte de energia a mitad de escribir v3: la ranura destino queda truncada.
  be.truncateNextWrite = 7;
  CHECK(!st.save(v3, 1));
  RecordStore after(be, 1);
  CHECK_EQ(after.load(out, sizeof(out)), 1u);
  CHECK_EQ(out[0], 22);  // el ultimo valido sobrevive
  CHECK(after.state() == StoreState::Ok);
  // Recuperacion: la siguiente escritura funciona y gana por generacion.
  CHECK(after.save(v3, 1));
  RecordStore again(be, 1);
  again.load(out, sizeof(out));
  CHECK_EQ(out[0], 33);
}

TEST(store_failed_write_reports_false_and_preserves) {
  RamBackend be;
  RecordStore st(be, 1);
  uint8_t out[64];
  st.load(out, sizeof(out));
  const uint8_t v1[] = {5};
  CHECK(st.save(v1, 1));
  be.failWrite = true;
  const uint8_t v2[] = {6};
  CHECK(!st.save(v2, 1));
  CHECK(!st.lastWriteOk());
  be.failWrite = false;
  be.corruptReadback = true;
  CHECK(!st.save(v2, 1));
  be.corruptReadback = false;
  RecordStore re(be, 1);
  re.load(out, sizeof(out));
  CHECK_EQ(out[0], 5);
}

TEST(store_detects_corruption_and_future_format) {
  RamBackend be;
  RecordStore st(be, 1);
  uint8_t out[64];
  st.load(out, sizeof(out));
  const uint8_t v[] = {9, 9};
  st.save(v, 2);
  be.slot[0][6] ^= 0xFF;  // dano en flash
  RecordStore c(be, 1);
  CHECK_EQ(c.load(out, sizeof(out)), 0u);
  CHECK(c.state() == StoreState::Corrupt);
  // Registro valido escrito por un firmware con formato 2: no se sobrescribe.
  uint8_t buf[64];
  const size_t n = RecordStore::encode(2, 50, v, 2, buf, sizeof(buf));
  be.slot[1].assign(buf, buf + n);
  RecordStore f(be, 1);
  CHECK_EQ(f.load(out, sizeof(out)), 0u);
  CHECK(f.state() == StoreState::FutureFormat);
  CHECK(!f.save(v, 2));
  CHECK_EQ(be.slot[1].size(), n);
}

TEST(store_generation_wraps_serially) {
  RamBackend be;
  uint8_t buf[64];
  const uint8_t a[] = {1}, b[] = {2};
  size_t n = RecordStore::encode(1, 0xFFFFFFFFu, a, 1, buf, sizeof(buf));
  be.slot[0].assign(buf, buf + n);
  n = RecordStore::encode(1, 0, b, 1, buf, sizeof(buf));  // generacion siguiente tras desbordar
  be.slot[1].assign(buf, buf + n);
  RecordStore st(be, 1);
  uint8_t out[8];
  st.load(out, sizeof(out));
  CHECK_EQ(out[0], 2);
}
