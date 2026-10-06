// Pruebas 3 (almacenamiento nodo B): LittleFS sin montar nunca se formatea en
// produccion, las calibraciones se rechazan sin informarse como guardadas y las
// ranuras previas quedan intactas.
#include "BioIoTCommon.h"
#include "CalibrationLogicB.h"
#include "StorageGuardB.h"
#include "test_framework.h"

using namespace bioiot;
using nodeb::decideFormat;
using nodeb::FormatDecision;
using nodeb::FsMount;

namespace {
// Igual que LittleFsSlots de NodeB.cpp: sin montar, no lee ni escribe.
struct UnmountedFs : SlotBackend {
  bool mounted = false;
  std::vector<uint8_t> slot[2];
  int writes = 0;
  size_t readSlot(uint8_t s, uint8_t* buf, size_t cap) override {
    if (!mounted || slot[s].empty() || slot[s].size() > cap) return 0;
    memcpy(buf, slot[s].data(), slot[s].size());
    return slot[s].size();
  }
  bool writeSlot(uint8_t s, const uint8_t* buf, size_t len) override {
    if (!mounted) return false;
    writes++;
    slot[s].assign(buf, buf + len);
    return true;
  }
};

CalibrationCmd cmdB(uint8_t op, uint8_t target) {
  CalibrationCmd c;
  c.op = op; c.target = target; c.cmdId = 7;
  return c;
}
}  // namespace

TEST(storage_b_format_requires_commissioning_build_and_token) {
  char token[nodeb::kFormatTokenSize];
  nodeb::formatConfirmToken(0x00A1B2C3u, token);
  CHECK_STR(token, "FORMAT-NODE-B-A1B2C3");
  nodeb::formatConfirmToken(0xFF00000Fu, token);  // solo 24 bits, como ESP.getChipId()
  CHECK_STR(token, "FORMAT-NODE-B-00000F");
  const uint32_t chip = 0x00A1B2C3u;
  // Produccion: nunca, aunque el token sea correcto y LittleFS no monte.
  CHECK(decideFormat(false, FsMount::MountFailed, "FORMAT-NODE-B-A1B2C3", chip) ==
        FormatDecision::RejectedProductionBuild);
  // Puesta en marcha: un LittleFS que monta no se formatea.
  CHECK(decideFormat(true, FsMount::Mounted, "FORMAT-NODE-B-A1B2C3", chip) == FormatDecision::RejectedAlreadyMounted);
  CHECK(decideFormat(true, FsMount::NoPartition, "FORMAT-NODE-B-A1B2C3", chip) == FormatDecision::RejectedNoPartition);
  // Token de otra placa, vacio o ausente.
  CHECK(decideFormat(true, FsMount::MountFailed, "FORMAT-NODE-B-000001", chip) == FormatDecision::RejectedConfirmation);
  CHECK(decideFormat(true, FsMount::MountFailed, "", chip) == FormatDecision::RejectedConfirmation);
  CHECK(decideFormat(true, FsMount::MountFailed, nullptr, chip) == FormatDecision::RejectedConfirmation);
  CHECK(decideFormat(true, FsMount::MountFailed, "format-node-b-a1b2c3", chip) == FormatDecision::RejectedConfirmation);
  CHECK(decideFormat(true, FsMount::MountFailed, "FORMAT-NODE-B-A1B2C3", chip) == FormatDecision::Allowed);
  CHECK_STR(nodeb::fsMountName(FsMount::MountFailed), "mount_failed_not_formatted");
}

TEST(storage_b_unmounted_rejects_calibration_and_keeps_previous_slots) {
  // Calibracion anterior valida en flash, pero LittleFS no monta en este arranque.
  UnmountedFs fs;
  fs.mounted = true;
  RecordStore writer(fs, 1);
  uint8_t buf[kStoreMaxPayload];
  writer.load(buf, sizeof(buf));
  NodeBCal previous;
  previous.o2[0].gain = 1.07f;
  uint8_t blob[kNodeBCalEncodedSize];
  CHECK_EQ(encodeNodeBCal(previous, blob, sizeof(blob)), kNodeBCalEncodedSize);
  CHECK(writer.save(blob, sizeof(blob)));
  const std::vector<uint8_t> before0 = fs.slot[0], before1 = fs.slot[1];
  const int writesBefore = fs.writes;

  fs.mounted = false;
  RecordStore st(fs, 1);
  CHECK_EQ(st.load(buf, sizeof(buf)), 0u);
  const bool storageWritable = fs.mounted && st.state() != StoreState::FutureFormat;
  NodeBCal active, cand;  // defaults en RAM

  CalibrationCmd set = cmdB(kCalOpSet, kTargetO2Gas1);
  set.present = 1u << calp::kGain; set.params[calp::kGain] = 1.2f;
  auto r = nodeb::applyCalibrationB(active, set, storageWritable, false, false, cand);
  nodeb::reportStorageUnavailableB(r, fs.mounted);
  CHECK(r.outcome == kOutcomeRejected && r.status == kStRejectedStorageFailed && !r.needsPersist);

  CalibrationCmd air = cmdB(kCalOpO2Air, kTargetO2Gas2);
  air.present = 1; air.params[0] = 20.9f;
  r = nodeb::applyCalibrationB(active, air, storageWritable, false, false, cand);
  nodeb::reportStorageUnavailableB(r, fs.mounted);
  CHECK(!r.startAirCalibration && r.status == kStRejectedStorageFailed);  // no toca el SEN0322

  CalibrationCmd imp = cmdB(kCalOpImport, kTargetAll);
  imp.blobLen = uint8_t(encodeNodeBCal(previous, imp.blob, sizeof(imp.blob)));
  imp.force = 1;
  r = nodeb::applyCalibrationB(active, imp, storageWritable, false, false, cand);
  nodeb::reportStorageUnavailableB(r, fs.mounted);
  CHECK(r.status == kStRejectedStorageFailed && !r.needsPersist);

  CHECK(!st.save(blob, sizeof(blob)));  // aunque se intentara, no se informa guardado
  CHECK(!st.lastWriteOk());
  CHECK(active.o2[0].gain == 1.0f);
  CHECK(fs.writes == writesBefore && fs.slot[0] == before0 && fs.slot[1] == before1);

  // Al volver a montar (siguiente arranque), la calibracion previa sigue ahi.
  fs.mounted = true;
  RecordStore again(fs, 1);
  CHECK_EQ(again.load(buf, sizeof(buf)), kNodeBCalEncodedSize);
  NodeBCal loaded;
  CHECK(decodeNodeBCal(buf, kNodeBCalEncodedSize, loaded) && loaded.o2[0].gain == 1.07f);
}

TEST(storage_b_mounted_rejections_keep_their_reason) {
  NodeBCal active, cand;
  CalibrationCmd air = cmdB(kCalOpO2Air, kTargetO2Gas1);
  air.present = 1; air.params[0] = 20.9f;
  auto r = nodeb::applyCalibrationB(active, air, true, true, false, cand);  // warm-up
  nodeb::reportStorageUnavailableB(r, true);
  CHECK(r.status == kStRejectedWarmingUpOrBusy);
  // Montado con formato futuro: sigue siendo "esquema futuro", no fallo de montaje.
  CalibrationCmd set = cmdB(kCalOpSet, kTargetO2Gas1);
  set.present = 1u << calp::kGain; set.params[calp::kGain] = 1.1f;
  r = nodeb::applyCalibrationB(active, set, false, false, false, cand);
  nodeb::reportStorageUnavailableB(r, true);
  CHECK(r.status == kStRejectedBusyOrFutureSchema && !r.needsPersist);
  // Un rechazo por parametros con almacenamiento montado no se reetiqueta.
  set.params[calp::kGain] = -1;
  r = nodeb::applyCalibrationB(active, set, true, false, false, cand);
  nodeb::reportStorageUnavailableB(r, true);
  CHECK(r.status == kStRejectedInvalidParameters);
}
