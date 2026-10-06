#include "SensorsB.h"

namespace nodeb {

void SensorsB::begin(I2CBusB* bus, WarmupPolicy* warmup, const bioiot::NodeBCal* cal) {
  bus_ = bus;
  warmup_ = warmup;
  cal_ = cal;
  lastLightMs_ = millis() - NODE_B_LIGHT_INTERVAL_MS;
  lastWarmProbeMs_ = millis() - 10000;
}

uint8_t SensorsB::expectedAddress(uint8_t channel) const {
  if (channel == TCA_CH_BH1750_1) return BH1750_ADDR_1;
  if (channel == TCA_CH_BH1750_2) return BH1750_ADDR_2;
  if (channel == TCA_CH_O2_1) return O2_ADDR_1;
  if (channel == TCA_CH_O2_2) return O2_ADDR_2;
  return 0;
}

bool SensorsB::prepare(uint8_t dev, uint8_t channel, uint8_t address) {
  I2CReadingDiag& d = readingDiag[dev];
  d = I2CReadingDiag();
  d.observed = true;
  d.probe.address = address;
  d.selection = bus_->select(channel);
  if (!d.selection.selected) {
    d.status = "tca_channel_select_failed";  // no se acusa al sensor
    return false;
  }
  return true;
}

bool SensorsB::probeReading(uint8_t dev, uint8_t address) {
  I2CReadingDiag& d = readingDiag[dev];
  d.probeAttempted = true;
  d.probe = bus_->probe(address);
  if (!d.probe.detected) {
    d.status = "expected_device_missing";
    deviceHistory[dev].record(false, millis());
    return false;
  }
  return true;
}

void SensorsB::finish(uint8_t dev, bool ok, const char* status) {
  readingDiag[dev].status = status;
  deviceHistory[dev].record(ok, millis());
}

void SensorsB::clearFilter(uint8_t sensor) {
  filter[sensor].clear();
  o2[sensor] = O2Reading();
}

void SensorsB::requestFreshReads(bool lightReq, bool o2Req) {
  if (lightReq) lightQueued_[0] = lightQueued_[1] = true;
  if (o2Req) o2Queued_[0] = o2Queued_[1] = true;
}

bool SensorsB::takeScanResult() {
  if (scanPhase_ != ScanDone) return false;
  scanPhase_ = ScanIdle;
  return true;
}

bool SensorsB::startAirCalibration(uint8_t sensor) {
  if (airPending_ || scanActive() || sensor > 1) return false;
  airPending_ = true;
  airDone_ = false;
  airSensor_ = sensor;
  return true;
}

bool SensorsB::airCalibrationFinished(bool& commOk, TcaSelectResult& disabled) {
  if (!airDone_) return false;
  airDone_ = false;
  commOk = airCommOk_;
  disabled = airDisabled_;
  return true;
}

bool SensorsB::startScan(bool recoverFirst) {
  if (scanActive() || airPending_) return false;
  scanRecover_ = recoverFirst;
  scanNeedsInit_ = true;
  scanAddress_ = 1;
  scanChannel_ = 0;
  scanPhase_ = ScanMain;
  scanStartedMs = millis();
  return true;
}

void SensorsB::startTask(Task t, uint32_t nowMs) {
  task_ = t;
  step_ = 0;
  waitUntil_ = nowMs;
}

void SensorsB::loop(uint32_t nowMs) {
  if (task_ == TaskNone) {
    // Punto seguro: el escaneo diagnostico tiene prioridad sobre las lecturas.
    if (scanPhase_ != ScanIdle && scanPhase_ != ScanDone) { stepScan(nowMs); return; }
    if (airPending_) { startTask(TaskAirCal, nowMs); }
    else {
      const bool warming = warmup_->warming(nowMs);
      if (nowMs - lastLightMs_ >= NODE_B_LIGHT_INTERVAL_MS) {
        lastLightMs_ = nowMs;
        lightQueued_[0] = lightQueued_[1] = true;
      }
      if (warming) {
        // Durante el warm-up solo se prueba el transporte (como v4); cada 10 s.
        if (nowMs - lastWarmProbeMs_ >= 10000) {
          lastWarmProbeMs_ = nowMs;
          o2Queued_[0] = o2Queued_[1] = true;
        }
      } else if (!o2EverAfterWarmup_ || nowMs - lastO2Ms_ >= NODE_B_O2_SAMPLE_INTERVAL_MS) {
        o2EverAfterWarmup_ = true;
        lastO2Ms_ = nowMs;
        o2Queued_[0] = o2Queued_[1] = true;
      }
      if (lightQueued_[0]) { lightQueued_[0] = false; startTask(TaskBh1, nowMs); }
      else if (lightQueued_[1]) { lightQueued_[1] = false; startTask(TaskBh2, nowMs); }
      else if (o2Queued_[0]) { o2Queued_[0] = false; startTask(TaskO2_1, nowMs); }
      else if (o2Queued_[1]) { o2Queued_[1] = false; startTask(TaskO2_2, nowMs); }
      else return;
    }
  }
  if (int32_t(nowMs - waitUntil_) < 0) return;
  switch (task_) {
    case TaskBh1: stepLight(0, nowMs); break;
    case TaskBh2: stepLight(1, nowMs); break;
    case TaskO2_1: stepO2(0, nowMs); break;
    case TaskO2_2: stepO2(1, nowMs); break;
    case TaskAirCal: stepAirCal(nowMs); break;
    default: task_ = TaskNone; break;
  }
}

void SensorsB::stepLight(uint8_t i, uint32_t nowMs) {
  const uint8_t dev = i == 0 ? kDevBh1 : kDevBh2;
  const uint8_t channel = i == 0 ? TCA_CH_BH1750_1 : TCA_CH_BH1750_2;
  const uint8_t addr = i == 0 ? BH1750_ADDR_1 : BH1750_ADDR_2;
  LightResult& r = light[i];
  if (step_ == 0) {
    r.connected = false;
    r.lux = NAN;
    r.sampled = true;
    r.sampledAtMs = nowMs;
    if (!prepare(dev, channel, addr) || !probeReading(dev, addr)) {
      if (i == 1) lightCycles++;
      task_ = TaskNone;
      return;
    }
    // delay(5) de v4 tras seleccionar: incluido en la espera siguiente.
    if (!bh_.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, addr, &Wire)) {
      finish(dev, false, "driver_init_failed");
      if (i == 1) lightCycles++;
      task_ = TaskNone;
      return;
    }
    step_ = 1;
    waitUntil_ = nowMs + NODE_B_BH1750_MEASURE_MS;
    return;
  }
  // El canal sigue seleccionado: ninguna otra tarea usa el bus durante la espera.
  const float lux = bh_.readLightLevel();
  r.sampledAtMs = millis();
  r.connected = !(isnan(lux) || lux < 0);
  r.lux = r.connected ? lux : NAN;
  r.driverCode = r.connected ? NAN : lux;
  finish(dev, r.connected, r.connected ? "measurement_ok" : "invalid_measurement");
  if (i == 1) lightCycles++;
  task_ = TaskNone;
}

void SensorsB::stepO2(uint8_t i, uint32_t nowMs) {
  const uint8_t dev = i == 0 ? kDevO2_1 : kDevO2_2;
  const uint8_t channel = i == 0 ? TCA_CH_O2_1 : TCA_CH_O2_2;
  const uint8_t addr = i == 0 ? O2_ADDR_1 : O2_ADDR_2;
  DFRobot_OxygenSensor& drv = o2Driver_[i];
  O2Transport& t = transport_;
  auto conclude = [&](const char* status, bool ok) {
    evaluateO2(t, cal_->o2[i], filter[i], o2[i], millis());
    if (status) finish(dev, ok, status);
    if (i == 1) o2Cycles++;
    task_ = TaskNone;
  };
  switch (step_) {
    case 0:
      t = O2Transport();
      t.warming = warmup_->warming(nowMs);
      t.selected = prepare(dev, channel, addr);
      if (!t.selected) { t.selectError = readingDiag[dev].selection.errorCode; conclude(nullptr, false); return; }
      t.probeOk = probeReading(dev, addr);
      if (!t.probeOk) { t.probeError = readingDiag[dev].probe.errorCode; conclude(nullptr, false); return; }
      if (t.warming) { readingDiag[dev].status = "warming_up"; conclude(nullptr, true); return; }
      t.beginOk = drv.begin(addr);
      if (!t.beginOk) {
        t.wireError = drv.lastWireError(); t.readCount = drv.lastReadCount();
        conclude("communication_fault", false);
        return;
      }
      if (!drv.startKeyRead()) {
        t.wireError = drv.lastWireError(); t.readCount = drv.lastReadCount();
        conclude("communication_fault", false);
        return;
      }
      step_ = 1;
      waitUntil_ = nowMs + 50;  // espera de readFlash() en v4
      return;
    case 1:
      if (!drv.finishKeyRead() || !drv.startDataRead()) {
        t.wireError = drv.lastWireError(); t.readCount = drv.lastReadCount();
        conclude("communication_fault", false);
        return;
      }
      step_ = 2;
      waitUntil_ = nowMs + 100;  // espera de lectura de dato en v4
      return;
    default: {
      float raw = NAN;
      t.commOk = drv.finishDataRead(raw);
      t.raw = raw;
      t.wireError = drv.lastWireError();
      t.readCount = drv.lastReadCount();
      const float value = evaluateO2(t, cal_->o2[i], filter[i], o2[i], millis());
      if (!t.commOk) finish(dev, false, "communication_fault");
      else if (!isfinite(value)) finish(dev, false, "invalid_measurement");
      else finish(dev, true, "measurement_ok");
      if (i == 1) o2Cycles++;
      task_ = TaskNone;
      return;
    }
  }
}

void SensorsB::stepAirCal(uint32_t nowMs) {
  const uint8_t i = airSensor_;
  const uint8_t channel = i == 0 ? TCA_CH_O2_1 : TCA_CH_O2_2;
  const uint8_t addr = i == 0 ? O2_ADDR_1 : O2_ADDR_2;
  DFRobot_OxygenSensor& drv = o2Driver_[i];
  airCommOk_ = false;
  if (!warmup_->warming(nowMs) && bus_->select(channel).selected && bus_->probe(addr).detected && drv.begin(addr)) {
    drv.calibrate(20.9f);  // API void: el ACK de escritura NO prueba la calibracion
    airCommOk_ = drv.communicationOk();
  }
  clearFilter(i);
  airDisabled_ = bus_->disableAll();
  airPending_ = false;
  airDone_ = true;
  task_ = TaskNone;
}

void SensorsB::stepScan(uint32_t nowMs) {
  switch (scanPhase_) {
    case ScanMain:
      if (scanNeedsInit_) {
        scanNeedsInit_ = false;
        for (auto& s : channelScans) s = ScanSegment();
        mainScan = ScanSegment();
        recovery = RecoveryEvidence();
        if (scanRecover_) recovery = bus_->recover();
        isolation = bus_->disableAll();
      }
      {
        const I2CProbeResult p = bus_->probe(scanAddress_);
        mainScan.tested[scanAddress_] = true;
        mainScan.codes[scanAddress_] = p.errorCode;
        if (scanAddress_ == TCA_ADDR) tcaProbe = p;
        if (scanAddress_ == TCA_ALT_ADDR) altTcaProbe = p;
      }
      if (++scanAddress_ > 0x7E) {
        mainScan.completed = true;
        scanChannel_ = 0;
        scanPhase_ = ScanSelect;
      }
      return;
    case ScanSelect:
      if (scanChannel_ >= 8) {
        cleanup = bus_->disableAll();
        scanPhase_ = ScanDone;
        scanCompletedMs = nowMs;
        return;
      }
      if (!tcaProbe.detected) { scanChannel_++; return; }  // no comprobable: no se acusa al sensor
      channelScans[scanChannel_].selection = bus_->select(scanChannel_);
      if (!channelScans[scanChannel_].selection.selected) { scanChannel_++; return; }
      scanAddress_ = 1;
      scanPhase_ = ScanChannel;
      return;
    case ScanChannel: {
      ScanSegment& s = channelScans[scanChannel_];
      const bool o2Channel = scanChannel_ == TCA_CH_O2_1 || scanChannel_ == TCA_CH_O2_2;
      if (o2Channel && scanAddress_ == expectedAddress(scanChannel_) && warmup_->warming(nowMs)) {
        s.warmupSkipped = true;  // sin probe del O2 durante el calentamiento
      } else {
        const I2CProbeResult p = bus_->probe(scanAddress_);
        s.tested[scanAddress_] = true;
        s.codes[scanAddress_] = p.errorCode;
      }
      if (++scanAddress_ > 0x7E) {
        s.completed = true;
        scanChannel_++;
        scanPhase_ = ScanSelect;
      }
      return;
    }
    default:
      return;
  }
}

}  // namespace nodeb
