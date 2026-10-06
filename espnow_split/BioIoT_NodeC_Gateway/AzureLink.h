#pragma once
// Wi-Fi + NTP + Azure IoT Hub (MQTT/TLS, SAS) del gateway. Sin bucles indefinidos:
// cada llamada a loop() avanza una maquina de estados. La conexion TLS puede
// bloquear el loop() de Arduino unos segundos, pero los actuadores y ESP-NOW
// corren en una tarea local independiente.
//
// Las decisiones Wi-Fi (orden de credenciales, plazos, portal, reintentos y
// busquedas completas) estan en WifiPolicy (portable, probada en PC); aqui solo
// se ejecutan con WiFi/WiFiManager:
//  * Credenciales: primero las guardadas en NVS; si no hay, las de gateway_secrets.h.
//    El almacenamiento Wi-Fi del nucleo esta en RAM (WiFi.persistent(false)), asi que
//    ni los intentos con las predeterminadas ni los cambios de modo tocan la NVS: solo
//    una red elegida en el portal y conectada se guarda (commitPortalCredentials).
//  * Portal WIFI_PORTAL_SSID (192.168.4.1), no bloqueante, en AP+STA y en el canal
//    actual de ESP-NOW: la interfaz STA (la de ESP-NOW) nunca se apaga.
//  * Asociado: ESP-NOW adopta el canal del AP; A/B lo encuentran con channel_hunting.
//  * Nunca se imprime SSID, contrasena ni claves (WiFiManager sin depuracion).
#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include "WifiPolicy.h"

class AzureLink {
 public:
  using MessageHandler = void (*)(const uint8_t* payload, size_t len);

  void begin(MessageHandler handler, uint8_t espnowChannel, bool publishingAllowed = true);
  void loop();
  bool wifiConnected() const;
  bool mqttConnected();
  bool timeValid() const;
  int64_t utcMs() const;
  bool azureConfigured() const;
  bool azureEnabled() const { return azureReady_; }
  // Comprueba tamano del paquete antes de publicar (QoS 0: aceptado por la biblioteca
  // no implica recepcion en IoT Hub).
  bool publish(const char* payload, size_t len);
  int rssi() const;
  uint8_t wifiChannel() const;
  // Canal que debe usar ESP-NOW ahora (el del AP si esta asociado).
  uint8_t espnowChannel() const { return espnowChannel_; }
  bool takeChannelChange(uint8_t& ch);
  uint32_t channelChanges() const { return channelChanges_; }
  const char* stateName() const;
  const char* searchStateName() const { return policy_.searchStateName(); }
  bool msSinceConnected(uint32_t& ms) const { return policy_.msSinceConnected(millis(), ms); }
  bool portalOpen() const;
  uint32_t portalRemainingS() const { return portalOpen() ? policy_.portalRemainingMs(millis()) / 1000 : 0; }
  const char* credentialSource() const { return gateway::wifiCredName(policy_.activeCred()); }
  bool hasStoredCredentials() const { return policy_.hasStored(); }
  uint32_t nextFullScanInS() const { return policy_.nextFullInMs(millis()) / 1000; }
  // Serial {"action":"wifi_portal"}.
  void startPortal();
  const char* telemetryTopic() const;
  uint32_t publishOk = 0, publishFail = 0, publishTooLarge = 0, mqttConnects = 0, mqttFailures = 0;
  uint32_t wifiAttempts() const { return policy_.attempts(); }
  uint32_t fullScans() const { return policy_.fullScans(); }

 private:
  void apply(const gateway::WifiAction& a);
  void startConnect(gateway::WifiCred cred, bool fullScan);
  void onConnected();
  void openPortal(bool automatic);
  void closePortal();
  void commitPortalCredentials();
  void loadStoredCredentials();
  void connectMqtt();
  bool initAzure();
  void restoreEspNowChannel();
  gateway::LinkStatus readLink() const;

  MessageHandler handler_ = nullptr;
  gateway::WifiPolicy policy_;
  char storedSsid_[33] = {};
  char storedPass_[65] = {};
  uint8_t espnowChannel_ = 1, lastBssid_[6] = {};
  bool channelChanged_ = false, ntpStarted_ = false, azureReady_ = false;
  uint32_t channelChanges_ = 0;
  uint32_t lastMqttAttemptMs_ = 0;
};
