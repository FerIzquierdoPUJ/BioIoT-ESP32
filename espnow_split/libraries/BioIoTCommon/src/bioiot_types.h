#pragma once
// Identificadores compartidos: sensores, calidad, banderas y codigos de resultado.
// Los nombres de texto reproducen exactamente los del firmware integrado v4.
#include <stdint.h>

#include "bioiot_protocol.h"

namespace bioiot {

enum SensorId : uint8_t {
  kSensorPh = 0,
  kSensorCo2_1 = 1,
  kSensorCo2_2 = 2,
  kSensorTurbidity = 3,
  kSensorDissolvedOxygen = 4,
  kSensorTds = 5,
  kSensorTemperature = 6,
  kSensorColor1 = 7,
  kSensorColor2 = 8,
  kSensorLight1 = 9,
  kSensorLight2 = 10,
  kSensorO2Gas1 = 11,
  kSensorO2Gas2 = 12,
};
constexpr uint8_t kSensorCount = 13;
constexpr uint8_t kMaxRecordValues = 9;

// Orden de valores por sensor (indices de TelemetryRecord::vals).
namespace val {
// ph, turbidity, tds
constexpr uint8_t kAnalogRaw = 0, kAnalogVoltage = 1, kAnalogValue = 2, kAnalogCount = 3;
// co2_1, co2_2
constexpr uint8_t kCo2Raw = 0, kCo2Voltage = 1, kCo2Value = 2, kCo2ModuleV = 3, kCo2SensorV = 4,
                  kCo2Count = 5;
// dissolved_oxygen
constexpr uint8_t kDoRaw = 0, kDoVoltage = 1, kDoSatPct = 2, kDoValue = 3, kDoCount = 4;
// temperature
constexpr uint8_t kTempRaw = 0, kTempValue = 1, kTempCount = 2;
// color_1, color_2
constexpr uint8_t kColorRPulse = 0, kColorGPulse = 1, kColorBPulse = 2, kColorR = 3, kColorG = 4,
                  kColorB = 5, kColorH = 6, kColorS = 7, kColorL = 8, kColorCount = 9;
// light_1, light_2 (driverCode conserva -1/-2 de BH1750 como evidencia, nunca como lux)
constexpr uint8_t kLightValue = 0, kLightDriverCode = 1, kLightCount = 2;
// o2_gas_1, o2_gas_2
constexpr uint8_t kO2Value = 0, kO2Raw = 1, kO2Filtered = 2, kO2WireError = 3, kO2ReadCount = 4,
                  kO2FilterCount = 5, kO2Count = 6;
}  // namespace val

enum RecordFlags : uint16_t {
  kRecConnected = 1u << 0,
  kRecExpected = 1u << 1,
  kRecValid = 1u << 2,        // measurement_valid
  kRecWarming = 1u << 3,      // warming_up
  kRecStabilizing = 1u << 4,
  kRecI2cDetected = 1u << 5,
  kRecBelowRef = 1u << 6,     // below_reference_range
  kRecProvisional = 1u << 7,
  kRecCalLegacyMode = 1u << 8,  // co2 calibration_mode legacy_exponential
  kRecCalUser = 1u << 9,        // calibracion de usuario activa
  kRecObserved = 1u << 10,
  kRecCalEnabled = 1u << 11,
};

enum Quality : uint8_t {
  kQNotSampled = 0,
  kQGood = 1,
  kQOutOfRange = 2,
  kQDisconnected = 3,
  kQWarmingUp = 4,
  kQUncalibrated = 5,
  kQBelowReferenceRange = 6,
  kQUserCalibrated = 7,
  kQVendorReference = 8,
  kQCommunicationFault = 9,
  kQAboveNominalRange = 10,
  kQStabilizing = 11,
  // Solo el gateway las asigna (contrato 1.1):
  kQStale = 12,
  kQNodeOffline = 13,
};
constexpr uint8_t kMaxQuality = 13;
const char* qualityName(uint8_t q);
// Inversa para cadenas del modelo de calibracion original (evaluateCo2, O2Reading).
uint8_t qualityFromName(const char* name);

struct SensorInfo {
  const char* key;   // clave JSON v4
  const char* unit;  // unidad v4 (nullptr si el objeto no tenia unit)
  uint8_t owner;     // nodo propietario
  uint8_t valueCount;
};
const SensorInfo& sensorInfo(uint8_t sensor);

// Resultado de comandos (texto identico a v4 donde existia).
enum StatusCode : uint8_t {
  kStSaved = 0,
  kStAppliedRamNvsFailed = 1,  // solo para compatibilidad; los nodos nuevos no lo emiten
  kStRejectedBusyOrFutureSchema = 2,
  kStRejectedInvalidParameters = 3,
  kStRejectedReferenceUse209 = 4,
  kStRejectedWarmingUpOrBusy = 5,
  kStCommandSent = 6,
  kStCommunicationFault = 7,
  kStRejectedStorageFailed = 8,
  kStRejectedImportNotAllowed = 9,
  kStImported = 10,
  kStRejectedUnknownTarget = 11,
  kStRebooting = 12,
  kStDiagStarted = 13,
  kStDiagBusy = 14,
  kStConfigSaved = 15,
  kStCommandSentRecordNotPersisted = 16,
  kStStateSent = 17,
  kStRejectedUnsupported = 18,
};
constexpr uint8_t kMaxStatusCode = 18;
const char* statusName(uint8_t code);

// Etapa del resultado de un comando entregado a un nodo.
enum CommandOutcome : uint8_t {
  kOutcomeApplied = 1,    // ejecutado y, si aplica, persistido
  kOutcomeRejected = 2,   // no aplicado (validacion, ocupado, almacenamiento)
  kOutcomeFailed = 3,     // se intento y fallo (p. ej. comunicacion con el sensor)
  kOutcomeStarted = 4,    // trabajo asincrono iniciado (diagnostico, reinicio)
};
const char* outcomeName(uint8_t outcome);

// ACK de aplicacion: confirma recepcion/encolado, NO ejecucion ni persistencia.
enum AckStatus : uint8_t {
  kAckAccepted = 0,
  kAckDuplicate = 1,
  kAckRejectedInvalid = 2,
  kAckQueueFull = 3,
  kAckUnsupported = 4,
};
const char* ackStatusName(uint8_t s);

enum ResetReasonCode : uint8_t {
  kResetUnknown = 0,
  kResetPowerOn = 1,
  kResetExternal = 2,
  kResetSoftware = 3,
  kResetPanic = 4,
  kResetWatchdog = 5,
  kResetBrownout = 6,
  kResetDeepSleep = 7,
  kResetException = 8,
};
const char* resetReasonText(uint8_t r);

}  // namespace bioiot
