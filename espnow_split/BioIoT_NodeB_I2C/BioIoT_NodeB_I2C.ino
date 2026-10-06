/*****************************************************************************************
  BioIoT Nodo B - ESP8266 (NodeMCU v2 / Wemos D1 mini): sensores I2C detras del
  PCA9548A/TCA9548A (2x BH1750, 2x SEN0322 O2 gas). Envia mediciones al gateway
  (nodo C) por ESP-NOW cifrado. Sin credenciales Azure. Sin Preferences/NVS de ESP32:
  almacenamiento LittleFS de doble ranura con version y CRC32.

  Compilar: ESP8266 core 3.1.2, placa NodeMCU 1.0 (ESP-12E) o LOLIN(WEMOS) D1 mini,
  "Flash Size: 4MB (FS:1MB ...)", y la biblioteca local ../libraries/BioIoTCommon.
  Serial 115200: {"action":"diagnostics","scope":"i2c"} | {"action":"status"} |
  {"action":"pairing_info"} | {"action":"calibration_export"} |
  {"action":"calibration_import","export":{...},"force":false}
  LittleFS nunca se autoformatea; placa nueva: docs/INSTALACION.md 3.1 (storage_format).
******************************************************************************************/
#include "NodeB.h"

void setup() { nodeBSetup(); }
void loop() { nodeBLoop(); }
