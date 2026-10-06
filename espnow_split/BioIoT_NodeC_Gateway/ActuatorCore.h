#pragma once
// Nucleo portable de actuadores del gateway (sin Arduino). Prioridades explicitas:
//   1. Bloqueo de seguridad (salidas no listas, parametros electricos sin confirmar)
//   2. Orden manual vigente (lease con expiresAt; un OFF manual suspende el programa)
//   3. Programa local de respaldo (deshabilitado hasta tener parametros del proceso)
//   4. Apagado
// La perdida de Internet, la reconexion o la renovacion SAS NO intervienen.
#include <stdint.h>

namespace gw {

class OutputDriver {
 public:
  virtual ~OutputDriver() {}
  virtual void writeCompressor(bool on) = 0;
  virtual void writeLed(uint8_t dutyR, uint8_t dutyG, uint8_t dutyB) = 0;
};

struct ActuatorConfig {
  bool commissioning = true;          // salidas en alta impedancia, comandos simulados
  bool compressorConfirmed = false;
  bool ledConfirmed = false;
  bool outputsReady = true;           // fallo de PWM/GPIO al iniciar => false
  uint32_t compressorMinOffMs = 180000;
  uint32_t maxLeaseSeconds = 900;
};

struct LocalProgram {
  bool enabled = false;
  int16_t lightOnMinuteUtc = -1, lightOffMinuteUtc = -1;
  uint8_t r = 0, g = 0, b = 0, brightness = 0;
  uint32_t aerationOnSeconds = 0, aerationOffSeconds = 0;
  bool lightConfigured() const;
  bool aerationConfigured() const;
};

enum ActTarget : uint8_t { kActNone = 0, kActCompressor = 1, kActLed = 2 };

struct ActuatorCommand {
  bool allOff = false;
  bool hasCommandId = false;
  char commandId[65] = "";
  const char* parseError = nullptr;  // commandId invalido (se evalua primero, como v4)
  const char* fieldError = nullptr;  // campos invalidos (se evalua tras outputs_not_ready)
  uint8_t target = kActNone;
  bool on = false;
  bool hasExpiresAt = false;
  uint64_t expiresAt = 0;
  uint8_t r = 0, g = 0, b = 0, brightness = 0;
};

struct ActuatorState {
  bool commissioning = true, outputsDriven = false, ready = false;
  bool compressorLocked = false, ledLocked = false;
  bool compressorOn = false;
  uint64_t compressorExpiresAt = 0;
  uint32_t restartAllowedInMs = 0;
  const char* compressorSource = "off";
  uint8_t rgb[3] = {0, 0, 0}, brightness = 0, duty[3] = {0, 0, 0};
  bool ledOn = false;
  uint64_t ledExpiresAt = 0;
  const char* ledSource = "off";
  const char* lastStopReason = "boot";
  char lastCommandId[65] = "";
  bool lastAccepted = false;
  const char* lastReason = "no_command";
  bool programEnabled = false, programValid = false;
};

uint8_t dutyFor(uint8_t color, uint8_t level);

class ActuatorCore {
 public:
  void begin(const ActuatorConfig& cfg, const LocalProgram& program, OutputDriver* driver, uint32_t nowMs);
  // utcSeconds solo se usa para validar expiresAt al aceptar un encendido.
  void handle(const ActuatorCommand& cmd, uint32_t nowMs, bool utcValid, int64_t utcSeconds);
  // Vencimientos y arbitraje; llamar periodicamente (tarea local, ~20 ms).
  void service(uint32_t nowMs, bool utcValid, int64_t utcSeconds);
  void stopAll(const char* reason, uint32_t nowMs);
  ActuatorState state(uint32_t nowMs) const;
  bool reportPending() const { return reportPending_; }
  void clearReportPending() { reportPending_ = false; }

 private:
  struct Manual {
    bool active = false;   // orden manual vigente (on u off)
    bool on = false;
    uint32_t startMs = 0, leaseMs = 0;
    uint64_t expiresAt = 0;
  };
  void record(bool accepted, const char* reason);
  bool isDuplicate(const ActuatorCommand& cmd);
  void remember(const ActuatorCommand& cmd);
  void applyCompressor(bool on, const char* source, uint32_t nowMs);
  void applyLed(const uint8_t rgb[3], uint8_t brightness, const char* source);
  bool leaseFrom(const ActuatorCommand& cmd, bool utcValid, int64_t utcSeconds, uint32_t& leaseMs) const;
  void arbitrate(uint32_t nowMs, bool utcValid, int64_t utcSeconds);

  ActuatorConfig cfg_;
  LocalProgram program_;
  bool programValid_ = false;
  OutputDriver* driver_ = nullptr;
  Manual manualCompressor_, manualLed_;
  uint8_t manualRgb_[3] = {0, 0, 0}, manualBrightness_ = 0;
  bool compressorOn_ = false;
  uint32_t compressorOffAt_ = 0;
  const char* compressorSource_ = "off";
  uint8_t rgb_[3] = {0, 0, 0}, brightness_ = 0;
  const char* ledSource_ = "off";
  const char* lastStopReason_ = "boot";
  char lastCommandId_[65] = "";
  bool lastAccepted_ = false;
  const char* lastReason_ = "no_command";
  bool reportPending_ = false;
  static constexpr uint8_t kRecent = 16;
  char recentIds_[kRecent][65] = {};
  uint8_t recentNext_ = 0;
};

}  // namespace gw
