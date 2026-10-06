#include "bioiot_fragment.h"

#include <string.h>

namespace bioiot {

void Reassembler::expect(uint32_t requestId, uint32_t nowMs) {
  active_ = true; complete_ = false; requestId_ = requestId; lastMs_ = nowMs;
  mask_ = 0; totalLen_ = 0; count_ = 0;
}

uint8_t Reassembler::received() const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < 32; ++i) n += (mask_ >> i) & 1u;
  return n;
}

Reassembler::Result Reassembler::add(const DiagFragmentMsg& f, uint32_t nowMs) {
  if (!active_ || f.requestId != requestId_) return kRejected;
  if (complete_) return kDuplicate;
  if (count_ == 0) {
    count_ = f.count;
    totalLen_ = f.totalLen;
  } else if (f.count != count_ || f.totalLen != totalLen_) {
    return kRejected;
  }
  if (f.index >= count_ || f.totalLen > kDiagMaxBytes) return kRejected;
  const size_t offset = size_t(f.index) * kDiagChunkSize;
  if (offset + f.chunkLen > totalLen_) return kRejected;
  if (mask_ & (1u << f.index)) return kDuplicate;
  memcpy(buf_ + offset, f.chunk, f.chunkLen);
  mask_ |= 1u << f.index;
  lastMs_ = nowMs;
  const uint32_t full = count_ >= 32 ? 0xFFFFFFFFu : ((1u << count_) - 1u);
  if (mask_ == full) {
    complete_ = true;
    return kComplete;
  }
  return kAccepted;
}

bool Fragmenter::start(uint32_t requestId, const uint8_t* data, size_t len) {
  if (!data || len == 0 || len > kDiagMaxBytes) return false;
  active_ = true; requestId_ = requestId; data_ = data; len_ = uint16_t(len);
  count_ = uint8_t((len + kDiagChunkSize - 1) / kDiagChunkSize); next_ = 0;
  return true;
}

size_t Fragmenter::next(uint8_t* out, size_t cap) {
  if (!hasNext()) return 0;
  DiagFragmentMsg f;
  f.requestId = requestId_; f.index = next_; f.count = count_; f.totalLen = len_;
  const size_t offset = size_t(next_) * kDiagChunkSize;
  const size_t remain = len_ - offset;
  f.chunkLen = uint8_t(remain > kDiagChunkSize ? kDiagChunkSize : remain);
  f.chunk = data_ + offset;
  const size_t n = encodeDiagFragment(f, out, cap);
  if (n) next_++;
  if (next_ >= count_) active_ = next_ < count_;
  return n;
}

}  // namespace bioiot
