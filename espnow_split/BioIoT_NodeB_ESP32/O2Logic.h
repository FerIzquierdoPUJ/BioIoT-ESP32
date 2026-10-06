#pragma once
// Evaluacion de una lectura SEN0322, portada de readO2Percent() de v4 sin cambiar
// limites (0..30 medida, 0..25 nominal), filtro (O2_FILTER_SAMPLES) ni calidades.
// Distingue: calentamiento (transporte probado, concentracion no adquirida),
// desconexion/fallo de comunicacion y medicion invalida (fuera de rango).
#include <stdint.h>

#include "bioiot_calibration_model.h"

namespace nodeb {

struct O2Transport {
  bool selected = false;  // canal TCA seleccionado
  uint8_t selectError = 0;
  bool probeOk = false;   // ACK de la direccion esperada
  uint8_t probeError = 0;
  bool warming = false;
  bool beginOk = false;   // driver.begin() (lectura de version verificada)
  bool commOk = false;    // lecturas de clave y dato completas
  float raw = 0;
  uint8_t wireError = 0, readCount = 0;
};

// Rellena r y actualiza el filtro exactamente como v4. Devuelve r.value (NaN si no valida).
float evaluateO2(const O2Transport& t, const O2Calibration& cal, O2Filter& filter, O2Reading& r, uint32_t nowMs);

}  // namespace nodeb
