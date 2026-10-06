# Matriz de funciones: v4 integrado → nodos A, B y C

Versión de referencia: `BioIoT_Azure_Integrated.ino`, más los cambios sin commit del 2026-10-02 de la copia de trabajo, `CalibrationModel.h`, `DFRobot_OxygenSensor.*`, `ActuatorControl.*`, `actuator_configs.h`, `AzureIoTSasToken.*`, `azure_ca.h` e `iot_configs.h`. No existe `AGENTS.md` en el repositorio.

Leyenda de comprobación:
- **PC**: prueba automática en `tests/host`.
- **Copia**: archivo copiado sin cambios, verificado con `cmp`.
- **Compila**: verificado solo por compilación.
- **Física**: requiere placa (pendiente).

## Adquisición, unidades, crudos, filtrado y desconexión

| Función v4 | Implementación v4 | Destino | Conservación |
| --- | --- | --- | --- |
| Mux CD74HC4067: 16 muestras, descarte inicial, asentamiento 250 µs + 3 ms, 3 ms entre muestras, ADC 12 bits, 11 dB | `readAnalogMux()` con `delay()` | A `SensorsA.cpp` `AnalogSampler` (estados, sin `delay`) | Mismo método y tiempos. Compila; física |
| `raw = acc/16` y `voltage = raw·3,3/4095` (escala de las curvas) | `readAnalogMux()` | A `SensorMathA::adcToVoltage` | PC `node_a_equations_match_v4` |
| `connected` heurístico (raw ≤ 5 o ≥ 4090) | idem | A `analogConnected` | PC |
| pH `m·V+b`, nulo fuera de 0..14 | `calculatePh` | A `SensorMathA` | PC |
| Turbidez `m·V+b`, nula fuera de 0..4000 | `calculateTurbidityNtu` | A | PC |
| DO: % sat = `m·mV` (limitado 0..150), mg/L con polinomio de temperatura | `calculateDoSaturationPct/MgL` | A (usa la temperatura de A si tiene ≤ 10 s) | PC. La temperatura sale del mismo ciclo, igual que en v4 |
| TDS polinomio ×0,5 | `calculateTdsPpm` | A | PC |
| CO2 SEN0159: divisor, ganancia, perfiles individuales, legacy exponencial | `CalibrationModel.h` `evaluateCo2` | A (biblioteca, copia literal `bioiot_calibration_model.h`) | Copia + PC `co2_model_is_the_v4_header_verbatim` |
| DS18B20: `-127`/rango ⇒ desconectado; `raw` y calibrada | `readRawTemperatureC` (bloqueante 750 ms) | A `TempSampler` (asíncrono) | PC (criterio); física |
| TCS3200: S0=H, S1=L; filtros R/G/B; `pulseIn(LOW, 30 ms)` y HSL | `readTcs3200`, `pulseToIntensity`, `rgbToHsl` | A `ColorSampler` (un `pulseIn` por vuelta) | Curva provisional actualizada junto con el integrado: 15 µs negro / 200 µs blanco; espera de 10 ms antes de medir. Ver [COLOR.md](COLOR.md). Prueba de código con reloj simulado; física pendiente |
| BH1750 `CONTINUOUS_HIGH_RES`, espera 180 ms | `readBh1750Lux` | B `SensorsB` (estados) | Compila; física. Los códigos −1/−2 se conservan en `driver_code` y `value` queda nulo (v4 los publicaba como lux) |
| SEN0322: canal TCA, probe, `begin`, una muestra, límites 0..30 y 0..25, filtro de 10 | `readO2Percent` + fork local del driver | B `O2Logic` (portable) y `DFRobot_OxygenSensor_B` (copia del fork + lectura por etapas) | PC `o2_warmup_disconnection_and_invalid_are_distinct`. La lectura por etapas usa los mismos registros, esperas (50/100 ms) y fórmula |
| Cadencia del filtro O2: 1 muestra por telemetría (120 s) | ciclo de 120 s | B `NODE_B_O2_SAMPLE_INTERVAL_MS = 120000` | Ventana de unos 20 min preservada |
| Calidades `good/out_of_range/disconnected/uncalibrated/below_reference_range/user_calibrated/vendor_reference/communication_fault/above_nominal_range/stabilizing/warming_up` | `sensorQuality`, `evaluateCo2`, `readO2Percent` | Las calcula el nodo propietario; viajan como código | PC (tabla de nombres) |
| `expected` por sensor | constantes `EXPECT_*` | A y B (persistible por `config`) | Valores por defecto idénticos (turbidez=false) |

## Calibraciones, esquema y persistencia

| Función v4 | Implementación v4 | Destino | Conservación |
| --- | --- | --- | --- |
| Curvas iniciales (pH 3 puntos, DO 2 puntos, turbidez, temperatura, CO2 proveedor) | `#define *_DEFAULT` | `bioiot_calibration.h` (constantes literales) | PC: ida y vuelta bit a bit de los valores por defecto |
| `set/reset` pH, turb, DO (solo `m`), temp | `handleCalibrationCommand` | A `CalibrationLogicA` | PC `calibration_a_set_reset_and_rollback` |
| `set` CO2 (`co2`, `co2_1`, `co2_2`): legacy a/b vs proveedor zero/reaction, `enabled`, instantánea legacy | `setCo2Profile` | A | PC `calibration_a_co2_profiles_like_v4` |
| `set/reset` `o2_gas_N` (gain > 0, offset) | idem | B `CalibrationLogicB` | PC |
| `reset_all` (A: curvas; B: gain/offset, conservando referencia, fecha y conteo) | idem | A y B por separado, resultado por nodo | PC; resumen `partial` |
| `calibrate` O2 en aire (`reference_vol` exactamente 20,9; rechazo en calentamiento u ocupado; `command_sent` ≠ calibración confirmada) | `requestO2Calibration`/`processO2Calibration` | B (asíncrono en el planificador I2C) | PC (reglas); física |
| Flags de procedencia `ph_user`, `do_user`, `user/provisional` de CO2, fuente O2 | NVS | Se conservan; se añaden `turb_user`, `temp_user` e `importedMask` | PC |
| Revisión `ver` global | NVS | Se conserva como `legacy_v4_version`; ahora hay revisión **por sensor y por nodo** | PC |
| Migración de esquema 1→2 | `loadCalibrationFromNVS` | Herramienta `BioIoT_CalibrationExport` (en memoria, solo lectura) | Compila |
| Esquema futuro bloquea escrituras | `calibrationFutureSchema` | `RecordStore` `FutureFormat` | PC `store_detects_corruption_and_future_format` |
| Persistencia NVS | Preferences | A: NVS en doble ranura con CRC; B: LittleFS en doble ranura con CRC (sin Preferences) | PC (escritura interrumpida, relectura fallida, CRC, generación) |
| `commandId` y `calibration_ack` | `queueCalibrationAck` | C publica `calibration_ack` con `state` | PC (JSON) |

## Warm-up, diagnósticos y recuperación I2C

| Función v4 | Implementación v4 | Destino | Conservación |
| --- | --- | --- | --- |
| Warm-up O2 180 s con latch ante desbordamiento de `millis()` | `o2IsWarmingUp` (desde el arranque) | B `WarmupPolicy`: depende de la alimentación; memoria RTC; política conservadora | PC `warmup_belongs_to_power_not_to_link` |
| Calentamiento ≠ desconexión ≠ inválida | `readO2Percent` | B | PC |
| Diagnóstico C2D y serial (`full/i2c/analog/color/system/quick/i2c_recover`) por etapas | `processDiagnostics` | C coordina; A analógico/color/temperatura; B I2C/O2; respuesta fragmentada | PC (fragmentación, agregado); física |
| Barrido del bus principal 0x01..0x7E y 8 canales, una sonda por vuelta | idem | B `SensorsB::stepScan` | Compila; física. Códigos agregados por tipo (`error_counts`, `non_nack_errors`) en lugar de 126 códigos por segmento, para caber en 4 KB |
| `i2c_recover` | `Wire.end/begin` | B: 9 pulsos SCL, STOP, `Wire.begin` y estado de línea `twi_status` antes/después | Compila; física |
| Hallazgos analógicos (todo en cero, canales demasiado similares) y color (camino individual) | `write*` | A | Compila |
| Historial de éxitos/fallos | RAM | A (barridos), B (`DiagnosticHistory` por dispositivo y TCA) | Compila |
| Resumen `diagnostics` en cada telemetría | `quickDiagnosticsJson` | C, con el STATUS de A y B | PC (JSON) |

## Telemetría, alertas, tiempo, MQTT/TLS y SAS

| Función v4 | Implementación v4 | Destino | Conservación |
| --- | --- | --- | --- |
| JSON 1.0 con 13 sensores, `calibration`, `actuators`, `diagnostics`, `alerts`, `status` | `buildTelemetryJson` | C `TelemetryJson` (contrato 1.1 aditivo) | PC `telemetry_json_contract_11`; ver [MIGRACION.md](MIGRACION.md) |
| Intervalo de 120 s | `TELEMETRY_INTERVAL_MS` | C (instantánea cada 120 s); nodos informan cada 5 s | — |
| `timestampUtc` ISO, `null` sin hora | `utcTimestampIso8601` | C `formatIsoUtc`; derivado por reloj monótono si la hora llega después | PC |
| MQTT con propiedades `$.ct=application/json`, `$.ce=utf-8` y `messages/events` | `initAzureSDK` | C `AzureLink::initAzure` (mismas macros) | Compila |
| TLS con DigiCert G2, sin `setInsecure` | `azure_ca.h` | C (copia) | Copia + compila |
| SAS de 60 min, renovación 2 min antes | `AzureIoTSasToken` | C (copia) | Copia |
| NTP antes de TLS | `initTime` (bloqueaba hasta 60 s y reiniciaba) | C no bloqueante; sin hora no hay TLS y **no reinicia** | Compila |
| WiFiManager si no hay SSID | `initWiFi` (bloqueante, reinicio) | C: credenciales NVS → predeterminadas; portal no bloqueante `BioIoT-AP` 180 s (automático tras fallo y por Serial) en AP+STA y en el canal ESP-NOW; red del portal guardada en NVS solo si conecta | PC (`wifi_*`, `hotspot_*`, `espnow_survives_*`); compila |
| Suscripción C2D QoS 1, reboot tras PUBACK | `subscribeToC2D`, `rebootRequested` | C (igual) | Compila |
| Watchdog de `loop()` 120 s | `esp_task_wdt` | C (igual) y además vigilancia de la tarea local (10 s); A 30 s; B watchdog del núcleo | Compila |

## Actuadores, comandos y control local

| Función v4 | Implementación v4 | Destino | Conservación |
| --- | --- | --- | --- |
| Compresor por relé y RGB LEDC de 8 bits, 1 kHz, `duty = round(c·brillo/255)` | `ActuatorControl.cpp` | C `ActuatorCore` + `ActuatorOutputsEsp32` | PC |
| Precarga del nivel de reposo antes de `pinMode` | `gpio_set_level` | C (solo actuadores confirmados) | Compila |
| `control`/`all_off` con `expiresAt` ≤ 900 s, reposo mínimo de 180 s desde el arranque, OFF siempre permitido | `handleActuatorCommand` | C | PC `actuators_v4_protections_preserved` |
| Rechazos `on_must_be_boolean`, `rgb_and_brightness_must_be_0_to_255`, `invalid_or_expired_lease`, `compressor_min_off_time`, `unknown_target`, `invalid_command_id`, `outputs_not_ready` | idem | C (mismo orden de evaluación) | PC |
| Evento `actuator_state` | `publishActuatorReport` | C | PC (JSON) |
| Fallo de PWM ⇒ `ready=false` | `initActuators` | C | Compila |

## Cambios de comportamiento deliberados

1. **La pérdida de Internet ya no apaga las cargas.** v4 las apagaba tras 60 s sin IoT Hub (`ACTUATOR_DISCONNECT_GRACE_MS`) y su README describía apagados por SAS. En C, el lease manual vence con el reloj monótono local y no depende de UTC, Wi-Fi, MQTT ni SAS. Prueba: `actuators_internet_loss_does_not_stop_lease_expires_monotonic`.
2. **Idempotencia por `commandId`.** Un C2D reentregado con el mismo `commandId` no se reaplica, así que no puede extender un lease ni reactivar una orden vencida. v4 lo documentaba como «no es un registro de deduplicación».
3. **Calibración: guardar antes de aplicar.** v4 aplicaba en RAM y, si fallaba la NVS, reportaba `applied_ram_nvs_failed`. Ahora la candidata se guarda y verifica antes de activarse; si falla, `rejected_storage_failed` y nada cambia.
4. **Sin reinicio por NTP o Wi-Fi.** v4 reiniciaba si fallaban NTP (60 s) o el portal. Ahora opera localmente y reintenta de forma espaciada.
5. **Modo de puesta en marcha por defecto** y bloqueo de actuadores sin parámetros eléctricos confirmados.
6. **BH1750 con error:** `value=null` y `driver_code=-1/-2`, en lugar de publicar −1 como lux.
7. **Diagnóstico `quick`:** usa el último STATUS de cada nodo en lugar de sondear el bus desde C.
8. **`reboot`:** sin `node` reinicia solo el gateway. Con `node` = `node_a`, `node_b` o `all` se enruta. `all` reinicia el gateway 5 s después.
