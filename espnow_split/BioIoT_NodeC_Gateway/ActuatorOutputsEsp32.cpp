#include "ActuatorOutputsEsp32.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_arduino_version.h>

#include "gateway_config.h"

static_assert(COMPRESSOR_PIN != LED_R_PIN && COMPRESSOR_PIN != LED_G_PIN && COMPRESSOR_PIN != LED_B_PIN &&
                  LED_R_PIN != LED_G_PIN && LED_R_PIN != LED_B_PIN && LED_G_PIN != LED_B_PIN,
              "Los pines de actuadores deben ser distintos");
static_assert(COMPRESSOR_PIN != PUMP_PIN_RESERVED && LED_R_PIN != PUMP_PIN_RESERVED && LED_G_PIN != PUMP_PIN_RESERVED &&
                  LED_B_PIN != PUMP_PIN_RESERVED,
              "GPIO2 queda reservado (pin de arranque)");
static_assert(LED_PWM_RESOLUTION_BITS == 8, "Los comandos RGB usan 8 bits (v4)");
#if ESP_ARDUINO_VERSION_MAJOR < 3
#error "El gateway requiere Arduino-ESP32 3.x"
#endif

namespace {
const uint8_t kRgbPins[3] = {LED_R_PIN, LED_G_PIN, LED_B_PIN};
}

bool Esp32Outputs::begin(bool commissioning, bool compressorConfirmed, bool ledConfirmed) {
  compressorEnabled_ = ledEnabled_ = false;
  if (commissioning) return true;  // ningun GPIO de carga se toca
  if (compressorConfirmed) {
    // Precargar el nivel de reposo antes de habilitar la salida (v4).
    gpio_set_level(static_cast<gpio_num_t>(COMPRESSOR_PIN), COMPRESSOR_ACTIVE_LOW ? 1 : 0);
    pinMode(COMPRESSOR_PIN, OUTPUT);
    digitalWrite(COMPRESSOR_PIN, COMPRESSOR_ACTIVE_LOW ? HIGH : LOW);
    compressorEnabled_ = true;
  }
  if (!ledConfirmed) return true;
  bool attached[3] = {false, false, false};
  bool ok = true;
  for (uint8_t i = 0; i < 3; ++i) {
    gpio_set_level(static_cast<gpio_num_t>(kRgbPins[i]), LED_PWM_ACTIVE_LOW ? 1 : 0);
    pinMode(kRgbPins[i], OUTPUT);
    attached[i] = ledcAttach(kRgbPins[i], LED_PWM_FREQUENCY_HZ, LED_PWM_RESOLUTION_BITS);
    if (attached[i]) ledcWrite(kRgbPins[i], LED_PWM_ACTIVE_LOW ? 255 : 0);
    ok = ok && attached[i];
  }
  if (!ok) {
    for (uint8_t i = 0; i < 3; ++i) {
      if (attached[i]) ledcDetach(kRgbPins[i]);
      pinMode(kRgbPins[i], OUTPUT);
      digitalWrite(kRgbPins[i], LED_PWM_ACTIVE_LOW ? HIGH : LOW);
    }
    return false;  // ready=false: encendidos bloqueados (v4: pwm_init_failed)
  }
  ledEnabled_ = true;
  return true;
}

void Esp32Outputs::writeCompressor(bool on) {
  if (!compressorEnabled_) return;
  digitalWrite(COMPRESSOR_PIN, on != COMPRESSOR_ACTIVE_LOW ? HIGH : LOW);
}

void Esp32Outputs::writeLed(uint8_t dutyR, uint8_t dutyG, uint8_t dutyB) {
  if (!ledEnabled_) return;
  const uint8_t d[3] = {dutyR, dutyG, dutyB};
  for (uint8_t i = 0; i < 3; ++i) ledcWrite(kRgbPins[i], LED_PWM_ACTIVE_LOW ? 255 - d[i] : d[i]);
}

void Esp32Outputs::forceOff() {
  writeCompressor(false);
  writeLed(0, 0, 0);
}
