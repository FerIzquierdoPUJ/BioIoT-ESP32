/*****************************************************************************************
  BioIoT Nodo C - ESP32 WROOM: gateway ESP-NOW <-> Azure IoT Hub (MQTT/TLS, SAS) y
  control local de actuadores. Unico cliente de Azure; no es un broker MQTT.

  Por defecto arranca en MODO DE PUESTA EN MARCHA: ningun GPIO de carga se configura
  como salida y los comandos se simulan (ver gateway_config.h).

  Compilar: "ESP32 Dev Module", Arduino-ESP32 3.3.12, bibliotecas fijadas en
  sketch.yaml y ../libraries/BioIoTCommon. Copiar iot_configs.example.h ->
  iot_configs.h y gateway_secrets.example.h -> gateway_secrets.h (excluidos de Git).
  Serial 115200: {"action":"diagnostics","scope":"full"} | {"action":"status"} |
  {"action":"pairing_info"} | {"action":"wifi_portal"}
******************************************************************************************/
#include "NodeC.h"

void setup() { nodeCSetup(); }
void loop() { nodeCLoop(); }
