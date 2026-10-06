/*****************************************************************************************
  BioIoT_CalibrationExport - exporta las calibraciones del firmware integrado v4.

  Se carga TEMPORALMENTE en la placa ESP32 que ejecuta BioIoT_Azure_Integrated v4.
  Abre la NVS "biocal" en MODO SOLO LECTURA (nunca escribe ni borra), reproduce en
  memoria la migracion de esquema de loadCalibrationFromNVS() de v4 y escribe por
  Serial (115200) un JSON "bioiot-calibration-export" v1 con cada coeficiente en
  decimal y en patron IEEE-754 ("_f32"), para importar bit a bit en los nodos A y B.

  La NVS no se toca: al volver a cargar BioIoT_Azure_Integrated.ino la placa conserva
  sus calibraciones (cargar el sketch no borra la particion NVS salvo "Erase All Flash").
  No actua sobre GPIO, Wi-Fi ni Azure.

  Compilar con la biblioteca ../../libraries/BioIoTCommon (ver docs/MIGRACION.md).
******************************************************************************************/
#include <ArduinoJson.h>
#include <BioIoTCommon.h>
#include <Preferences.h>

using namespace bioiot;

// Valores de v4 (BioIoT_Azure_Integrated.ino) usados por su migracion de esquema.
static bool isOldPhDefaultV4(float m, float b) { return isOldPhDefault(m, b); }
static bool isOldDoDefaultV4(float m) { return isOldDoDefault(m); }

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n===== BioIoT Calibration Export (solo lectura NVS v4) =====");
  NodeACal a;  // curvas iniciales vigentes de v4
  NodeBCal b;
  Preferences prefs;
  const bool opened = prefs.begin("biocal", true);  // true = solo lectura
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  JsonObject meta = root["export_meta"].to<JsonObject>();
  meta["nvs_namespace_present"] = opened;
  uint32_t schema = 0, ver = 0;
  bool co2Enabled = false;
  bool blobOk[2] = {false, false};
  if (opened) {
    schema = prefs.getUInt("schema", 0);
    ver = prefs.getUInt("ver", 0);
    a.ph_m = prefs.getFloat("ph_m", a.ph_m); a.ph_b = prefs.getFloat("ph_b", a.ph_b);
    a.turb_m = prefs.getFloat("turb_m", a.turb_m); a.turb_b = prefs.getFloat("turb_b", a.turb_b);
    a.do_m = prefs.getFloat("do_m", a.do_m);
    a.temp_m = prefs.getFloat("temp_m", a.temp_m); a.temp_b = prefs.getFloat("temp_b", a.temp_b);
    a.co2_a = prefs.getFloat("co2_a", a.co2_a); a.co2_b = prefs.getFloat("co2_b", a.co2_b);
    co2Enabled = prefs.getBool("co2_en", false);
    a.co2_enabled = co2Enabled;
    a.ph_user = prefs.getBool("ph_user", false);
    a.do_user = prefs.getBool("do_user", false);
  }
  // Migracion de v4 (schema < 2), aplicada SOLO en memoria.
  if (schema < CALIBRATION_SCHEMA_VERSION) {
    if (!a.ph_user && isOldPhDefaultV4(a.ph_m, a.ph_b)) { a.ph_m = kPhCalMDefault; a.ph_b = kPhCalBDefault; }
    else if (a.ph_m != kPhCalMDefault || a.ph_b != kPhCalBDefault) a.ph_user = 1;
    if (!a.do_user && isOldDoDefaultV4(a.do_m)) a.do_m = kDoCalMDefault;
    else if (a.do_m != kDoCalMDefault) a.do_user = 1;
    if (co2Enabled && isfinite(a.co2_a) && a.co2_a > 0 && isfinite(a.co2_b)) {
      for (uint8_t i = 0; i < 2; ++i) {
        a.co2[i].mode = 1; a.co2[i].a = a.co2_a; a.co2[i].b = a.co2_b;
        a.co2[i].user = 1; a.co2[i].provisional = 0;
      }
    }
    meta["schema_migration_applied_in_memory"] = true;
  }
  if (opened) {
    for (uint8_t i = 0; i < 2; ++i) {
      const char* key = i == 0 ? "c1" : "c2";
      Co2Calibration stored;
      if (prefs.getBytesLength(key) == sizeof(stored) && prefs.getBytes(key, &stored, sizeof(stored)) == sizeof(stored) &&
          validCo2Calibration(stored)) {
        a.co2[i] = stored;
        blobOk[i] = true;
      }
      const String o = i == 0 ? "o1" : "o2";
      b.o2[i].gain = prefs.getFloat((o + "_gain").c_str(), 1);
      b.o2[i].offset = prefs.getFloat((o + "_off").c_str(), 0);
      b.o2[i].reference = prefs.getFloat((o + "_ref").c_str(), 0);
      b.o2[i].commandUtc = prefs.getLong64((o + "_utc").c_str(), 0);
      b.o2[i].commandCount = prefs.getUInt((o + "_cnt").c_str(), 0);
      if (!isfinite(b.o2[i].gain) || b.o2[i].gain <= 0 || !isfinite(b.o2[i].offset)) {
        b.o2[i].gain = 1; b.o2[i].offset = 0;
        meta[i == 0 ? "o2_gas_1_invalid_reset_to_identity" : "o2_gas_2_invalid_reset_to_identity"] = true;
      }
      if (!isfinite(b.o2[i].reference)) b.o2[i].reference = 0;
    }
    prefs.end();
  }
  a.legacyVersion = ver;
  a.legacySchema = schema;
  b.legacyVersion = ver;
  meta["schema_found"] = schema;
  meta["future_schema"] = schema > CALIBRATION_SCHEMA_VERSION;
  meta["co2_1_blob_ok"] = blobOk[0];
  meta["co2_2_blob_ok"] = blobOk[1];
  meta["valid_node_a"] = validNodeACal(a);
  meta["valid_node_b"] = validNodeBCal(b);
  writeCalibrationExport(root, "v4_integrated_nvs", "esp32-bioiot-01 (v4)", "", &a, &b);
  Serial.println("----- COPIAR DESDE LA SIGUIENTE LINEA -----");
  serializeJson(doc, Serial);
  Serial.println();
  Serial.println("----- FIN -----");
  if (schema > CALIBRATION_SCHEMA_VERSION)
    Serial.println("AVISO: esquema NVS mas nuevo que v4 conocido; revisar antes de importar.");
}

void loop() { delay(1000); }
