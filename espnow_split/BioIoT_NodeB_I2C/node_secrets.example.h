#pragma once
// PLANTILLA. Copiar como node_secrets.h (excluido de Git) o generar con
//   node tools/generate-secrets.mjs --gateway-mac .. --node-a-mac .. --node-b-mac ..
// Con estos placeholders el nodo mide y diagnostica por Serial, pero NO activa
// ESP-NOW (nunca transmite con claves nulas ni en claro).
#define BIOIOT_SECRETS_PLACEHOLDER 1

// Identificador del sistema (32 bits, aleatorio, igual en A, B y C).
#define BIOIOT_SYSTEM_ID 0x00000000UL
// Canal ESP-NOW inicial (1..13). El nodo lo corrige si encuentra al gateway en otro.
#define BIOIOT_INITIAL_CHANNEL 1
// MAC STA del gateway (nodo C), impresa por su monitor serie al arrancar.
#define BIOIOT_GATEWAY_MAC {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
// PMK ESP-NOW (16 bytes, igual en los tres nodos).
#define BIOIOT_ESPNOW_PMK {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
// LMK del enlace B<->C (16 bytes, exclusiva de este enlace).
#define BIOIOT_LINK_LMK {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
// Clave HMAC de aplicacion del enlace B<->C (32 bytes, exclusiva de este enlace).
#define BIOIOT_LINK_APP_KEY {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, \
                             0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
