# Parámetros pendientes de confirmar

Mientras un dato no se confirme, la parte afectada queda deshabilitada o en su valor conservador. El resto del sistema funciona.

| Parámetro | Dónde | Estado actual (seguro) | Qué confirmar |
| --- | --- | --- | --- |
| Modelo de placa B | `node_b_config.h` `NODE_B_BOARD` | NodeMCU v2 (pines iguales en D1 mini) | Placa real; con D1 mini, usar el perfil `nodo_b_d1mini` y definir `NODE_B_BOARD_WEMOS_D1_MINI` |
| MAC de A, B y C | `generate-secrets.mjs` | Marcadores ⇒ ESP-NOW desactivado | `{"action":"pairing_info"}` en cada placa |
| Claves PMK/LMK/HMAC y `systemId` | archivos de secretos (no versionados) | Marcadores | Generarlas y custodiarlas fuera del repositorio |
| Canal ESP-NOW / router | `--channel`; canal fijo de 2,4 GHz recomendado | 1, con búsqueda automática | Fijar el canal del router si es posible |
| Canales del CD74HC4067 | `node_a_config.h` | Copia de trabajo 15/14/13/3/12/11 | Cableado real (o perfil 0..5) — [DISCREPANCIAS.md](DISCREPANCIAS.md) |
| Dirección del TCA9548A | `node_b_config.h` | 0x77 | DIP real (0x77) o 0x72 |
| Dirección de cada SEN0322 | `O2_ADDR_1/2` | 0x73 | Selectores físicos |
| Alimentación de los SEN0322 | `O2_SENSOR_SHARED_SUPPLY` | 1 (misma fuente que el ESP8266) | Si es independiente, poner 0 (warm-up completo en cada arranque) |
| Compresor: tipo de carga (12 V DC o red AC) y etapa (relé, SSR, MOSFET) | `gateway_config.h` | `COMPRESSOR_ELECTRICAL_CONFIRMED 0` ⇒ alta impedancia y encendidos bloqueados en producción | Etapa, aislamiento, protección inductiva, corriente de arranque y polaridad (`COMPRESSOR_ACTIVE_LOW`) |
| Módulos de luz 12 V (¿LR7843 + opto?) | `gateway_config.h` | `LED_ELECTRICAL_CONFIRMED 0` | Modelo, polaridad (`LED_PWM_ACTIVE_LOW`) y frecuencia PWM máxima (`LED_PWM_MAX_FREQUENCY_HZ`); 1 kHz (v4) solo si el módulo lo admite |
| Reposo mínimo del compresor | `COMPRESSOR_MIN_OFF_MS` | 180 s (valor inicial de v4, sin respaldo del fabricante) | Ficha técnica |
| Bomba en GPIO2 | `PUMP_ENABLED 0` (error de compilación si se activa) | Reservada, sin implementar | Requisitos, etapa y arranque: GPIO2 es pin de *strapping*, debe estar flotante o en bajo para el modo de descarga y suele tener LED integrado |
| Modo de puesta en marcha | `ACTUATOR_COMMISSIONING_MODE 1` | Comandos simulados | Pasar a 0 solo con lo anterior confirmado |
| Programa local de respaldo (fotoperiodo, ciclo de aireación) | `LOCAL_*` | Deshabilitado, sin valores | Consignas del proceso. No se inventaron fotoperiodos, umbrales, PID ni tiempos |
| Política productiva ante pérdida de Internet | — | Las cargas siguen hasta el vencimiento de su lease manual (≤ 900 s) y luego quedan según el programa local (apagado) | Si el proceso necesita luz o aireación continuas sin Internet, definir el programa local |
| Máximas edades por sensor | `SENSOR_FAST_MAX_AGE_MS` 20 s; `SENSOR_O2_MAX_AGE_MS` 270 s | Valores iniciales | Ajustar a la dinámica del proceso |
| Raíz TLS adicional | `azure_ca.h` (DigiCert G2, copia de v4) | Igual que v4 | Microsoft recomienda añadir también «Microsoft RSA Root CA 2017» (pendiente desde v4) |
| Identidad IoT Hub y clave publicada | `iot_configs.h` de C y de la raíz (no versionados desde 2026-10-05) | Plantillas `iot_configs.example.h` | **Rotar en Azure Portal** la clave expuesta en el historial de GitHub y decidir si C reutiliza `esp32-bioiot-01` (nunca simultáneo con v4) |
| LittleFS de una placa B nueva | `NODE_B_STORAGE_COMMISSIONING` (`node_b_config.h`) | 0: nunca formatea; sin montaje, rechaza calibraciones | Inicializar cada placa nueva una vez ([INSTALACION.md](INSTALACION.md) §3.1) y verificar P11 y P12 en placa |
| Persistencia offline ante corte de energía en C | — | Solo RAM (80 min) | Opcional: microSD (no disponible ni implementada) |
