# BioIoT dividido en tres nodos ESP-NOW

Esta carpeta contiene la división del firmware integrado `BioIoT_Azure_Integrated.ino` (v4) en tres firmwares que se coordinan por ESP-NOW cifrado. El integrado sigue en la carpeta superior. La corrección de color del 2026-10-05 se aplica a ambos: curva provisional 15 µs negro / 200 µs blanco y asentamiento antes de medir ([COLOR.md](docs/COLOR.md)). Arduino solo compila la raíz del sketch y `src/`, por eso esta subcarpeta no interfiere.

```
            ESP-NOW unicast cifrado (CCMP + HMAC por enlace), canal del router
  ┌──────────────────────────┐          ┌──────────────────────────────┐
  │ Nodo A  ESP32 WROOM      │◀────────▶│ Nodo C  ESP32 WROOM          │──MQTT/TLS──▶ Azure IoT Hub
  │ CD74HC4067: pH, CO2 x2,  │  5 s     │ gateway + actuadores         │   120 s      (identidad v4)
  │ turbidez, DO, TDS        │          │ compresor, LED RGB           │
  │ 2x TCS3200, DS18B20      │          │ instantaneas en RAM (80 min) │
  └──────────────────────────┘          └──────────────────────────────┘
  ┌──────────────────────────┐                 ▲
  │ Nodo B  ESP8266          │◀────────────────┘ 5 s
  │ TCA9548A: 2x BH1750,     │
  │ 2x SEN0322 (O2 gas)      │
  └──────────────────────────┘
```

| Carpeta | Contenido |
| --- | --- |
| [BioIoT_NodeA_Sensors](BioIoT_NodeA_Sensors) | ESP32: analógicos, color y OneWire; calibraciones de A en NVS (doble ranura + CRC) |
| [BioIoT_NodeB_I2C](BioIoT_NodeB_I2C) | ESP8266: BH1750 y SEN0322 tras el TCA9548A; calibraciones O2 en LittleFS (doble ranura + CRC) |
| [BioIoT_NodeC_Gateway](BioIoT_NodeC_Gateway) | ESP32: único cliente de Azure; actuadores; enrutamiento de comandos; buffer offline |
| [libraries/BioIoTCommon](libraries/BioIoTCommon) | Contrato de mensajes, serialización explícita, HMAC/CRC, enlace (sesiones, ACK, reintentos, fragmentación), almacenamiento, adaptadores ESP-NOW por plataforma |
| [tools](tools) | Compilación, pruebas, generador de secretos, exportación de calibraciones v4 |
| [tests/host](tests/host) | 58 pruebas de PC de la lógica portable |
| [docs](docs) | Documentación detallada (abajo) |

## Documentación

1. [Matriz de funciones v4 → nodos](docs/FUNCIONES.md): qué existía, dónde quedó y cómo se comprobó. Incluye los cambios de comportamiento deliberados.
2. [Discrepancias detectadas](docs/DISCREPANCIAS.md): mapa acordado frente a repositorio, perfiles elegidos y riesgo de la clave publicada.
3. [Protocolo ESP-NOW](docs/PROTOCOLO.md): formato, tipos, sesiones, ACK, reintentos, fragmentación, tiempo, seguridad, canal, tamaños, memoria y autonomía.
4. [Instalación, compilación, carga y emparejamiento](docs/INSTALACION.md), con el cableado por nodo.
5. [Migración de calibraciones, respaldo y regreso a v4](docs/MIGRACION.md), contrato JSON 1.1 para Cosmos/app.
6. [Ejemplos de telemetría, comandos y resultados](docs/EJEMPLOS.md) (generados por el código, datos sintéticos).
7. [Pruebas realizadas y procedimiento de pruebas físicas](docs/PRUEBAS.md).
8. [Parámetros pendientes de confirmar](docs/PENDIENTES.md).

## Estado

| Aspecto | Estado |
| --- | --- |
| Tres firmwares implementados | Sí |
| Compilación | Sí, con arduino-cli 1.5.1: A y C con Arduino-ESP32 3.3.12, B con ESP8266 3.1.2. Con secretos de marcador y con secretos generados |
| Proyecto integrado | Conservado, con la corrección de color compartida con A; ver [COLOR.md](docs/COLOR.md) |
| Pruebas de PC | 83/83: incluye Wi-Fi/hotspot, almacenamiento B y seis casos de reservas del arranque C, además de protocolo, persistencia, sensores, actuadores y JSON |
| Cargado en placas | **C sí**, COM3, en commissioning con cargas desconectadas. A/B no se verificaron físicamente en esta revisión |
| Pruebas físicas | C: 10 resets por EN, banner/status/pairing correctos, 0 assertions. Pendientes ESP-NOW A/B y MQTT/TLS real. Ver [arranque C](docs/NODE_C_ARRANQUE.md) y [PRUEBAS.md](docs/PRUEBAS.md) |
| Azure | Sin cambios en recursos ni credenciales. El gateway reutiliza el formato JSON UTF-8 y las propiedades de contenido |

Por defecto el gateway arranca en **modo de puesta en marcha**: ningún GPIO de carga se configura como salida y los comandos se simulan. Los actuadores con parámetros eléctricos sin confirmar quedan bloqueados también en producción ([PENDIENTES.md](docs/PENDIENTES.md)).

Si C reinicia antes de mostrar el banner con `app_startup.c:86`, ver el
[diagnóstico y corrección del arranque](docs/NODE_C_ARRANQUE.md): las reservas
grandes se realizan en setup, con comprobaciones y modo seguro ante fallos.

> **Seguridad:** `../iot_configs.h` contiene la `DEVICE_KEY` real y está versionado en `origin` (GitHub). Conviene regenerar la clave del dispositivo en IoT Hub y retirarla del historial. No se hizo nada de esto automáticamente ([DISCREPANCIAS.md](docs/DISCREPANCIAS.md)).
