#include "BusB.h"

// Codigos de estado de linea: mismos valores que el diagnostico del ESP8266 para
// conservar sus nombres; en ESP32 se leen los niveles de SDA/SCL.
namespace {
constexpr uint8_t I2C_OK = 0, I2C_SCL_HELD_LOW = 1, I2C_SCL_HELD_LOW_AFTER_READ = 2, I2C_SDA_HELD_LOW = 3,
                  I2C_SDA_HELD_LOW_AFTER_INIT = 4;
}  // namespace

namespace nodeb {

const char* i2cErrorName(uint8_t code) {
  // Arduino-ESP32 Wire.endTransmission(): 0 ok, 1 dato largo, 2 NACK direccion, 3 NACK dato,
  // 4 otro error, 5 timeout.
  switch (code) {
    case 0: return "ok";
    case 1: return "data_too_long";
    case 2: return "address_nack";
    case 3: return "data_nack";
    case 4: return "other_error";
    case 5: return "timeout";
    default: return "unknown";
  }
}

const char* twiStatusName(uint8_t status) {
  switch (status) {
    case I2C_OK: return "ok";
    case I2C_SCL_HELD_LOW: return "scl_held_low";
    case I2C_SCL_HELD_LOW_AFTER_READ: return "scl_held_low_after_read";
    case I2C_SDA_HELD_LOW: return "sda_held_low";
    case I2C_SDA_HELD_LOW_AFTER_INIT: return "sda_held_low_after_init";
    default: return "unknown";
  }
}

void I2CBusB::begin() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(I2C_CLOCK_HZ);
  Wire.setTimeOut(I2C_TIMEOUT_MS);
}

I2CProbeResult I2CBusB::probe(uint8_t address) {
  Wire.beginTransmission(address);
  I2CProbeResult r;
  r.address = address;
  r.errorCode = Wire.endTransmission();
  r.detected = r.errorCode == 0;
  if (address == TCA_ADDR) {
    lastTcaProbe = r;
    tcaObserved = true;
    tcaProbeHistory.record(r.detected, millis());
  }
  return r;
}

TcaSelectResult I2CBusB::select(uint8_t channel) {
  TcaSelectResult r;
  r.channel = channel;
  if (channel > 7) return r;
  r.attempted = true;
  Wire.beginTransmission(TCA_ADDR);
  Wire.write(uint8_t(1u << channel));  // un solo canal: nunca dos sensores 0x23/0x73 a la vez
  r.errorCode = Wire.endTransmission();
  r.selected = r.errorCode == 0;
  tcaSelectHistory.record(r.selected, millis());
  return r;
}

TcaSelectResult I2CBusB::disableAll() {
  TcaSelectResult r;
  r.attempted = true;
  Wire.beginTransmission(TCA_ADDR);
  Wire.write(uint8_t(0x00));
  r.errorCode = Wire.endTransmission();
  r.selected = r.errorCode == 0;
  tcaDisableHistory.record(r.selected, millis());
  return r;
}

// Con el bus en reposo ambas lineas deben estar altas (pull-up). Se leen sin cambiar
// su funcion: el periferico I2C mantiene la entrada habilitada (drenador abierto).
uint8_t I2CBusB::lineStatus() {
  if (digitalRead(I2C_SCL_PIN) == LOW) return I2C_SCL_HELD_LOW;
  if (digitalRead(I2C_SDA_PIN) == LOW) return I2C_SDA_HELD_LOW;
  return I2C_OK;
}

RecoveryEvidence I2CBusB::recover() {
  RecoveryEvidence e;
  e.performed = true;
  e.tcaBefore = probe(TCA_ADDR);
  e.statusBefore = lineStatus();
  pinMode(I2C_SDA_PIN, INPUT_PULLUP);
  pinMode(I2C_SCL_PIN, INPUT_PULLUP);
  delayMicroseconds(5);
  for (uint8_t i = 0; i < 9 && digitalRead(I2C_SDA_PIN) == LOW; ++i) {
    pinMode(I2C_SCL_PIN, OUTPUT);
    digitalWrite(I2C_SCL_PIN, LOW);
    delayMicroseconds(5);
    pinMode(I2C_SCL_PIN, INPUT_PULLUP);
    delayMicroseconds(5);
    e.pulses++;
  }
  // Condicion STOP: SDA baja->alta con SCL alta.
  pinMode(I2C_SDA_PIN, OUTPUT);
  digitalWrite(I2C_SDA_PIN, LOW);
  delayMicroseconds(5);
  pinMode(I2C_SDA_PIN, INPUT_PULLUP);
  delayMicroseconds(5);
  e.sdaReleased = digitalRead(I2C_SDA_PIN) == HIGH;
  Wire.end();  // ESP32: liberar el periferico antes de reiniciarlo
  begin();
  disableAll();
  e.statusAfter = lineStatus();
  e.tcaAfter = probe(TCA_ADDR);
  return e;
}

}  // namespace nodeb
