#include "bioiot_protocol.h"

#include "bioiot_codec.h"
#include "bioiot_crypto.h"

namespace bioiot {

const char* msgTypeName(MsgType t) {
  switch (t) {
    case MsgType::Hello: return "HELLO";
    case MsgType::Status: return "STATUS";
    case MsgType::Telemetry: return "TELEMETRY";
    case MsgType::Ack: return "ACK";
    case MsgType::Config: return "CONFIG";
    case MsgType::Calibration: return "CALIBRATION";
    case MsgType::CommandResult: return "COMMAND_RESULT";
    case MsgType::DiagRequest: return "DIAGNOSTIC_REQUEST";
    case MsgType::DiagResult: return "DIAGNOSTIC_RESULT";
    case MsgType::TimeSync: return "TIME_SYNC";
    case MsgType::Command: return "COMMAND";
    case MsgType::CalState: return "CALIBRATION_STATE";
  }
  return "UNKNOWN";
}

const char* nodeKey(uint8_t node) {
  switch (node) {
    case kNodeGateway: return "gateway";
    case kNodeA: return "node_a";
    case kNodeB: return "node_b";
    default: return "unknown";
  }
}

const char* frameErrorName(FrameError e) {
  switch (e) {
    case FrameError::None: return "ok";
    case FrameError::TooShort: return "too_short";
    case FrameError::TooLong: return "too_long";
    case FrameError::BadMagic: return "bad_magic";
    case FrameError::BadVersion: return "incompatible_version";
    case FrameError::WrongSystem: return "wrong_system";
    case FrameError::WrongDestination: return "wrong_destination";
    case FrameError::LengthMismatch: return "length_mismatch";
    case FrameError::UnknownSource: return "unknown_source";
    case FrameError::UnknownType: return "unknown_type";
    case FrameError::BadTag: return "bad_tag";
    case FrameError::NoKey: return "no_key";
  }
  return "unknown";
}

size_t encodeFrame(const FrameHeader& h, const uint8_t* payload, const uint8_t* key, size_t keyLen,
                   uint8_t* out, size_t outCap) {
  if (h.payloadLen > kMaxPayload || !key || !keyLen) return 0;
  const size_t total = kHeaderSize + h.payloadLen + kTagSize;
  if (total > outCap || total > kEspNowMaxFrame) return 0;
  Writer w(out, outCap);
  w.u8(kMagic0); w.u8(kMagic1); w.u8(kProtocolVersion); w.u8(uint8_t(h.type));
  w.u32(h.systemId); w.u8(h.src); w.u8(h.dst); w.u8(h.flags); w.u8(h.payloadLen);
  w.u32(h.srcBoot); w.u32(h.dstBoot); w.u32(h.seq);
  w.bytes(payload, h.payloadLen);
  if (!w.ok() || w.size() != kHeaderSize + h.payloadLen) return 0;
  uint8_t mac[32];
  hmacSha256(key, keyLen, out, w.size(), mac);
  w.bytes(mac, kTagSize);
  return w.ok() ? w.size() : 0;
}

FrameError precheckFrame(const uint8_t* data, size_t len, uint32_t systemId, uint8_t self) {
  if (!data || len < kMinFrame) return FrameError::TooShort;
  if (len > kEspNowMaxFrame) return FrameError::TooLong;
  if (data[0] != kMagic0 || data[1] != kMagic1) return FrameError::BadMagic;
  if (data[2] != kProtocolVersion) return FrameError::BadVersion;
  if (getU32(data + 4) != systemId) return FrameError::WrongSystem;
  if (data[9] != self) return FrameError::WrongDestination;
  const uint8_t src = data[8];
  if (src == kNodeNone || src > kMaxNodeId || src == self) return FrameError::UnknownSource;
  if (data[3] == 0 || data[3] > kMaxMsgType) return FrameError::UnknownType;
  if (size_t(data[11]) + kMinFrame != len) return FrameError::LengthMismatch;
  return FrameError::None;
}

FrameError decodeFrame(const uint8_t* data, size_t len, uint32_t systemId, uint8_t self,
                       const uint8_t* key, size_t keyLen, FrameHeader& h, const uint8_t*& payload) {
  const FrameError pre = precheckFrame(data, len, systemId, self);
  if (pre != FrameError::None) return pre;
  if (!key || !keyLen) return FrameError::NoKey;
  uint8_t mac[32];
  hmacSha256(key, keyLen, data, len - kTagSize, mac);
  if (!constantTimeEqual(mac, data + len - kTagSize, kTagSize)) return FrameError::BadTag;
  Reader r(data, kHeaderSize);
  r.u8(); r.u8();
  h.version = r.u8();
  h.type = MsgType(r.u8());
  h.systemId = r.u32();
  h.src = r.u8(); h.dst = r.u8(); h.flags = r.u8(); h.payloadLen = r.u8();
  h.srcBoot = r.u32(); h.dstBoot = r.u32(); h.seq = r.u32();
  payload = data + kHeaderSize;
  return FrameError::None;
}

}  // namespace bioiot
