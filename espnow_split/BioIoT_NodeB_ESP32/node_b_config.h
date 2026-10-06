#pragma once
// Nodo B - ESP32 WROOM (ESP32 Dev Module): sensores I2C detras del PCA9548A/TCA9548A:
// 2x BH1750 y 2x SEN0322 (O2 gas). Sin credenciales Azure. Misma logica, protocolo,
// calibraciones y warm-up que BioIoT_NodeB_I2C (ESP8266); cambian la placa, los pines
// I2C (los de v4 en ESP32) y el almacenamiento (NVS de doble ranura, como el nodo A).

#if !defined(ARDUINO_ARCH_ESP32)
#error "BioIoT_NodeB_ESP32 es para ESP32 (ESP32 Dev Module). Para ESP8266 use BioIoT_NodeB_I2C."
#endif

#define NODE_B_BOARD_NAME "esp32_devkit"
// Mismo cableado I2C que el firmware integrado v4 en ESP32.
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22

#define I2C_CLOCK_HZ 100000
// Timeout de Wire (ms): el mismo de v4 en ESP32; acota el bloqueo con un bus colgado.
#define I2C_TIMEOUT_MS 50

// ---------------- Multiplexor I2C ----------------
// DISCREPANCIA DOCUMENTADA (ver docs/DISCREPANCIAS.md):
//  * Prompt y commit 31f6a58: 0x72 (A0=GND, A1=3V3, A2=GND).
//  * Copia de trabajo v4 (2026-10-02): DFR0576 con DIP A2/A1/A0=111 -> 0x77, para
//    salir del rango 0x70-0x73 de SEN0322 (v4 marcaba address_0x72_conflicts_with_tca).
// Por defecto la copia de trabajo. El diagnostico i2c informa si responde la otra.
#define NODE_B_TCA_PROFILE_WORKING_COPY 1  // 0x77
#define NODE_B_TCA_PROFILE_PROMPT 2        // 0x72
#ifndef NODE_B_TCA_PROFILE
#define NODE_B_TCA_PROFILE NODE_B_TCA_PROFILE_WORKING_COPY
#endif
#if NODE_B_TCA_PROFILE == NODE_B_TCA_PROFILE_WORKING_COPY
#define TCA_ADDR 0x77
#define TCA_ALT_ADDR 0x72
#elif NODE_B_TCA_PROFILE == NODE_B_TCA_PROFILE_PROMPT
#define TCA_ADDR 0x72
#define TCA_ALT_ADDR 0x77
#else
#error "NODE_B_TCA_PROFILE desconocido"
#endif

#define TCA_CH_BH1750_1 0
#define TCA_CH_BH1750_2 1
#define TCA_CH_O2_1 2
#define TCA_CH_O2_2 3
#define BH1750_ADDR_1 0x23
#define BH1750_ADDR_2 0x23
#define O2_ADDR_1 0x73  // verificar selectores fisicos de cada SEN0322
#define O2_ADDR_2 0x73

// ---------------- Esperados (v4) ----------------
#define EXPECT_BH1_DEFAULT true
#define EXPECT_BH2_DEFAULT true
#define EXPECT_O2_1_DEFAULT true
#define EXPECT_O2_2_DEFAULT true

// ---------------- Warm-up SEN0322 ----------------
#define O2_WARMUP_MS 180000UL  // v4: 3 min
// 1 = los SEN0322 se alimentan del mismo 3,3 V que el ESP32: un reinicio por
// software/watchdog no corta su alimentacion y se descuenta el tiempo ya alimentado.
// 0 = alimentacion independiente/desconocida: warm-up completo en cada arranque.
#define O2_SENSOR_SHARED_SUPPLY 1

// ---------------- Almacenamiento ----------------
// NVS (Preferences "bioiot_b") en doble ranura con version y CRC32, como el nodo A.
// No requiere formatear: no existe modo de puesta en marcha de almacenamiento.

// ---------------- Tiempos ----------------
#define NODE_B_LIGHT_INTERVAL_MS 2000UL
// v4 tomaba UNA muestra de O2 por telemetria (120 s) y promediaba 10 (O2_FILTER_SAMPLES):
// ventana ~20 min. Se conserva esa cadencia para no cambiar el filtro.
#define NODE_B_O2_SAMPLE_INTERVAL_MS 120000UL
#define NODE_B_REPORT_INTERVAL_MS 5000UL
#define NODE_B_REPORT_PHASE_MS 2500UL
#define NODE_B_LINK_TIMEOUT_MS 20000UL
#define NODE_B_TELEMETRY_TTL_MS 600000UL
#define NODE_B_BH1750_MEASURE_MS 180UL  // delay(180) de v4
