#pragma once
// Politica de LittleFS del nodo B (portable, probada en PC):
//  * Produccion: nunca se formatea. Un fallo de montaje deja el almacenamiento
//    "no disponible": el nodo mide, lo informa y rechaza calibraciones.
//  * Placa nueva: solo un firmware de puesta en marcha (NODE_B_STORAGE_COMMISSIONING=1)
//    acepta {"action":"storage_format","confirm":"FORMAT-NODE-B-<chip>"} por Serial,
//    con el token del propio chip y unicamente si LittleFS no monta.
#include <stdint.h>

namespace nodeb {

enum class FsMount : uint8_t {
  Mounted = 0,
  NoPartition = 1,  // FQBN sin particion FS (p. ej. eesz=4M): format() tampoco sirve
  MountFailed = 2,  // placa nueva, particion de otro sistema o danada: NO se formatea
};
const char* fsMountName(FsMount m);

enum class FormatDecision : uint8_t {
  Allowed = 0,
  RejectedProductionBuild = 1,
  RejectedAlreadyMounted = 2,
  RejectedNoPartition = 3,
  RejectedConfirmation = 4,
};
const char* formatDecisionName(FormatDecision d);

constexpr unsigned kFormatTokenSize = 24;
// "FORMAT-NODE-B-" + 6 digitos hex (ESP.getChipId(), mayusculas).
void formatConfirmToken(uint32_t chipId, char out[kFormatTokenSize]);
FormatDecision decideFormat(bool commissioningBuild, FsMount mount, const char* confirm, uint32_t chipId);

}  // namespace nodeb
