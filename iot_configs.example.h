#pragma once
// PLANTILLA con valores ficticios. Copiar como iot_configs.h (excluido de Git)
// y completar con los datos reales del dispositivo en IoT Hub.

#define IOT_HUB_HOSTNAME "<tu-hub>.azure-devices.net"
#define DEVICE_ID        "esp32-bioiot-01"

// Identificador configurable del experimento (cambiar para cada ensayo).
#define EXPERIMENT_ID    "EXP-001"

// Clave primaria del dispositivo (Base64). Nunca la cadena de conexion completa.
#define DEVICE_KEY       "<DEVICE_KEY_BASE64>"
