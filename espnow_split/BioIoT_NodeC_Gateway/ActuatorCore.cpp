#include "ActuatorCore.h"

#include <string.h>

namespace gw {

uint8_t dutyFor(uint8_t color, uint8_t level) { return uint8_t((uint16_t(color) * level + 127U) / 255U); }

bool LocalProgram::lightConfigured() const {
  return lightOnMinuteUtc >= 0 && lightOnMinuteUtc < 1440 && lightOffMinuteUtc >= 0 && lightOffMinuteUtc < 1440 &&
         lightOnMinuteUtc != lightOffMinuteUtc && brightness > 0 && (r || g || b);
}
bool LocalProgram::aerationConfigured() const { return aerationOnSeconds > 0 && aerationOffSeconds > 0; }

void ActuatorCore::begin(const ActuatorConfig& cfg, const LocalProgram& program, OutputDriver* driver, uint32_t nowMs) {
  cfg_ = cfg;
  program_ = program;
  // Sin parametros completos el programa queda deshabilitado (nunca se inventan).
  programValid_ = program_.enabled && (program_.lightConfigured() || program_.aerationConfigured());
  driver_ = driver;
  compressorOn_ = false;
  compressorOffAt_ = nowMs;  // el reposo minimo aplica tambien desde el arranque
  memset(rgb_, 0, sizeof(rgb_));
  brightness_ = 0;
  if (driver_ && !cfg_.commissioning) {
    driver_->writeCompressor(false);
    driver_->writeLed(0, 0, 0);
  }
  lastStopReason_ = cfg_.outputsReady ? "boot" : "pwm_init_failed";
}

void ActuatorCore::record(bool accepted, const char* reason) {
  lastAccepted_ = accepted;
  lastReason_ = reason;
  reportPending_ = true;
}

bool ActuatorCore::isDuplicate(const ActuatorCommand& cmd) {
  if (!cmd.hasCommandId || !cmd.commandId[0]) return false;
  for (uint8_t i = 0; i < kRecent; ++i)
    if (strcmp(recentIds_[i], cmd.commandId) == 0) return true;
  return false;
}

void ActuatorCore::remember(const ActuatorCommand& cmd) {
  if (!cmd.hasCommandId || !cmd.commandId[0]) return;
  strncpy(recentIds_[recentNext_], cmd.commandId, 64);
  recentIds_[recentNext_][64] = 0;
  recentNext_ = uint8_t((recentNext_ + 1) % kRecent);
}

void ActuatorCore::applyCompressor(bool on, const char* source, uint32_t nowMs) {
  if (compressorOn_ && !on) compressorOffAt_ = nowMs;
  compressorOn_ = on;
  compressorSource_ = on ? source : "off";
  const bool drive = !cfg_.commissioning && cfg_.compressorConfirmed && cfg_.outputsReady;
  if (driver_ && drive) driver_->writeCompressor(on);
}

void ActuatorCore::applyLed(const uint8_t rgb[3], uint8_t brightness, const char* source) {
  for (uint8_t i = 0; i < 3; ++i) rgb_[i] = rgb[i];
  brightness_ = brightness;
  const bool on = dutyFor(rgb_[0], brightness_) || dutyFor(rgb_[1], brightness_) || dutyFor(rgb_[2], brightness_);
  ledSource_ = on ? source : "off";
  const bool drive = !cfg_.commissioning && cfg_.ledConfirmed && cfg_.outputsReady;
  if (driver_ && drive)
    driver_->writeLed(dutyFor(rgb_[0], brightness_), dutyFor(rgb_[1], brightness_), dutyFor(rgb_[2], brightness_));
}

bool ActuatorCore::leaseFrom(const ActuatorCommand& cmd, bool utcValid, int64_t utcSeconds, uint32_t& leaseMs) const {
  if (!cmd.hasExpiresAt || !utcValid || utcSeconds < 1600000000LL) return false;
  const uint64_t now = uint64_t(utcSeconds);
  if (cmd.expiresAt <= now || cmd.expiresAt - now > cfg_.maxLeaseSeconds) return false;
  leaseMs = uint32_t(cmd.expiresAt - now) * 1000UL;
  return true;
}

void ActuatorCore::stopAll(const char* reason, uint32_t nowMs) {
  const bool wasOn = compressorOn_ || brightness_;
  manualCompressor_ = Manual();
  manualLed_ = Manual();
  applyCompressor(false, "off", nowMs);
  const uint8_t off[3] = {0, 0, 0};
  applyLed(off, 0, "off");
  if (wasOn) {
    lastStopReason_ = reason;
    reportPending_ = true;
  }
}

void ActuatorCore::handle(const ActuatorCommand& cmd, uint32_t nowMs, bool utcValid, int64_t utcSeconds) {
  strncpy(lastCommandId_, cmd.hasCommandId ? cmd.commandId : "", 64);
  lastCommandId_[64] = 0;
  if (cmd.parseError) { record(false, cmd.parseError); return; }
  // Un C2D reentregado (QoS 1) o repetido con el mismo commandId no se re-aplica:
  // no puede extender un lease ni reactivar una orden vencida.
  if (isDuplicate(cmd)) { record(false, "duplicate_command_ignored"); return; }
  if (cmd.allOff) {
    remember(cmd);
    stopAll("all_off", nowMs);
    // all_off tambien suspende el programa local hasta el maximo lease.
    manualCompressor_.active = manualLed_.active = true;
    manualCompressor_.on = manualLed_.on = false;
    manualCompressor_.startMs = manualLed_.startMs = nowMs;
    manualCompressor_.leaseMs = manualLed_.leaseMs = cfg_.maxLeaseSeconds * 1000UL;
    record(true, "all_off");
    return;
  }
  if (!cfg_.outputsReady) { record(false, "outputs_not_ready"); return; }
  if (cmd.fieldError) { record(false, cmd.fieldError); return; }
  if (cmd.target == kActCompressor) {
    if (!cmd.on) {
      remember(cmd);
      manualCompressor_ = Manual();
      manualCompressor_.active = true;  // OFF manual: suspende el programa
      manualCompressor_.startMs = nowMs;
      manualCompressor_.leaseMs = cfg_.maxLeaseSeconds * 1000UL;
      if (cmd.hasExpiresAt && utcValid && cmd.expiresAt > uint64_t(utcSeconds) &&
          cmd.expiresAt - uint64_t(utcSeconds) <= cfg_.maxLeaseSeconds)
        manualCompressor_.leaseMs = uint32_t(cmd.expiresAt - uint64_t(utcSeconds)) * 1000UL;
      applyCompressor(false, "manual", nowMs);
      record(true, "compressor_off");
      return;
    }
    if (!cfg_.commissioning && !cfg_.compressorConfirmed) { record(false, "electrical_config_unconfirmed"); return; }
    uint32_t leaseMs = 0;
    if (!leaseFrom(cmd, utcValid, utcSeconds, leaseMs)) { record(false, "invalid_or_expired_lease"); return; }
    if (!compressorOn_ && nowMs - compressorOffAt_ < cfg_.compressorMinOffMs) {
      record(false, "compressor_min_off_time");  // no se programa arranque diferido
      return;
    }
    remember(cmd);
    manualCompressor_.active = true;
    manualCompressor_.on = true;
    manualCompressor_.startMs = nowMs;
    manualCompressor_.leaseMs = leaseMs;
    manualCompressor_.expiresAt = cmd.expiresAt;
    applyCompressor(true, "manual", nowMs);
    record(true, "compressor_on");
    return;
  }
  if (cmd.target == kActLed) {
    const bool on = dutyFor(cmd.r, cmd.brightness) || dutyFor(cmd.g, cmd.brightness) || dutyFor(cmd.b, cmd.brightness);
    uint32_t leaseMs = cfg_.maxLeaseSeconds * 1000UL;
    if (on) {
      if (!cfg_.commissioning && !cfg_.ledConfirmed) { record(false, "electrical_config_unconfirmed"); return; }
      if (!leaseFrom(cmd, utcValid, utcSeconds, leaseMs)) { record(false, "invalid_or_expired_lease"); return; }
    }
    remember(cmd);
    manualLed_.active = true;
    manualLed_.on = on;
    manualLed_.startMs = nowMs;
    manualLed_.leaseMs = leaseMs;
    manualLed_.expiresAt = on ? cmd.expiresAt : 0;
    manualRgb_[0] = cmd.r; manualRgb_[1] = cmd.g; manualRgb_[2] = cmd.b;
    manualBrightness_ = cmd.brightness;
    applyLed(manualRgb_, manualBrightness_, "manual");
    record(true, on ? "led_set" : "led_off");
    return;
  }
  record(false, "unknown_target");
}

void ActuatorCore::arbitrate(uint32_t nowMs, bool utcValid, int64_t utcSeconds) {
  // Compresor
  bool wantCompressor = false;
  const char* compSource = "off";
  if (manualCompressor_.active) {
    wantCompressor = manualCompressor_.on;
    compSource = "manual";
  } else if (programValid_ && program_.aerationConfigured() && utcValid) {
    const uint32_t period = program_.aerationOnSeconds + program_.aerationOffSeconds;
    wantCompressor = uint32_t(utcSeconds % period) < program_.aerationOnSeconds;
    compSource = "local_program";
    // El programa respeta el reposo minimo (no fuerza arranques).
    if (wantCompressor && !compressorOn_ && nowMs - compressorOffAt_ < cfg_.compressorMinOffMs) wantCompressor = false;
  }
  if (wantCompressor != compressorOn_) applyCompressor(wantCompressor, compSource, nowMs);
  // Luz
  if (!manualLed_.active) {
    uint8_t rgb[3] = {0, 0, 0};
    uint8_t level = 0;
    const char* src = "off";
    if (programValid_ && program_.lightConfigured() && utcValid) {
      const int minute = int((utcSeconds / 60) % 1440);
      const int on = program_.lightOnMinuteUtc, off = program_.lightOffMinuteUtc;
      const bool lit = on < off ? (minute >= on && minute < off) : (minute >= on || minute < off);
      if (lit) {
        rgb[0] = program_.r; rgb[1] = program_.g; rgb[2] = program_.b;
        level = program_.brightness;
        src = "local_program";
      }
    }
    if (level != brightness_ || rgb[0] != rgb_[0] || rgb[1] != rgb_[1] || rgb[2] != rgb_[2]) applyLed(rgb, level, src);
  }
}

void ActuatorCore::service(uint32_t nowMs, bool utcValid, int64_t utcSeconds) {
  // Vencimiento por reloj monotono desde la aceptacion: no depende de UTC ni de la nube.
  if (manualCompressor_.active && nowMs - manualCompressor_.startMs >= manualCompressor_.leaseMs) {
    const bool wasOn = manualCompressor_.on;
    manualCompressor_ = Manual();
    if (wasOn) {
      applyCompressor(false, "off", nowMs);
      lastStopReason_ = "compressor_lease_expired";
      reportPending_ = true;
    }
  }
  if (manualLed_.active && nowMs - manualLed_.startMs >= manualLed_.leaseMs) {
    const bool wasOn = manualLed_.on;
    manualLed_ = Manual();
    if (wasOn) {
      const uint8_t off[3] = {0, 0, 0};
      applyLed(off, 0, "off");
      lastStopReason_ = "led_lease_expired";
      reportPending_ = true;
    }
  }
  arbitrate(nowMs, utcValid, utcSeconds);
}

ActuatorState ActuatorCore::state(uint32_t nowMs) const {
  ActuatorState s;
  s.commissioning = cfg_.commissioning;
  s.ready = cfg_.outputsReady;
  s.outputsDriven = !cfg_.commissioning && cfg_.outputsReady && (cfg_.compressorConfirmed || cfg_.ledConfirmed);
  s.compressorLocked = !cfg_.commissioning && !cfg_.compressorConfirmed;
  s.ledLocked = !cfg_.commissioning && !cfg_.ledConfirmed;
  s.compressorOn = compressorOn_;
  s.compressorExpiresAt = manualCompressor_.active && manualCompressor_.on ? manualCompressor_.expiresAt : 0;
  const uint32_t elapsed = nowMs - compressorOffAt_;
  s.restartAllowedInMs = (!compressorOn_ && elapsed < cfg_.compressorMinOffMs) ? cfg_.compressorMinOffMs - elapsed : 0;
  s.compressorSource = compressorSource_;
  for (uint8_t i = 0; i < 3; ++i) {
    s.rgb[i] = rgb_[i];
    s.duty[i] = dutyFor(rgb_[i], brightness_);
  }
  s.brightness = brightness_;
  s.ledOn = s.duty[0] || s.duty[1] || s.duty[2];
  s.ledExpiresAt = manualLed_.active && manualLed_.on ? manualLed_.expiresAt : 0;
  s.ledSource = ledSource_;
  s.lastStopReason = lastStopReason_;
  strncpy(s.lastCommandId, lastCommandId_, 64);
  s.lastCommandId[64] = 0;
  s.lastAccepted = lastAccepted_;
  s.lastReason = lastReason_;
  s.programEnabled = program_.enabled;
  s.programValid = programValid_;
  return s;
}

}  // namespace gw
