#pragma once
// Autoprueba de arranque: codifica una trama conocida y la compara con el vector
// dorado generado en las pruebas de PC. Si ESP32, ESP8266 y PC producen los mismos
// bytes, la serializacion explicita es interoperable en esa placa/nucleo.
#include <stddef.h>
#include <stdint.h>

namespace bioiot {

bool protocolSelfTest();
// Para pruebas: genera la trama dorada en out (cap >= 250). Devuelve longitud.
size_t selfTestFrame(uint8_t* out, size_t cap);
extern const uint8_t kSelfTestGolden[];
extern const size_t kSelfTestGoldenLen;

}  // namespace bioiot
