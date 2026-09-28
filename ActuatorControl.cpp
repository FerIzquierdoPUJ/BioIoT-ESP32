#include "ActuatorControl.h"
#include "actuator_configs.h"
#include <esp_arduino_version.h>
#include <driver/gpio.h>
#include <time.h>

namespace {
static_assert(LED_PWM_RESOLUTION_BITS == 8, "RGB commands require 8-bit PWM");
static_assert(COMPRESSOR_RELAY_PIN != LED_R_PIN && COMPRESSOR_RELAY_PIN != LED_G_PIN &&
              COMPRESSOR_RELAY_PIN != LED_B_PIN && LED_R_PIN != LED_G_PIN &&
              LED_R_PIN != LED_B_PIN && LED_G_PIN != LED_B_PIN,
              "Actuator pins must be distinct");
static_assert(ACTUATOR_DISCONNECT_GRACE_MS < ACTUATOR_MAX_LEASE_SECONDS * 1000UL,
              "Disconnect grace must be shorter than the maximum lease");
const uint8_t rgbPins[] = {LED_R_PIN, LED_G_PIN, LED_B_PIN};
bool outputsReady = false;
bool compressorOn = false;
uint8_t rgb[3] = {0, 0, 0};
uint8_t brightness = 0;
uint32_t compressorOffAt = 0;
uint32_t compressorLeaseAt = 0, compressorLeaseMs = 0;
uint32_t ledLeaseAt = 0, ledLeaseMs = 0;
uint64_t compressorExpiresAt = 0, ledExpiresAt = 0;
bool reportPending = false;
bool cloudLost = false;
uint32_t cloudLostAt = 0;
String lastCommandId;
bool lastAccepted = false;
const char* lastCommandReason = "no_command";
const char* lastStopReason = "boot";

uint8_t dutyFor(uint8_t color, uint8_t level) {
  return (uint16_t(color) * level + 127U) / 255U;
}

void writeColor(uint8_t index, uint8_t duty) {
  const uint8_t output = LED_PWM_ACTIVE_LOW ? 255 - duty : duty;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(rgbPins[index], output);
#else
  ledcWrite(index, output);
#endif
}

void setCompressor(bool on) {
  digitalWrite(COMPRESSOR_RELAY_PIN,
               on != COMPRESSOR_RELAY_ACTIVE_LOW ? HIGH : LOW);
  if (compressorOn && !on) compressorOffAt = millis();
  compressorOn = on;
  if (!on) { compressorLeaseMs = 0; compressorExpiresAt = 0; }
}

bool ledIsOn() {
  return dutyFor(rgb[0], brightness) || dutyFor(rgb[1], brightness) ||
         dutyFor(rgb[2], brightness);
}

void clearLed() {
  for (uint8_t i = 0; i < 3; ++i) {
    if (outputsReady) writeColor(i, 0);
    rgb[i] = 0;
  }
  brightness = 0;
  ledLeaseMs = 0;
  ledExpiresAt = 0;
}

void recordResult(bool accepted, const char* reason) {
  lastAccepted = accepted;
  lastCommandReason = reason;
  reportPending = true;
  Serial.printf("Actuadores: %s (%s)\n", accepted ? "aceptado" : "rechazado", reason);
}

bool readLease(JsonObjectConst command, uint64_t& expiresAt, uint32_t& leaseMs) {
  if (!command["expiresAt"].is<uint64_t>()) return false;
  const time_t now = time(NULL);
  if (now < 1600000000) return false;
  expiresAt = command["expiresAt"].as<uint64_t>();
  if (expiresAt <= uint64_t(now) || expiresAt - uint64_t(now) > ACTUATOR_MAX_LEASE_SECONDS)
    return false;
  leaseMs = uint32_t(expiresAt - uint64_t(now)) * 1000UL;
  return true;
}
} // namespace

void initActuators() {
  // Configurar las salidas antes de WiFi/NTP; los estados no se restauran de NVS.
  // Precargar el latch antes de habilitar salida. digitalWrite antes de pinMode
  // no es fiable en Arduino-ESP32 3.x (administracion de perifericos).
  gpio_set_level(static_cast<gpio_num_t>(COMPRESSOR_RELAY_PIN),
                 COMPRESSOR_RELAY_ACTIVE_LOW ? HIGH : LOW);
  pinMode(COMPRESSOR_RELAY_PIN, OUTPUT);
  setCompressor(false);
  compressorOffAt = millis();
  bool attached[3] = {false, false, false};
  outputsReady = true;
  for (uint8_t i = 0; i < 3; ++i) {
    gpio_set_level(static_cast<gpio_num_t>(rgbPins[i]), LED_PWM_ACTIVE_LOW ? HIGH : LOW);
    pinMode(rgbPins[i], OUTPUT);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    attached[i] = ledcAttach(rgbPins[i], LED_PWM_FREQUENCY_HZ, LED_PWM_RESOLUTION_BITS);
#else
    attached[i] = ledcSetup(i, LED_PWM_FREQUENCY_HZ, LED_PWM_RESOLUTION_BITS) > 0;
    if (attached[i]) ledcAttachPin(rgbPins[i], i);
#endif
    if (attached[i]) writeColor(i, 0);
    outputsReady = outputsReady && attached[i];
  }
  if (!outputsReady) {
    for (uint8_t i = 0; i < 3; ++i) {
      if (attached[i]) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
        ledcDetach(rgbPins[i]);
#else
        ledcDetachPin(rgbPins[i]);
#endif
      }
      pinMode(rgbPins[i], OUTPUT);
      digitalWrite(rgbPins[i], LED_PWM_ACTIVE_LOW ? HIGH : LOW);
    }
    lastStopReason = "pwm_init_failed";
    Serial.println("Error inicializando PWM; actuadores bloqueados.");
  }
}

void stopActuators(const char* reason) {
  if (compressorOn || ledIsOn()) {
    lastStopReason = reason;
    reportPending = true;
  }
  setCompressor(false);
  clearLed();
}

void serviceActuators(bool cloudConnected) {
  const uint32_t nowMs = millis();
  if (cloudConnected) {
    cloudLost = false;
  } else {
    if (!cloudLost) { cloudLost = true; cloudLostAt = nowMs; }
    if (nowMs - cloudLostAt >= ACTUATOR_DISCONNECT_GRACE_MS) {
      stopActuators("cloud_disconnected"); return;
    }
  }
  const time_t now = time(NULL);
  if (compressorOn && (nowMs - compressorLeaseAt >= compressorLeaseMs ||
                       now < 1600000000 || uint64_t(now) >= compressorExpiresAt)) {
    setCompressor(false);
    lastStopReason = "compressor_lease_expired";
    reportPending = true;
  }
  if (ledIsOn() && (nowMs - ledLeaseAt >= ledLeaseMs ||
                    now < 1600000000 || uint64_t(now) >= ledExpiresAt)) {
    clearLed();
    lastStopReason = "led_lease_expired";
    reportPending = true;
  }
}

void handleActuatorCommand(JsonObjectConst command) {
  lastCommandId = "";
  if (command.containsKey("commandId")) {
    if (!command["commandId"].is<const char*>() ||
        strlen(command["commandId"].as<const char*>()) > 64) {
      recordResult(false, "invalid_command_id"); return;
    }
    lastCommandId = command["commandId"].as<const char*>();
  }
  const char* action = command["action"] | "";
  if (strcmp(action, "all_off") == 0) {
    stopActuators("all_off"); recordResult(true, "all_off"); return;
  }
  const char* target = command["target"] | "";
  if (!outputsReady) { recordResult(false, "outputs_not_ready"); return; }
  if (strcmp(target, "compressor") == 0) {
    if (!command["on"].is<bool>()) { recordResult(false, "on_must_be_boolean"); return; }
    const bool on = command["on"].as<bool>();
    if (!on) { setCompressor(false); recordResult(true, "compressor_off"); return; }
    uint64_t expiresAt;
    uint32_t leaseMs;
    if (!readLease(command, expiresAt, leaseMs)) {
      recordResult(false, "invalid_or_expired_lease"); return;
    }
    if (!compressorOn && millis() - compressorOffAt < COMPRESSOR_MIN_OFF_MS) {
      recordResult(false, "compressor_min_off_time"); return;
    }
    compressorLeaseAt = millis(); compressorLeaseMs = leaseMs;
    compressorExpiresAt = expiresAt;
    setCompressor(true);
    recordResult(true, "compressor_on");
  } else if (strcmp(target, "led") == 0) {
    const char* keys[] = {"r", "g", "b", "brightness"};
    uint8_t values[4];
    for (uint8_t i = 0; i < 4; ++i) {
      if (!command[keys[i]].is<int>() || command[keys[i]].as<int>() < 0 ||
          command[keys[i]].as<int>() > 255) {
        recordResult(false, "rgb_and_brightness_must_be_0_to_255"); return;
      }
      values[i] = command[keys[i]].as<int>();
    }
    const bool on = dutyFor(values[0], values[3]) || dutyFor(values[1], values[3]) ||
                    dutyFor(values[2], values[3]);
    uint64_t expiresAt = 0;
    uint32_t leaseMs = 0;
    if (on && !readLease(command, expiresAt, leaseMs)) {
      recordResult(false, "invalid_or_expired_lease"); return;
    }
    brightness = values[3];
    for (uint8_t i = 0; i < 3; ++i) {
      rgb[i] = values[i]; writeColor(i, dutyFor(rgb[i], brightness));
    }
    ledLeaseAt = millis(); ledLeaseMs = leaseMs; ledExpiresAt = expiresAt;
    recordResult(true, on ? "led_set" : "led_off");
  } else {
    recordResult(false, "unknown_target");
  }
}

String actuatorsJson() {
  JsonDocument doc;
  doc["ready"] = outputsReady;
  doc["mode"] = "manual_c2d";
  doc["lastStopReason"] = lastStopReason;
  JsonObject compressor = doc["compressor"].to<JsonObject>();
  compressor["on"] = compressorOn;
  compressor["expiresAt"] = compressorExpiresAt;
  const uint32_t elapsed = millis() - compressorOffAt;
  compressor["restartAllowedInMs"] = (!compressorOn && elapsed < COMPRESSOR_MIN_OFF_MS)
      ? COMPRESSOR_MIN_OFF_MS - elapsed : 0;
  JsonObject led = doc["led"].to<JsonObject>();
  led["on"] = ledIsOn();
  led["r"] = rgb[0]; led["g"] = rgb[1]; led["b"] = rgb[2];
  led["brightness"] = brightness;
  led["dutyR"] = dutyFor(rgb[0], brightness);
  led["dutyG"] = dutyFor(rgb[1], brightness);
  led["dutyB"] = dutyFor(rgb[2], brightness);
  led["expiresAt"] = ledExpiresAt;
  JsonObject last = doc["lastCommand"].to<JsonObject>();
  last["commandId"] = lastCommandId;
  last["accepted"] = lastAccepted;
  last["reason"] = lastCommandReason;
  String json;
  serializeJson(doc, json);
  return json;
}

bool actuatorReportPending() { return reportPending; }
void actuatorReportSent() { reportPending = false; }
