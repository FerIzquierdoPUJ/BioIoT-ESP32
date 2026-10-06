#pragma once
// Nodo A - ESP32 WROOM clasica: CD74HC4067 (analogicos), 2x TCS3200, DS18B20.
// Sin credenciales Azure. Todos los pines y canales se centralizan aqui.

// ---------------- Pines (mapa acordado = repositorio v4) ----------------
#define MUX_SIG_PIN 36  // ADC1, 12 bits, atenuacion 11 dB (igual que v4)
#define MUX_S0_PIN 25
#define MUX_S1_PIN 26
#define MUX_S2_PIN 27
#define MUX_S3_PIN 14
// EN del CD74HC4067: cableado fijo a GND (no es un GPIO).
#define TCS_S0 16
#define TCS_S1 17
#define TCS_S2 18
#define TCS_S3 19
#define TCS_OUT_1 34
#define TCS_OUT_2 35
// Referencias provisionales asumidas por el usuario (2026-10-05), ambos sensores.
// Curva empirica en ancho de pulso: negro -> 0, blanco -> 255, por canal RGB.
#define TCS_BLACK_PULSE_US 15UL
#define TCS_WHITE_PULSE_US 200UL
static_assert(TCS_BLACK_PULSE_US > 0 && TCS_BLACK_PULSE_US < 30000UL &&
              TCS_WHITE_PULSE_US > 0 && TCS_WHITE_PULSE_US < 30000UL &&
              TCS_BLACK_PULSE_US != TCS_WHITE_PULSE_US, "Referencias TCS3200 invalidas");
#define ONE_WIRE_BUS 23

// ---------------- Canales del CD74HC4067 ----------------
// DISCREPANCIA DOCUMENTADA (ver docs/DISCREPANCIAS.md):
//  * Mapa del prompt y commit 31f6a58: pH=0, CO2-1=1, CO2-2=2, turbidez=3, DO=4, TDS=5.
//  * Copia de trabajo v4 (2026-10-02, sin commit): pH=15, CO2-1=14, CO2-2=13,
//    turbidez=3, DO=12, TDS=11; introducida tras el diagnostico "analogicos 113-118".
// Se elige por defecto la copia de trabajo (evidencia fisica mas reciente).
// Cambiar a NODE_A_MUX_PROFILE_SEQUENTIAL si el cableado real es CH0..CH5.
#define NODE_A_MUX_PROFILE_WORKING_COPY 1
#define NODE_A_MUX_PROFILE_SEQUENTIAL 2
#ifndef NODE_A_MUX_PROFILE
#define NODE_A_MUX_PROFILE NODE_A_MUX_PROFILE_WORKING_COPY
#endif

#if NODE_A_MUX_PROFILE == NODE_A_MUX_PROFILE_WORKING_COPY
#define CH_PH 15
#define CH_CO2_1 14
#define CH_CO2_2 13
#define CH_TURB 3
#define CH_DO 12
#define CH_TDS 11
#elif NODE_A_MUX_PROFILE == NODE_A_MUX_PROFILE_SEQUENTIAL
#define CH_PH 0
#define CH_CO2_1 1
#define CH_CO2_2 2
#define CH_TURB 3
#define CH_DO 4
#define CH_TDS 5
#else
#error "NODE_A_MUX_PROFILE desconocido"
#endif

// ---------------- Sensores esperados (valores v4) ----------------
#define EXPECT_PH_DEFAULT true
#define EXPECT_CO2_1_DEFAULT true
#define EXPECT_CO2_2_DEFAULT true
#define EXPECT_TURB_DEFAULT false
#define EXPECT_DO_DEFAULT true
#define EXPECT_TDS_DEFAULT true
#define EXPECT_TEMP_DEFAULT true
#define EXPECT_TCS1_DEFAULT true
#define EXPECT_TCS2_DEFAULT true

// ---------------- Tiempos ----------------
// Lectura local continua, independiente del reporte.
#define NODE_A_ANALOG_SWEEP_INTERVAL_MS 2000UL
#define NODE_A_COLOR_INTERVAL_MS 2000UL
#define NODE_A_TEMP_INTERVAL_MS 2000UL
// Reporte al gateway (configurable en ejecucion por CONFIG; se persiste).
#define NODE_A_REPORT_INTERVAL_MS 5000UL
#define NODE_A_REPORT_PHASE_MS 0UL  // B usa 2500 ms: evita colisiones sistematicas
// DO usa la temperatura calibrada mas reciente si no es mas vieja que esto.
#define NODE_A_DO_TEMP_MAX_AGE_MS 10000UL
#define NODE_A_LINK_TIMEOUT_MS 20000UL
#define NODE_A_TELEMETRY_TTL_MS 600000UL
#define NODE_A_LOOP_WDT_TIMEOUT_MS 30000UL
