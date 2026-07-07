// EcoFlow BLE wire format (ported from ha-ef-ble eflib/packet.py,
// eflib/encpacket.py and eflib/frame_assembler.py — see PROTOCOL.md).
//
// Two layers:
//   inner "Packet"  — 0xAA-prefixed V2/V3 command/telemetry packet
//   outer framing   — depends on the device's advertised encrypt_type:
//     type 0: inner packet sent as-is
//     type 1: 5-byte plaintext header + AES-CBC body (key/iv from MD5 of SN)
//     type 7: 0x5A5A "EncPacket" wrapper around an AES-CBC encrypted packet
//             (session key negotiated via ECDH — the River 2 default)
#pragma once

#include <stdint.h>

#include <vector>

#include "crypto.h"

namespace ef {

struct Packet {
  uint8_t src = 0;
  uint8_t dst = 0;
  uint8_t cmd_set = 0;
  uint8_t cmd_id = 0;
  std::vector<uint8_t> payload;
  uint8_t dsrc = 1;
  uint8_t ddst = 1;
  uint8_t version = 2;  // River 2 talks V2
  uint8_t seq[4] = {0, 0, 0, 0};

  Packet() = default;
  Packet(uint8_t src_, uint8_t dst_, uint8_t cmd_set_, uint8_t cmd_id_,
         std::vector<uint8_t> payload_ = {}, uint8_t version_ = 2)
      : src(src_), dst(dst_), cmd_set(cmd_set_), cmd_id(cmd_id_),
        payload(std::move(payload_)), version(version_) {}

  // Serialize as V2 (16-byte header) or V3 (18-byte header) + CRC16.
  std::vector<uint8_t> toBytes() const;

  // Parse one complete V2/V3 packet (incl. 0x1X sentinel variants).
  // Returns false on any CRC/size mismatch.
  static bool fromBytes(const uint8_t *data, size_t len, Packet &out);
};

// --- Plaintext handshake framing ("SimplePacket": 0x5A5A, frame_type 0) ---

std::vector<uint8_t> simplePacketEncode(const uint8_t *payload, size_t len);

// Streaming parser: feed BLE notifications, get at most one payload back per
// call (matching the reference: one in-flight handshake frame at a time).
class SimplePacketParser {
 public:
  // Returns true and fills `out` when a complete valid frame was assembled.
  bool feed(const uint8_t *data, size_t len, std::vector<uint8_t> &out);
  void reset() { buffer_.clear(); }

 private:
  std::vector<uint8_t> buffer_;
};

// --- Post-handshake framing, one class per encrypt_type ---

class FrameAssembler {
 public:
  // cipher may be null only for encrypt_type 0.
  FrameAssembler(int encrypt_type, Aes128Cbc *cipher)
      : encrypt_type_(encrypt_type), cipher_(cipher) {}

  int encryptType() const { return encrypt_type_; }

  // Whether BLE writes should use write-with-response (type 7 does).
  bool writeWithResponse() const { return encrypt_type_ == 7; }

  // Frame + encrypt an inner packet for sending.
  std::vector<uint8_t> encode(const Packet &packet);

  // Feed raw notification bytes; appends every complete inner packet's raw
  // bytes (decrypted, ready for Packet::fromBytes) to `out`.
  void feed(const uint8_t *data, size_t len,
            std::vector<std::vector<uint8_t>> &out);

  void reset() { buffer_.clear(); }

 private:
  void feedEncPacket(std::vector<std::vector<uint8_t>> &out);
  void feedPassthrough(std::vector<std::vector<uint8_t>> &out);
  void feedRawHeader(std::vector<std::vector<uint8_t>> &out);

  int encrypt_type_;
  Aes128Cbc *cipher_;
  std::vector<uint8_t> buffer_;
};

}  // namespace ef
