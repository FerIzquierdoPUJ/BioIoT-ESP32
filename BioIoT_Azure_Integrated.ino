/*****************************************************************************************
  BioIoT_Azure_Integrated.ino
  ESP32 WROOM + Azure IoT Hub MQTT + sensores de fotobiorreactor

  v3: intervalo 2 min, calibración dinámica vía Cloud-to-Device, persistencia NVS.

  NOVEDAD - Cloud-to-Device messages:
  El ESP32 escucha mensajes desde Azure IoT Hub para actualizar las curvas de
  calibración sin reflashear. Los valores se guardan en NVS (persisten al reset).

  Formato de mensaje C2D (enviar como JSON):
    {"action": "set", "sensor": "ph",   "m": -6.1125, "b": 15.013}
    {"action": "set", "sensor": "turb", "m": -2310.5, "b": 5435.4}
    {"action": "set", "sensor": "do",   "m": 0.1335}
    {"action": "set", "sensor": "temp", "m": 0.9741, "b": 1.0038}
    {"action": "set", "sensor": "co2",  "a": 12.34,   "b": -0.5}      // habilita CO2
    {"action": "reset", "sensor": "ph"}                                // vuelve a default
    {"action": "reset_all"}                                            // todo a default
    {"action": "reboot"}                                               // reinicia el ESP32

  Cómo enviar desde Azure (CLI):
    az iot device c2d-message send --hub-name <HUB> --device-id esp32-bioiot-01 \
      --data '{"action":"set","sensor":"ph","m":-6.1125,"b":15.013}'

  El JSON de telemetría ahora incluye un nodo "calibration" con los valores
  activos para que puedas verificar en Azure que los comandos llegaron.

  Librerías requeridas (Arduino IDE Library Manager):
  - WiFiManager
  - PubSubClient
  - Azure SDK for C
  - OneWire
  - DallasTemperature
  - BH1750
  - DFRobot_OxygenSensor
  - ArduinoJson  (NUEVO)
******************************************************************************************/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <BH1750.h>
#include "DFRobot_OxygenSensor.h"

#include <az_core.h>
#include <az_iot.h>
#include "AzureIoTSasToken.h"
#include "azure_ca.h"
#include "iot_configs.h"

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

// ===================== CONFIGURACIÓN GENERAL =====================
#define MQTT_PACKET_SIZE          4096
#define TELEMETRY_INTERVAL_MS     120000UL   // 2 minutos
#define O2_WARMUP_MS              180000UL   // 3 min de warm-up para SEN0322
#define USE_INSECURE_TLS          true

// ===================== PINES ESP32 =====================
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22

#define MUX_SIG_PIN 36
#define MUX_S0_PIN  25
#define MUX_S1_PIN  26
#define MUX_S2_PIN  27
#define MUX_S3_PIN  14
#define MUX_EN_PIN  13

#define ONE_WIRE_BUS 23

#define TCS_S0 16
#define TCS_S1 17
#define TCS_S2 18
#define TCS_S3 19
#define TCS_OUT_1 34
#define TCS_OUT_2 35

// ===================== DIRECCIONES Y CANALES I2C =====================
#define TCA_ADDR 0x72
#define TCA_CH_BH1750_1 0
#define TCA_CH_BH1750_2 1
#define TCA_CH_O2_1     2
#define TCA_CH_O2_2     3
#define BH1750_ADDR_1 0x23
#define BH1750_ADDR_2 0x23
#define O2_ADDR_1     0x73
#define O2_ADDR_2     0x73

// ===================== CANALES DEL CD74HC4067 =====================
#define CH_PH       0
#define CH_CO2_1    1
#define CH_CO2_2    2
#define CH_TURB     3
#define CH_DO       4
#define CH_TDS      5

// ===================== DEFAULTS DE CALIBRACIÓN =====================
// Valores que se cargan si NVS está vacío o tras un reset_all.
#define PH_CAL_M_DEFAULT     (-6.1125f)
#define PH_CAL_B_DEFAULT     ( 15.013f)
#define TURB_CAL_M_DEFAULT   (-2310.5f)
#define TURB_CAL_B_DEFAULT   ( 5435.4f)
#define DO_CAL_M_DEFAULT     (0.1335f)
#define TEMP_CAL_M_DEFAULT   (0.9741f)
#define TEMP_CAL_B_DEFAULT   (1.0038f)
#define CO2_CAL_A_DEFAULT    (0.0f)
#define CO2_CAL_B_DEFAULT    (0.0f)

// ===================== SENSORES ESPERADOS =====================
const bool EXPECT_PH    = true;
const bool EXPECT_CO2_1 = true;
const bool EXPECT_CO2_2 = true;
const bool EXPECT_TURB  = false;
const bool EXPECT_DO    = true;
const bool EXPECT_TDS   = true;
const bool EXPECT_TEMP  = true;
const bool EXPECT_BH1   = true;
const bool EXPECT_BH2   = true;
const bool EXPECT_TCS1  = true;
const bool EXPECT_TCS2  = true;
const bool EXPECT_O2_1  = true;
const bool EXPECT_O2_2  = true;

// ===================== ESTRUCTURA DE CALIBRACIÓN =====================
struct Calibration {
  float    ph_m, ph_b;
  float    turb_m, turb_b;
  float    do_m;
  float    temp_m, temp_b;
  float    co2_a, co2_b;
  bool     co2_enabled;
  uint32_t version;
};

Calibration cal;
Preferences prefs;

void loadCalibrationDefaults() {
  cal.ph_m        = PH_CAL_M_DEFAULT;
  cal.ph_b        = PH_CAL_B_DEFAULT;
  cal.turb_m      = TURB_CAL_M_DEFAULT;
  cal.turb_b      = TURB_CAL_B_DEFAULT;
  cal.do_m        = DO_CAL_M_DEFAULT;
  cal.temp_m      = TEMP_CAL_M_DEFAULT;
  cal.temp_b      = TEMP_CAL_B_DEFAULT;
  cal.co2_a       = CO2_CAL_A_DEFAULT;
  cal.co2_b       = CO2_CAL_B_DEFAULT;
  cal.co2_enabled = false;
}

void loadCalibrationFromNVS() {
  loadCalibrationDefaults();
  prefs.begin("biocal", true);
  cal.ph_m        = prefs.getFloat("ph_m",   cal.ph_m);
  cal.ph_b        = prefs.getFloat("ph_b",   cal.ph_b);
  cal.turb_m      = prefs.getFloat("turb_m", cal.turb_m);
  cal.turb_b      = prefs.getFloat("turb_b", cal.turb_b);
  cal.do_m        = prefs.getFloat("do_m",   cal.do_m);
  cal.temp_m      = prefs.getFloat("temp_m", cal.temp_m);
  cal.temp_b      = prefs.getFloat("temp_b", cal.temp_b);
  cal.co2_a       = prefs.getFloat("co2_a",  cal.co2_a);
  cal.co2_b       = prefs.getFloat("co2_b",  cal.co2_b);
  cal.co2_enabled = prefs.getBool ("co2_en", cal.co2_enabled);
  cal.version     = prefs.getUInt ("ver",    0);
  prefs.end();
  Serial.printf("Calibración cargada de NVS, version=%u\n", cal.version);
}

void saveCalibrationToNVS() {
  prefs.begin("biocal", false);
  prefs.putFloat("ph_m",   cal.ph_m);
  prefs.putFloat("ph_b",   cal.ph_b);
  prefs.putFloat("turb_m", cal.turb_m);
  prefs.putFloat("turb_b", cal.turb_b);
  prefs.putFloat("do_m",   cal.do_m);
  prefs.putFloat("temp_m", cal.temp_m);
  prefs.putFloat("temp_b", cal.temp_b);
  prefs.putFloat("co2_a",  cal.co2_a);
  prefs.putFloat("co2_b",  cal.co2_b);
  prefs.putBool ("co2_en", cal.co2_enabled);
  prefs.putUInt ("ver",    cal.version);
  prefs.end();
  Serial.printf("Calibración guardada en NVS, version=%u\n", cal.version);
}

// ===================== OBJETOS =====================
WiFiClientSecure sslClient;
PubSubClient mqttClient(sslClient);

static az_iot_hub_client hubClient;
char mqttClientId[128];
char mqttUsername[256];
char telemetryTopic[128];
char c2dTopic[128];
char sasToken[512];
char sasSignatureBuffer[512];
char sasBuffer[512];

AzIoTSasToken sasTokenObj(
  &hubClient,
  AZ_SPAN_FROM_STR(DEVICE_KEY),
  AZ_SPAN_FROM_BUFFER(sasSignatureBuffer),
  AZ_SPAN_FROM_BUFFER(sasBuffer)
);

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature ds18(&oneWire);
BH1750 bh1750;
DFRobot_OxygenSensor o2sensor;

unsigned long bootMillis = 0;

// ===================== ESTRUCTURAS DE LECTURA =====================
struct AnalogReading {
  int raw;
  float voltage;
  bool connected;
};

struct TcsReading {
  unsigned long rPulse, gPulse, bPulse;
  uint8_t r, g, b;
  float h, s, l;
  bool connected;
};

// ===================== PROTOTIPOS MANUALES =====================
// Necesarios para evitar que el preprocesador de Arduino genere prototipos
// antes de conocer las estructuras AnalogReading y TcsReading.
AnalogReading readAnalogMux(byte channel);
TcsReading readTcs3200(int outPin);
String analogJson(const char *name, const char *unit, AnalogReading r, float value, bool expected);
String colorJson(const char *name, TcsReading t, bool expected);


// ===================== UTILIDADES =====================
bool detectI2C(byte address) {
  Wire.beginTransmission(address);
  return (Wire.endTransmission() == 0);
}

bool tcaSelect(uint8_t channel) {
  if (channel > 7) return false;
  Wire.beginTransmission(TCA_ADDR);
  Wire.write(1 << channel);
  return Wire.endTransmission() == 0;
}

void muxSelect(byte channel) {
  digitalWrite(MUX_S0_PIN, bitRead(channel, 0));
  digitalWrite(MUX_S1_PIN, bitRead(channel, 1));
  digitalWrite(MUX_S2_PIN, bitRead(channel, 2));
  digitalWrite(MUX_S3_PIN, bitRead(channel, 3));
  delayMicroseconds(250);
}

AnalogReading readAnalogMux(byte channel) {
  muxSelect(channel);
  const int N = 16;
  long acc = 0;
  for (int i = 0; i < N; i++) {
    acc += analogRead(MUX_SIG_PIN);
    delay(3);
  }
  AnalogReading r;
  r.raw = acc / N;
  r.voltage = r.raw * (3.3f / 4095.0f);
  r.connected = !(r.raw <= 5 || r.raw >= 4090);
  return r;
}

String jsonNumberOrNull(float value, int decimals = 3) {
  if (isnan(value) || isinf(value)) return "null";
  return String(value, decimals);
}

bool o2IsWarmingUp() {
  return (millis() - bootMillis) < O2_WARMUP_MS;
}

// ===================== FUNCIONES DE CALIBRACIÓN =====================
float calculatePh(float voltage_V) {
  float ph = cal.ph_m * voltage_V + cal.ph_b;
  if (ph < 0.0f || ph > 14.0f) return NAN;
  return ph;
}

float calculateTurbidityNtu(float voltage_V) {
  float ntu = cal.turb_m * voltage_V + cal.turb_b;
  if (ntu < 0.0f || ntu > 4000.0f) return NAN;
  return ntu;
}

float calculateDoSaturationPct(float voltage_V) {
  float voltage_mV = voltage_V * 1000.0f;
  float pct = cal.do_m * voltage_mV;
  if (pct < 0.0f) pct = 0.0f;
  if (pct > 150.0f) pct = 150.0f;
  return pct;
}

float doSaturationConcentrationMgL(float tempC) {
  if (isnan(tempC)) return NAN;
  float t = tempC, t2 = t*t, t3 = t2*t;
  return 14.652f - 0.41022f * t + 0.0079910f * t2 - 0.000077774f * t3;
}

float calculateDoMgL(float voltage_V, float tempC) {
  float satPct = calculateDoSaturationPct(voltage_V);
  if (isnan(satPct)) return NAN;
  float doSat = doSaturationConcentrationMgL(tempC);
  if (isnan(doSat)) return NAN;
  return (satPct / 100.0f) * doSat;
}

float calibrateTemperature(float reading) {
  return cal.temp_m * reading + cal.temp_b;
}

float calculateCo2Ppm(float voltage_V) {
  if (!cal.co2_enabled) return NAN;
  return cal.co2_a * expf(cal.co2_b * voltage_V);
}

float calculateTdsPpm(float voltage_V) {
  return (133.42f * voltage_V * voltage_V * voltage_V
        - 255.86f * voltage_V * voltage_V
        + 857.39f * voltage_V) * 0.5f;
}

// ===================== LECTURA DE SENSORES =====================
float readRawTemperatureC(bool &connected) {
  ds18.requestTemperatures();
  float tempC = ds18.getTempCByIndex(0);
  connected = !(tempC == DEVICE_DISCONNECTED_C || tempC < -55.0f || tempC > 125.0f);
  return tempC;
}

float readBh1750Lux(uint8_t tcaChannel, uint8_t address, bool &connected) {
  connected = false;
  if (!tcaSelect(tcaChannel)) return NAN;
  delay(5);
  if (!detectI2C(address)) return NAN;
  if (!bh1750.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, address, &Wire)) return NAN;
  delay(180);
  float lux = bh1750.readLightLevel();
  connected = !(isnan(lux) || lux < 0);
  return lux;
}

float readO2Percent(uint8_t tcaChannel, uint8_t address, bool &connected) {
  connected = false;
  if (!tcaSelect(tcaChannel)) return NAN;
  delay(5);
  if (!detectI2C(address)) return NAN;
  if (!o2sensor.begin(address)) return NAN;
  delay(10);
  float pct = o2sensor.getOxygenData(10);
  connected = !(isnan(pct) || pct < 0.0f || pct > 100.0f);
  return pct;
}

// ===================== TCS3200 + HSL =====================
uint8_t pulseToIntensity(unsigned long pulse) {
  if (pulse == 0 || pulse >= 30000UL) return 0;
  long mapped = map((long)pulse, 50L, 30000L, 255L, 0L);
  if (mapped < 0) mapped = 0;
  if (mapped > 255) mapped = 255;
  return (uint8_t)mapped;
}

void rgbToHsl(uint8_t r, uint8_t g, uint8_t b, float &h, float &s, float &l) {
  float rf = r/255.0f, gf = g/255.0f, bf = b/255.0f;
  float cMax = fmaxf(rf, fmaxf(gf, bf));
  float cMin = fminf(rf, fminf(gf, bf));
  float delta = cMax - cMin;
  l = (cMax + cMin) * 0.5f;
  if (delta < 1e-6f) { h = 0; s = 0; return; }
  s = (l < 0.5f) ? (delta / (cMax + cMin)) : (delta / (2.0f - cMax - cMin));
  if (cMax == rf)      h = 60.0f * fmodf((gf - bf) / delta, 6.0f);
  else if (cMax == gf) h = 60.0f * (((bf - rf) / delta) + 2.0f);
  else                 h = 60.0f * (((rf - gf) / delta) + 4.0f);
  if (h < 0) h += 360.0f;
}

TcsReading readTcs3200(int outPin) {
  TcsReading r;
  digitalWrite(TCS_S2, LOW);  digitalWrite(TCS_S3, LOW);
  r.rPulse = pulseIn(outPin, LOW, 30000); delay(10);
  digitalWrite(TCS_S2, HIGH); digitalWrite(TCS_S3, HIGH);
  r.gPulse = pulseIn(outPin, LOW, 30000); delay(10);
  digitalWrite(TCS_S2, LOW);  digitalWrite(TCS_S3, HIGH);
  r.bPulse = pulseIn(outPin, LOW, 30000); delay(10);
  r.connected = !(r.rPulse == 0 || r.gPulse == 0 || r.bPulse == 0);
  r.r = pulseToIntensity(r.rPulse);
  r.g = pulseToIntensity(r.gPulse);
  r.b = pulseToIntensity(r.bPulse);
  rgbToHsl(r.r, r.g, r.b, r.h, r.s, r.l);
  return r;
}

// ===================== HELPERS DE ALERTAS Y JSON =====================
void appendAlert(String &alerts, bool condition, const char *sensorName) {
  if (!condition) return;
  if (alerts.length() > 0) alerts += ",";
  alerts += "\"";
  alerts += sensorName;
  alerts += "_disconnected\"";
}

String analogJson(const char *name, const char *unit, AnalogReading r,
                  float value, bool expected) {
  String s = "\"" + String(name) + "\":{";
  s += "\"expected\":" + String(expected ? "true" : "false") + ",";
  s += "\"connected\":" + String(r.connected ? "true" : "false") + ",";
  s += "\"raw\":" + String(r.raw) + ",";
  s += "\"voltage\":" + String(r.voltage, 4) + ",";
  s += "\"value\":" + jsonNumberOrNull(value, 3) + ",";
  s += "\"unit\":\"" + String(unit) + "\"";
  s += "}";
  return s;
}

String colorJson(const char *name, TcsReading t, bool expected) {
  String s = "\"" + String(name) + "\":{";
  s += "\"expected\":" + String(expected ? "true" : "false") + ",";
  s += "\"connected\":" + String(t.connected ? "true" : "false") + ",";
  s += "\"rPulse\":" + String(t.rPulse) + ",";
  s += "\"gPulse\":" + String(t.gPulse) + ",";
  s += "\"bPulse\":" + String(t.bPulse) + ",";
  s += "\"r\":" + String(t.r) + ",\"g\":" + String(t.g) + ",\"b\":" + String(t.b) + ",";
  s += "\"h\":" + String(t.h, 1) + ",\"s\":" + String(t.s, 3) + ",\"l\":" + String(t.l, 3);
  s += "}";
  return s;
}

String calibrationJson() {
  String s = "\"calibration\":{";
  s += "\"version\":" + String(cal.version) + ",";
  s += "\"ph\":{\"m\":" + String(cal.ph_m, 4) + ",\"b\":" + String(cal.ph_b, 4) + "},";
  s += "\"turb\":{\"m\":" + String(cal.turb_m, 4) + ",\"b\":" + String(cal.turb_b, 4) + "},";
  s += "\"do\":{\"m\":" + String(cal.do_m, 4) + "},";
  s += "\"temp\":{\"m\":" + String(cal.temp_m, 4) + ",\"b\":" + String(cal.temp_b, 4) + "},";
  s += "\"co2\":{\"enabled\":" + String(cal.co2_enabled ? "true" : "false");
  s += ",\"a\":" + String(cal.co2_a, 4) + ",\"b\":" + String(cal.co2_b, 4) + "}";
  s += "}";
  return s;
}

// ===================== CALLBACK MQTT (Cloud-to-Device) =====================
// Recibe mensajes desde Azure y actualiza la calibración en runtime.
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("C2D recibido en topic: ");
  Serial.println(topic);

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.print("Error parseando JSON C2D: ");
    Serial.println(err.c_str());
    return;
  }

  const char* action = doc["action"] | "";
  Serial.print("Acción: ");
  Serial.println(action);

  if (strcmp(action, "set") == 0) {
    const char* sensor = doc["sensor"] | "";
    bool changed = false;

    if (strcmp(sensor, "ph") == 0) {
      if (doc.containsKey("m")) { cal.ph_m = doc["m"]; changed = true; }
      if (doc.containsKey("b")) { cal.ph_b = doc["b"]; changed = true; }
      if (changed) Serial.printf("pH: m=%.4f b=%.4f\n", cal.ph_m, cal.ph_b);
    }
    else if (strcmp(sensor, "turb") == 0) {
      if (doc.containsKey("m")) { cal.turb_m = doc["m"]; changed = true; }
      if (doc.containsKey("b")) { cal.turb_b = doc["b"]; changed = true; }
      if (changed) Serial.printf("turb: m=%.4f b=%.4f\n", cal.turb_m, cal.turb_b);
    }
    else if (strcmp(sensor, "do") == 0) {
      if (doc.containsKey("m")) { cal.do_m = doc["m"]; changed = true; }
      if (changed) Serial.printf("DO: m=%.4f\n", cal.do_m);
    }
    else if (strcmp(sensor, "temp") == 0) {
      if (doc.containsKey("m")) { cal.temp_m = doc["m"]; changed = true; }
      if (doc.containsKey("b")) { cal.temp_b = doc["b"]; changed = true; }
      if (changed) Serial.printf("temp: m=%.4f b=%.4f\n", cal.temp_m, cal.temp_b);
    }
    else if (strcmp(sensor, "co2") == 0) {
      if (doc.containsKey("a")) { cal.co2_a = doc["a"]; changed = true; }
      if (doc.containsKey("b")) { cal.co2_b = doc["b"]; changed = true; }
      cal.co2_enabled = true;
      changed = true;
      Serial.printf("CO2: enabled, a=%.4f b=%.4f\n", cal.co2_a, cal.co2_b);
    }
    else {
      Serial.println("Sensor desconocido");
      return;
    }

    if (changed) {
      cal.version++;
      saveCalibrationToNVS();
    }
  }
  else if (strcmp(action, "reset") == 0) {
    const char* sensor = doc["sensor"] | "";
    if (strcmp(sensor, "ph") == 0) { cal.ph_m = PH_CAL_M_DEFAULT; cal.ph_b = PH_CAL_B_DEFAULT; }
    else if (strcmp(sensor, "turb") == 0) { cal.turb_m = TURB_CAL_M_DEFAULT; cal.turb_b = TURB_CAL_B_DEFAULT; }
    else if (strcmp(sensor, "do") == 0)   { cal.do_m = DO_CAL_M_DEFAULT; }
    else if (strcmp(sensor, "temp") == 0) { cal.temp_m = TEMP_CAL_M_DEFAULT; cal.temp_b = TEMP_CAL_B_DEFAULT; }
    else if (strcmp(sensor, "co2") == 0)  { cal.co2_enabled = false; cal.co2_a = 0; cal.co2_b = 0; }
    else { Serial.println("Sensor desconocido para reset"); return; }
    cal.version++;
    saveCalibrationToNVS();
    Serial.println("Sensor reseteado a default");
  }
  else if (strcmp(action, "reset_all") == 0) {
    loadCalibrationDefaults();
    cal.version++;
    saveCalibrationToNVS();
    Serial.println("Todas las calibraciones reseteadas");
  }
  else if (strcmp(action, "reboot") == 0) {
    Serial.println("Reboot solicitado por C2D");
    delay(500);
    ESP.restart();
  }
  else {
    Serial.println("Acción desconocida");
  }
}

void subscribeToC2D() {
  snprintf(c2dTopic, sizeof(c2dTopic), "devices/%s/messages/devicebound/#", DEVICE_ID);
  if (mqttClient.subscribe(c2dTopic, 1)) {
    Serial.print("Suscrito a topic C2D: ");
    Serial.println(c2dTopic);
  } else {
    Serial.println("Error suscribiendo al topic C2D");
  }
}

// ===================== WIFI + AZURE =====================
void initWiFi() {
  WiFi.mode(WIFI_STA);
  if (strlen(WIFI_SSID) > 0) {
    Serial.print("WiFi configurado: "); Serial.println(WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000UL) {
      delay(500); Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi conectado.");
      return;
    }
  }
  Serial.println("\nIniciando WiFiManager: red BioIoT-AP.");
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  if (!wm.autoConnect("BioIoT-AP")) {
    Serial.println("No se configuró WiFi. Reiniciando...");
    ESP.restart();
  }
}

void initTime() {
  Serial.print("Sincronizando hora NTP");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  time_t now = time(NULL);
  while (now < 1600000000) { delay(500); Serial.print("."); now = time(NULL); }
  Serial.println(" OK");
}

void initAzureSDK() {
  az_iot_hub_client_options options = az_iot_hub_client_options_default();
  if (az_result_failed(az_iot_hub_client_init(
        &hubClient,
        AZ_SPAN_FROM_STR(IOT_HUB_HOSTNAME),
        AZ_SPAN_FROM_STR(DEVICE_ID),
        &options))) {
    Serial.println("Error inicializando Azure IoT Hub client.");
    while (true) delay(1000);
  }
  az_iot_hub_client_get_client_id(&hubClient, mqttClientId, sizeof(mqttClientId), NULL);
  az_iot_hub_client_get_user_name(&hubClient, mqttUsername, sizeof(mqttUsername), NULL);
  az_iot_hub_client_telemetry_get_publish_topic(&hubClient, NULL, telemetryTopic, sizeof(telemetryTopic), NULL);

  mqttClient.setServer(IOT_HUB_HOSTNAME, 8883);
  mqttClient.setBufferSize(MQTT_PACKET_SIZE);
  mqttClient.setCallback(mqttCallback);
}

void connectToAzure() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    delay(1000);
    return;
  }
  if (sasTokenObj.Generate(60) != 0) {
    Serial.println("Error generando SAS Token.");
    return;
  }
  az_span span = sasTokenObj.Get();
  memcpy(sasToken, (char*)az_span_ptr(span), az_span_size(span));
  sasToken[az_span_size(span)] = 0;

  Serial.print("Conectando MQTT a Azure IoT Hub... ");
  if (mqttClient.connect(mqttClientId, mqttUsername, sasToken)) {
    Serial.println("OK");
    subscribeToC2D();
  } else {
    Serial.print("Fallo RC=");
    Serial.println(mqttClient.state());
  }
}

// ===================== TELEMETRÍA =====================
String buildTelemetryJson() {
  unsigned long nowMs = millis();
  String alerts = "";

  AnalogReading ph   = readAnalogMux(CH_PH);
  AnalogReading co21 = readAnalogMux(CH_CO2_1);
  AnalogReading co22 = readAnalogMux(CH_CO2_2);
  AnalogReading turb = readAnalogMux(CH_TURB);
  AnalogReading dor  = readAnalogMux(CH_DO);
  AnalogReading tds  = readAnalogMux(CH_TDS);

  bool tempConnected;
  float tempRaw = readRawTemperatureC(tempConnected);
  float tempCal = tempConnected ? calibrateTemperature(tempRaw) : NAN;

  bool bh1Connected, bh2Connected;
  float lux1 = readBh1750Lux(TCA_CH_BH1750_1, BH1750_ADDR_1, bh1Connected);
  float lux2 = readBh1750Lux(TCA_CH_BH1750_2, BH1750_ADDR_2, bh2Connected);

  bool o2_1_connected = false, o2_2_connected = false;
  float o2_1_pct = NAN, o2_2_pct = NAN;
  bool warming = o2IsWarmingUp();
  if (!warming) {
    o2_1_pct = readO2Percent(TCA_CH_O2_1, O2_ADDR_1, o2_1_connected);
    o2_2_pct = readO2Percent(TCA_CH_O2_2, O2_ADDR_2, o2_2_connected);
  }

  TcsReading tcs1 = readTcs3200(TCS_OUT_1);
  TcsReading tcs2 = readTcs3200(TCS_OUT_2);

  float phCal     = ph.connected   ? calculatePh(ph.voltage)              : NAN;
  float co21Cal   = co21.connected ? calculateCo2Ppm(co21.voltage)        : NAN;
  float co22Cal   = co22.connected ? calculateCo2Ppm(co22.voltage)        : NAN;
  float turbCal   = turb.connected ? calculateTurbidityNtu(turb.voltage)  : NAN;
  float doSatPct  = dor.connected  ? calculateDoSaturationPct(dor.voltage): NAN;
  float doMgL     = dor.connected  ? calculateDoMgL(dor.voltage, tempCal) : NAN;
  float tdsCal    = tds.connected  ? calculateTdsPpm(tds.voltage)         : NAN;

  appendAlert(alerts, EXPECT_PH    && !ph.connected, "ph");
  appendAlert(alerts, EXPECT_CO2_1 && !co21.connected, "co2_1");
  appendAlert(alerts, EXPECT_CO2_2 && !co22.connected, "co2_2");
  appendAlert(alerts, EXPECT_TURB  && !turb.connected, "turbidity");
  appendAlert(alerts, EXPECT_DO    && !dor.connected, "dissolved_oxygen");
  appendAlert(alerts, EXPECT_TDS   && !tds.connected, "tds");
  appendAlert(alerts, EXPECT_TEMP  && !tempConnected, "temperature");
  appendAlert(alerts, EXPECT_BH1   && !bh1Connected, "light_1");
  appendAlert(alerts, EXPECT_BH2   && !bh2Connected, "light_2");
  appendAlert(alerts, EXPECT_O2_1  && !warming && !o2_1_connected, "o2_gas_1");
  appendAlert(alerts, EXPECT_O2_2  && !warming && !o2_2_connected, "o2_gas_2");
  appendAlert(alerts, EXPECT_TCS1  && !tcs1.connected, "color_1");
  appendAlert(alerts, EXPECT_TCS2  && !tcs2.connected, "color_2");

  String json = "{";
  json += "\"deviceId\":\"" DEVICE_ID "\",";
  json += "\"uptimeMs\":" + String(nowMs) + ",";
  json += "\"wifiRssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"status\":\"" + String(alerts.length() == 0 ? "ok" : "warning") + "\",";
  json += "\"alerts\":[" + alerts + "],";
  json += calibrationJson() + ",";

  json += "\"sensors\":{";
  json += analogJson("ph", "pH", ph, phCal, EXPECT_PH) + ",";
  json += analogJson("co2_1", "ppm", co21, co21Cal, EXPECT_CO2_1) + ",";
  json += analogJson("co2_2", "ppm", co22, co22Cal, EXPECT_CO2_2) + ",";
  json += analogJson("turbidity", "NTU", turb, turbCal, EXPECT_TURB) + ",";

  json += "\"dissolved_oxygen\":{";
  json += "\"expected\":" + String(EXPECT_DO ? "true" : "false") + ",";
  json += "\"connected\":" + String(dor.connected ? "true" : "false") + ",";
  json += "\"raw\":" + String(dor.raw) + ",";
  json += "\"voltage\":" + String(dor.voltage, 4) + ",";
  json += "\"saturation_pct\":" + jsonNumberOrNull(doSatPct, 2) + ",";
  json += "\"value\":" + jsonNumberOrNull(doMgL, 3) + ",";
  json += "\"unit\":\"mg/L\"";
  json += "},";

  json += analogJson("tds", "ppm", tds, tdsCal, EXPECT_TDS) + ",";

  json += "\"temperature\":{";
  json += "\"expected\":" + String(EXPECT_TEMP ? "true" : "false") + ",";
  json += "\"connected\":" + String(tempConnected ? "true" : "false") + ",";
  json += "\"raw\":" + jsonNumberOrNull(tempRaw, 2) + ",";
  json += "\"value\":" + jsonNumberOrNull(tempCal, 2) + ",";
  json += "\"unit\":\"C\"},";

  json += "\"light_1\":{";
  json += "\"expected\":" + String(EXPECT_BH1 ? "true" : "false") + ",";
  json += "\"connected\":" + String(bh1Connected ? "true" : "false") + ",";
  json += "\"value\":" + jsonNumberOrNull(lux1, 2) + ",\"unit\":\"lux\"},";

  json += "\"light_2\":{";
  json += "\"expected\":" + String(EXPECT_BH2 ? "true" : "false") + ",";
  json += "\"connected\":" + String(bh2Connected ? "true" : "false") + ",";
  json += "\"value\":" + jsonNumberOrNull(lux2, 2) + ",\"unit\":\"lux\"},";

  json += "\"o2_gas_1\":{";
  json += "\"expected\":" + String(EXPECT_O2_1 ? "true" : "false") + ",";
  json += "\"connected\":" + String(o2_1_connected ? "true" : "false") + ",";
  json += "\"warming_up\":" + String(warming ? "true" : "false") + ",";
  json += "\"value\":" + jsonNumberOrNull(o2_1_pct, 2) + ",\"unit\":\"%vol\"},";

  json += "\"o2_gas_2\":{";
  json += "\"expected\":" + String(EXPECT_O2_2 ? "true" : "false") + ",";
  json += "\"connected\":" + String(o2_2_connected ? "true" : "false") + ",";
  json += "\"warming_up\":" + String(warming ? "true" : "false") + ",";
  json += "\"value\":" + jsonNumberOrNull(o2_2_pct, 2) + ",\"unit\":\"%vol\"},";

  json += colorJson("color_1", tcs1, EXPECT_TCS1) + ",";
  json += colorJson("color_2", tcs2, EXPECT_TCS2);

  json += "}}";
  return json;
}

void publishTelemetry() {
  if (!mqttClient.connected()) return;
  String payload = buildTelemetryJson();
  Serial.print("Payload bytes: ");
  Serial.println(payload.length());
  Serial.println(payload);
  bool ok = mqttClient.publish(telemetryTopic, payload.c_str());
  Serial.println(ok ? "Telemetría enviada." : "Error publicando telemetría.");
}

// ===================== SETUP / LOOP =====================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n===== BIO-IOT ESP32 + AZURE (v3) =====");
  bootMillis = millis();

  loadCalibrationFromNVS();

  pinMode(MUX_S0_PIN, OUTPUT);
  pinMode(MUX_S1_PIN, OUTPUT);
  pinMode(MUX_S2_PIN, OUTPUT);
  pinMode(MUX_S3_PIN, OUTPUT);
  pinMode(MUX_EN_PIN, OUTPUT);
  digitalWrite(MUX_EN_PIN, LOW);

  pinMode(TCS_S0, OUTPUT); pinMode(TCS_S1, OUTPUT);
  pinMode(TCS_S2, OUTPUT); pinMode(TCS_S3, OUTPUT);
  pinMode(TCS_OUT_1, INPUT); pinMode(TCS_OUT_2, INPUT);
  digitalWrite(TCS_S0, HIGH); digitalWrite(TCS_S1, LOW);

  analogReadResolution(12);
  analogSetPinAttenuation(MUX_SIG_PIN, ADC_11db);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  ds18.begin();

  Serial.print("TCA9548A en 0x");
  Serial.print(TCA_ADDR, HEX);
  Serial.println(detectI2C(TCA_ADDR) ? " detectado." : " NO detectado.");

  Serial.printf("Warm-up SEN0322: %lu s\n", O2_WARMUP_MS / 1000UL);
  Serial.printf("Intervalo de telemetría: %lu s\n", TELEMETRY_INTERVAL_MS / 1000UL);

#if USE_INSECURE_TLS
  sslClient.setInsecure();
#else
  sslClient.setCACert(ca_pem);
#endif

  initWiFi();
  initTime();
  initAzureSDK();
  connectToAzure();
}

void loop() {
  if (!mqttClient.connected()) {
    connectToAzure();
  }
  mqttClient.loop();  // procesa mensajes entrantes (C2D)

  static unsigned long lastTelemetry = 0;
  if (millis() - lastTelemetry >= TELEMETRY_INTERVAL_MS) {
    publishTelemetry();
    lastTelemetry = millis();
  }
}
