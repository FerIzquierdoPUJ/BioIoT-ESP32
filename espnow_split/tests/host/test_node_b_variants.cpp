// El nodo B existe para ESP8266 (BioIoT_NodeB_I2C) y ESP32 (BioIoT_NodeB_ESP32).
// La logica de calibracion, O2, warm-up y sensores debe ser la misma en ambos:
// solo cambian reinicio/RTC, almacenamiento, bus I2C y configuracion de placa.
#include <stdio.h>

#include <string>

#include "test_framework.h"

namespace {
std::string readNormalized(const char* path) {
  FILE* f = fopen(path, "rb");
  std::string s;
  if (!f) return s;
  int c;
  while ((c = fgetc(f)) != EOF)
    if (c != '\r') s.push_back(char(c));
  fclose(f);
  return s;
}
}  // namespace

TEST(node_b_esp8266_and_esp32_share_portable_logic) {
  const char* shared[] = {
      "CalibrationLogicB.cpp", "CalibrationLogicB.h", "O2Logic.cpp",     "O2Logic.h",
      "WarmupPolicy.cpp",      "WarmupPolicy.h",      "SensorsB.cpp",    "SensorsB.h",
      "DFRobot_OxygenSensor_B.cpp", "NodeB.h",        "node_secrets.example.h",
  };
  for (const char* name : shared) {
    const std::string a = readNormalized((std::string("BioIoT_NodeB_I2C/") + name).c_str());
    const std::string b = readNormalized((std::string("BioIoT_NodeB_ESP32/") + name).c_str());
    if (a.empty() || a != b) printf("  distinto o ausente: %s\n", name);
    CHECK(!a.empty() && a == b);
  }
}

TEST(node_b_esp32_keeps_v4_i2c_pins_and_addresses) {
  const std::string cfg = readNormalized("BioIoT_NodeB_ESP32/node_b_config.h");
  CHECK(cfg.find("#define I2C_SDA_PIN 21") != std::string::npos);
  CHECK(cfg.find("#define I2C_SCL_PIN 22") != std::string::npos);
  CHECK(cfg.find("#define TCA_ADDR 0x77") != std::string::npos);
  CHECK(cfg.find("#define O2_ADDR_1 0x73") != std::string::npos);
  CHECK(cfg.find("#define O2_WARMUP_MS 180000UL") != std::string::npos);
  // Sin modo de puesta en marcha de LittleFS: la NVS no se formatea.
  CHECK(cfg.find("NODE_B_STORAGE_COMMISSIONING") == std::string::npos);
}
