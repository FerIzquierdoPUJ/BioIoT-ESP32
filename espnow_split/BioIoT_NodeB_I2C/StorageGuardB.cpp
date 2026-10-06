#include "StorageGuardB.h"

#include <stdio.h>
#include <string.h>

namespace nodeb {

const char* fsMountName(FsMount m) {
  switch (m) {
    case FsMount::Mounted: return "mounted";
    case FsMount::NoPartition: return "no_fs_partition";
    case FsMount::MountFailed: return "mount_failed_not_formatted";
  }
  return "unknown";
}

const char* formatDecisionName(FormatDecision d) {
  switch (d) {
    case FormatDecision::Allowed: return "allowed";
    case FormatDecision::RejectedProductionBuild: return "storage_format_requires_commissioning_build";
    case FormatDecision::RejectedAlreadyMounted: return "storage_already_mounted_not_formatted";
    case FormatDecision::RejectedNoPartition: return "no_fs_partition_check_flash_size";
    case FormatDecision::RejectedConfirmation: return "confirm_token_mismatch";
  }
  return "unknown";
}

void formatConfirmToken(uint32_t chipId, char out[kFormatTokenSize]) {
  snprintf(out, kFormatTokenSize, "FORMAT-NODE-B-%06lX", (unsigned long)(chipId & 0xFFFFFFu));
}

FormatDecision decideFormat(bool commissioningBuild, FsMount mount, const char* confirm, uint32_t chipId) {
  if (!commissioningBuild) return FormatDecision::RejectedProductionBuild;
  // Un LittleFS que monta puede contener calibraciones: nunca se formatea.
  if (mount == FsMount::Mounted) return FormatDecision::RejectedAlreadyMounted;
  if (mount == FsMount::NoPartition) return FormatDecision::RejectedNoPartition;
  char expected[kFormatTokenSize];
  formatConfirmToken(chipId, expected);
  if (!confirm || strcmp(confirm, expected) != 0) return FormatDecision::RejectedConfirmation;
  return FormatDecision::Allowed;
}

}  // namespace nodeb
