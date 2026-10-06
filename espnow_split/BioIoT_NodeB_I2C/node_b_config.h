#pragma once
// Nodo B - ESP8266 (NodeMCU v2 o Wemos D1 mini): sensores I2C detras del
// PCA9548A/TCA9548A: 2x BH1750 y 2x SEN0322 (O2 gas). Sin credenciales Azure.

// ---------------- Perfil de placa (explicito) ----------------
#define NODE_B_BOARD_NODEMCU_V2 1
#define NODE_B_BOARD_WEMOS_D1_MINI 2
#ifndef NODE_B_BOARD
#define NODE_B_BOARD NODE_B_BOARD_NODEMCU_V2
#endif

#if defined(ARDUINO_ESP8266_ESP01)
#error "ESP-01 no expone GPIO4/GPIO5: use NodeMCU v2 o Wemos D1 mini (o defina un perfil propio)."
#endif

#if NODE_B_BOARD == NODE_B_BOARD_NODEMCU_V2 || NODE_B_BOARD == NODE_B_BOARD_WEMOS_D1_MINI
// En ambas placas: SDA = GPIO4 (rotulado D2), SCL = GPIO5 (rotulado D1).
#define I2C_SDA_PIN 4
#define I2C_SCL_PIN 5
#define NODE_B_BOARD_NAME (NODE_B_BOARD == NODE_B_BOARD_NODEMCU_V2 ? "nodemcu_v2" : "wemos_d1_mini")
#else
#error "NODE_B_BOARD desconocido"
#endif

#if defined(ARDUINO_ESP8266_NODEMCU_ESP12E) && NODE_B_BOARD != NODE_B_BOARD_NODEMCU_V2
#warning "Placa compilada NodeMCU pero NODE_B_BOARD no es NodeMCU v2: revisar pines."
#endif
#if defined(ARDUINO_ESP8266_WEMOS_D1MINI) && NODE_B_BOARD != NODE_B_BOARD_WEMOS_D1_MINI
#warning "Placa compilada Wemos D1 mini pero NODE_B_BOARD no coincide: revisar pines."
#endif

#define I2C_CLOCK_HZ 100000
// Limite de clock stretching (us). El nucleo usa 150 ms; 50 ms iguala el timeout
// Wire que tenia v4 en ESP32 y acota el bloqueo con un bus colgado.
#define I2C_CLOCK_STRETCH_LIMIT_US 50000

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
// 1 = los SEN0322 se alimentan del mismo 3,3 V que el ESP8266: un reinicio por
// software/watchdog no corta su alimentacion y se descuenta el tiempo ya alimentado.
// 0 = alimentacion independiente/desconocida: warm-up completo en cada arranque.
#define O2_SENSOR_SHARED_SUPPLY 1

// ---------------- Almacenamiento LittleFS ----------------
// 0 = produccion (por defecto): autoformato desactivado y sin ninguna llamada a
//     LittleFS.format(). Si LittleFS no monta, el nodo mide, informa
//     storage_mounted=false y rechaza calibraciones (rejected_storage_failed).
// 1 = puesta en marcha de una placa B nueva: ESP-NOW deshabilitado y Serial acepta
//     {"action":"storage_format","confirm":"FORMAT-NODE-B-<chip>"} solo si LittleFS
//     no monta. Despues recompilar con 0 y recargar (docs/INSTALACION.md, 3.1).
// Preferible activarlo sin editar este archivo:
//   --build-property "compiler.cpp.extra_flags=-DNODE_B_STORAGE_COMMISSIONING=1"
#ifndef NODE_B_STORAGE_COMMISSIONING
#define NODE_B_STORAGE_COMMISSIONING 0
#endif
#if NODE_B_STORAGE_COMMISSIONING
#warning "NODE_B_STORAGE_COMMISSIONING=1: firmware de puesta en marcha de LittleFS, sin ESP-NOW. No usar en produccion."
#endif

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
