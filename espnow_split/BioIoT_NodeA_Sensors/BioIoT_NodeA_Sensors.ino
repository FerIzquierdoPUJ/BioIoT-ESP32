/*****************************************************************************************
  BioIoT Nodo A - ESP32 WROOM: sensores analogicos (CD74HC4067), color (2x TCS3200)
  y temperatura (DS18B20). Envia mediciones calibradas al gateway (nodo C) por ESP-NOW
  cifrado. No tiene credenciales Azure.

  Compilar: placa "ESP32 Dev Module", Arduino-ESP32 3.3.12, bibliotecas fijadas en
  sketch.yaml y la biblioteca local ../libraries/BioIoTCommon. Ver ../README.md.
  Serial 115200: {"action":"diagnostics","scope":"full"} | {"action":"status"} |
  {"action":"pairing_info"} | {"action":"calibration_export"} |
  {"action":"calibration_import","export":{...},"force":false}
******************************************************************************************/
#include "NodeA.h"

void setup() { nodeASetup(); }
void loop() { nodeALoop(); }
