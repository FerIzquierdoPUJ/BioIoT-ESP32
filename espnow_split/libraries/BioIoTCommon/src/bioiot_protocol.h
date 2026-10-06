#pragma once
// Contrato de trama ESP-NOW BioIoT, version 1.
//
//  off  tam  campo
//   0    2   magic 0xB1 0x0E
//   2    1   protocolVersion (=1; distinto => rechazo)
//   3    1   msgType
//   4    4   systemId (rechaza otros sistemas)
//   8    1   srcNode
//   9    1   dstNode
//  10    1   flags (bit0 ACK solicitado, bit1 retransmision)
//  11    1   payloadLen
//  12    4   srcBoot (aleatorio por arranque del emisor)
//  16    4   dstBoot (arranque del receptor conocido por el emisor; 0 = desconocido)
//  20    4   seq (por emisor y arranque; la retransmision conserva seq)
//  24    N   payload (N <= 218)
//  24+N  8   tag = HMAC-SHA256(clave de enlace, bytes[0..24+N))[0..8]
//
// Todo multi-byte es little-endian. Trama maxima = 24 + 218 + 8 = 250 bytes,
// el limite ESP-NOW v1 que ESP8266 y ESP32 aceptan.
#include <stddef.h>
#include <stdint.h>

namespace bioiot {

constexpr uint8_t kMagic0 = 0xB1;
constexpr uint8_t kMagic1 = 0x0E;
constexpr uint8_t kProtocolVersion = 1;
constexpr size_t kEspNowMaxFrame = 250;
constexpr size_t kHeaderSize = 24;
constexpr size_t kTagSize = 8;
constexpr size_t kMaxPayload = kEspNowMaxFrame - kHeaderSize - kTagSize;  // 218
constexpr size_t kMinFrame = kHeaderSize + kTagSize;
constexpr size_t kAppKeySize = 32;
constexpr size_t kEspNowKeySize = 16;
static_assert(kHeaderSize + kMaxPayload + kTagSize == 250, "Trama completa debe ser <= 250 bytes");

enum class MsgType : uint8_t {
  Hello = 1,
  Status = 2,
  Telemetry = 3,
  Ack = 4,
  Config = 5,
  Calibration = 6,
  CommandResult = 7,
  DiagRequest = 8,
  DiagResult = 9,
  TimeSync = 10,
  Command = 11,
  CalState = 12,
};
constexpr uint8_t kMaxMsgType = 12;
const char* msgTypeName(MsgType t);

enum NodeId : uint8_t { kNodeNone = 0, kNodeGateway = 1, kNodeA = 2, kNodeB = 3 };
constexpr uint8_t kMaxNodeId = 3;
const char* nodeKey(uint8_t node);  // "gateway", "node_a", "node_b"

enum FrameFlags : uint8_t { kFlagAckRequested = 0x01, kFlagRetransmission = 0x02 };

struct FrameHeader {
  uint8_t version = kProtocolVersion;
  MsgType type = MsgType::Hello;
  uint32_t systemId = 0;
  uint8_t src = 0, dst = 0, flags = 0, payloadLen = 0;
  uint32_t srcBoot = 0, dstBoot = 0, seq = 0;
};

enum class FrameError : uint8_t {
  None = 0,
  TooShort,
  TooLong,
  BadMagic,
  BadVersion,
  WrongSystem,
  WrongDestination,
  LengthMismatch,
  UnknownSource,
  UnknownType,
  BadTag,
  NoKey,
};
const char* frameErrorName(FrameError e);

// Codifica la trama completa y firma. Devuelve longitud total o 0 si no cabe.
size_t encodeFrame(const FrameHeader& h, const uint8_t* payload, const uint8_t* key, size_t keyLen,
                   uint8_t* out, size_t outCap);

// Comprobacion barata apta para el callback de recepcion: tamanos, magic, version,
// sistema, destino, origen conocido y longitud declarada. No verifica el tag.
FrameError precheckFrame(const uint8_t* data, size_t len, uint32_t systemId, uint8_t self);

// Verificacion completa (incluye HMAC). payload apunta dentro de data.
FrameError decodeFrame(const uint8_t* data, size_t len, uint32_t systemId, uint8_t self,
                       const uint8_t* key, size_t keyLen, FrameHeader& h, const uint8_t*& payload);

inline uint8_t frameSource(const uint8_t* data) { return data[8]; }

}  // namespace bioiot
