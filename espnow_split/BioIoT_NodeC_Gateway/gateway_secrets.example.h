#pragma once
// PLANTILLA. Copiar como gateway_secrets.h (excluido de Git) o generar con
//   node tools/generate-secrets.mjs --gateway-mac .. --node-a-mac .. --node-b-mac ..
// Con placeholders el gateway controla actuadores (modo configurado) y puede ir a
// Azure, pero NO activa ESP-NOW.
#define BIOIOT_SECRETS_PLACEHOLDER 1

#define BIOIOT_SYSTEM_ID 0x00000000UL
// Canal inicial ESP-NOW sin router. Asociado al AP, el canal lo impone el router.
#define BIOIOT_INITIAL_CHANNEL 1
#define BIOIOT_ESPNOW_PMK {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}

// Nodo A (ESP32 sensores): MAC STA, LMK y clave HMAC del enlace A<->C.
#define BIOIOT_NODE_A_MAC {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
#define BIOIOT_NODE_A_LMK {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
#define BIOIOT_NODE_A_APP_KEY {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, \
                               0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
// Nodo B (ESP8266 I2C): MAC STA, LMK y clave HMAC del enlace B<->C.
#define BIOIOT_NODE_B_MAC {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
#define BIOIOT_NODE_B_LMK {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
#define BIOIOT_NODE_B_APP_KEY {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, \
                               0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}

// Wi-Fi 2,4 GHz predeterminado: solo se usa si no hay una red guardada en NVS (portal).
// Vacio y sin red guardada => portal WiFiManager "BioIoT-AP" no bloqueante (180 s).
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
