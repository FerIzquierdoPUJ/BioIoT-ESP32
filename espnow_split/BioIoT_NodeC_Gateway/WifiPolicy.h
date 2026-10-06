#pragma once
// Politica Wi-Fi del gateway (portable, probada en PC). Solo C se asocia al router
// o al hotspot movil; su canal y BSSID pueden cambiar en cada reinicio del hotspot.
//  1. Arranque: credenciales guardadas en NVS (WiFiManager); si no hay, las
//     predeterminadas de gateway_secrets.h. Busqueda completa del SSID (sin BSSID).
//  2. Si ese primer intento acotado falla, o no hay credenciales, el portal se abre
//     solo (una vez por arranque). Tambien por Serial, en cualquier estado.
//  3. Portal: duracion fija desde su apertura y sin intentos STA propios mientras
//     esta abierto (un barrido moveria el canal del AP y de ESP-NOW).
//  4. Red elegida en el portal y conectada => pasa a ser la credencial guardada.
//  5. Perdida del enlace: pocos intentos rapidos en el canal/BSSID conocidos (sin
//     barrido; ESP-NOW no cambia de canal), y busqueda completa del SSID en todos
//     los canales a mas tardar firstFullAfterLossMs. Si falla, busquedas completas
//     espaciadas fullMin..fullMax (nunca continuas). Nunca pide reiniciar.
// El ejecutor (AzureLink) traduce cada WifiAction a llamadas WiFi/WiFiManager.
#include <stdint.h>

namespace gateway {

enum class WifiCred : uint8_t { None = 0, Stored = 1, Default = 2 };
enum class WifiPhase : uint8_t { NoCredentials = 0, Connecting = 1, Connected = 2, Backoff = 3 };
enum class LinkStatus : uint8_t { Pending = 0, Up = 1, Failed = 2 };

struct WifiTimings {
  uint32_t connectTimeoutMs = 15000;       // busqueda completa + asociacion + DHCP
  uint32_t quickConnectTimeoutMs = 8000;   // canal/BSSID conocidos
  uint32_t failGraceMs = 3000;             // ignora un estado de fallo anterior al intento
  uint32_t portalMs = 180000;
  uint32_t lostQuickRetryMs = 5000;        // primer intento rapido tras perder el enlace
  uint32_t quickRetryMs = 10000;           // entre intentos rapidos
  uint8_t quickBeforeFull = 2;             // intentos rapidos fallidos antes de buscar el SSID
  uint32_t firstFullAfterLossMs = 45000;   // limite para la primera busqueda completa
  uint32_t fullMinMs = 60000;              // espera tras una busqueda completa fallida
  uint32_t fullMaxMs = 600000;
};

struct WifiAction {
  enum Kind : uint8_t {
    None = 0,
    Connect,       // iniciar intento con `cred` (fullScan o canal/BSSID conocidos)
    AbortConnect,  // intento fallido: soltar STA y volver al canal de ESP-NOW
    LinkLost,      // se perdio la asociacion: cerrar MQTT y volver al canal de ESP-NOW
    OpenPortal,    // abrir el portal (abortando cualquier intento en curso)
    ClosePortal,   // vencio el portal: cerrarlo y volver a STA en el canal de ESP-NOW
    Connected,     // asociado: adoptar el canal del AP para ESP-NOW, BSSID, NTP
  };
  Kind kind = None;
  WifiCred cred = WifiCred::None;
  bool fullScan = true;
  bool automatic = false;  // OpenPortal abierto por la politica (no por Serial)
};

class WifiPolicy {
 public:
  WifiAction begin(const WifiTimings& t, bool hasStored, bool hasDefault, uint32_t now);
  // Un paso de la maquina: como mucho una accion.
  WifiAction tick(uint32_t now, LinkStatus link);
  // Serial {"action":"wifi_portal"}. None si ya estaba abierto.
  WifiAction requestPortal(uint32_t now);
  // El portal guardo una red y C quedo conectado con ella (WiFiManager ya cerro el portal).
  WifiAction onPortalConnected(uint32_t now);

  WifiPhase phase() const { return phase_; }
  bool portalOpen() const { return portalOpen_; }
  uint32_t portalRemainingMs(uint32_t now) const;
  WifiCred activeCred() const { return cred_; }
  bool hasStored() const { return hasStored_; }
  bool hasDefault() const { return hasDefault_; }
  bool attemptIsFull() const { return attemptFull_; }
  uint32_t attempts() const { return attempts_; }
  uint32_t fullScans() const { return fullScans_; }
  uint32_t portalOpens() const { return portalOpens_; }
  bool autoPortalUsed() const { return autoPortalUsed_; }
  // Tiempo hasta la proxima busqueda completa (0 si no hay ninguna programada o ya toca).
  uint32_t nextFullInMs(uint32_t now) const;
  // "connected", "quick_retry_known_channel", "full_scan", "waiting_backoff",
  // "portal", "no_credentials".
  const char* searchStateName() const;
  // Tiempo desde la ultima vez que el enlace estuvo arriba; false si nunca.
  bool msSinceConnected(uint32_t now, uint32_t& ms) const;

 private:
  WifiCred retryCred() const;
  WifiAction connect(WifiCred c, bool fullScan, uint32_t now);
  WifiAction openPortal(uint32_t now, bool automatic);
  void startOutage(uint32_t now, uint32_t firstQuickMs, uint32_t firstFullMs);
  void markUp(uint32_t now);

  WifiTimings t_;
  WifiPhase phase_ = WifiPhase::NoCredentials;
  WifiCred cred_ = WifiCred::None;
  bool hasStored_ = false, hasDefault_ = false;
  bool portalOpen_ = false, autoPortalUsed_ = false, everUp_ = false, haveBssid_ = false;
  bool attemptFull_ = false, fullSinceLoss_ = false;
  uint8_t quickFails_ = 0;
  uint32_t attemptStartMs_ = 0, portalOpenedMs_ = 0, lastUpMs_ = 0;
  uint32_t nextQuickMs_ = 0, nextFullMs_ = 0, fullBackoffMs_ = 0;
  uint32_t attempts_ = 0, fullScans_ = 0, portalOpens_ = 0;
};

const char* wifiPhaseName(WifiPhase p, bool portalOpen);
const char* wifiCredName(WifiCred c);

}  // namespace gateway
