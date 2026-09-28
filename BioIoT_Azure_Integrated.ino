/*****************************************************************************************
  BioIoT_Azure_Integrated.ino
  ESP32 WROOM + Azure IoT Hub MQTT + sensores de fotobiorreactor

  v4: TLS verificado, JSON UTF-8, timestamp UTC y actuadores por Cloud-to-Device.

  Diagnostico de hardware C2D (solo encola; se ejecuta por etapas en loop):
    {"action":"diagnostics","scope":"full"}
    {"action":"diagnostics","scope":"i2c"}
    {"action":"diagnostics","scope":"analog"}
    {"action":"diagnostics","scope":"color"}
    {"action":"diagnostics","scope":"system"}
    {"action":"diagnostics","scope":"quick"}
    {"action":"diagnostics","scope":"i2c_recover"}
  Serial Monitor: enviar esos mismos JSON, una linea por comando (diagnostico).

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
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <esp_timer.h>
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
#include "ActuatorControl.h"
#include "CalibrationModel.h"

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

// ===================== CONFIGURACIÓN GENERAL =====================
#define MQTT_PACKET_SIZE          32768  // informe conservador <= 27 KiB; envio normal sigue compacto
#define TELEMETRY_INTERVAL_MS     120000UL   // 2 minutos
#define O2_WARMUP_MS              180000UL   // 3 min de warm-up para SEN0322
// Peor vuelta legitima de loop(): DNS + TCP (30 s) + TLS (5 s) + CONNACK + lecturas.
#define LOOP_WDT_TIMEOUT_MS       120000UL

// ===================== PINES ESP32 =====================
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22

#define MUX_SIG_PIN 36
#define MUX_S0_PIN  25
#define MUX_S1_PIN  26
#define MUX_S2_PIN  27
#define MUX_S3_PIN  14
// EN del CD74HC4067 conectado permanentemente a GND; GPIO13 es ahora LED B.

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
#define PH_CAL_M_DEFAULT     (-6.112459f)
#define PH_CAL_B_DEFAULT     ( 15.012668f)
#define TURB_CAL_M_DEFAULT   (-2310.5f)
#define TURB_CAL_B_DEFAULT   ( 5435.4f)
#define DO_CAL_M_DEFAULT     (0.13351912f)
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
  bool ph_user = false, do_user = false;
  uint32_t schema = CALIBRATION_SCHEMA_VERSION;
};

Calibration cal;
Preferences prefs;
Co2Calibration co2Cal[2];
O2Calibration o2Cal[2];
O2Filter o2Filter1, o2Filter2;
O2Reading o2Readings[2];
bool calibrationStorageOk = true;
bool calibrationFutureSchema = false;

// Arduino prototypes must follow the custom types.
bool saveCalibrationToNVS();
void resetO2Filters();


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
  cal.co2_enabled = false; // legacy snapshot, not the active independent profiles
  cal.ph_user = false; cal.do_user = false;
  cal.schema = CALIBRATION_SCHEMA_VERSION;
  for (uint8_t i = 0; i < 2; ++i) { co2Cal[i] = Co2Calibration(); o2Cal[i] = O2Calibration(); }
  resetO2Filters();
}

void loadCalibrationFromNVS() {
  loadCalibrationDefaults();
  // An absent namespace on first boot is normal; defaults will be persisted.
  const bool opened = prefs.begin("biocal", true);
  const uint32_t schema = opened ? prefs.getUInt("schema", 0) : 0;
  cal.schema = schema;
  if (opened) {
    cal.ph_m = prefs.getFloat("ph_m", cal.ph_m); cal.ph_b = prefs.getFloat("ph_b", cal.ph_b);
    cal.turb_m = prefs.getFloat("turb_m", cal.turb_m); cal.turb_b = prefs.getFloat("turb_b", cal.turb_b);
    cal.do_m = prefs.getFloat("do_m", cal.do_m);
    cal.temp_m = prefs.getFloat("temp_m", cal.temp_m); cal.temp_b = prefs.getFloat("temp_b", cal.temp_b);
    cal.co2_a = prefs.getFloat("co2_a", cal.co2_a); cal.co2_b = prefs.getFloat("co2_b", cal.co2_b);
    cal.co2_enabled = prefs.getBool("co2_en", false);
    cal.version = prefs.getUInt("ver", 0);
    cal.ph_user = prefs.getBool("ph_user", false); cal.do_user = prefs.getBool("do_user", false);
  }
  calibrationFutureSchema = schema > CALIBRATION_SCHEMA_VERSION;
  if (schema < CALIBRATION_SCHEMA_VERSION) {
    if (!cal.ph_user && isOldPhDefault(cal.ph_m, cal.ph_b)) {
      cal.ph_m = PH_CAL_M_DEFAULT; cal.ph_b = PH_CAL_B_DEFAULT;
      Serial.println("NVS migration: pH legacy defaults -> experimental_3point (V).");
    } else if (cal.ph_m != PH_CAL_M_DEFAULT || cal.ph_b != PH_CAL_B_DEFAULT) cal.ph_user = true;
    if (!cal.do_user && isOldDoDefault(cal.do_m)) {
      cal.do_m = DO_CAL_M_DEFAULT;
      Serial.println("NVS migration: DO legacy default -> experimental_2point (mV).");
    } else if (cal.do_m != DO_CAL_M_DEFAULT) cal.do_user = true;
    // Valid enabled legacy a/b keep exactly their ADC-voltage interpretation.
    if (cal.co2_enabled && isfinite(cal.co2_a) && cal.co2_a > 0 && isfinite(cal.co2_b)) {
      for (uint8_t i = 0; i < 2; ++i) {
        co2Cal[i].mode = 1; co2Cal[i].a = cal.co2_a; co2Cal[i].b = cal.co2_b;
        co2Cal[i].user = 1; co2Cal[i].provisional = 0;
      }
    }
  }
  if (opened) {
    // Independent blobs override migration defaults, also on a retried partial migration.
    for (uint8_t i = 0; i < 2; ++i) {
      const String prefix = i == 0 ? "c1" : "c2";
      Co2Calibration stored;
      if (prefs.getBytesLength(prefix.c_str()) == sizeof(stored) &&
          prefs.getBytes(prefix.c_str(), &stored, sizeof(stored)) == sizeof(stored) &&
          validCo2Calibration(stored)) co2Cal[i] = stored;
      else if (schema >= CALIBRATION_SCHEMA_VERSION) calibrationStorageOk = false;
      const String o = i == 0 ? "o1" : "o2";
      o2Cal[i].gain = prefs.getFloat((o + "_gain").c_str(), 1);
      o2Cal[i].offset = prefs.getFloat((o + "_off").c_str(), 0);
      o2Cal[i].reference = prefs.getFloat((o + "_ref").c_str(), 0);
      o2Cal[i].commandUtc = prefs.getLong64((o + "_utc").c_str(), 0);
      o2Cal[i].commandCount = prefs.getUInt((o + "_cnt").c_str(), 0);
      if (!isfinite(o2Cal[i].gain) || o2Cal[i].gain <= 0 || !isfinite(o2Cal[i].offset)) {
        o2Cal[i].gain = 1; o2Cal[i].offset = 0; calibrationStorageOk = false;
      }
    }
    prefs.end();
  }
  if (schema < CALIBRATION_SCHEMA_VERSION) {
    cal.schema = CALIBRATION_SCHEMA_VERSION;
    // Keep calibration revision "ver"; schema migration is not a user command.
    const bool saved = saveCalibrationToNVS();
    Serial.printf("NVS schema %lu -> %lu: %s; calibration revision=%lu\n",
        (unsigned long)schema, (unsigned long)cal.schema, saved ? "saved" : "WRITE FAILED",
        (unsigned long)cal.version);
  }
  if (calibrationFutureSchema) {
    calibrationStorageOk = false;
    Serial.println("NVS future schema: writes blocked; upgrade firmware.");
  }
}

bool saveCalibrationToNVS() {
  if (calibrationFutureSchema || !prefs.begin("biocal", false)) {
    calibrationStorageOk = false; return false;
  }
  bool ok = true;
  ok &= prefs.putFloat("ph_m", cal.ph_m) == sizeof(float);
  ok &= prefs.putFloat("ph_b", cal.ph_b) == sizeof(float);
  ok &= prefs.putFloat("turb_m", cal.turb_m) == sizeof(float);
  ok &= prefs.putFloat("turb_b", cal.turb_b) == sizeof(float);
  ok &= prefs.putFloat("do_m", cal.do_m) == sizeof(float);
  ok &= prefs.putFloat("temp_m", cal.temp_m) == sizeof(float);
  ok &= prefs.putFloat("temp_b", cal.temp_b) == sizeof(float);
  ok &= prefs.putFloat("co2_a", cal.co2_a) == sizeof(float);
  ok &= prefs.putFloat("co2_b", cal.co2_b) == sizeof(float);
  ok &= prefs.putBool("co2_en", cal.co2_enabled) == 1;
  ok &= prefs.putBool("ph_user", cal.ph_user) == 1;
  ok &= prefs.putBool("do_user", cal.do_user) == 1;
  for (uint8_t i = 0; i < 2; ++i) {
    ok &= prefs.putBytes(i == 0 ? "c1" : "c2", &co2Cal[i], sizeof(Co2Calibration)) == sizeof(Co2Calibration);
    const String o = i == 0 ? "o1" : "o2";
    ok &= prefs.putFloat((o + "_gain").c_str(), o2Cal[i].gain) == sizeof(float);
    ok &= prefs.putFloat((o + "_off").c_str(), o2Cal[i].offset) == sizeof(float);
    ok &= prefs.putFloat((o + "_ref").c_str(), o2Cal[i].reference) == sizeof(float);
    ok &= prefs.putLong64((o + "_utc").c_str(), o2Cal[i].commandUtc) == sizeof(int64_t);
    ok &= prefs.putUInt((o + "_cnt").c_str(), o2Cal[i].commandCount) == sizeof(uint32_t);
  }
  ok &= prefs.putUInt("ver", cal.version) == sizeof(uint32_t);
  // Commit schema only after every preceding write succeeds; migration can retry.
  if (ok) ok &= prefs.putUInt("schema", CALIBRATION_SCHEMA_VERSION) == sizeof(uint32_t);
  prefs.end();
  calibrationStorageOk = ok;
  Serial.printf("Calibration NVS revision=%lu: %s\n", (unsigned long)cal.version, ok ? "saved" : "WRITE FAILED");
  return ok;
}

void resetO2Filters() {
  o2Filter1.clear(); o2Filter2.clear();
  o2Readings[0] = O2Reading(); o2Readings[1] = O2Reading();
}

// ===================== OBJETOS =====================
WiFiClientSecure sslClient;
PubSubClient mqttClient(sslClient);

static az_iot_hub_client hubClient;
char mqttClientId[128];
char mqttUsername[256];
char telemetryTopic[256];
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
DFRobot_OxygenSensor o2Sensor1;
DFRobot_OxygenSensor o2Sensor2;

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

// ===================== ESTADO DE DIAGNOSTICO (solo RAM) =====================
struct I2CProbeResult {
  uint8_t address = 0;
  bool detected = false;
  uint8_t errorCode = 0;
};
struct TcaSelectResult {
  bool attempted = false;
  bool selected = false;
  uint8_t channel = 0;
  uint8_t errorCode = 0;
};
struct DiagnosticHistory {
  uint32_t success = 0, failure = 0, consecutiveFailures = 0;
  uint32_t lastSuccessMs = 0, lastFailureMs = 0;
  time_t lastSuccessUtc = 0, lastFailureUtc = 0;
};
struct AnalogDiagnostic {
  bool sampled = false;
  uint8_t samples = 0;
  int rawMin = 0, rawMax = 0;
  float rawAvg = 0, voltageAvg = 0;
  uint32_t sampledAt = 0;
};
struct I2CReadingDiagnostic {
  bool observed = false, probeAttempted = false;
  TcaSelectResult selection;
  I2CProbeResult probe;
  const char* status = "not_sampled";
};
struct I2CScanSegment {
  // codes[a] is exact endTransmission result; tested[a] distinguishes skipped addresses.
  uint8_t codes[127] = {};
  bool tested[127] = {};
  TcaSelectResult selection;
  bool completed = false;
  bool warmupSkipped = false;
};

const int ANALOG_SIMILAR_SPREAD = 8; // evidencia, nunca modifica connected
const char* const diagnosticDeviceNames[] = {
  "i2c_tca", "bh1", "bh2", "o2_1", "o2_2", "analog_mux", "color1", "color2", "temperature"
};
const char* const analogSensorNames[] = {"ph", "co2_1", "co2_2", "turbidity", "dissolved_oxygen", "tds"};
DiagnosticHistory diagnosticHistory[9];
DiagnosticHistory tcaProbeHistory, tcaSelectHistory, tcaDisableHistory;
AnalogDiagnostic analogDiagnostics[6];
I2CReadingDiagnostic i2cReadingDiagnostics[4];
TcsReading diagnosticColor[2] = {};
bool diagnosticColorSampled[2] = {};
uint32_t diagnosticColorAt[2] = {};
bool analogBatchValid = false, analogAllNearZero = false, analogTooSimilar = false;
float analogMinAverage = 0, analogMaxAverage = 0;
uint32_t analogAllZeroCount = 0;
bool diagnosticTempSampled = false, diagnosticTempDetected = false;
float diagnosticTempRaw = NAN;
uint8_t diagnosticTempCount = 0;
uint32_t diagnosticTempAt = 0;
I2CProbeResult lastTcaProbe;
bool lastTcaObserved = false;
uint32_t lastTcaProbeAt = 0;

enum DiagnosticScope : uint8_t { DIAG_FULL, DIAG_I2C, DIAG_ANALOG, DIAG_COLOR,
  DIAG_SYSTEM, DIAG_QUICK, DIAG_RECOVER };
enum DiagnosticPhase : uint8_t { DIAG_IDLE, DIAG_BEGIN, DIAG_MAIN, DIAG_SELECT,
  DIAG_CHANNEL, DIAG_ANALOG_READ, DIAG_COLOR_READ, DIAG_TEMP_READ, DIAG_READY, DIAG_O2_READ };
bool diagnosticRequestPending = false;
uint8_t diagnosticScope = DIAG_QUICK;
uint8_t diagnosticPhase = DIAG_IDLE;
uint8_t diagnosticAddress = 1, diagnosticChannel = 0, diagnosticIndex = 0;
bool diagnosticBusDirty = false, diagnosticWarmup = false;
uint32_t diagnosticStartedAt = 0, diagnosticCompletedAt = 0, diagnosticWarmupRemaining = 0;
I2CScanSegment diagnosticMainScan, diagnosticChannelScans[8];
TcaSelectResult diagnosticIsolation, diagnosticCleanup;
I2CProbeResult diagnosticTca, recoveryBefore, recoveryAfter;
bool recoveryEndOk = false, recoveryBeginOk = false;
uint32_t diagnosticReportSequence = 0, diagnosticPublishAttempt = 0;
String pendingDiagnosticPayload;
bool o2CalibrationPending = false;
uint8_t pendingO2Index = 0;
String pendingCalibrationAck;
uint32_t calibrationAckAt = 0;
bool diagnosticSerialPrinted = false;
// commandId opcional del C2D, devuelto en calibration_ack para que la UI correlacione.
char calibrationCommandId[65] = "";
char pendingO2CommandId[65] = "";
bool rebootRequested = false;
uint32_t rebootRequestedAt = 0;

// ===================== PROTOTIPOS MANUALES =====================
// Necesarios para evitar que el preprocesador de Arduino genere prototipos
// antes de conocer las estructuras AnalogReading y TcsReading.
AnalogReading readAnalogMux(byte channel);
TcsReading readTcs3200(int outPin);
void writeCo2Reading(JsonObject o, uint8_t index, AnalogReading reading, bool detailed);
String co2Json(uint8_t index, AnalogReading reading);
String analogJson(const char *name, const char *unit, AnalogReading r, float value, bool expected);
String colorJson(const char *name, TcsReading t, bool expected);
I2CProbeResult probeI2C(uint8_t address);
TcaSelectResult tcaSelectDetailed(uint8_t channel);
TcaSelectResult tcaDisableAll();
void recordDiagnosticHistory(DiagnosticHistory &h, bool success);
void writeDiagnosticHistory(JsonObject object, const DiagnosticHistory &h);
void writeScanDevices(JsonArray array, const I2CScanSegment &scan, bool downstreamOnly);
void writeScanErrors(JsonObject object, const I2CScanSegment &scan);


// ===================== UTILIDADES =====================
bool detectI2C(byte address) {
  return probeI2C(address).detected;
}

bool tcaSelect(uint8_t channel) {
  return tcaSelectDetailed(channel).selected;
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
  delay(3); // CD74HC4067 settling; preserve raw*3.3/4095 calibration scale
  analogRead(MUX_SIG_PIN); // discard the first conversion after switching
  const int N = 16;
  long acc = 0;
  int rawMin = 4095, rawMax = 0;
  for (int i = 0; i < N; i++) {
    const int sample = analogRead(MUX_SIG_PIN);
    acc += sample;
    rawMin = min(rawMin, sample);
    rawMax = max(rawMax, sample);
    delay(3);
  }
  AnalogReading r;
  r.raw = acc / N;
  r.voltage = r.raw * (3.3f / 4095.0f);
  r.connected = !(r.raw <= 5 || r.raw >= 4090);
  if (channel < 6) {
    AnalogDiagnostic &d = analogDiagnostics[channel];
    d.sampled = true; d.samples = N; d.rawMin = rawMin; d.rawMax = rawMax;
    d.rawAvg = float(acc) / N;
    d.voltageAvg = d.rawAvg * (3.3f / 4095.0f);
    d.sampledAt = millis();
  }
  return r;
}

String jsonNumberOrNull(float value, int decimals = 3) {
  if (isnan(value) || isinf(value)) return "null";
  return String(value, decimals);
}

// Escapar la configuracion como string JSON (comillas, barras y UTF-8).
String jsonStringValue(const char* value) {
  JsonDocument doc;
  doc.set(value);
  String json;
  serializeJson(doc, json);
  return json;
}

const char* sensorQuality(bool connected, bool validValue, bool warmingUp = false) {
  if (warmingUp) return "warming_up";
  if (!connected) return "disconnected";
  // El valor nulo se conserva; no se presume una medicion valida.
  return validValue ? "good" : "out_of_range";
}

String utcTimestampIso8601() {
  time_t now = time(NULL);
  if (now < 1600000000) return "";

  struct tm utcTime;
  gmtime_r(&now, &utcTime);

  char timestamp[21];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utcTime);
  return String(timestamp);
}

bool o2IsWarmingUp() {
  // Latch: millis() se desborda a los ~49.7 dias; el warm-up no debe repetirse.
  static bool warmed = false;
  if (!warmed && millis() - bootMillis >= O2_WARMUP_MS) warmed = true;
  return !warmed;
}

const char* resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_EXT: return "external_pin";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt_watchdog";
    case ESP_RST_TASK_WDT: return "task_watchdog";
    case ESP_RST_WDT: return "other_watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    default: return "other";
  }
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
  return evaluateCo2(voltage_V, true, co2Cal[0]).value; // legacy helper, sensor #1
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
  diagnosticTempSampled = true;
  diagnosticTempDetected = connected;
  diagnosticTempRaw = tempC;
  diagnosticTempCount = ds18.getDeviceCount();
  diagnosticTempAt = millis();
  recordDiagnosticHistory(diagnosticHistory[8], connected);
  return tempC;
}

float readBh1750Lux(uint8_t tcaChannel, uint8_t address, bool &connected) {
  connected = false;
  const uint8_t index = tcaChannel == TCA_CH_BH1750_1 ? 0 : 1;
  if (!prepareI2CReading(index, tcaChannel, address)) return NAN;
  delay(5);
  if (!probeI2CReading(index, address)) return NAN;
  if (!bh1750.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, address, &Wire)) {
    finishI2CReading(index, false, "driver_init_failed"); return NAN;
  }
  delay(180);
  float lux = bh1750.readLightLevel();
  connected = !(isnan(lux) || lux < 0);
  finishI2CReading(index, connected, connected ? "measurement_ok" : "invalid_measurement");
  return lux;
}

float readO2Percent(uint8_t tcaChannel, uint8_t address, bool &connected) {
  const uint8_t sensor = tcaChannel == TCA_CH_O2_1 ? 0 : 1;
  const uint8_t index = sensor + 2;
  O2Reading &r = o2Readings[sensor];
  O2Filter &filter = sensor == 0 ? o2Filter1 : o2Filter2;
  DFRobot_OxygenSensor &driver = sensor == 0 ? o2Sensor1 : o2Sensor2;
  r = O2Reading(); r.observed = true; r.sampledAt = millis(); r.warming = o2IsWarmingUp();
  connected = false;
  r.quality = "communication_fault";
  if (!prepareI2CReading(index, tcaChannel, address)) {
    r.wireError = i2cReadingDiagnostics[index].selection.errorCode; filter.clear(); return NAN;
  }
  if (!probeI2CReading(index, address)) {
    r.wireError = i2cReadingDiagnostics[index].probe.errorCode; filter.clear(); return NAN;
  }
  r.i2cDetected = true;
  if (r.warming) {
    // Transport is testable now; concentration is deliberately not acquired.
    connected = r.connected = true;
    r.quality = "warming_up"; i2cReadingDiagnostics[index].status = "warming_up";
    filter.clear(); return NAN;
  }
  if (!driver.begin(address)) {
    r.wireError = driver.lastWireError(); r.readCount = driver.lastReadCount();
    finishI2CReading(index, false, "communication_fault"); filter.clear(); return NAN;
  }
  r.raw = driver.getOxygenData(1); // one sample; never internal multiplexed smoothing
  r.wireError = driver.lastWireError(); r.readCount = driver.lastReadCount();
  connected = r.connected = driver.communicationOk();
  if (!connected) {
    finishI2CReading(index, false, "communication_fault"); filter.clear(); return NAN;
  }
  const float corrected = r.raw * o2Cal[sensor].gain + o2Cal[sensor].offset;
  if (!isfinite(r.raw) || r.raw < 0 || r.raw > 30 ||
      !isfinite(corrected) || corrected < 0 || corrected > 30) {
    r.quality = "out_of_range"; filter.clear();
    finishI2CReading(index, false, "invalid_measurement"); return NAN;
  }
  r.filtered = filter.add(r.raw);
  r.value = r.filtered * o2Cal[sensor].gain + o2Cal[sensor].offset;
  r.valid = true;
  r.stabilizing = filter.count < O2_FILTER_SAMPLES;
  r.quality = corrected > 25 || r.value > 25 ? "above_nominal_range" :
      r.stabilizing ? "stabilizing" : "good";
  finishI2CReading(index, true, "measurement_ok");
  return r.value;
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
  const uint8_t index = outPin == TCS_OUT_1 ? 0 : 1;
  diagnosticColor[index] = r;
  diagnosticColorSampled[index] = true;
  diagnosticColorAt[index] = millis();
  recordDiagnosticHistory(diagnosticHistory[6 + index], r.connected);
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

void appendQualityAlert(String &alerts, bool condition, const char* message) {
  if (!condition) return;
  if (alerts.length()) alerts += ",";
  alerts += jsonStringValue(message);
}

String analogJson(const char *name, const char *unit, AnalogReading r,
                  float value, bool expected) {
  String s = "\"" + String(name) + "\":{";
  s += "\"expected\":" + String(expected ? "true" : "false") + ",";
  s += "\"connected\":" + String(r.connected ? "true" : "false") + ",";
  s += "\"raw\":" + String(r.raw) + ",";
  s += "\"voltage\":" + String(r.voltage, 4) + ",";
  s += "\"value\":" + jsonNumberOrNull(value, 3) + ",";
  s += "\"unit\":\"" + String(unit) + "\",";
  s += "\"quality\":\"" + String(sensorQuality(r.connected, isfinite(value))) + "\"";
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
  s += ",\"quality\":\"" + String(sensorQuality(
      t.connected, isfinite(t.h) && isfinite(t.s) && isfinite(t.l))) + "\"";
  s += "}";
  return s;
}

String calibrationJson() {
  JsonDocument doc;
  JsonObject c = doc["calibration"].to<JsonObject>();
  c["version"] = cal.version; c["schema_version"] = cal.schema;
  c["nvs_ok"] = calibrationStorageOk;
  c["ph"]["m"] = cal.ph_m; c["ph"]["b"] = cal.ph_b;
  c["ph"]["source"] = cal.ph_user ? "user_calibrated" : "experimental_3point";
  c["ph"]["input_unit"] = "V";
  if (!cal.ph_user) {
    JsonArray buffers = c["ph"]["buffers"].to<JsonArray>();
    buffers.add(4.01); buffers.add(7.01); buffers.add(10.01);
    c["ph"]["r2"] = 0.9999586071;
  }
  c["turb"]["m"] = cal.turb_m; c["turb"]["b"] = cal.turb_b;
  c["do"]["m"] = cal.do_m; c["do"]["input_unit"] = "mV";
  c["do"]["source"] = cal.do_user ? "user_calibrated" : "experimental_2point";
  if (!cal.do_user) {
    c["do"]["zero_mv"] = 0.0; c["do"]["saturation_100_mv"] = 748.9564;
    c["do"]["r2"] = 1.0;
  }
  c["temp"]["m"] = cal.temp_m; c["temp"]["b"] = cal.temp_b;
  // Compatibility snapshot: independent nodes are authoritative.
  c["co2"]["enabled"] = cal.co2_enabled; c["co2"]["a"] = cal.co2_a; c["co2"]["b"] = cal.co2_b;
  c["co2"]["legacy_snapshot"] = true;
  for (uint8_t i = 0; i < 2; ++i) {
    writeCo2Calibration(c[i == 0 ? "co2_1" : "co2_2"].to<JsonObject>(), i);
    JsonObject o = c[i == 0 ? "o2_gas_1" : "o2_gas_2"].to<JsonObject>();
    o["gain"] = o2Cal[i].gain; o["offset"] = o2Cal[i].offset;
    o["source"] = o2CalibrationSource(i);
  }
  String text;
  serializeJson(doc["calibration"], text);
  return "\"calibration\":" + text;
}

// New profiles retain legacy fields; sources describe the ACTIVE curve.
const char* co2Mode(uint8_t index) { return co2Cal[index].mode ? "legacy_exponential" : "sen0159_vendor"; }
const char* co2Source(uint8_t index) { return co2Cal[index].user ? "user_calibrated" : "vendor_reference"; }
const char* o2CalibrationSource(uint8_t index) {
  return o2Cal[index].gain == 1 && o2Cal[index].offset == 0 ?
      "sensor_internal_calibration" : "user_software_correction";
}
void writeCo2Calibration(JsonObject o, uint8_t index) {
  const auto &c = co2Cal[index];
  o["mode"] = co2Mode(index); o["source"] = co2Source(index);
  o["provisional"] = bool(c.provisional); o["enabled"] = bool(c.enabled);
  o["zero_point_v"] = c.zero_point_v; o["reaction_voltage_v"] = c.reaction_voltage_v;
  o["a"] = c.a; o["b"] = c.b;
}
void writeCo2Reading(JsonObject o, uint8_t index, AnalogReading reading, bool detailed) {
  const auto &c = co2Cal[index];
  const Co2Reading r = evaluateCo2(reading.voltage, reading.connected, c);
  o["expected"] = index == 0 ? EXPECT_CO2_1 : EXPECT_CO2_2;
  o["connected"] = reading.connected; o["raw"] = reading.raw; o["voltage"] = reading.voltage;
  if (isfinite(r.value)) o["value"] = r.value; else o["value"] = nullptr;
  o["unit"] = "ppm"; o["quality"] = r.quality;
  o["measurement_valid"] = r.valid; o["below_reference_range"] = r.below;
  o["calibration_mode"] = co2Mode(index); o["calibration_source"] = co2Source(index);
  o["provisional"] = bool(c.provisional);
  if (detailed) {
    o["model"] = "SEN0159"; o["adc_voltage"] = reading.voltage;
    o["module_voltage"] = r.moduleVoltage; o["estimated_module_voltage"] = r.moduleVoltage;
    o["sensor_voltage"] = r.sensorVoltage;
    o["zero_point_v"] = c.zero_point_v; o["reaction_voltage_v"] = c.reaction_voltage_v;
    o["dc_gain"] = SEN0159_DC_GAIN; o["divider_ratio"] = CO2_DIVIDER_RATIO;
  }
}
String co2Json(uint8_t index, AnalogReading reading) {
  JsonDocument doc;
  writeCo2Reading(doc.to<JsonObject>(), index, reading, false);
  String text; serializeJson(doc, text);
  return String(index == 0 ? "\"co2_1\":" : "\"co2_2\":") + text;
}
void writeO2Reading(JsonObject o, uint8_t index, bool detailed) {
  const auto &r = o2Readings[index];
  o["expected"] = index == 0 ? EXPECT_O2_1 : EXPECT_O2_2;
  o["connected"] = r.connected; o["i2c_detected"] = r.i2cDetected;
  o["warming_up"] = r.warming; o["stabilizing"] = r.stabilizing;
  o["measurement_valid"] = r.valid;
  if (isfinite(r.value)) o["value"] = r.value; else o["value"] = nullptr;
  o["unit"] = "%vol"; o["quality"] = r.quality;
  if (detailed) {
    o["expected_address"] = diagnosticAddressText(index == 0 ? O2_ADDR_1 : O2_ADDR_2);
    o["sampled_at_ms"] = r.sampledAt; o["observed"] = r.observed;
    if (isfinite(r.raw)) o["raw_value"] = r.raw; else o["raw_value"] = nullptr;
    if (isfinite(r.filtered)) o["filtered_value"] = r.filtered; else o["filtered_value"] = nullptr;
    o["wire_error"] = r.wireError; o["received_bytes"] = r.readCount;
    JsonArray range = o["nominal_range"].to<JsonArray>(); range.add(0); range.add(25);
    o["measurement_limit"] = 30; o["resolution_vol"] = 0.15;
    o["filter_samples"] = index == 0 ? o2Filter1.count : o2Filter2.count;
    o["filter_target_samples"] = O2_FILTER_SAMPLES;
    o["calibration_source"] = o2CalibrationSource(index);
    o["gain"] = o2Cal[index].gain; o["offset"] = o2Cal[index].offset;
    o["calibration_command_count"] = o2Cal[index].commandCount;
    o["last_calibration_status"] = o2Cal[index].commandCount ? "command_sent" : "not_requested";
    o["last_reference_vol"] = o2Cal[index].reference;
    writeDiagnosticUtc(o["last_calibration_command_utc"], o2Cal[index].commandUtc);
  }
}
String o2Json(uint8_t index) {
  JsonDocument doc; writeO2Reading(doc.to<JsonObject>(), index, false);
  String text; serializeJson(doc, text);
  return String(index == 0 ? "\"o2_gas_1\":" : "\"o2_gas_2\":") + text;
}

// Keep incoming values typed and finite; never silently turn null/text into zero.
bool calibrationNumber(JsonVariantConst value) {
  return value.is<double>() && isfinite(value.as<double>());
}
void queueCalibrationAck(const char* sensor, const char* status) {
  JsonDocument doc;
  doc["schema_version"] = "1.0"; doc["type"] = "calibration_ack"; doc["deviceId"] = DEVICE_ID;
  doc["experiment_id"] = EXPERIMENT_ID; doc["sensor"] = sensor; doc["status"] = status;
  if (calibrationCommandId[0]) doc["commandId"] = calibrationCommandId; else doc["commandId"] = nullptr;
  doc["calibration_version"] = cal.version; doc["nvs_ok"] = calibrationStorageOk;
  writeDiagnosticUtc(doc["timestampUtc"], time(NULL) >= 1600000000 ? time(NULL) : 0);
  doc["physical_calibration_confirmed"] = false;
  pendingCalibrationAck = ""; serializeJson(doc, pendingCalibrationAck);
  calibrationAckAt = millis();
  Serial.println(pendingCalibrationAck);
}
bool setCo2Profile(JsonObjectConst cmd, uint8_t index) {
  Co2Calibration candidate = co2Cal[index];
  const bool legacy = !cmd["a"].isNull() || !cmd["b"].isNull();
  const bool vendor = !cmd["zero_point_v"].isNull() || !cmd["reaction_voltage_v"].isNull();
  if (legacy && vendor) return false;
  if (legacy) {
    if (cmd.containsKey("a")) { if (!calibrationNumber(cmd["a"])) return false; candidate.a = cmd["a"]; }
    if (cmd.containsKey("b")) { if (!calibrationNumber(cmd["b"])) return false; candidate.b = cmd["b"]; }
    candidate.mode = 1; candidate.user = 1; candidate.provisional = 0;
  } else if (vendor) {
    if (cmd.containsKey("zero_point_v")) {
      if (!calibrationNumber(cmd["zero_point_v"])) return false;
      candidate.zero_point_v = cmd["zero_point_v"];
    }
    if (cmd.containsKey("reaction_voltage_v")) {
      if (!calibrationNumber(cmd["reaction_voltage_v"])) return false;
      candidate.reaction_voltage_v = cmd["reaction_voltage_v"];
    }
    candidate.mode = 0; candidate.user = 1; candidate.provisional = 0;
  } else if (!cmd.containsKey("enabled")) return false;
  candidate.enabled = true;
  if (cmd.containsKey("enabled")) {
    if (!cmd["enabled"].is<bool>()) return false;
    candidate.enabled = cmd["enabled"].as<bool>();
  }
  if (!validCo2Calibration(candidate)) return false;
  co2Cal[index] = candidate; return true;
}
bool handleCalibrationCommand(JsonObjectConst cmd) {
  const char* action = cmd["action"] | "";
  const char* sensor = cmd["sensor"] | "";
  const bool set = strcmp(action, "set") == 0;
  const bool reset = strcmp(action, "reset") == 0;
  const bool resetAll = strcmp(action, "reset_all") == 0;
  if (!set && !reset && !resetAll) return false;
  if (calibrationFutureSchema || o2CalibrationPending) {
    queueCalibrationAck(sensor, "rejected_busy_or_future_schema"); return true;
  }
  // Roll back the whole command when one of its fields is invalid.
  const Calibration prior = cal;
  const Co2Calibration c0 = co2Cal[0], c1 = co2Cal[1];
  const O2Calibration o0 = o2Cal[0], o1 = o2Cal[1];
  bool ok = true;
  if (resetAll) loadCalibrationDefaults();
  else if (strcmp(sensor, "co2") == 0 || strcmp(sensor, "co2_1") == 0 || strcmp(sensor, "co2_2") == 0) {
    const uint8_t first = strcmp(sensor, "co2_2") == 0 ? 1 : 0;
    const uint8_t last = strcmp(sensor, "co2_1") == 0 ? 0 : 1;
    for (uint8_t i = first; i <= last; ++i) {
      if (reset) co2Cal[i] = Co2Calibration();
      else if (!setCo2Profile(cmd, i)) ok = false;
    }
    if (strcmp(sensor, "co2") == 0 && ok) {
      cal.co2_a = co2Cal[0].a; cal.co2_b = co2Cal[0].b;
      cal.co2_enabled = co2Cal[0].mode == 1 && co2Cal[0].enabled;
    }
  } else if (strcmp(sensor, "o2_gas_1") == 0 || strcmp(sensor, "o2_gas_2") == 0) {
    const uint8_t i = strcmp(sensor, "o2_gas_1") == 0 ? 0 : 1;
    if (reset) { o2Cal[i].gain = 1; o2Cal[i].offset = 0; }
    else {
      if (!cmd.containsKey("gain") && !cmd.containsKey("offset")) ok = false;
      if (cmd.containsKey("gain")) {
        if (!calibrationNumber(cmd["gain"]) || cmd["gain"].as<float>() <= 0) ok = false;
        else o2Cal[i].gain = cmd["gain"];
      }
      if (cmd.containsKey("offset")) {
        if (!calibrationNumber(cmd["offset"])) ok = false; else o2Cal[i].offset = cmd["offset"];
      }
      if (!isfinite(o2Cal[i].gain) || !isfinite(o2Cal[i].offset)) ok = false;
    }
  } else {
    float *m = nullptr, *b = nullptr;
    float defaultM = 0, defaultB = 0;
    if (strcmp(sensor, "ph") == 0) { m = &cal.ph_m; b = &cal.ph_b; defaultM = PH_CAL_M_DEFAULT; defaultB = PH_CAL_B_DEFAULT; }
    else if (strcmp(sensor, "turb") == 0) { m = &cal.turb_m; b = &cal.turb_b; defaultM = TURB_CAL_M_DEFAULT; defaultB = TURB_CAL_B_DEFAULT; }
    else if (strcmp(sensor, "do") == 0) { m = &cal.do_m; defaultM = DO_CAL_M_DEFAULT; }
    else if (strcmp(sensor, "temp") == 0) { m = &cal.temp_m; b = &cal.temp_b; defaultM = TEMP_CAL_M_DEFAULT; defaultB = TEMP_CAL_B_DEFAULT; }
    else ok = false;
    if (ok && reset) { *m = defaultM; if (b) *b = defaultB; }
    if (ok && set) {
      if (!cmd.containsKey("m") && (!b || !cmd.containsKey("b"))) ok = false;
      if (cmd.containsKey("m")) { if (!calibrationNumber(cmd["m"])) ok = false; else *m = cmd["m"]; }
      if (b && cmd.containsKey("b")) { if (!calibrationNumber(cmd["b"])) ok = false; else *b = cmd["b"]; }
      if (!isfinite(*m) || (b && !isfinite(*b))) ok = false;
    }
    if (ok && strcmp(sensor, "ph") == 0) cal.ph_user = set;
    if (ok && strcmp(sensor, "do") == 0) cal.do_user = set;
  }
  if (!ok) {
    cal = prior; co2Cal[0] = c0; co2Cal[1] = c1; o2Cal[0] = o0; o2Cal[1] = o1;
    queueCalibrationAck(sensor, "rejected_invalid_parameters"); return true;
  }
  if (resetAll) {
    // Reset host corrections, not the module's internal calibration.
    o2Cal[0].reference = o0.reference; o2Cal[0].commandUtc = o0.commandUtc; o2Cal[0].commandCount = o0.commandCount;
    o2Cal[1].reference = o1.reference; o2Cal[1].commandUtc = o1.commandUtc; o2Cal[1].commandCount = o1.commandCount;
    resetO2Filters();
  } else if (strncmp(sensor, "o2_gas_", 7) == 0) {
    const uint8_t i = strcmp(sensor, "o2_gas_1") == 0 ? 0 : 1;
    (i == 0 ? o2Filter1 : o2Filter2).clear();
    o2Readings[i] = O2Reading();
  }
  cal.version++; saveCalibrationToNVS();
  queueCalibrationAck(resetAll ? "all" : sensor, calibrationStorageOk ? "saved" : "applied_ram_nvs_failed");
  return true;
}
void requestO2Calibration(JsonObjectConst cmd) {
  const char* sensor = cmd["sensor"] | "";
  const int index = strcmp(sensor, "o2_gas_1") == 0 ? 0 : strcmp(sensor, "o2_gas_2") == 0 ? 1 : -1;
  if (index < 0 || !calibrationNumber(cmd["reference_vol"]) ||
      fabsf(cmd["reference_vol"].as<float>() - 20.9f) > 0.0001f) {
    queueCalibrationAck(sensor, "rejected_reference_use_20.9"); return;
  }
  if (o2IsWarmingUp() || o2CalibrationPending || diagnosticRequestPending || diagnosticPhase != DIAG_IDLE ||
      calibrationFutureSchema) {
    queueCalibrationAck(sensor, "rejected_warming_up_or_busy"); return;
  }
  pendingO2Index = index; o2CalibrationPending = true;
}
void processO2Calibration() {
  if (!o2CalibrationPending) return;
  o2CalibrationPending = false;
  const uint8_t i = pendingO2Index;
  const char* name = i == 0 ? "o2_gas_1" : "o2_gas_2";
  const uint8_t channel = i == 0 ? TCA_CH_O2_1 : TCA_CH_O2_2;
  const uint8_t address = i == 0 ? O2_ADDR_1 : O2_ADDR_2;
  const char* status = "communication_fault";
  DFRobot_OxygenSensor &driver = i == 0 ? o2Sensor1 : o2Sensor2;
  if (!o2IsWarmingUp() && tcaSelectDetailed(channel).selected && probeI2C(address).detected && driver.begin(address)) {
    driver.calibrate(20.9f); // void API: ACK of write does NOT prove calibration.
    status = driver.communicationOk() ? "command_sent" : "communication_fault";
    if (driver.communicationOk()) {
      o2Cal[i].reference = 20.9f;
      o2Cal[i].commandUtc = time(NULL) >= 1600000000 ? time(NULL) : 0;
      o2Cal[i].commandCount++; cal.version++; saveCalibrationToNVS();
    }
  }
  (i == 0 ? o2Filter1 : o2Filter2).clear();
  o2Readings[i] = O2Reading();
  const auto disabled = tcaDisableAll();
  diagnosticBusDirty = true;
  strlcpy(calibrationCommandId, pendingO2CommandId, sizeof(calibrationCommandId));
  queueCalibrationAck(name, status);
  // Add cleanup evidence without claiming physical repair.
  JsonDocument ack; deserializeJson(ack, pendingCalibrationAck);
  ack["channels_disabled"] = disabled.selected; ack["disable_error"] = disabled.errorCode;
  ack["reference_vol"] = 20.9;
  pendingCalibrationAck = ""; serializeJson(ack, pendingCalibrationAck);
}
void publishCalibrationAck() {
  if (!pendingCalibrationAck.length()) return;
  if (millis() - calibrationAckAt > 60000UL) {
    Serial.println("Calibration ack expired without MQTT delivery."); pendingCalibrationAck = ""; return;
  }
  if (!mqttClient.connected()) return;
  if (7 + strlen(telemetryTopic) + pendingCalibrationAck.length() > MQTT_PACKET_SIZE) {
    Serial.println("Calibration ack too large."); pendingCalibrationAck = ""; return;
  }
  if (mqttClient.publish(telemetryTopic, pendingCalibrationAck.c_str())) pendingCalibrationAck = "";
}

// ===================== CALLBACK MQTT (Cloud-to-Device) =====================
// Recibe mensajes desde Azure y actualiza la calibración en runtime.
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("C2D recibido en topic: ");
  Serial.println(topic);

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.print("Error parseando JSON C2D: ");
    Serial.println(err.c_str());
    return;
  }

  const char* action = doc["action"] | "";
  Serial.print("Acción: ");
  Serial.println(action);

  if (strcmp(action, "diagnostics") == 0) {
    // Copiar solo el scope a un enum. No conservar punteros al buffer MQTT.
    requestDiagnostics(doc["scope"] | "full");
    return;
  }
  if (strcmp(action, "control") == 0 || strcmp(action, "all_off") == 0) {
    serviceActuators(WiFi.status() == WL_CONNECTED && mqttClient.connected());
    handleActuatorCommand(doc.as<JsonObjectConst>());
    return;
  }
  strlcpy(calibrationCommandId, doc["commandId"] | "", sizeof(calibrationCommandId));
  if (strcmp(action, "calibrate") == 0) {
    const bool wasPending = o2CalibrationPending;
    requestO2Calibration(doc.as<JsonObjectConst>());
    if (!wasPending && o2CalibrationPending)
      strlcpy(pendingO2CommandId, calibrationCommandId, sizeof(pendingO2CommandId));
    return;
  }
  if (handleCalibrationCommand(doc.as<JsonObjectConst>())) return;
  if (strcmp(action, "reboot") == 0) {
    // Reiniciar desde loop(): PubSubClient envia el PUBACK al volver de este callback.
    // Reiniciar aqui deja el C2D sin confirmar; IoT Hub lo reentrega => bucle de reinicios.
    Serial.println("Reboot solicitado por C2D");
    stopActuators("reboot");
    rebootRequested = true;
    rebootRequestedAt = millis();
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
    // Sin suscripcion la UI no puede controlar el equipo: reconectar en el siguiente ciclo.
    Serial.println("Error suscribiendo al topic C2D; reconectando.");
    mqttClient.disconnect();
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
  const unsigned long started = millis();
  while (now < 1600000000) {
    if (millis() - started >= 60000UL) {
      Serial.println(" NTP no disponible; reinicio con salidas apagadas.");
      stopActuators("ntp_timeout");
      ESP.restart();
    }
    delay(500); Serial.print("."); now = time(NULL);
  }
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
  // System properties (not JSON fields): tell IoT Hub to route the body as JSON.
  char propertyBuffer[80];
  az_iot_message_properties properties;
  if (az_result_failed(az_iot_message_properties_init(
          &properties, AZ_SPAN_FROM_BUFFER(propertyBuffer), 0))) {
    Serial.println("Error: az_iot_message_properties_init.");
    while (true) delay(1000);
  }
  if (az_result_failed(az_iot_message_properties_append(
          &properties, AZ_SPAN_FROM_STR(AZ_IOT_MESSAGE_PROPERTIES_CONTENT_TYPE),
          AZ_SPAN_FROM_STR("application%2Fjson")))) {
    Serial.println("Error: az_iot_message_properties_append Content-Type.");
    while (true) delay(1000);
  }
  if (az_result_failed(az_iot_message_properties_append(
          &properties, AZ_SPAN_FROM_STR(AZ_IOT_MESSAGE_PROPERTIES_CONTENT_ENCODING),
          AZ_SPAN_FROM_STR("utf-8")))) {
    Serial.println("Error: az_iot_message_properties_append Content-Encoding.");
    while (true) delay(1000);
  }
  if (az_result_failed(az_iot_hub_client_get_client_id(
          &hubClient, mqttClientId, sizeof(mqttClientId), NULL))) {
    Serial.println("Error: az_iot_hub_client_get_client_id.");
    while (true) delay(1000);
  }
  if (az_result_failed(az_iot_hub_client_get_user_name(
          &hubClient, mqttUsername, sizeof(mqttUsername), NULL))) {
    Serial.println("Error: az_iot_hub_client_get_user_name.");
    while (true) delay(1000);
  }
  if (az_result_failed(az_iot_hub_client_telemetry_get_publish_topic(
          &hubClient, &properties, telemetryTopic, sizeof(telemetryTopic), NULL))) {
    Serial.println("Error: az_iot_hub_client_telemetry_get_publish_topic.");
    while (true) delay(1000);
  }
  Serial.print("Telemetry MQTT topic: ");
  Serial.println(telemetryTopic);

  mqttClient.setServer(IOT_HUB_HOSTNAME, 8883);
  if (!mqttClient.setBufferSize(MQTT_PACKET_SIZE)) {
    Serial.println("Sin memoria para el buffer MQTT.");
    while (true) delay(1000);
  }
  mqttClient.setSocketTimeout(5);
  mqttClient.setCallback(mqttCallback);
}

void connectToAzure() {
  if (WiFi.status() != WL_CONNECTED) {
    // El driver ya reintenta solo; reconnect() aborta un intento en curso.
    static unsigned long lastWifiKick = 0;
    if (millis() - lastWifiKick >= 30000UL) {
      lastWifiKick = millis();
      WiFi.reconnect();
    }
    return;
  }
  if (sasTokenObj.Generate(60) != 0) {
    Serial.println("Error generando SAS Token.");
    return;
  }
  az_span span = sasTokenObj.Get();
  if (az_span_size(span) <= 0 || size_t(az_span_size(span)) >= sizeof(sasToken)) {
    Serial.println("SAS token excede el buffer.");
    return;
  }
  memcpy(sasToken, (char*)az_span_ptr(span), az_span_size(span));
  sasToken[az_span_size(span)] = 0;

  Serial.print("Conectando MQTT a Azure IoT Hub... ");
  if (mqttClient.connect(mqttClientId, mqttUsername, sasToken)) {
    Serial.println("OK");
    subscribeToC2D();
  } else {
    Serial.print("Fallo RC=");
    Serial.println(mqttClient.state());
    char tlsError[128];
    sslClient.lastError(tlsError, sizeof(tlsError));
    Serial.println(tlsError);
  }
}

// ===================== TELEMETRÍA =====================
String buildTelemetryJson() {
  const uint64_t nowMs = esp_timer_get_time() / 1000ULL; // 64 bits: millis() vuelve a 0 a los 49.7 dias
  String timestampUtc = utcTimestampIso8601();
  String alerts = "";

  AnalogReading ph   = readAnalogMux(CH_PH);
  AnalogReading co21 = readAnalogMux(CH_CO2_1);
  AnalogReading co22 = readAnalogMux(CH_CO2_2);
  AnalogReading turb = readAnalogMux(CH_TURB);
  AnalogReading dor  = readAnalogMux(CH_DO);
  AnalogReading tds  = readAnalogMux(CH_TDS);
  updateAnalogDiagnostics();

  bool tempConnected;
  float tempRaw = readRawTemperatureC(tempConnected);
  float tempCal = tempConnected ? calibrateTemperature(tempRaw) : NAN;

  bool bh1Connected, bh2Connected;
  float lux1 = readBh1750Lux(TCA_CH_BH1750_1, BH1750_ADDR_1, bh1Connected);
  float lux2 = readBh1750Lux(TCA_CH_BH1750_2, BH1750_ADDR_2, bh2Connected);

  // El valor queda en o2Readings[]; o2Json() lo serializa.
  bool o2_1_connected = false, o2_2_connected = false;
  bool warming = o2IsWarmingUp();
  readO2Percent(TCA_CH_O2_1, O2_ADDR_1, o2_1_connected);
  readO2Percent(TCA_CH_O2_2, O2_ADDR_2, o2_2_connected);

  TcsReading tcs1 = readTcs3200(TCS_OUT_1);
  TcsReading tcs2 = readTcs3200(TCS_OUT_2);

  // CO2 se evalua dentro de co2Json(); no recalcular aqui.
  float phCal     = ph.connected   ? calculatePh(ph.voltage)              : NAN;
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
  appendAlert(alerts, EXPECT_O2_1  && !o2_1_connected, "o2_gas_1");
  appendAlert(alerts, EXPECT_O2_2  && !o2_2_connected, "o2_gas_2");
  appendQualityAlert(alerts, EXPECT_O2_1 && o2_1_connected && !warming && !o2Readings[0].valid, "o2_gas_1_invalid_measurement");
  appendQualityAlert(alerts, EXPECT_O2_2 && o2_2_connected && !warming && !o2Readings[1].valid, "o2_gas_2_invalid_measurement");
  appendAlert(alerts, EXPECT_TCS1  && !tcs1.connected, "color_1");
  appendAlert(alerts, EXPECT_TCS2  && !tcs2.connected, "color_2");

  String json = "{\"schema_version\":\"1.0\",";
  json.reserve(6144); // ~4-6 KB: una sola reserva evita ~200 realloc y fragmentacion
  json += "\"deviceId\":\"" DEVICE_ID "\",";
  json += "\"experiment_id\":" + jsonStringValue(EXPERIMENT_ID) + ",";
  json += "\"timestampUtc\":";
  json += (timestampUtc.length() > 0) ? "\"" + timestampUtc + "\"" : "null";
  json += ",";
  json += "\"uptimeMs\":" + String(nowMs) + ",";
  json += "\"wifiRssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"status\":\"" + String(alerts.length() == 0 ? "ok" : "warning") + "\",";
  json += "\"alerts\":[" + alerts + "],";
  json += calibrationJson() + ",";
  // Revisar expiraciones despues de las lecturas (algunas son bloqueantes).
  serviceActuators(WiFi.status() == WL_CONNECTED && mqttClient.connected());
  json += "\"actuators\":" + actuatorsJson() + ",";
  json += "\"diagnostics\":" + quickDiagnosticsJson() + ",";

  json += "\"sensors\":{";
  json += analogJson("ph", "pH", ph, phCal, EXPECT_PH) + ",";
  json += co2Json(0, co21) + ",";
  json += co2Json(1, co22) + ",";
  json += analogJson("turbidity", "NTU", turb, turbCal, EXPECT_TURB) + ",";

  json += "\"dissolved_oxygen\":{";
  json += "\"expected\":" + String(EXPECT_DO ? "true" : "false") + ",";
  json += "\"connected\":" + String(dor.connected ? "true" : "false") + ",";
  json += "\"raw\":" + String(dor.raw) + ",";
  json += "\"voltage\":" + String(dor.voltage, 4) + ",";
  json += "\"saturation_pct\":" + jsonNumberOrNull(doSatPct, 2) + ",";
  json += "\"value\":" + jsonNumberOrNull(doMgL, 3) + ",";
  json += "\"unit\":\"mg/L\",";
  json += "\"quality\":\"" + String(sensorQuality(dor.connected, isfinite(doMgL))) + "\"";
  json += "},";

  json += analogJson("tds", "ppm", tds, tdsCal, EXPECT_TDS) + ",";

  json += "\"temperature\":{";
  json += "\"expected\":" + String(EXPECT_TEMP ? "true" : "false") + ",";
  json += "\"connected\":" + String(tempConnected ? "true" : "false") + ",";
  json += "\"raw\":" + jsonNumberOrNull(tempRaw, 2) + ",";
  json += "\"value\":" + jsonNumberOrNull(tempCal, 2) + ",";
  json += "\"unit\":\"C\",";
  json += "\"quality\":\"" + String(sensorQuality(tempConnected, isfinite(tempCal))) + "\"},";

  json += "\"light_1\":{";
  json += "\"expected\":" + String(EXPECT_BH1 ? "true" : "false") + ",";
  json += "\"connected\":" + String(bh1Connected ? "true" : "false") + ",";
  json += "\"value\":" + jsonNumberOrNull(lux1, 2) + ",\"unit\":\"lux\",";
  json += "\"quality\":\"" + String(sensorQuality(bh1Connected, isfinite(lux1))) + "\"},";

  json += "\"light_2\":{";
  json += "\"expected\":" + String(EXPECT_BH2 ? "true" : "false") + ",";
  json += "\"connected\":" + String(bh2Connected ? "true" : "false") + ",";
  json += "\"value\":" + jsonNumberOrNull(lux2, 2) + ",\"unit\":\"lux\",";
  json += "\"quality\":\"" + String(sensorQuality(bh2Connected, isfinite(lux2))) + "\"},";

  json += o2Json(0) + ",";
  json += o2Json(1) + ",";

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
  if (5 + 2 + strlen(telemetryTopic) + payload.length() > MQTT_PACKET_SIZE) {
    Serial.println("Telemetria excede el buffer MQTT; no enviada.");
    return;
  }
  bool ok = mqttClient.publish(telemetryTopic, payload.c_str());
  Serial.println(ok ? "Telemetría enviada." : "Error publicando telemetría.");
}

// Publicar fuera del callback: PubSubClient reutiliza el buffer de recepcion.
void publishActuatorReport() {
  if (!mqttClient.connected() || !actuatorReportPending()) return;
  static unsigned long lastAttempt = 0;
  if (millis() - lastAttempt < 1000UL) return;
  lastAttempt = millis();
  String payload = "{\"schema_version\":\"1.0\",\"deviceId\":\"" DEVICE_ID "\",";
  payload += "\"experiment_id\":" + jsonStringValue(EXPERIMENT_ID) + ",";
  payload += "\"type\":\"actuator_state\",";
  const String timestampUtc = utcTimestampIso8601();
  payload += "\"timestampUtc\":";
  payload += (timestampUtc.length() > 0) ? "\"" + timestampUtc + "\"" : "null";
  payload += ",";
  payload += "\"actuators\":" + actuatorsJson() + "}";
  if (5 + 2 + strlen(telemetryTopic) + payload.length() > MQTT_PACKET_SIZE) {
    Serial.println("Reporte de actuadores excede el buffer MQTT; no enviado.");
    return;
  }
  if (mqttClient.publish(telemetryTopic, payload.c_str())) actuatorReportSent();
}


// ===================== DIAGNOSTICO DE HARDWARE =====================
const char* i2cErrorName(uint8_t code) {
  // Wire 3.3.11 retorna 0, 2, 4, 5. Los nombres 1/3 son convenciones Wire;
  // no se generan artificialmente ni se asume que ESP32 distinga todos los NACK.
  switch (code) {
    case 0: return "ok";
    case 1: return "data_too_long";
    case 2: return "address_nack";
    case 3: return "data_nack";
    case 4: return "other_error";
    case 5: return "timeout";
    default: return "unknown";
  }
}

String diagnosticAddressText(uint8_t address) {
  char text[5];
  snprintf(text, sizeof(text), "0x%02X", address);
  return String(text);
}

void recordDiagnosticHistory(DiagnosticHistory &h, bool success) {
  const time_t now = time(NULL);
  const time_t utc = now >= 1600000000 ? now : 0;
  if (success) {
    h.success++; h.consecutiveFailures = 0;
    h.lastSuccessMs = millis(); h.lastSuccessUtc = utc;
  } else {
    h.failure++; h.consecutiveFailures++;
    h.lastFailureMs = millis(); h.lastFailureUtc = utc;
  }
}

I2CProbeResult probeI2C(uint8_t address) {
  Wire.beginTransmission(address);
  I2CProbeResult result;
  result.address = address;
  result.errorCode = Wire.endTransmission();
  result.detected = result.errorCode == 0;
  if (address == TCA_ADDR) {
    lastTcaProbe = result; lastTcaObserved = true; lastTcaProbeAt = millis();
    recordDiagnosticHistory(tcaProbeHistory, result.detected);
    diagnosticHistory[0] = tcaProbeHistory; // legacy alias now PROBE ONLY
  }
  return result;
}

TcaSelectResult tcaSelectDetailed(uint8_t channel) {
  TcaSelectResult result;
  result.channel = channel;
  if (channel > 7) return result; // no transaccion: error JSON null, no codigo inventado
  result.attempted = true;
  Wire.beginTransmission(TCA_ADDR);
  Wire.write(1 << channel);
  result.errorCode = Wire.endTransmission();
  result.selected = result.errorCode == 0;
  recordDiagnosticHistory(tcaSelectHistory, result.selected);
  return result;
}

TcaSelectResult tcaDisableAll() {
  TcaSelectResult result;
  result.attempted = true;
  Wire.beginTransmission(TCA_ADDR);
  Wire.write(uint8_t(0x00));
  result.errorCode = Wire.endTransmission();
  result.selected = result.errorCode == 0;
  recordDiagnosticHistory(tcaDisableHistory, result.selected);
  return result;
}

bool prepareI2CReading(uint8_t index, uint8_t channel, uint8_t address) {
  I2CReadingDiagnostic &d = i2cReadingDiagnostics[index];
  d = I2CReadingDiagnostic();
  d.observed = true;
  d.probe.address = address;
  d.selection = tcaSelectDetailed(channel);
  if (!d.selection.selected) {
    d.status = "tca_channel_select_failed";
    // No contabilizar un fallo del sensor que ni siquiera pudo probarse.
    return false;
  }
  return true;
}

bool probeI2CReading(uint8_t index, uint8_t address) {
  I2CReadingDiagnostic &d = i2cReadingDiagnostics[index];
  d.probeAttempted = true;
  d.probe = probeI2C(address);
  if (!d.probe.detected) {
    d.status = "expected_device_missing";
    recordDiagnosticHistory(diagnosticHistory[1 + index], false);
    return false;
  }
  return true;
}

void finishI2CReading(uint8_t index, bool success, const char* status) {
  i2cReadingDiagnostics[index].status = status;
  recordDiagnosticHistory(diagnosticHistory[1 + index], success);
}

void updateAnalogDiagnostics() {
  analogBatchValid = true;
  analogMinAverage = 4095; analogMaxAverage = 0;
  analogAllNearZero = true;
  for (uint8_t i = 0; i < 6; ++i) {
    analogBatchValid = analogBatchValid && analogDiagnostics[i].sampled;
    analogMinAverage = fminf(analogMinAverage, analogDiagnostics[i].rawAvg);
    analogMaxAverage = fmaxf(analogMaxAverage, analogDiagnostics[i].rawAvg);
    analogAllNearZero = analogAllNearZero && analogDiagnostics[i].rawAvg <= 5;
  }
  analogAllNearZero = analogBatchValid && analogAllNearZero;
  analogTooSimilar = analogBatchValid &&
      analogMaxAverage - analogMinAverage <= ANALOG_SIMILAR_SPREAD;
  if (analogBatchValid) {
    if (analogAllNearZero) analogAllZeroCount++;
    recordDiagnosticHistory(diagnosticHistory[5], !analogAllNearZero && !analogTooSimilar);
  }
}

void writeDiagnosticUtc(JsonVariant target, time_t value) {
  if (value == 0) { target.set(nullptr); return; }
  struct tm utc;
  char text[21];
  gmtime_r(&value, &utc);
  strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
  target.set(String(text));
}

void writeDiagnosticSystem(JsonObject object) {
  object["uptime_ms"] = millis();
  object["free_heap"] = ESP.getFreeHeap();
  object["min_free_heap"] = ESP.getMinFreeHeap();
  object["max_alloc_heap"] = ESP.getMaxAllocHeap();
  object["reset_reason"] = resetReasonName(esp_reset_reason());
  object["wifi_connected"] = WiFi.status() == WL_CONNECTED;
  object["wifi_rssi"] = WiFi.RSSI();
  object["mqtt_connected"] = mqttClient.connected();
  object["mqtt_state"] = mqttClient.state();
  object["time_synced"] = time(NULL) >= 1600000000;
}

bool diagnosticColorPulseOk(uint8_t index) {
  return diagnosticColorSampled[index] && diagnosticColor[index].connected;
}

bool diagnosticColorAllZero(uint8_t index) {
  const TcsReading &c = diagnosticColor[index];
  return diagnosticColorSampled[index] && c.rPulse == 0 && c.gPulse == 0 && c.bPulse == 0;
}

void writeQuickDiagnostics(JsonObject object) {
  // Dos transacciones cortas, nunca un barrido de direcciones.
  const TcaSelectResult isolated = tcaDisableAll();
  const I2CProbeResult tca = probeI2C(TCA_ADDR);
  diagnosticBusDirty = true; // el escaner restaura su segmento despues de la telemetria normal
  bool warning = !tca.detected || !isolated.selected || analogAllNearZero || analogTooSimilar;
  bool incomplete = !analogBatchValid || !diagnosticColorSampled[0] ||
                    !diagnosticColorSampled[1] || !diagnosticTempSampled;
  for (uint8_t i = 0; i < 4; ++i) {

    const auto &d = i2cReadingDiagnostics[i];
    if (!d.observed) incomplete = true;
    else if (strcmp(d.status, "measurement_ok") != 0 && strcmp(d.status, "warming_up") != 0) warning = true;
  }
  for (uint8_t i = 0; i < 2; ++i)
    if (diagnosticColorSampled[i] && !diagnosticColorPulseOk(i)) warning = true;
  if (diagnosticTempSampled && !diagnosticTempDetected) warning = true;
  if (!mqttClient.connected() || WiFi.status() != WL_CONNECTED || time(NULL) < 1600000000)
    warning = true;
  object["health"] = warning ? "warning" : incomplete ? "unknown" : "ok";
  JsonObject bus = object["i2c"].to<JsonObject>();
  bus["tca_address"] = diagnosticAddressText(TCA_ADDR);
  bus["tca_detected"] = tca.detected;
  bus["last_error"] = tca.errorCode;
  bus["last_error_name"] = i2cErrorName(tca.errorCode);
  bus["main_bus_isolated"] = isolated.selected;
  bus["consecutive_failures"] = diagnosticHistory[0].consecutiveFailures;
  JsonObject analog = object["analog_mux"].to<JsonObject>();
  analog["sampled"] = analogBatchValid;
  analog["all_near_zero"] = analogAllNearZero;
  analog["channels_too_similar"] = analogTooSimilar;
  analog["all_zero_count"] = analogAllZeroCount;
  analog["sampled_at_ms"] = analogDiagnostics[5].sampledAt;
  JsonObject color = object["color"].to<JsonObject>();
  for (uint8_t i = 0; i < 2; ++i) {
    const char* key = i == 0 ? "color_1_pulse_ok" : "color_2_pulse_ok";
    if (diagnosticColorSampled[i]) color[key] = diagnosticColorPulseOk(i);
    else color[key] = nullptr;
  }
  color["sampled_at_ms"] = max(diagnosticColorAt[0], diagnosticColorAt[1]);
  JsonObject system = object["system"].to<JsonObject>();
  system["free_heap"] = ESP.getFreeHeap();
  // Tendencia en Cosmos DB durante operacion larga: minimo historico y mayor bloque libre.
  system["min_free_heap"] = ESP.getMinFreeHeap();
  system["max_alloc_heap"] = ESP.getMaxAllocHeap();
  system["reset_reason"] = resetReasonName(esp_reset_reason());
  system["mqtt_connected"] = mqttClient.connected();
}

String quickDiagnosticsJson() {
  JsonDocument doc;
  writeQuickDiagnostics(doc.to<JsonObject>());
  String json;
  serializeJson(doc, json);
  return json;
}

const char* diagnosticScopeName(uint8_t scope) {
  switch (scope) {
    case DIAG_FULL: return "full";
    case DIAG_I2C: return "i2c";
    case DIAG_ANALOG: return "analog";
    case DIAG_COLOR: return "color";
    case DIAG_SYSTEM: return "system";
    case DIAG_QUICK: return "quick";
    case DIAG_RECOVER: return "i2c_recover";
    default: return "unknown";
  }
}

bool requestDiagnostics(const char* scope) {
  if (diagnosticRequestPending || diagnosticPhase != DIAG_IDLE || o2CalibrationPending) {
    Serial.println("Diagnostics busy: solicitud rechazada; reintentar al finalizar.");
    return false;
  }
  uint8_t selected = 255;
  for (uint8_t i = DIAG_FULL; i <= DIAG_RECOVER; ++i)
    if (strcmp(scope, diagnosticScopeName(i)) == 0) selected = i;
  if (selected == 255) {
    Serial.println("Diagnostics: scope desconocido."); return false;
  }
  diagnosticScope = selected;
  diagnosticRequestPending = true;
  Serial.printf("Diagnostics encolado: %s\n", diagnosticScopeName(selected));
  return true;
}

void pollDiagnosticSerial() {
  static char input[160];
  static size_t length = 0;
  static bool overflow = false;
  // Limitar el trabajo por vuelta; un emisor Serial no debe monopolizar loop.
  for (uint8_t budget = 0; budget < 32 && Serial.available(); ++budget) {
    const char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (!overflow && length) {
        input[length] = 0;
        JsonDocument doc;
        if (!deserializeJson(doc, input) && strcmp(doc["action"] | "", "diagnostics") == 0)
          requestDiagnostics(doc["scope"] | "full");
        else Serial.println("Serial: enviar JSON action=diagnostics.");
      } else if (overflow) Serial.println("Serial: comando demasiado largo.");
      length = 0; overflow = false;
    } else if (length < sizeof(input) - 1) input[length++] = c;
    else overflow = true;
  }
}

void scanMainI2CBus() {
  diagnosticMainScan = I2CScanSegment();
  diagnosticIsolation = tcaDisableAll();
  diagnosticAddress = 1;
  diagnosticBusDirty = false;
  diagnosticPhase = DIAG_MAIN;
  Serial.printf("\n========== I2C MAIN BUS ==========\nSDA GPIO: %d\nSCL GPIO: %d\nExpected TCA: 0x%02X\n",
                I2C_SDA_PIN, I2C_SCL_PIN, TCA_ADDR);
}

void scanTcaChannels() {
  diagnosticChannel = 0;
  diagnosticPhase = DIAG_SELECT;
  Serial.println("\n========== PCA9548A SCAN ==========");
}

void diagnosticNextGroup() {
  if (diagnosticScope == DIAG_FULL || diagnosticScope == DIAG_ANALOG) {
    diagnosticIndex = 0; diagnosticPhase = DIAG_ANALOG_READ;
  } else if (diagnosticScope == DIAG_COLOR) {
    diagnosticIndex = 0; diagnosticPhase = DIAG_COLOR_READ;
  } else {
    diagnosticCompletedAt = millis(); diagnosticPhase = DIAG_READY;
  }
}

uint8_t diagnosticExpectedAddress(uint8_t channel) {
  if (channel == TCA_CH_BH1750_1) return BH1750_ADDR_1;
  if (channel == TCA_CH_BH1750_2) return BH1750_ADDR_2;
  if (channel == TCA_CH_O2_1) return O2_ADDR_1;
  if (channel == TCA_CH_O2_2) return O2_ADDR_2;
  return 0;
}

void processDiagnostics() {
  // Cada vuelta realiza como maximo un probe I2C o una lectura de sensor.
  if (diagnosticRequestPending && diagnosticPhase == DIAG_IDLE) {
    diagnosticRequestPending = false;
    diagnosticPhase = DIAG_BEGIN;
  }
  switch (diagnosticPhase) {
    case DIAG_IDLE: case DIAG_READY: return;
    case DIAG_BEGIN: {
      diagnosticStartedAt = millis();
      const uint32_t warmupElapsed = diagnosticStartedAt - bootMillis;
      diagnosticWarmup = o2IsWarmingUp();
      diagnosticWarmupRemaining = diagnosticWarmup ?
          O2_WARMUP_MS - warmupElapsed : 0;
      diagnosticSerialPrinted = false;
      diagnosticReportSequence++;
      pendingDiagnosticPayload = "";
      for (auto &scan : diagnosticChannelScans) scan = I2CScanSegment();
      Serial.printf("\n==================================================\nBIOIOT HARDWARE DIAGNOSTIC #%lu (%s)\n",
                    (unsigned long)diagnosticReportSequence, diagnosticScopeName(diagnosticScope));
      if (diagnosticScope == DIAG_RECOVER) {
        recoveryBefore = probeI2C(TCA_ADDR);
        Serial.printf("Recovery before: %u (%s)\n", recoveryBefore.errorCode, i2cErrorName(recoveryBefore.errorCode));
        recoveryEndOk = Wire.end();
        recoveryBeginOk = Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
        // Registrar y continuar con un barrido completo; no se acciona RESET fisico.
        diagnosticIsolation = tcaDisableAll();
        recoveryAfter = probeI2C(TCA_ADDR);
      }
      if (diagnosticScope == DIAG_FULL || diagnosticScope == DIAG_I2C || diagnosticScope == DIAG_RECOVER)
        scanMainI2CBus();
      else diagnosticNextGroup();
      return;
    }
    case DIAG_MAIN: {
      if (diagnosticBusDirty) {
        const auto isolated = tcaDisableAll();
        if (!isolated.selected) diagnosticIsolation = isolated;
        diagnosticBusDirty = false;
      }
      I2CProbeResult probe = probeI2C(diagnosticAddress);
      diagnosticMainScan.tested[diagnosticAddress] = true;
      diagnosticMainScan.codes[diagnosticAddress] = probe.errorCode;
      if (probe.detected) Serial.printf("0x%02X -> ACK\n", diagnosticAddress);
      if (diagnosticAddress == TCA_ADDR) {
        diagnosticTca = probe;
        if (!probe.detected) Serial.printf("0x%02X -> %u (%s)\n", TCA_ADDR, probe.errorCode, i2cErrorName(probe.errorCode));
      }
      if (++diagnosticAddress > 0x7E) {
        diagnosticMainScan.completed = true;
        Serial.printf("TCA9548A: %s\n", diagnosticTca.detected ? "DETECTED" : "NOT DETECTED");
        scanTcaChannels();
      }
      return;
    }
    case DIAG_SELECT: {
      if (diagnosticChannel >= 8) {
        diagnosticCleanup = tcaDisableAll();
        diagnosticNextGroup();
        return;
      }
      auto &scan = diagnosticChannelScans[diagnosticChannel];
      Serial.printf("\nCH%u\n", diagnosticChannel);
      if (!diagnosticTca.detected) {
        Serial.println("not_testable: PCA no ACK; no se acusa al sensor.");
        diagnosticChannel++; return;
      }
      scan.selection = tcaSelectDetailed(diagnosticChannel);
      Serial.printf("TCA select: %s; Wire error: %u (%s)\n",
                    scan.selection.selected ? "OK" : "FAILED",
                    scan.selection.errorCode, i2cErrorName(scan.selection.errorCode));
      if (!scan.selection.selected) { diagnosticChannel++; return; }
      diagnosticAddress = 1;
      diagnosticBusDirty = false;
      diagnosticPhase = DIAG_CHANNEL;
      return;
    }
    case DIAG_CHANNEL: {
      auto &scan = diagnosticChannelScans[diagnosticChannel];
      if (diagnosticBusDirty) {
        const auto restored = tcaSelectDetailed(diagnosticChannel);
        diagnosticBusDirty = false;
        if (!restored.selected) {
          scan.selection = restored; diagnosticChannel++;
          diagnosticPhase = DIAG_SELECT; return;
        }
      }
      const auto probe = probeI2C(diagnosticAddress);
      scan.tested[diagnosticAddress] = true;
      scan.codes[diagnosticAddress] = probe.errorCode;
      const bool upstream = diagnosticMainScan.tested[diagnosticAddress] &&
          diagnosticMainScan.codes[diagnosticAddress] == 0;
      if (probe.detected && !upstream) Serial.printf("  0x%02X -> ACK\n", diagnosticAddress);
      if (++diagnosticAddress > 0x7E) {
        scan.completed = true;
        bool foundAny = false;
        for (uint8_t a = 1; a <= 0x7E; ++a)
          if (scan.tested[a] && scan.codes[a] == 0 &&
              (!diagnosticMainScan.tested[a] || diagnosticMainScan.codes[a] != 0)) foundAny = true;
        if (!foundAny) Serial.println("Found: none (excluyendo bus principal)");
        const uint8_t expected = diagnosticExpectedAddress(diagnosticChannel);
        if (expected) {
          Serial.printf("Expected: 0x%02X; %s\n", expected, scan.warmupSkipped ?
              "warming_up (not tested)" : scan.tested[expected] && scan.codes[expected] == 0 ? "ACK (ver atribucion en JSON)" : "MISSING");
        }
        diagnosticChannel++;
        diagnosticPhase = DIAG_SELECT;
      }
      return;
    }
    case DIAG_ANALOG_READ:
      readAnalogMux(diagnosticIndex);
      if (++diagnosticIndex >= 6) {
        updateAnalogDiagnostics();
        if (diagnosticScope == DIAG_FULL) { diagnosticIndex = 0; diagnosticPhase = DIAG_COLOR_READ; }
        else { diagnosticCompletedAt = millis(); diagnosticPhase = DIAG_READY; }
      }
      return;
    case DIAG_COLOR_READ:
      readTcs3200(diagnosticIndex == 0 ? TCS_OUT_1 : TCS_OUT_2);
      if (++diagnosticIndex >= 2) {
        if (diagnosticScope == DIAG_FULL) diagnosticPhase = DIAG_TEMP_READ;
        else { diagnosticCompletedAt = millis(); diagnosticPhase = DIAG_READY; }
      }
      return;
    case DIAG_O2_READ: {
      bool connected;
      readO2Percent(diagnosticIndex == 0 ? TCA_CH_O2_1 : TCA_CH_O2_2,
          diagnosticIndex == 0 ? O2_ADDR_1 : O2_ADDR_2, connected);
      if (++diagnosticIndex >= 2) {
        diagnosticCleanup = tcaDisableAll();
        diagnosticCompletedAt = millis(); diagnosticPhase = DIAG_READY;
      }
      return;
    }
    case DIAG_TEMP_READ: {
      bool connected;
      readRawTemperatureC(connected);
      diagnosticIndex = 0; diagnosticPhase = DIAG_O2_READ;
      return;
    }
  }
}

void writeDiagnosticHistory(JsonObject object, const DiagnosticHistory &h) {
  object["success_count"] = h.success; object["failure_count"] = h.failure;
  object["consecutive_failures"] = h.consecutiveFailures;
  if (h.success) object["last_success_uptime_ms"] = h.lastSuccessMs;
  else object["last_success_uptime_ms"] = nullptr;
  if (h.failure) object["last_failure_uptime_ms"] = h.lastFailureMs;
  else object["last_failure_uptime_ms"] = nullptr;
  writeDiagnosticUtc(object["last_success_utc"], h.lastSuccessUtc);
  writeDiagnosticUtc(object["last_failure_utc"], h.lastFailureUtc);
}

void writeDiagnosticHistories(JsonObject object) {
  object["tca_probe_success"] = tcaProbeHistory.success;
  object["tca_probe_failure"] = tcaProbeHistory.failure;
  object["tca_select_success"] = tcaSelectHistory.success;
  object["tca_select_failure"] = tcaSelectHistory.failure;
  object["tca_disable_success"] = tcaDisableHistory.success;
  object["tca_disable_failure"] = tcaDisableHistory.failure;
  writeDiagnosticHistory(object["tca_probe"].to<JsonObject>(), tcaProbeHistory);
  writeDiagnosticHistory(object["tca_select"].to<JsonObject>(), tcaSelectHistory);
  writeDiagnosticHistory(object["tca_disable"].to<JsonObject>(), tcaDisableHistory);
  object["analog_mux_all_zero_count"] = analogAllZeroCount;
  JsonObject devices = object["devices"].to<JsonObject>();
  for (uint8_t i = 0; i < 9; ++i) {
    const String name = diagnosticDeviceNames[i];
    object[name + "_success_count"] = diagnosticHistory[i].success;
    object[name + "_failure_count"] = diagnosticHistory[i].failure;
    writeDiagnosticHistory(devices[name].to<JsonObject>(), diagnosticHistory[i]);
  }
}

void writeScanDevices(JsonArray array, const I2CScanSegment &scan, bool downstreamOnly) {
  for (uint8_t a = 1; a <= 0x7E; ++a) {
    if (scan.tested[a] && scan.codes[a] == 0 &&
        (!downstreamOnly || !diagnosticMainScan.tested[a] || diagnosticMainScan.codes[a] != 0))
      array.add(diagnosticAddressText(a));
  }
}

void writeScanErrors(JsonObject object, const I2CScanSegment &scan) {
  // Indice 0 = direccion 0x01; null = no probado. Conserva TODOS los codigos.
  object["probe_first_address"] = "0x01";
  JsonArray codes = object["probe_codes"].to<JsonArray>();
  for (uint8_t a = 1; a <= 0x7E; ++a) {
    if (scan.tested[a]) codes.add(scan.codes[a]); else codes.add(nullptr);
  }
}

void writeI2CReadingEvidence(JsonObject object) {
  const char* names[] = {"bh1750_1", "bh1750_2", "o2_1", "o2_2"};
  for (uint8_t i = 0; i < 4; ++i) {
    const auto &d = i2cReadingDiagnostics[i];
    JsonObject device = object[names[i]].to<JsonObject>();
    const bool warming = i >= 2 && o2IsWarmingUp();
    device["status"] = d.status;
    device["warming_up"] = warming;
    device["measurement_testable"] = !warming;
    device["testable"] = d.observed && d.selection.selected;
    device["probe_attempted"] = d.probeAttempted;
    if (d.selection.attempted) {
      device["select_error"] = d.selection.errorCode;
      device["select_error_name"] = i2cErrorName(d.selection.errorCode);
    } else device["select_error"] = nullptr;
    if (d.probeAttempted) {
      device["probe_error"] = d.probe.errorCode;
      device["probe_error_name"] = i2cErrorName(d.probe.errorCode);
    } else device["probe_error"] = nullptr;
  }
}

void writeDiagnosticI2C(JsonObject object, JsonArray findings) {
  JsonObject mainBus = object["main_bus"].to<JsonObject>();
  mainBus["sda_gpio"] = I2C_SDA_PIN; mainBus["scl_gpio"] = I2C_SCL_PIN;
  mainBus["isolated"] = diagnosticIsolation.selected;
  mainBus["isolation_error"] = diagnosticIsolation.errorCode;
  mainBus["isolation_error_name"] = i2cErrorName(diagnosticIsolation.errorCode);
  writeScanDevices(mainBus["devices"].to<JsonArray>(), diagnosticMainScan, false);
  writeScanErrors(mainBus, diagnosticMainScan);
  if (!diagnosticIsolation.selected) findings.add("main_bus_isolation_failed");
  bool busErrors = false;
  for (uint8_t a = 1; a <= 0x7E; ++a)
    if (diagnosticMainScan.tested[a] && diagnosticMainScan.codes[a] != 0 &&
        diagnosticMainScan.codes[a] != 2) busErrors = true;
  if (busErrors) findings.add("main_i2c_bus_errors");
  JsonObject tca = object["tca9548a"].to<JsonObject>();
  tca["expected_address"] = diagnosticAddressText(TCA_ADDR);
  tca["detected"] = diagnosticTca.detected;
  tca["probe_error"] = diagnosticTca.errorCode;
  tca["probe_error_name"] = i2cErrorName(diagnosticTca.errorCode);
  tca["i2c_downstream_status"] = diagnosticTca.detected ? "see_channels" : "not_testable";
  tca["channels_disabled"] = diagnosticCleanup.selected;
  tca["disable_error"] = diagnosticCleanup.errorCode;
  tca["disable_error_name"] = i2cErrorName(diagnosticCleanup.errorCode);
  if (!diagnosticTca.detected) findings.add("tca9548a_not_detected");
  if (!diagnosticCleanup.selected) findings.add("tca_disable_failed");
  JsonObject channels = tca["channels"].to<JsonObject>();
  unsigned missing = 0;
  for (uint8_t i = 0; i < 8; ++i) {
    const auto &scan = diagnosticChannelScans[i];
    JsonObject channel = channels[String(i)].to<JsonObject>();
    const uint8_t expected = diagnosticExpectedAddress(i);
    const bool warming = o2IsWarmingUp() && (i == TCA_CH_O2_1 || i == TCA_CH_O2_2);
    channel["warming_up"] = warming;
    channel["measurement_testable"] = !warming;
    channel["channel"] = i;
    channel["select_attempted"] = scan.selection.attempted;
    channel["select_ok"] = scan.selection.selected;
    if (scan.selection.attempted) {
      channel["select_error"] = scan.selection.errorCode;
      channel["select_error_name"] = i2cErrorName(scan.selection.errorCode);
    } else { channel["select_error"] = nullptr; channel["select_error_name"] = nullptr; }
    if (expected) channel["expected"] = diagnosticAddressText(expected);
    else channel["expected"] = nullptr;
    channel["scan_complete"] = scan.completed;
    writeScanDevices(channel["devices"].to<JsonArray>(), scan, true);
    // Direcciones del bus principal no prueban un dispositivo downstream.
    const bool ambiguous = !diagnosticIsolation.selected ||
        (expected && diagnosticMainScan.tested[expected] && diagnosticMainScan.codes[expected] == 0);
    const bool testable = diagnosticTca.detected && scan.selection.selected &&
        scan.completed && !ambiguous;
    channel["testable"] = testable;
    if (expected && testable) {
      const bool found = scan.tested[expected] && scan.codes[expected] == 0;
      channel["expected_found"] = found;
      channel["expected_probe_error"] = scan.codes[expected];
      channel["expected_probe_error_name"] = i2cErrorName(scan.codes[expected]);
      channel["status"] = found ? "expected_device_found" : "expected_device_missing";
      if (!found) { missing++; findings.add("tca_ch" + String(i) + "_expected_device_missing"); }
    } else {
      channel["expected_found"] = nullptr;
      channel["expected_probe_error"] = nullptr;
      channel["status"] = !diagnosticTca.detected ? "not_testable" :
          !scan.selection.selected ? "tca_channel_select_failed" :
          ambiguous ? "ambiguous_upstream_address" :
          channel["devices"].as<JsonArray>().size() ? "unexpected_device_found" : "channel_empty";
    }
    unsigned unexpected = 0;
    for (uint8_t a = 1; a <= 0x7E; ++a)
      if (scan.tested[a] && scan.codes[a] == 0 && a != expected &&
          (!diagnosticMainScan.tested[a] || diagnosticMainScan.codes[a] != 0)) unexpected++;
    channel["unexpected_device_found"] = unexpected > 0;
    channel["expected_address"] = channel["expected"];
    // Include upstream replies too; 0x72 conflicts with the mux itself.
    JsonArray detectedAddresses = channel["detected_addresses"].to<JsonArray>();
    // Candidate O2 addresses, including the upstream collision at 0x72.
    for (uint8_t a = 0x70; a <= 0x73; ++a)
      if (scan.tested[a] && scan.codes[a] == 0) detectedAddresses.add(diagnosticAddressText(a));
    bool alternate = false;
    for (uint8_t a = 0x70; a <= 0x72; ++a)
      if (scan.tested[a] && scan.codes[a] == 0 &&
          (!diagnosticMainScan.tested[a] || diagnosticMainScan.codes[a] != 0)) alternate = true;
    const bool o2Channel = i == TCA_CH_O2_1 || i == TCA_CH_O2_2;
    channel["address_mismatch"] = o2Channel && testable && !channel["expected_found"].as<bool>() && alternate;
    channel["address_0x72_conflicts_with_tca"] = o2Channel;
    if (channel["address_mismatch"].as<bool>()) findings.add("tca_ch" + String(i) + "_address_mismatch");
    if (unexpected) findings.add("tca_ch" + String(i) + "_unexpected_device_found");
    if (diagnosticTca.detected && scan.selection.attempted && !scan.selection.selected)
      findings.add("tca_ch" + String(i) + "_select_failed");
    if (diagnosticTca.detected && ambiguous) findings.add("tca_ch" + String(i) + "_attribution_uncertain");
    if (warming) {
      channel["reason"] = "warming_up";
      channel["remaining_ms_at_start"] = diagnosticWarmupRemaining;
      const uint32_t elapsed = millis() - bootMillis;
      channel["remaining_ms"] = elapsed < O2_WARMUP_MS ? O2_WARMUP_MS - elapsed : 0;
    }
    channel["i2c_downstream_status"] = channel["status"];
    writeScanErrors(channel, scan);
  }
  if (missing >= 4) findings.add("multiple_tca_channels_missing_devices");
  writeI2CReadingEvidence(object["last_measurement_attempts"].to<JsonObject>());
  if (diagnosticScope == DIAG_RECOVER) {
    JsonObject recovery = object["recovery"].to<JsonObject>();
    recovery["wire_end_ok"] = recoveryEndOk; recovery["wire_begin_ok"] = recoveryBeginOk;
    recovery["before"]["detected"] = recoveryBefore.detected;
    recovery["before"]["wire_error"] = recoveryBefore.errorCode;
    recovery["before"]["wire_error_name"] = i2cErrorName(recoveryBefore.errorCode);
    recovery["after"]["detected"] = recoveryAfter.detected;
    recovery["after"]["wire_error"] = recoveryAfter.errorCode;
    recovery["after"]["wire_error_name"] = i2cErrorName(recoveryAfter.errorCode);
    recovery["physical_repair_confirmed"] = false;
    if (!recoveryEndOk || !recoveryBeginOk) findings.add("i2c_reinitialization_failed");
  }
}

void writeDiagnosticAnalog(JsonObject object, JsonArray findings) {
  object["sig_gpio"] = MUX_SIG_PIN;
  object["enable_wiring_configured"] = "GND (not measured)";
  object["all_near_zero"] = analogAllNearZero;
  object["channels_too_similar"] = analogTooSimilar;
  object["similar_spread_threshold"] = ANALOG_SIMILAR_SPREAD;
  object["min_raw"] = analogMinAverage;
  object["max_raw"] = analogMaxAverage;
  object["spread"] = analogMaxAverage - analogMinAverage;
  object["suspected_fault"] = analogAllNearZero || analogTooSimilar;
  object["suspected_area"] = analogAllNearZero ?
      "mux_common_path_power_enable_or_sig" : analogTooSimilar ?
      "mux_channel_selection_common_path_or_similar_inputs" : "none";
  JsonObject channels = object["channels"].to<JsonObject>();
  for (uint8_t i = 0; i < 6; ++i) {
    const auto &a = analogDiagnostics[i];
    JsonObject channel = channels[String(i)].to<JsonObject>();
    channel["sensor"] = analogSensorNames[i]; channel["channel"] = i;
    channel["samples"] = a.samples;
    channel["raw_min"] = a.rawMin; channel["raw_max"] = a.rawMax;
    channel["raw_avg"] = a.rawAvg; channel["raw_span"] = a.rawMax - a.rawMin;
    channel["voltage_avg"] = a.voltageAvg;
    if (i == CH_CO2_1 || i == CH_CO2_2) {
      AnalogReading reading;
      reading.raw = int(a.rawAvg);
      reading.voltage = reading.raw * (3.3f / 4095.0f);
      reading.connected = !(reading.raw <= 5 || reading.raw >= 4090);
      writeCo2Reading(channel["measurement"].to<JsonObject>(), i == CH_CO2_1 ? 0 : 1, reading, true);
    }
    channel["sampled_at_ms"] = a.sampledAt;
    channel["connection_confidence"] = !a.sampled ? "unknown" :
        analogTooSimilar || a.rawAvg <= 5 || a.rawAvg >= 4090 ? "weak_evidence" : "plausible_signal";
  }
  if (analogAllNearZero) findings.add("analog_mux_common_path_suspected");
  if (analogTooSimilar) findings.add("analog_mux_channels_suspiciously_similar");
}

void writeDiagnosticColor(JsonObject object, JsonArray findings) {
  for (uint8_t i = 0; i < 2; ++i) {
    const auto &c = diagnosticColor[i];
    JsonObject color = object[i == 0 ? "color_1" : "color_2"].to<JsonObject>();
    color["out_gpio"] = i == 0 ? TCS_OUT_1 : TCS_OUT_2;
    color["sampled"] = diagnosticColorSampled[i];
    color["sampled_at_ms"] = diagnosticColorAt[i];
    color["rPulse"] = c.rPulse; color["gPulse"] = c.gPulse; color["bPulse"] = c.bPulse;
    color["r_timeout"] = c.rPulse == 0;
    color["g_timeout"] = c.gPulse == 0;
    color["b_timeout"] = c.bPulse == 0;
    color["all_pulses_zero"] = diagnosticColorAllZero(i);
    color["pulse_ok"] = diagnosticColorPulseOk(i);
    if (!diagnosticColorPulseOk(i)) findings.add(i == 0 ? "color_1_pulse_failure" : "color_2_pulse_failure");
  }
  const bool individual1 = diagnosticColorAllZero(0) && diagnosticColorPulseOk(1);
  const bool individual2 = diagnosticColorAllZero(1) && diagnosticColorPulseOk(0);
  object["shared_control_lines_likely_working"] = individual1 || individual2;
  object["suspected_area"] = individual1 ? "tcs1_power_oe_out_or_gpio34" :
      individual2 ? "tcs2_power_oe_out_or_gpio35" : "not_localized";
  if (individual1 || individual2) {
    findings.add(individual1 ? "color_1_individual_path_suspected" : "color_2_individual_path_suspected");
    findings.add("tcs_shared_control_lines_likely_working");
  }
}

void writeDiagnosticTemperature(JsonObject object, JsonArray findings) {
  object["gpio"] = ONE_WIRE_BUS; object["device_count"] = diagnosticTempCount;
  object["device_count_source"] = "DallasTemperature last bus enumeration";
  object["detected"] = diagnosticTempDetected;
  object["sampled_at_ms"] = diagnosticTempAt;
  if (isfinite(diagnosticTempRaw)) object["raw_temperature"] = diagnosticTempRaw;
  else object["raw_temperature"] = nullptr;
  if (!diagnosticTempDetected) findings.add("ds18b20_no_valid_reading");
}

void printDiagnosticSummary(JsonObjectConst report) {
  Serial.printf("\nSYSTEM\nWiFi: %s\nMQTT: %s\nRSSI: %d dBm\nFree heap: %lu\n",
      WiFi.status() == WL_CONNECTED ? "OK" : "OFFLINE",
      mqttClient.connected() ? "OK" : "OFFLINE", WiFi.RSSI(), (unsigned long)ESP.getFreeHeap());
  if (report["analog_mux"].is<JsonObjectConst>()) {
    Serial.println("\nANALOG MUX CD74HC4067");
    for (uint8_t i = 0; i < 6; ++i)
      Serial.printf("CH%u %-16s avg=%.2f min=%d max=%d span=%d\n", i, analogSensorNames[i],
          analogDiagnostics[i].rawAvg, analogDiagnostics[i].rawMin, analogDiagnostics[i].rawMax,
          analogDiagnostics[i].rawMax - analogDiagnostics[i].rawMin);
    if (analogAllNearZero)
      Serial.println("WARNING: todos cerca de cero. Sospecha: alimentacion/GND/EN/SIG/multiplexor; no confirmada.");
    if (analogTooSimilar)
      Serial.println("WARNING: canales muy similares; evidencia debil de conexion fisica.");
  }
  if (report["color"].is<JsonObjectConst>()) {
    Serial.println("\nTCS3200");
    for (uint8_t i = 0; i < 2; ++i)
      Serial.printf("Color%u GPIO%d: r=%lu g=%lu b=%lu %s\n", i + 1, i == 0 ? TCS_OUT_1 : TCS_OUT_2,
          diagnosticColor[i].rPulse, diagnosticColor[i].gPulse, diagnosticColor[i].bPulse,
          diagnosticColorPulseOk(i) ? "PULSES OK" : "MISSING PULSES");
    if ((diagnosticColorAllZero(0) && diagnosticColorPulseOk(1)) ||
        (diagnosticColorAllZero(1) && diagnosticColorPulseOk(0)))
      Serial.println("S0-S3 compartidos probablemente funcionan; sospechar el camino individual sin pulsos.");
  }
  if (report["temperature"].is<JsonObjectConst>())
    Serial.printf("\nDS18B20 GPIO%d: device_count=%u raw=%.2f detected=%s\n",
        ONE_WIRE_BUS, diagnosticTempCount, diagnosticTempRaw, diagnosticTempDetected ? "YES" : "NO");
  Serial.println("\nASSESSMENT (evidencia, no confirmacion de dano fisico)");
  for (JsonVariantConst finding : report["assessment"]["findings"].as<JsonArrayConst>())
    Serial.println(finding.as<const char*>());
  Serial.println("==================================================");
}

void publishDiagnosticReport() {
  if (diagnosticPhase != DIAG_READY) return;
  // Una espera de red no debe retener indefinidamente el diagnostico ni admitir otra solicitud.
  if (millis() - diagnosticCompletedAt > 60000UL) {
    Serial.println("Diagnostico: sin entrega MQTT tras 60 s; repetir solicitud.");
    pendingDiagnosticPayload = ""; diagnosticPhase = DIAG_IDLE; return;
  }
  if (!pendingDiagnosticPayload.length()) {
    if (ESP.getFreeHeap() < 90000UL) {
      Serial.println("Diagnostico: RAM insuficiente para construir informe completo.");
      pendingDiagnosticPayload = "{\"schema_version\":\"1.0\",\"type\":\"diagnostic_report\",\"deviceId\":\"" DEVICE_ID
          "\",\"error\":\"insufficient_heap\"}";
    } else {
      JsonDocument report;
      report["schema_version"] = "1.0"; report["type"] = "diagnostic_report";
      report["deviceId"] = DEVICE_ID; report["experiment_id"] = EXPERIMENT_ID;
      report["scope"] = diagnosticScopeName(diagnosticScope);
      report["report_id"] = diagnosticReportSequence;
      writeDiagnosticUtc(report["timestampUtc"], time(NULL) >= 1600000000 ? time(NULL) : 0);
      report["uptimeMs"] = millis();
      report["started_uptime_ms"] = diagnosticStartedAt;
      report["completed_uptime_ms"] = diagnosticCompletedAt;
      writeDiagnosticSystem(report["system"].to<JsonObject>());
      JsonObject assessment = report["assessment"].to<JsonObject>();
      JsonArray findings = assessment["findings"].to<JsonArray>();
      if (WiFi.status() != WL_CONNECTED) findings.add("wifi_disconnected");
      if (!mqttClient.connected()) findings.add("mqtt_disconnected");
      if (time(NULL) < 1600000000) findings.add("utc_not_synchronized");
      if (diagnosticScope == DIAG_FULL || diagnosticScope == DIAG_I2C || diagnosticScope == DIAG_RECOVER)
        writeDiagnosticI2C(report["i2c"].to<JsonObject>(), findings);
      if (diagnosticScope == DIAG_FULL || diagnosticScope == DIAG_ANALOG)
        writeDiagnosticAnalog(report["analog_mux"].to<JsonObject>(), findings);
      if (diagnosticScope == DIAG_FULL || diagnosticScope == DIAG_COLOR)
        writeDiagnosticColor(report["color"].to<JsonObject>(), findings);
      if (diagnosticScope == DIAG_FULL)
        writeDiagnosticTemperature(report["temperature"].to<JsonObject>(), findings);
      if (diagnosticScope == DIAG_QUICK)
        writeQuickDiagnostics(report["diagnostics"].to<JsonObject>());
      if (diagnosticScope == DIAG_FULL) {
        for (uint8_t i = 0; i < 2; ++i) {
          writeO2Reading(report[i == 0 ? "o2_gas_1" : "o2_gas_2"].to<JsonObject>(), i, true);
          if (!o2Readings[i].connected) findings.add(i == 0 ? "o2_gas_1_communication_fault" : "o2_gas_2_communication_fault");
          else if (!o2Readings[i].warming && !o2Readings[i].valid)
            findings.add(i == 0 ? "o2_gas_1_invalid_measurement" : "o2_gas_2_invalid_measurement");
        }
        JsonDocument calibration;
        deserializeJson(calibration, "{" + calibrationJson() + "}");
        report["calibration"] = calibration["calibration"];
      }
      writeDiagnosticHistories(report["history"].to<JsonObject>());
      assessment["severity"] = findings.size() ? "warning" : "ok";
      assessment["physical_fault_confirmed"] = false;
      if (diagnosticScope == DIAG_QUICK)
        assessment["severity"] = report["diagnostics"]["health"];
      printDiagnosticSummary(report.as<JsonObjectConst>());
      const size_t length = measureJson(report);
      Serial.printf("Diagnostic payload bytes: %u; MQTT buffer: %u\n", unsigned(length), MQTT_PACKET_SIZE);
      if (report.overflowed() || length + 7 + strlen(telemetryTopic) > MQTT_PACKET_SIZE) {
        Serial.println("Diagnostico excede memoria/paquete: no se envia un JSON truncado.");
        pendingDiagnosticPayload = "{\"schema_version\":\"1.0\",\"type\":\"diagnostic_report\",\"deviceId\":\"" DEVICE_ID
            "\",\"error\":\"diagnostic_report_too_large\"}";
      } else if (!pendingDiagnosticPayload.reserve(length + 1)) {
        pendingDiagnosticPayload = "{\"schema_version\":\"1.0\",\"type\":\"diagnostic_report\",\"deviceId\":\"" DEVICE_ID
            "\",\"error\":\"payload_allocation_failed\"}";
      } else serializeJson(report, pendingDiagnosticPayload);
      // report se libera antes del publish; no StaticJsonDocument gigante.
    }
  }
  if (!diagnosticSerialPrinted) {
    Serial.println(pendingDiagnosticPayload);
    diagnosticSerialPrinted = true;
  }
  serviceActuators(WiFi.status() == WL_CONNECTED && mqttClient.connected());
  if (!mqttClient.connected() || millis() - diagnosticPublishAttempt < 1000UL) return;
  diagnosticPublishAttempt = millis();
  if (5 + 2 + strlen(telemetryTopic) + pendingDiagnosticPayload.length() > MQTT_PACKET_SIZE) {
    Serial.println("Diagnostic report excede buffer MQTT.");
    pendingDiagnosticPayload = ""; diagnosticPhase = DIAG_IDLE; return;
  }
  if (mqttClient.publish(telemetryTopic, pendingDiagnosticPayload.c_str())) {
    Serial.println("Diagnostic report enviado (QoS 0).");
    pendingDiagnosticPayload = ""; diagnosticPhase = DIAG_IDLE;
  }
}

// ===================== SETUP / LOOP =====================
void setup() {
  Serial.begin(115200);
  initActuators();
  delay(1000);
  Serial.println("\n===== BIO-IOT ESP32 + AZURE (v4) =====");
  bootMillis = millis();

  loadCalibrationFromNVS();

  pinMode(MUX_S0_PIN, OUTPUT);
  pinMode(MUX_S1_PIN, OUTPUT);
  pinMode(MUX_S2_PIN, OUTPUT);
  pinMode(MUX_S3_PIN, OUTPUT);

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

  sslClient.setCACert(ca_pem);
  sslClient.setHandshakeTimeout(5);

  initWiFi();
  initTime();
  initAzureSDK();
  connectToAzure();

  // El core no vigila loop(). Un bloqueo (I2C, TLS, DNS) reinicia; las salidas
  // arrancan apagadas en initActuators(). Se conserva la vigilancia de IDLE0.
  const esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = LOOP_WDT_TIMEOUT_MS, .idle_core_mask = 1 << 0, .trigger_panic = true};
  if (esp_task_wdt_reconfigure(&wdtConfig) != ESP_OK || esp_task_wdt_add(NULL) != ESP_OK)
    Serial.println("Watchdog de loop no disponible.");
  Serial.printf("Ultimo reinicio: %s\n", resetReasonName(esp_reset_reason()));
}

void loop() {
  esp_task_wdt_reset();
  if (mqttClient.connected() && sasTokenObj.IsExpired(120)) {
    // Reconexion programada (~segundos): los actuadores siguen bajo su expiresAt y
    // el margen ACTUATOR_DISCONNECT_GRACE_MS; no se apagan.
    Serial.println("Renovando SAS token: reconexion MQTT.");
    mqttClient.disconnect();
  }
  serviceActuators(WiFi.status() == WL_CONNECTED && mqttClient.connected());
  if (!mqttClient.connected()) {
    static unsigned long lastConnectAttempt = 0;
    if (millis() - lastConnectAttempt >= 5000UL) {
      lastConnectAttempt = millis();
      connectToAzure();
    }
  }
  mqttClient.loop();  // procesa mensajes entrantes (C2D) y envia su PUBACK
  if (rebootRequested && millis() - rebootRequestedAt >= 1000UL) {
    mqttClient.disconnect();
    delay(100);
    ESP.restart();
  }
  serviceActuators(WiFi.status() == WL_CONNECTED && mqttClient.connected());
  publishActuatorReport();

  static unsigned long lastTelemetry = 0;
  if (millis() - lastTelemetry >= TELEMETRY_INTERVAL_MS) {
    lastTelemetry = millis(); // antes de leer: las lecturas (~2-3 s) no desplazan el periodo
    publishTelemetry();
  }
  serviceActuators(WiFi.status() == WL_CONNECTED && mqttClient.connected());
  pollDiagnosticSerial();
  processO2Calibration();
  publishCalibrationAck();
  processDiagnostics();
  publishDiagnosticReport();
  serviceActuators(WiFi.status() == WL_CONNECTED && mqttClient.connected());
}
