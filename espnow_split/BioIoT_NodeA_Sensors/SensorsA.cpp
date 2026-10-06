#include "SensorsA.h"

#include "SensorMathA.h"

namespace nodea {

const uint8_t kAnalogMuxChannel[kAnalogChannels] = {CH_PH, CH_CO2_1, CH_CO2_2, CH_TURB, CH_DO, CH_TDS};
const char* const kAnalogSensorNames[kAnalogChannels] = {"ph", "co2_1", "co2_2", "turbidity", "dissolved_oxygen", "tds"};

// ---------------- CD74HC4067 ----------------
void AnalogSampler::begin() {
  pinMode(MUX_S0_PIN, OUTPUT);
  pinMode(MUX_S1_PIN, OUTPUT);
  pinMode(MUX_S2_PIN, OUTPUT);
  pinMode(MUX_S3_PIN, OUTPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(MUX_SIG_PIN, ADC_11db);
}

void AnalogSampler::select(uint8_t channel) {
  digitalWrite(MUX_S0_PIN, bitRead(channel, 0));
  digitalWrite(MUX_S1_PIN, bitRead(channel, 1));
  digitalWrite(MUX_S2_PIN, bitRead(channel, 2));
  digitalWrite(MUX_S3_PIN, bitRead(channel, 3));
}

void AnalogSampler::loop(uint32_t nowMs) {
  const uint32_t nowUs = micros();
  switch (state_) {
    case Idle:
      if (!sweepRequested_ && nowMs - lastSweepMs < NODE_A_ANALOG_SWEEP_INTERVAL_MS) return;
      sweepRequested_ = false;
      index_ = 0;
      select(kAnalogMuxChannel[index_]);
      stepUs_ = nowUs;
      state_ = Settling;
      return;
    case Settling:
      if (nowUs - stepUs_ < kMuxSettleUs) return;
      analogRead(MUX_SIG_PIN);  // descarta la primera conversion tras conmutar
      acc_ = 0; count_ = 0; rawMin_ = 4095; rawMax_ = 0;
      stepUs_ = nowUs - kAnalogSampleGapUs;  // primera muestra inmediata
      state_ = Sampling;
      return;
    case Sampling: {
      if (nowUs - stepUs_ < kAnalogSampleGapUs) return;
      const int sample = analogRead(MUX_SIG_PIN);
      stepUs_ = micros();
      acc_ += sample;
      rawMin_ = min(rawMin_, sample);
      rawMax_ = max(rawMax_, sample);
      if (++count_ < kAnalogSamples) return;
      finishChannel(nowMs);
      if (++index_ >= kAnalogChannels) {
        state_ = Idle;
        lastSweepMs = nowMs;
        sweeps_++;
        updateDiagnostics();
      } else {
        select(kAnalogMuxChannel[index_]);
        stepUs_ = micros();
        state_ = Settling;
      }
      return;
    }
  }
}

void AnalogSampler::finishChannel(uint32_t nowMs) {
  AnalogResult& r = results_[index_];
  r.sampled = true;
  r.raw = int(acc_ / kAnalogSamples);
  r.voltage = adcToVoltage(float(r.raw));
  r.connected = analogConnected(r.raw);
  r.rawMin = rawMin_;
  r.rawMax = rawMax_;
  r.rawAvg = float(acc_) / kAnalogSamples;
  r.voltageAvg = adcToVoltage(r.rawAvg);
  r.sampledAtMs = nowMs;
}

void AnalogSampler::updateDiagnostics() {
  batchValid = true;
  minAverage = 4095; maxAverage = 0;
  allNearZero = true;
  for (uint8_t i = 0; i < kAnalogChannels; ++i) {
    batchValid = batchValid && results_[i].sampled;
    minAverage = fminf(minAverage, results_[i].rawAvg);
    maxAverage = fmaxf(maxAverage, results_[i].rawAvg);
    allNearZero = allNearZero && results_[i].rawAvg <= 5;
  }
  allNearZero = batchValid && allNearZero;
  tooSimilar = batchValid && maxAverage - minAverage <= kAnalogSimilarSpread;
  if (batchValid && allNearZero) allZeroCount++;
}

// ---------------- TCS3200 ----------------
void ColorSampler::begin() {
  pinMode(TCS_S0, OUTPUT); pinMode(TCS_S1, OUTPUT);
  pinMode(TCS_S2, OUTPUT); pinMode(TCS_S3, OUTPUT);
  pinMode(TCS_OUT_1, INPUT); pinMode(TCS_OUT_2, INPUT);
  digitalWrite(TCS_S0, HIGH); digitalWrite(TCS_S1, LOW);  // escala 20 % (v4)
}

void ColorSampler::loop(uint32_t nowMs) {
  if (step_ == 0) {
    if (!requested_ && nowMs - lastCycleMs_ < NODE_A_COLOR_INTERVAL_MS) return;
    requested_ = false;
    lastCycleMs_ = nowMs;
    sensor_ = 0;
    step_ = 1;
    nextMs_ = nowMs;
    working_ = ColorResult();
  }
  if (int32_t(nowMs - nextMs_) < 0) return;
  const int pin = sensor_ == 0 ? TCS_OUT_1 : TCS_OUT_2;
  // R=(L,L), G=(H,H), B=(L,H). Esperar tras conmutar ANTES de pulseIn:
  // el primer pulso durante la transicion puede pertenecer al filtro anterior.
  if (!filterSelected_) {
    digitalWrite(TCS_S2, step_ == 2 ? HIGH : LOW);
    digitalWrite(TCS_S3, step_ == 1 ? LOW : HIGH);
    filterSelected_ = true;
    nextMs_ = millis() + kTcsStepGapMs;
    return;
  }
  filterSelected_ = false;
  if (step_ == 1) {
    working_.rPulse = pulseIn(pin, LOW, kTcsPulseTimeoutUs);
  } else if (step_ == 2) {
    working_.gPulse = pulseIn(pin, LOW, kTcsPulseTimeoutUs);
  } else {
    working_.bPulse = pulseIn(pin, LOW, kTcsPulseTimeoutUs);
  }
  nextMs_ = millis();  // El siguiente filtro se selecciona en otra vuelta.
  if (++step_ <= 3) return;
  ColorResult& r = working_;
  r.connected = !(r.rPulse == 0 || r.gPulse == 0 || r.bPulse == 0);
  r.r = pulseToIntensity(r.rPulse);
  r.g = pulseToIntensity(r.gPulse);
  r.b = pulseToIntensity(r.bPulse);
  rgbToHsl(r.r, r.g, r.b, r.h, r.s, r.l);
  r.sampled = true;
  r.sampledAtMs = nowMs;
  results_[sensor_] = r;
  reads_++;
  if (sensor_ == 0) {
    sensor_ = 1;
    step_ = 1;
    working_ = ColorResult();
  } else {
    step_ = 0;
  }
}

// ---------------- DS18B20 ----------------
void TempSampler::begin() {
  ds18_.begin();
  ds18_.setWaitForConversion(false);  // conversion asincrona: sin esperas de 750 ms
  result_.deviceCount = ds18_.getDeviceCount();
  waitMs_ = ds18_.millisToWaitForConversion(ds18_.getResolution());
  if (waitMs_ < 94 || waitMs_ > 750) waitMs_ = 750;
}

void TempSampler::loop(uint32_t nowMs) {
  if (!converting_) {
    if (!requested_ && nowMs - lastMs_ < NODE_A_TEMP_INTERVAL_MS) return;
    requested_ = false;
    lastMs_ = nowMs;
    ds18_.requestTemperatures();
    startedMs_ = nowMs;
    converting_ = true;
    return;
  }
  if (nowMs - startedMs_ < waitMs_) return;
  converting_ = false;
  const float tempC = ds18_.getTempCByIndex(0);
  result_.sampled = true;
  result_.raw = tempC;
  result_.connected = ds18Connected(tempC);
  result_.deviceCount = ds18_.getDeviceCount();
  result_.sampledAtMs = nowMs;
}

}  // namespace nodea
