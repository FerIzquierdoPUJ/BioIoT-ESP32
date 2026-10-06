/*****************************************************************************************
  BioIoT Nodo B - ESP32 WROOM (ESP32 Dev Module): sensores I2C detras del
  PCA9548A/TCA9548A (2x BH1750, 2x SEN0322 O2 gas). Envia mediciones al gateway
  (nodo C) por ESP-NOW. Sin credenciales Azure. Calibraciones O2 en NVS de doble
  ranura con version y CRC32 (como el nodo A). Alternativa a BioIoT_NodeB_I2C (ESP8266):
  mismo protocolo, misma identidad de nodo B y mismas calibraciones.

  Cableado (el mismo I2C que v4 en ESP32):
    SDA = GPIO21, SCL = GPIO22, 3V3 y GND comunes con el DFR0576 (TCA9548A, 0x77).
  Compilar: Arduino-ESP32 3.3.12, placa "ESP32 Dev Module", biblioteca BioIoTCommon.
  Serial 115200: {"action":"pairing_info"} (MAC STA para emparejar) | {"action":"status"} |
  {"action":"diagnostics","scope":"i2c"} | {"action":"calibration_export"} |
  {"action":"calibration_import","export":{...},"force":false}
******************************************************************************************/
#include "NodeB.h"

void setup() { nodeBSetup(); }
void loop() { nodeBLoop(); }
