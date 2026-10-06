#include "bioiot_types.h"

#include <string.h>

namespace bioiot {

namespace {
const char* const kQualityNames[] = {
    "not_sampled", "good", "out_of_range", "disconnected", "warming_up", "uncalibrated",
    "below_reference_range", "user_calibrated", "vendor_reference", "communication_fault",
    "above_nominal_range", "stabilizing", "stale", "node_offline"};
static_assert(sizeof(kQualityNames) / sizeof(kQualityNames[0]) == kMaxQuality + 1, "quality table");

const SensorInfo kSensors[kSensorCount] = {
    {"ph", "pH", kNodeA, val::kAnalogCount},
    {"co2_1", "ppm", kNodeA, val::kCo2Count},
    {"co2_2", "ppm", kNodeA, val::kCo2Count},
    {"turbidity", "NTU", kNodeA, val::kAnalogCount},
    {"dissolved_oxygen", "mg/L", kNodeA, val::kDoCount},
    {"tds", "ppm", kNodeA, val::kAnalogCount},
    {"temperature", "C", kNodeA, val::kTempCount},
    {"color_1", nullptr, kNodeA, val::kColorCount},
    {"color_2", nullptr, kNodeA, val::kColorCount},
    {"light_1", "lux", kNodeB, val::kLightCount},
    {"light_2", "lux", kNodeB, val::kLightCount},
    {"o2_gas_1", "%vol", kNodeB, val::kO2Count},
    {"o2_gas_2", "%vol", kNodeB, val::kO2Count},
    {"temperature_b", "C", kNodeB, val::kTempCount},
};
const SensorInfo kUnknownSensor = {"unknown", nullptr, kNodeNone, 0};

const char* const kStatusNames[] = {
    "saved",
    "applied_ram_nvs_failed",
    "rejected_busy_or_future_schema",
    "rejected_invalid_parameters",
    "rejected_reference_use_20.9",
    "rejected_warming_up_or_busy",
    "command_sent",
    "communication_fault",
    "rejected_storage_failed",
    "rejected_import_not_allowed",
    "imported",
    "rejected_unknown_target",
    "rebooting",
    "diagnostics_started",
    "diagnostics_busy",
    "config_saved",
    "command_sent_record_not_persisted",
    "state_sent",
    "rejected_unsupported",
};
static_assert(sizeof(kStatusNames) / sizeof(kStatusNames[0]) == kMaxStatusCode + 1, "status table");
}  // namespace

const char* qualityName(uint8_t q) { return q <= kMaxQuality ? kQualityNames[q] : "unknown"; }

uint8_t qualityFromName(const char* name) {
  if (!name) return kQNotSampled;
  for (uint8_t i = 0; i <= kMaxQuality; ++i)
    if (strcmp(name, kQualityNames[i]) == 0) return i;
  return kQOutOfRange;
}

const SensorInfo& sensorInfo(uint8_t sensor) {
  return sensor < kSensorCount ? kSensors[sensor] : kUnknownSensor;
}

const char* statusName(uint8_t code) { return code <= kMaxStatusCode ? kStatusNames[code] : "unknown"; }

const char* outcomeName(uint8_t outcome) {
  switch (outcome) {
    case kOutcomeApplied: return "applied";
    case kOutcomeRejected: return "rejected";
    case kOutcomeFailed: return "failed";
    case kOutcomeStarted: return "started";
    default: return "unknown";
  }
}

const char* ackStatusName(uint8_t s) {
  switch (s) {
    case kAckAccepted: return "accepted";
    case kAckDuplicate: return "duplicate";
    case kAckRejectedInvalid: return "rejected_invalid";
    case kAckQueueFull: return "queue_full";
    case kAckUnsupported: return "unsupported";
    default: return "unknown";
  }
}

const char* resetReasonText(uint8_t r) {
  switch (r) {
    case kResetPowerOn: return "power_on";
    case kResetExternal: return "external_pin";
    case kResetSoftware: return "software";
    case kResetPanic: return "panic";
    case kResetWatchdog: return "watchdog";
    case kResetBrownout: return "brownout";
    case kResetDeepSleep: return "deep_sleep";
    case kResetException: return "exception";
    default: return "other";
  }
}

}  // namespace bioiot
