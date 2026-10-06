#pragma once
// PLANTILLA. Copiar como iot_configs.h (excluido de Git en esta carpeta).
// Puede reutilizarse la identidad del firmware v4 (esp32-bioiot-01) para conservar
// rutas y consultas, pero el firmware v4 y este gateway NO deben conectarse a la vez
// con la misma identidad: IoT Hub desconecta la sesion anterior y ambos se alternan.
#define BIOIOT_IOT_PLACEHOLDER 1

#define IOT_HUB_HOSTNAME "<tu-hub>.azure-devices.net"
#define DEVICE_ID "esp32-bioiot-01"
#define EXPERIMENT_ID "EXP-001"
// Clave primaria del dispositivo (Base64). Nunca la cadena de conexion completa.
#define DEVICE_KEY "<DEVICE_KEY_BASE64>"
