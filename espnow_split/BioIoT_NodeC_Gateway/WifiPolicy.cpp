#include "WifiPolicy.h"

namespace gateway {

namespace {
bool reached(uint32_t now, uint32_t at) { return int32_t(now - at) >= 0; }
}  // namespace

const char* wifiPhaseName(WifiPhase p, bool portalOpen) {
  if (portalOpen) return p == WifiPhase::Connected ? "connected_portal_open" : "portal";
  switch (p) {
    case WifiPhase::NoCredentials: return "no_credentials";
    case WifiPhase::Connecting: return "connecting";
    case WifiPhase::Connected: return "connected";
    case WifiPhase::Backoff: return "backoff_local_operation";
  }
  return "unknown";
}

const char* wifiCredName(WifiCred c) {
  switch (c) {
    case WifiCred::None: return "none";
    case WifiCred::Stored: return "stored_nvs";
    case WifiCred::Default: return "default_gateway_secrets";
  }
  return "unknown";
}

const char* WifiPolicy::searchStateName() const {
  if (portalOpen_ && phase_ != WifiPhase::Connected) return "portal";
  switch (phase_) {
    case WifiPhase::Connected: return "connected";
    case WifiPhase::Connecting: return attemptFull_ ? "full_scan" : "quick_retry_known_channel";
    case WifiPhase::Backoff: return "waiting_backoff";
    case WifiPhase::NoCredentials: return "no_credentials";
  }
  return "unknown";
}

bool WifiPolicy::msSinceConnected(uint32_t now, uint32_t& ms) const {
  if (!everUp_) return false;
  ms = phase_ == WifiPhase::Connected ? 0 : now - lastUpMs_;
  return true;
}

// Las predeterminadas solo se usan si no hay credenciales guardadas.
WifiCred WifiPolicy::retryCred() const {
  return hasStored_ ? WifiCred::Stored : hasDefault_ ? WifiCred::Default : WifiCred::None;
}

void WifiPolicy::markUp(uint32_t now) {
  phase_ = WifiPhase::Connected;
  everUp_ = true;
  haveBssid_ = true;
  lastUpMs_ = now;
  fullBackoffMs_ = t_.fullMinMs;
  quickFails_ = 0;
}

WifiAction WifiPolicy::connect(WifiCred c, bool fullScan, uint32_t now) {
  WifiAction a;
  a.kind = WifiAction::Connect;
  a.cred = c;
  a.fullScan = fullScan || !haveBssid_;
  attemptFull_ = a.fullScan;
  cred_ = c;
  phase_ = WifiPhase::Connecting;
  attemptStartMs_ = now;
  attempts_++;
  if (a.fullScan) fullScans_++;
  return a;
}

WifiAction WifiPolicy::openPortal(uint32_t now, bool automatic) {
  WifiAction a;
  a.kind = WifiAction::OpenPortal;
  a.automatic = automatic;
  portalOpen_ = true;
  portalOpenedMs_ = now;
  portalOpens_++;
  if (automatic) autoPortalUsed_ = true;
  // Un intento en curso se aborta; conectado, C sigue conectado.
  if (phase_ == WifiPhase::Connecting)
    phase_ = retryCred() == WifiCred::None ? WifiPhase::NoCredentials : WifiPhase::Backoff;
  return a;
}

// Empieza (o reinicia) una interrupcion: intentos rapidos y una busqueda completa acotada.
void WifiPolicy::startOutage(uint32_t now, uint32_t firstQuickMs, uint32_t firstFullMs) {
  phase_ = retryCred() == WifiCred::None ? WifiPhase::NoCredentials : WifiPhase::Backoff;
  quickFails_ = 0;
  fullSinceLoss_ = false;
  fullBackoffMs_ = t_.fullMinMs;
  nextQuickMs_ = now + firstQuickMs;
  nextFullMs_ = now + firstFullMs;
}

WifiAction WifiPolicy::begin(const WifiTimings& t, bool hasStored, bool hasDefault, uint32_t now) {
  *this = WifiPolicy();
  t_ = t;
  hasStored_ = hasStored;
  hasDefault_ = hasDefault;
  fullBackoffMs_ = t_.fullMinMs;
  const WifiCred c = retryCred();
  if (c == WifiCred::None) {
    phase_ = WifiPhase::NoCredentials;
    return openPortal(now, true);
  }
  return connect(c, true, now);  // el canal guardado es solo una pista: se busca el SSID
}

WifiAction WifiPolicy::requestPortal(uint32_t now) {
  if (portalOpen_) return WifiAction();
  return openPortal(now, false);
}

WifiAction WifiPolicy::onPortalConnected(uint32_t now) {
  portalOpen_ = false;
  hasStored_ = true;
  cred_ = WifiCred::Stored;
  attemptFull_ = true;
  markUp(now);
  WifiAction a;
  a.kind = WifiAction::Connected;
  a.cred = WifiCred::Stored;
  return a;
}

uint32_t WifiPolicy::portalRemainingMs(uint32_t now) const {
  if (!portalOpen_) return 0;
  const uint32_t elapsed = now - portalOpenedMs_;
  return elapsed >= t_.portalMs ? 0 : t_.portalMs - elapsed;
}

uint32_t WifiPolicy::nextFullInMs(uint32_t now) const {
  if (phase_ != WifiPhase::Backoff) return 0;
  return reached(now, nextFullMs_) ? 0 : nextFullMs_ - now;
}

WifiAction WifiPolicy::tick(uint32_t now, LinkStatus link) {
  WifiAction a;
  if (link == LinkStatus::Up && phase_ == WifiPhase::Connected) lastUpMs_ = now;
  if (portalOpen_) {
    if (now - portalOpenedMs_ >= t_.portalMs) {
      portalOpen_ = false;
      a.kind = WifiAction::ClosePortal;
      // Conectado (portal abierto por Serial): nada que reintentar. Si no, el portal
      // ya consumio su tiempo: buscar pronto.
      if (phase_ != WifiPhase::Connected) startOutage(now, t_.lostQuickRetryMs, t_.lostQuickRetryMs);
      return a;
    }
    if (phase_ == WifiPhase::Connected && link != LinkStatus::Up) {
      startOutage(now, t_.lostQuickRetryMs, t_.firstFullAfterLossMs);  // reintentos al cerrar el portal
      a.kind = WifiAction::LinkLost;
    }
    return a;
  }
  switch (phase_) {
    case WifiPhase::Connecting: {
      const uint32_t elapsed = now - attemptStartMs_;
      if (link == LinkStatus::Up) {
        markUp(now);
        a.kind = WifiAction::Connected;
        a.cred = cred_;
        return a;
      }
      const uint32_t limit = attemptFull_ ? t_.connectTimeoutMs : t_.quickConnectTimeoutMs;
      const bool failed = elapsed >= limit || (link == LinkStatus::Failed && elapsed >= t_.failGraceMs);
      if (!failed) return a;
      // Primer intento del arranque fallido (red ausente, contrasena incorrecta...):
      // portal automatico. Despues, solo reintentos espaciados.
      if (!everUp_ && !autoPortalUsed_) return openPortal(now, true);
      phase_ = WifiPhase::Backoff;
      if (attemptFull_) {
        fullSinceLoss_ = true;
        quickFails_ = 0;
        nextFullMs_ = now + fullBackoffMs_;
        fullBackoffMs_ = fullBackoffMs_ * 2 > t_.fullMaxMs ? t_.fullMaxMs : fullBackoffMs_ * 2;
        nextQuickMs_ = now + t_.quickRetryMs;
      } else {
        quickFails_++;
        nextQuickMs_ = now + t_.quickRetryMs;
        // Primera busqueda tras la perdida: el BSSID/canal conocido no basta (el
        // hotspot pudo cambiar de canal o BSSID). Buscar el SSID ya.
        if (!fullSinceLoss_ && quickFails_ >= t_.quickBeforeFull) nextFullMs_ = now;
      }
      a.kind = WifiAction::AbortConnect;
      return a;
    }
    case WifiPhase::Connected:
      if (link != LinkStatus::Up) {
        startOutage(now, t_.lostQuickRetryMs, t_.firstFullAfterLossMs);
        a.kind = WifiAction::LinkLost;
      }
      return a;
    case WifiPhase::Backoff: {
      const WifiCred c = retryCred();
      if (c == WifiCred::None) { phase_ = WifiPhase::NoCredentials; return a; }
      if (reached(now, nextFullMs_)) return connect(c, true, now);
      // Intentos rapidos acotados por ciclo: no dependen para siempre del BSSID.
      if (haveBssid_ && quickFails_ < t_.quickBeforeFull && reached(now, nextQuickMs_)) return connect(c, false, now);
      return a;
    }
    case WifiPhase::NoCredentials:
      return a;
  }
  return a;
}

}  // namespace gateway
