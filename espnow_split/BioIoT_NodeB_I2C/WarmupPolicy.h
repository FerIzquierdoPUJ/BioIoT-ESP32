#pragma once
// El warm-up pertenece al sensor y a su alimentacion, no al enlace ESP-NOW.
// Politica conservadora:
//  * Encendido (power-on) o memoria RTC invalida: warm-up completo desde el arranque.
//  * Reinicio por software/watchdog/excepcion/pin externo con memoria RTC valida y
//    alimentacion compartida: se descuenta el tiempo alimentado guardado (cota
//    inferior, actualizada cada pocos segundos) => nunca se acorta de mas.
//  * Alimentacion no compartida o causa desconocida: warm-up completo y se marca
//    "assumed" (no se sabe cuanto lleva alimentado el sensor).
// Perder ESP-NOW no toca este estado. Latch: no se repite al desbordar millis().
#include <stdint.h>

namespace nodeb {

enum class ResetKind : uint8_t { PowerOn, SoftOrWatchdog, External, DeepSleep, Unknown };

class WarmupPolicy {
 public:
  void begin(uint32_t warmupMs, ResetKind reset, bool rtcValid, uint32_t rtcPoweredMs, bool sharedSupply,
             uint32_t nowMs);
  bool warming(uint32_t nowMs);
  uint32_t remainingMs(uint32_t nowMs);
  // Cota inferior del tiempo alimentado (para guardar en RTC).
  uint32_t poweredLowerBoundMs(uint32_t nowMs) const;
  bool assumedFull() const { return assumed_; }
  bool fromPowerOn() const { return powerOn_; }
  uint32_t creditedMs() const { return credited_; }

 private:
  uint32_t warmupMs_ = 0, bootMs_ = 0, credited_ = 0;
  bool done_ = false, assumed_ = false, powerOn_ = false;
};

}  // namespace nodeb
