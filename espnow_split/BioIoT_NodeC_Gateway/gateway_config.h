#pragma once
// Nodo C - ESP32 WROOM clasica: gateway ESP-NOW <-> Azure IoT Hub (MQTT/TLS) y
// control local de actuadores. Unico cliente de Azure. No es un broker MQTT.

// ============================ ACTUADORES ============================
// Dos perfiles de pines; NUNCA se mezclan. El perfil objetivo es el acordado para C.
//   TARGET   : compresor=13, rojo=32, verde=33, azul=4   (perfil acordado para el nodo C)
//   LEGACY_V4: compresor=32, rojo=33, verde=4,  azul=13  (BioIoT_Azure_Integrated v4)
#define ACTUATOR_PIN_PROFILE_TARGET 1
#define ACTUATOR_PIN_PROFILE_LEGACY_V4 2
#ifndef ACTUATOR_PIN_PROFILE
#define ACTUATOR_PIN_PROFILE ACTUATOR_PIN_PROFILE_TARGET
#endif
#if ACTUATOR_PIN_PROFILE == ACTUATOR_PIN_PROFILE_TARGET
#define COMPRESSOR_PIN 13
#define LED_R_PIN 32
#define LED_G_PIN 33
#define LED_B_PIN 4
#define ACTUATOR_PIN_PROFILE_NAME "target_c_13_32_33_4"
#elif ACTUATOR_PIN_PROFILE == ACTUATOR_PIN_PROFILE_LEGACY_V4
#define COMPRESSOR_PIN 32
#define LED_R_PIN 33
#define LED_G_PIN 4
#define LED_B_PIN 13
#define ACTUATOR_PIN_PROFILE_NAME "legacy_v4_32_33_4_13"
#else
#error "ACTUATOR_PIN_PROFILE desconocido"
#endif
// Bomba en GPIO2: reservada, NO implementada (no existia en v4 ni hay requisitos).
// GPIO2 es pin de arranque (debe quedar flotante/bajo para entrar en modo descarga)
// y en muchas placas tiene LED integrado; ver docs/PENDIENTES.md antes de usarlo.
#define PUMP_PIN_RESERVED 2
#define PUMP_ENABLED 0
#if PUMP_ENABLED
#error "Bomba fuera de alcance: no hay etapa electrica, polaridad ni politica confirmadas."
#endif

// ---- Modo de puesta en marcha (por defecto) ----
// 1 = ningun GPIO de actuador se configura como salida (alta impedancia); los comandos
//     se validan y se SIMULAN (outputs_driven=false). Para probar la cadena Azure->C
//     sin cargas. No es la politica ante perdida de Internet.
#ifndef ACTUATOR_COMMISSIONING_MODE
#define ACTUATOR_COMMISSIONING_MODE 1
#endif

// ---- Parametros electricos (pendientes de confirmar) ----
// Mientras un actuador no este confirmado, en modo produccion su pin queda en alta
// impedancia y los encendidos se rechazan (electrical_config_unconfirmed); el resto
// del sistema funciona.
// Compresor: pendiente decidir 12 V DC o red AC y la etapa (rele/SSR/MOSFET).
#define COMPRESSOR_ELECTRICAL_CONFIRMED 0
// #define COMPRESSOR_ACTIVE_LOW true     // definir al confirmar la etapa
// Luces 12 V: modulos MOSFET (posiblemente LR7843 con optoacoplador), sin confirmar.
#define LED_ELECTRICAL_CONFIRMED 0
// #define LED_PWM_ACTIVE_LOW false        // definir al confirmar el modulo
// #define LED_PWM_MAX_FREQUENCY_HZ 1000   // maximo del modulo segun su hoja de datos
#define LED_PWM_FREQUENCY_HZ 1000          // v4
#define LED_PWM_RESOLUTION_BITS 8

#if COMPRESSOR_ELECTRICAL_CONFIRMED && !defined(COMPRESSOR_ACTIVE_LOW)
#error "COMPRESSOR_ELECTRICAL_CONFIRMED=1 exige definir COMPRESSOR_ACTIVE_LOW"
#endif
#if LED_ELECTRICAL_CONFIRMED && (!defined(LED_PWM_ACTIVE_LOW) || !defined(LED_PWM_MAX_FREQUENCY_HZ))
#error "LED_ELECTRICAL_CONFIRMED=1 exige LED_PWM_ACTIVE_LOW y LED_PWM_MAX_FREQUENCY_HZ"
#endif
#if LED_ELECTRICAL_CONFIRMED && LED_PWM_FREQUENCY_HZ > LED_PWM_MAX_FREQUENCY_HZ
#error "LED_PWM_FREQUENCY_HZ supera el maximo confirmado del modulo"
#endif
#ifndef COMPRESSOR_ACTIVE_LOW
#define COMPRESSOR_ACTIVE_LOW true   // solo informativo mientras no este confirmado
#endif
#ifndef LED_PWM_ACTIVE_LOW
#define LED_PWM_ACTIVE_LOW false
#endif

// ---- Protecciones conservadas de v4 ----
#define COMPRESSOR_MIN_OFF_MS 180000UL     // reposo minimo, tambien desde el arranque
#define ACTUATOR_MAX_LEASE_SECONDS 900UL   // todo encendido manual requiere expiresAt
// v4 apagaba todo tras 60 s sin IoT Hub (ACTUATOR_DISCONNECT_GRACE_MS). Se ELIMINA esa
// dependencia: la perdida de Internet/MQTT/SAS no apaga las cargas por si sola. Los
// encendidos manuales siguen venciendo por su expiresAt con el reloj monotono local.

// ---- Programa local de respaldo: DESHABILITADO sin parametros de proceso ----
#define LOCAL_PROGRAM_ENABLED 0
#define LOCAL_LIGHT_ON_MINUTE_UTC -1   // minuto del dia UTC; -1 = sin definir
#define LOCAL_LIGHT_OFF_MINUTE_UTC -1
#define LOCAL_LIGHT_R 0
#define LOCAL_LIGHT_G 0
#define LOCAL_LIGHT_B 0
#define LOCAL_LIGHT_BRIGHTNESS 0
#define LOCAL_AERATION_ON_SECONDS 0    // 0 = sin definir
#define LOCAL_AERATION_OFF_SECONDS 0

// ============================ TIEMPOS Y COLAS ============================
#define TELEMETRY_INTERVAL_MS 120000UL       // v4: 120 s hacia Azure
#define GATEWAY_SNAPSHOT_SLOTS 40            // RAM: ~40 x 0,8 KB = 32 KB (~80 min a 120 s)
#define GATEWAY_EVENT_MAX_AGE_MS 600000UL    // eventos sin publicar se descartan tras 10 min
#define NODE_LINK_TIMEOUT_MS 20000UL         // online del nodo
#define SENSOR_FAST_MAX_AGE_MS 20000UL       // analogicos, color, temperatura, luz
#define SENSOR_O2_MAX_AGE_MS 270000UL        // O2: muestra cada 120 s (filtro v4) + margen
#define COMMAND_RADIO_TTL_MS 30000UL         // entrega por radio a un nodo
#define COMMAND_RESULT_TIMEOUT_MS 30000UL    // espera de resultado tras ACK
#define DIAG_TIMEOUT_MS 100000UL             // escaneo I2C completo de B puede tardar ~60-90 s
#define TIME_SYNC_INTERVAL_MS 60000UL
#define MQTT_PACKET_SIZE 24576               // diagnostico agregado <= ~14 KB
#define LOOP_WDT_TIMEOUT_MS 120000UL         // v4
#define LOCAL_TASK_WDT_TIMEOUT_MS 10000UL

// ---- Wi-Fi (solo C) ----
// Prototipo con hotspot movil de 2,4 GHz: canal y BSSID pueden cambiar en cada
// reinicio del hotspot. El canal guardado/BIOIOT_INITIAL_CHANNEL es solo una pista:
// al asociarse, ESP-NOW adopta el canal del AP y A/B lo buscan (channel_hunting).
#define WIFI_CONNECT_TIMEOUT_MS 15000UL        // busqueda completa del SSID + asociacion + DHCP
#define WIFI_QUICK_CONNECT_TIMEOUT_MS 8000UL   // canal/BSSID conocidos (sin barrido)
#define WIFI_LOST_QUICK_RETRY_MS 5000UL        // primer intento rapido tras perder la red
#define WIFI_QUICK_RETRY_MS 10000UL            // entre intentos rapidos
#define WIFI_QUICK_BEFORE_FULL 2               // intentos rapidos fallidos antes de buscar el SSID
#define WIFI_FIRST_FULL_SCAN_MS 45000UL        // primera busqueda completa: <= 45 s tras la perdida
#define WIFI_FULL_SCAN_MIN_MS 60000UL          // busquedas completas fallidas: 60 s, 120 s, ... (interrumpen
#define WIFI_FULL_SCAN_MAX_MS 600000UL         //   ESP-NOW ~2-3 s cada una; nunca continuas) hasta 10 min
#define WIFI_PORTAL_SSID "BioIoT-AP"           // abierto, http://192.168.4.1
#define WIFI_PORTAL_TIMEOUT_S 180
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.nist.gov"
