#include "AzureLink.h"

#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <az_core.h>
#include <az_iot.h>
#include <esp_wifi.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <new>

#include "AzureIoTSasToken.h"
#include "azure_ca.h"
#include "gateway_config.h"
#include "MemoryDiagnostics.h"

#if __has_include("iot_configs.h")
#include "iot_configs.h"
#else
#include "iot_configs.example.h"
#endif
#if __has_include("gateway_secrets.h")
#include "gateway_secrets.h"
#else
#include "gateway_secrets.example.h"
#endif

using gateway::LinkStatus;
using gateway::WifiAction;
using gateway::WifiCred;

namespace {
WiFiClientSecure sslClient;
PubSubClient mqttClient(sslClient);
az_iot_hub_client hubClient;
char mqttClientId[128];
char mqttUsername[256];
char telemetryTopicBuf[256];
char c2dTopic[128];
char sasToken[512];
char sasSignatureBuffer[512];
char sasBuffer[512];
AzIoTSasToken sasTokenObj(&hubClient, AZ_SPAN_FROM_STR(DEVICE_KEY), AZ_SPAN_FROM_BUFFER(sasSignatureBuffer),
                          AZ_SPAN_FROM_BUFFER(sasBuffer));
AzureLink::MessageHandler g_handler = nullptr;

// WiFiManager apaga la STA al abrir el portal si no esta asociada (pasa a solo AP, o
// detiene el Wi-Fi si estaba en STA). ESP-NOW usa la interfaz STA: aqui sigue activa.
class GatewayPortal : public WiFiManager {
 public:
  GatewayPortal() { _disableSTAConn = false; }
};
GatewayPortal* portal = nullptr;

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Solo se interpreta y encola; PubSubClient envia el PUBACK al volver (v4).
  if (g_handler) g_handler(payload, length);
}

uint8_t radioChannel() {
  uint8_t primary = 0;
  wifi_second_chan_t second;
  return esp_wifi_get_channel(&primary, &second) == ESP_OK ? primary : 0;
}

void copyField(char* dst, size_t cap, const uint8_t* src, size_t srcLen) {
  size_t n = 0;
  while (n < srcLen && n + 1 < cap && src[n]) {
    dst[n] = char(src[n]);
    ++n;
  }
  dst[n] = 0;
}
}  // namespace

bool AzureLink::azureConfigured() const {
#ifdef BIOIOT_IOT_PLACEHOLDER
  return false;
#else
  return true;
#endif
}

const char* AzureLink::telemetryTopic() const { return telemetryTopicBuf; }

const char* AzureLink::stateName() const { return gateway::wifiPhaseName(policy_.phase(), policy_.portalOpen()); }

bool AzureLink::portalOpen() const { return portal && portal->getConfigPortalActive(); }

bool AzureLink::initAzure() {
  az_iot_hub_client_options options = az_iot_hub_client_options_default();
  if (az_result_failed(az_iot_hub_client_init(&hubClient, AZ_SPAN_FROM_STR(IOT_HUB_HOSTNAME),
                                              AZ_SPAN_FROM_STR(DEVICE_ID), &options))) {
    Serial.println("Error inicializando Azure IoT Hub client.");
    return false;
  }
  // Propiedades de sistema identicas a v4: JSON UTF-8 hacia Cosmos DB.
  char propertyBuffer[80];
  az_iot_message_properties properties;
  if (az_result_failed(az_iot_message_properties_init(&properties, AZ_SPAN_FROM_BUFFER(propertyBuffer), 0)) ||
      az_result_failed(az_iot_message_properties_append(&properties, AZ_SPAN_FROM_STR(AZ_IOT_MESSAGE_PROPERTIES_CONTENT_TYPE),
                                                        AZ_SPAN_FROM_STR("application%2Fjson"))) ||
      az_result_failed(az_iot_message_properties_append(&properties, AZ_SPAN_FROM_STR(AZ_IOT_MESSAGE_PROPERTIES_CONTENT_ENCODING),
                                                        AZ_SPAN_FROM_STR("utf-8")))) {
    Serial.println("Error: propiedades de contenido MQTT.");
    return false;
  }
  if (az_result_failed(az_iot_hub_client_get_client_id(&hubClient, mqttClientId, sizeof(mqttClientId), NULL)) ||
      az_result_failed(az_iot_hub_client_get_user_name(&hubClient, mqttUsername, sizeof(mqttUsername), NULL)) ||
      az_result_failed(az_iot_hub_client_telemetry_get_publish_topic(&hubClient, &properties, telemetryTopicBuf,
                                                                     sizeof(telemetryTopicBuf), NULL))) {
    Serial.println("Error: identificadores/topic de Azure IoT Hub.");
    return false;
  }
  mqttClient.setServer(IOT_HUB_HOSTNAME, 8883);
  if (!mqttClient.setBufferSize(MQTT_PACKET_SIZE)) {
    Serial.println("Sin memoria para el buffer MQTT; Azure deshabilitado.");
    return false;
  }
  mqttClient.setSocketTimeout(5);
  mqttClient.setCallback(mqttCallback);
  // TLS con validacion de CA y nombre de host; nunca setInsecure().
  sslClient.setCACert(ca_pem);
  sslClient.setHandshakeTimeout(5);
  return true;
}

// Credenciales que WiFiManager guardo en NVS: el nucleo las cargo en esp_wifi_init()
// (NVS habilitada) antes de pasar el almacenamiento a RAM. Copia propia: un intento
// fallido desde el portal o con las predeterminadas no las sustituye.
void AzureLink::loadStoredCredentials() {
  wifi_config_t conf;
  memset(&conf, 0, sizeof(conf));
  storedSsid_[0] = storedPass_[0] = 0;
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK && conf.sta.ssid[0]) {
    copyField(storedSsid_, sizeof(storedSsid_), conf.sta.ssid, sizeof(conf.sta.ssid));
    copyField(storedPass_, sizeof(storedPass_), conf.sta.password, sizeof(conf.sta.password));
  }
  memset(&conf, 0, sizeof(conf));
}

void AzureLink::begin(MessageHandler handler, uint8_t espnowChannel, bool publishingAllowed) {
  handler_ = g_handler = handler;
  espnowChannel_ = espnowChannel >= 1 && espnowChannel <= 13 ? espnowChannel : 1;
  gw::logInternalHeap("before_mqtt_init");
  azureReady_ = publishingAllowed && azureConfigured() && initAzure();
  gw::logInternalHeap("after_mqtt_init");
  if (!publishingAllowed) Serial.println("Azure deshabilitado por memoria: sin buffer de publicacion; Wi-Fi y operacion local disponibles.");
  if (!azureConfigured()) Serial.println("Azure: iot_configs.h no configurado (plantilla); solo operacion local.");
  loadStoredCredentials();
  const bool hasStored = storedSsid_[0] != 0;
  const bool hasDefault = strlen(WIFI_SSID) > 0;
  Serial.printf("Wi-Fi: credenciales guardadas en NVS: %s; predeterminadas (gateway_secrets.h): %s. "
                "Canal %u solo como pista inicial.\n",
                hasStored ? "si" : "no", hasDefault ? "si" : "no", espnowChannel_);
  gateway::WifiTimings t;
  t.connectTimeoutMs = WIFI_CONNECT_TIMEOUT_MS;
  t.quickConnectTimeoutMs = WIFI_QUICK_CONNECT_TIMEOUT_MS;
  t.portalMs = uint32_t(WIFI_PORTAL_TIMEOUT_S) * 1000UL;
  t.lostQuickRetryMs = WIFI_LOST_QUICK_RETRY_MS;
  t.quickRetryMs = WIFI_QUICK_RETRY_MS;
  t.quickBeforeFull = WIFI_QUICK_BEFORE_FULL;
  t.firstFullAfterLossMs = WIFI_FIRST_FULL_SCAN_MS;
  t.fullMinMs = WIFI_FULL_SCAN_MIN_MS;
  t.fullMaxMs = WIFI_FULL_SCAN_MAX_MS;
  apply(policy_.begin(t, hasStored, hasDefault, millis()));
}

gateway::LinkStatus AzureLink::readLink() const {
  switch (WiFi.status()) {
    case WL_CONNECTED: return LinkStatus::Up;
    case WL_CONNECT_FAILED:
    case WL_NO_SSID_AVAIL: return LinkStatus::Failed;
    default: return LinkStatus::Pending;
  }
}

void AzureLink::apply(const WifiAction& a) {
  switch (a.kind) {
    case WifiAction::None: return;
    case WifiAction::Connect: startConnect(a.cred, a.fullScan); return;
    case WifiAction::AbortConnect:
      restoreEspNowChannel();
      Serial.printf("Wi-Fi no disponible: operacion local en el canal %u; proxima busqueda completa en %lu s.\n",
                    espnowChannel_, (unsigned long)(policy_.nextFullInMs(millis()) / 1000));
      return;
    case WifiAction::LinkLost:
      Serial.println("Wi-Fi perdido: operacion local en el mismo canal; reintentos rapidos y busqueda completa acotada.");
      if (mqttClient.connected()) mqttClient.disconnect();
      restoreEspNowChannel();
      return;
    case WifiAction::OpenPortal: openPortal(a.automatic); return;
    case WifiAction::ClosePortal: closePortal(); return;
    case WifiAction::Connected: onConnected(); return;
  }
}

void AzureLink::startConnect(WifiCred cred, bool fullScan) {
  const bool stored = cred == WifiCred::Stored;
  const char* ssid = stored ? storedSsid_ : WIFI_SSID;
  const char* pass = stored ? storedPass_ : WIFI_PASSWORD;
  if (fullScan) {
    // Sin canal ni BSSID: el SSID se busca en todos los canales (el hotspot pudo cambiar).
    Serial.printf("Wi-Fi: busqueda completa del SSID (%s); ESP-NOW puede interrumpirse unos segundos.\n",
                  stored ? "credenciales guardadas" : "credenciales predeterminadas");
    WiFi.begin(ssid, pass);
  } else {
    // Mismo canal y BSSID: sin barrido, ESP-NOW no cambia de canal.
    Serial.printf("Wi-Fi: reintento rapido en el canal %u con el BSSID conocido.\n", espnowChannel_);
    WiFi.begin(ssid, pass, espnowChannel_, lastBssid_, true);
  }
}

void AzureLink::restoreEspNowChannel() {
  WiFi.disconnect(false, false);  // sin borrar configuracion; almacenamiento en RAM
  if (radioChannel() != espnowChannel_) esp_wifi_set_channel(espnowChannel_, WIFI_SECOND_CHAN_NONE);
}

void AzureLink::onConnected() {
  const uint8_t ch = uint8_t(WiFi.channel());
  memcpy(lastBssid_, WiFi.BSSID(), 6);
  Serial.printf("Wi-Fi conectado (%s): canal %u, RSSI %d dBm.\n", gateway::wifiCredName(policy_.activeCred()), ch,
                WiFi.RSSI());
  if (ch >= 1 && ch <= 13 && ch != espnowChannel_) {
    // ESP-NOW opera en el canal adoptado por la STA; A/B lo encuentran con channel_hunting.
    Serial.printf("ESP-NOW pasa del canal %u al %u (canal del AP).\n", espnowChannel_, ch);
    espnowChannel_ = ch;
    channelChanged_ = true;
    channelChanges_++;
  }
  if (!ntpStarted_) {
    configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);  // no bloqueante; validez via timeValid()
    ntpStarted_ = true;
  }
}

bool AzureLink::takeChannelChange(uint8_t& ch) {
  if (!channelChanged_) return false;
  channelChanged_ = false;
  ch = espnowChannel_;
  return true;
}

void AzureLink::startPortal() {
  const WifiAction a = policy_.requestPortal(millis());
  if (a.kind == WifiAction::None)
    Serial.printf("Portal %s ya abierto (%lu s restantes).\n", WIFI_PORTAL_SSID, (unsigned long)portalRemainingS());
  apply(a);
}

void AzureLink::openPortal(bool automatic) {
  const bool connected = wifiConnected();
  if (!connected) restoreEspNowChannel();  // aborta un intento en curso y fija el canal
  if (!portal) {
    portal = new (std::nothrow) GatewayPortal();
    if (!portal) {
      Serial.println("Portal no disponible: sin memoria; operacion local conservada, sin reinicio.");
      gw::logInternalHeap("portal_allocation_failed");
      return;
    }
    portal->setDebugOutput(false);         // su depuracion imprime el SSID elegido
    portal->setConfigPortalBlocking(false);
    portal->setConfigPortalTimeout(0);     // los 180 s los cuenta WifiPolicy (un solo temporizador)
    portal->setConnectTimeout(WIFI_CONNECT_TIMEOUT_MS / 1000);
    portal->setSaveConnectTimeout(WIFI_CONNECT_TIMEOUT_MS / 1000);  // acota el bloqueo al guardar
  }
  // Mismo canal que ESP-NOW (asociado, manda el canal del AP).
  portal->setWiFiAPChannel(connected ? uint8_t(WiFi.channel()) : espnowChannel_);
  portal->startConfigPortal(WIFI_PORTAL_SSID);
  WiFi.enableSTA(true);  // AP+STA: la interfaz de ESP-NOW sigue activa
  if (!connected && radioChannel() != espnowChannel_) esp_wifi_set_channel(espnowChannel_, WIFI_SECOND_CHAN_NONE);
  Serial.printf("Portal '%s' %s: http://%s durante %u s, canal %u (no bloqueante; ESP-NOW sigue activo).\n",
                WIFI_PORTAL_SSID, portal->getConfigPortalActive() ? "abierto" : "NO se pudo abrir",
                WiFi.softAPIP().toString().c_str(), unsigned(WIFI_PORTAL_TIMEOUT_S), radioChannel());
  if (automatic) Serial.println("Portal automatico: la conexion inicial no se consiguio en el plazo acotado.");
}

void AzureLink::closePortal() {
  if (portal && portal->getConfigPortalActive()) portal->stopConfigPortal();
  WiFi.mode(WIFI_STA);
  if (!wifiConnected()) restoreEspNowChannel();
  Serial.printf("Portal cerrado a los %u s. Serial {\"action\":\"wifi_portal\"} lo reabre.\n",
                unsigned(WIFI_PORTAL_TIMEOUT_S));
}

// La red elegida en el portal ya conecto: se guarda en la NVS de Wi-Fi (la misma que
// usa WiFiManager) solo ahora, para que una contrasena erronea no sustituya a una buena.
void AzureLink::commitPortalCredentials() {
  wifi_config_t conf;
  memset(&conf, 0, sizeof(conf));
  bool ok = esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK && conf.sta.ssid[0];
  if (ok) {
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    ok = esp_wifi_set_config(WIFI_IF_STA, &conf) == ESP_OK;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    copyField(storedSsid_, sizeof(storedSsid_), conf.sta.ssid, sizeof(conf.sta.ssid));
    copyField(storedPass_, sizeof(storedPass_), conf.sta.password, sizeof(conf.sta.password));
  }
  memset(&conf, 0, sizeof(conf));
  WiFi.mode(WIFI_STA);  // WiFiManager ya cerro el AP
  Serial.printf("Red elegida en el portal: %s en NVS; se reutilizara tras reiniciar.\n",
                ok ? "guardada" : "NO guardada (error NVS)");
}

bool AzureLink::wifiConnected() const { return WiFi.status() == WL_CONNECTED; }
bool AzureLink::mqttConnected() { return mqttClient.connected(); }
bool AzureLink::timeValid() const { return time(nullptr) >= 1600000000; }
int64_t AzureLink::utcMs() const {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec < 1600000000) return 0;
  return int64_t(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}
int AzureLink::rssi() const { return wifiConnected() ? WiFi.RSSI() : 0; }
uint8_t AzureLink::wifiChannel() const { return wifiConnected() ? uint8_t(WiFi.channel()) : 0; }

void AzureLink::connectMqtt() {
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
  gw::logInternalHeap("before_mqtt_tls_connect");
  const bool connected = mqttClient.connect(mqttClientId, mqttUsername, sasToken);
  gw::logInternalHeap("after_mqtt_tls_connect");
  if (connected) {
    Serial.println("OK");
    mqttConnects++;
    snprintf(c2dTopic, sizeof(c2dTopic), "devices/%s/messages/devicebound/#", DEVICE_ID);
    if (!mqttClient.subscribe(c2dTopic, 1)) {
      Serial.println("Error suscribiendo al topic C2D; reconectando.");
      mqttClient.disconnect();
    }
  } else {
    mqttFailures++;
    Serial.printf("Fallo RC=%d\n", mqttClient.state());
    char tlsError[128];
    sslClient.lastError(tlsError, sizeof(tlsError));
    Serial.println(tlsError);
  }
}

void AzureLink::loop() {
  if (policy_.portalOpen() && portal && portal->getConfigPortalActive() && portal->process()) {
    commitPortalCredentials();  // WiFiManager guardo, conecto y cerro el portal
    apply(policy_.onPortalConnected(millis()));
  }
  apply(policy_.tick(millis(), readLink()));
  // Un intento fallido desde el portal deja la radio donde acabo su barrido.
  if (policy_.portalOpen() && !wifiConnected() && radioChannel() != espnowChannel_) restoreEspNowChannel();
  if (policy_.phase() != gateway::WifiPhase::Connected || !wifiConnected()) return;
  // Conectado: MQTT. Sin hora valida no hay TLS/SAS (nunca se inventa la hora).
  const uint32_t now = millis();
  if (!azureReady_ || !timeValid()) return;
  if (mqttClient.connected() && sasTokenObj.IsExpired(120)) {
    // Renovacion SAS: reconexion breve; los actuadores no dependen de esto.
    Serial.println("Renovando SAS token: reconexion MQTT.");
    mqttClient.disconnect();
  }
  if (!mqttClient.connected() && now - lastMqttAttemptMs_ >= 5000UL) {
    lastMqttAttemptMs_ = now;
    connectMqtt();
  }
  mqttClient.loop();
}

bool AzureLink::publish(const char* payload, size_t len) {
  if (!mqttClient.connected()) return false;
  if (5 + 2 + strlen(telemetryTopicBuf) + len > MQTT_PACKET_SIZE) {
    publishTooLarge++;
    Serial.println("Payload excede el buffer MQTT; no se envia JSON truncado.");
    return false;
  }
  const bool ok = mqttClient.publish(telemetryTopicBuf, reinterpret_cast<const uint8_t*>(payload), unsigned(len), false);
  if (ok) publishOk++; else publishFail++;
  return ok;
}
