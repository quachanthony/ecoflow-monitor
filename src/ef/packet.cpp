#include "packet.h"

#include <string.h>

#include "crc.h"

namespace ef {

static constexpr uint8_t PACKET_PREFIX = 0xAA;
static const uint8_t ENC_PREFIX[2] = {0x5A, 0x5A};
// Runaway guard: no legitimate reassembly buffer gets anywhere near this.
static constexpr size_t MAX_BUFFER = 8192;

static uint16_t rd16le(const uint8_t *p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}

static void push16le(std::vector<uint8_t> &v, uint16_t value) {
  v.push_back((uint8_t)(value & 0xFF));
  v.push_back((uint8_t)(value >> 8));
}

std::vector<uint8_t> Packet::toBytes() const {
  std::vector<uint8_t> data;
  data.reserve(20 + payload.size());

  data.push_back(PACKET_PREFIX);
  data.push_back(version);
  push16le(data, (uint16_t)payload.size());
  data.push_back(crc8(data.data(), data.size()));
  data.push_back(0x0D);  // product byte
  data.insert(data.end(), seq, seq + 4);
  data.push_back(0x00);  // static zeroes
  data.push_back(0x00);
  data.push_back(src);
  data.push_back(dst);
  if (version >= 0x03) {
    data.push_back(dsrc);
    data.push_back(ddst);
  }
  data.push_back(cmd_set);
  data.push_back(cmd_id);
  data.insert(data.end(), payload.begin(), payload.end());
  push16le(data, crc16(data.data(), data.size()));

  return data;
}

bool Packet::fromBytes(const uint8_t *data, size_t len, Packet &out) {
  if (len < 5 || data[0] != PACKET_PREFIX) return false;

  uint8_t version_byte = data[1];
  if (version_byte == 4) return false;  // V4 devices are out of scope

  uint8_t version = version_byte & 0x0F;
  bool sentinel_format = (version_byte & 0x10) != 0;
  if (version != 2 && version != 3) return false;
  if ((version == 2 && len < 18) || (version == 3 && len < 20)) return false;

  uint16_t payload_length = rd16le(data + 2);

  if (!sentinel_format) {
    if (crc16(data, len - 2) != rd16le(data + len - 2)) return false;
  }
  if (crc8(data, 4) != data[4]) return false;

  size_t payload_start = (version == 2) ? 16 : 18;
  if (payload_start + payload_length > len) return false;

  memcpy(out.seq, data + 6, 4);
  out.src = data[12];
  out.dst = data[13];
  if (version == 2) {
    out.dsrc = 0;
    out.ddst = 0;
    out.cmd_set = data[14];
    out.cmd_id = data[15];
  } else {
    out.dsrc = data[14];
    out.ddst = data[15];
    out.cmd_set = data[16];
    out.cmd_id = data[17];
  }
  out.version = version_byte;

  out.payload.assign(data + payload_start, data + payload_start + payload_length);
  if (sentinel_format && out.payload.size() >= 2 &&
      out.payload[out.payload.size() - 2] == 0xBB &&
      out.payload[out.payload.size() - 1] == 0xBB) {
    out.payload.resize(out.payload.size() - 2);
  }

  return true;
}

// --- SimplePacket (plaintext handshake frames, frame_type 0) ---

static std::vector<uint8_t> encFrame(uint8_t frame_type, const uint8_t *payload,
                                     size_t len) {
  std::vector<uint8_t> data;
  data.reserve(8 + len);
  data.push_back(ENC_PREFIX[0]);
  data.push_back(ENC_PREFIX[1]);
  data.push_back((uint8_t)(frame_type << 4));
  data.push_back(0x01);  // unknown constant
  push16le(data, (uint16_t)(len + 2));  // +2 for the CRC16
  data.insert(data.end(), payload, payload + len);
  push16le(data, crc16(data.data(), data.size()));
  return data;
}

std::vector<uint8_t> simplePacketEncode(const uint8_t *payload, size_t len) {
  return encFrame(0x00, payload, len);
}

// Find the 0x5A5A prefix at or after `from`; SIZE_MAX when absent.
static size_t findEncPrefix(const std::vector<uint8_t> &data, size_t from) {
  if (data.size() < 2) return SIZE_MAX;
  for (size_t i = from; i + 1 < data.size(); i++) {
    if (data[i] == ENC_PREFIX[0] && data[i + 1] == ENC_PREFIX[1]) return i;
  }
  return SIZE_MAX;
}

bool SimplePacketParser::feed(const uint8_t *data, size_t len,
                              std::vector<uint8_t> &out) {
  buffer_.insert(buffer_.end(), data, data + len);
  if (buffer_.size() > MAX_BUFFER) buffer_.clear();

  while (!buffer_.empty()) {
    size_t start = findEncPrefix(buffer_, 0);
    if (start == SIZE_MAX) {
      buffer_.clear();
      return false;
    }
    if (start > 0) buffer_.erase(buffer_.begin(), buffer_.begin() + start);

    if (buffer_.size() < 8) return false;  // wait for more data

    size_t data_end = 6 + rd16le(buffer_.data() + 4);
    if (data_end > buffer_.size()) {
      // could be incomplete, or a false prefix inside payload bytes — if
      // another prefix exists later, this one was spurious
      size_t next = findEncPrefix(buffer_, 2);
      if (next != SIZE_MAX) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + next);
        continue;
      }
      return false;  // buffered, wait for the next notification
    }

    if (data_end < 8) {  // frame too short to hold header + CRC
      buffer_.erase(buffer_.begin(), buffer_.begin() + 2);
      continue;
    }

    if (crc16(buffer_.data(), data_end - 2) !=
        rd16le(buffer_.data() + data_end - 2)) {
      buffer_.erase(buffer_.begin(), buffer_.begin() + 2);
      continue;
    }

    out.assign(buffer_.begin() + 6, buffer_.begin() + data_end - 2);
    buffer_.clear();  // one handshake frame in flight at a time
    return true;
  }
  return false;
}

// --- Post-handshake frame assembly ---

std::vector<uint8_t> FrameAssembler::encode(const Packet &packet) {
  std::vector<uint8_t> raw = packet.toBytes();

  switch (encrypt_type_) {
    case 0:
      return raw;
    case 1: {
      // 5-byte plaintext header, AES-CBC body
      std::vector<uint8_t> out(raw.begin(), raw.begin() + 5);
      std::vector<uint8_t> enc(raw.size() - 5 + 16);
      size_t n = cipher_->encrypt(raw.data() + 5, raw.size() - 5, enc.data());
      out.insert(out.end(), enc.begin(), enc.begin() + n);
      return out;
    }
    default: {  // type 7
      std::vector<uint8_t> enc(raw.size() + 16);
      size_t n = cipher_->encrypt(raw.data(), raw.size(), enc.data());
      return encFrame(0x01, enc.data(), n);  // FRAME_TYPE_PROTOCOL
    }
  }
}

void FrameAssembler::feed(const uint8_t *data, size_t len,
                          std::vector<std::vector<uint8_t>> &out) {
  buffer_.insert(buffer_.end(), data, data + len);
  if (buffer_.size() > MAX_BUFFER) buffer_.clear();

  switch (encrypt_type_) {
    case 0:
      feedPassthrough(out);
      break;
    case 1:
      feedRawHeader(out);
      break;
    default:
      feedEncPacket(out);
      break;
  }
}

void FrameAssembler::feedEncPacket(std::vector<std::vector<uint8_t>> &out) {
  while (!buffer_.empty()) {
    size_t start = findEncPrefix(buffer_, 0);
    if (start == SIZE_MAX) {
      buffer_.clear();
      return;
    }
    if (start > 0) buffer_.erase(buffer_.begin(), buffer_.begin() + start);

    if (buffer_.size() < 8) return;

    uint16_t payload_len = rd16le(buffer_.data() + 4);
    if (payload_len > 10000 || payload_len < 2) {
      buffer_.erase(buffer_.begin(), buffer_.begin() + 2);
      continue;
    }

    size_t data_end = 6 + payload_len;
    if (data_end > buffer_.size()) {
      size_t next = findEncPrefix(buffer_, 2);
      if (next != SIZE_MAX) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + next);
        continue;
      }
      return;  // genuine incomplete frame — wait for more notifications
    }

    if (crc16(buffer_.data(), data_end - 2) !=
        rd16le(buffer_.data() + data_end - 2)) {
      buffer_.erase(buffer_.begin(), buffer_.begin() + 2);
      continue;
    }

    size_t ct_len = payload_len - 2;
    std::vector<uint8_t> plain(ct_len ? ct_len : 1);
    size_t n = cipher_->decrypt(buffer_.data() + 6, ct_len, plain.data());
    plain.resize(n);
    out.push_back(std::move(plain));

    buffer_.erase(buffer_.begin(), buffer_.begin() + data_end);
  }
}

// Find the 0xAA prefix at or after `from`; SIZE_MAX when absent.
static size_t findPacketPrefix(const std::vector<uint8_t> &data, size_t from) {
  for (size_t i = from; i < data.size(); i++) {
    if (data[i] == PACKET_PREFIX) return i;
  }
  return SIZE_MAX;
}

void FrameAssembler::feedPassthrough(std::vector<std::vector<uint8_t>> &out) {
  while (!buffer_.empty()) {
    size_t start = findPacketPrefix(buffer_, 0);
    if (start == SIZE_MAX) {
      buffer_.clear();
      return;
    }
    if (start > 0) buffer_.erase(buffer_.begin(), buffer_.begin() + start);

    if (buffer_.size() < 5) return;

    if (crc8(buffer_.data(), 4) != buffer_[4]) {
      buffer_.erase(buffer_.begin(), buffer_.begin() + 1);
      continue;
    }

    uint16_t payload_length = rd16le(buffer_.data() + 2);
    uint8_t version_byte = buffer_[1];

    size_t frame_len;
    if (version_byte == 4) {
      frame_len = 8 + payload_length + 2;
    } else {
      size_t inner_overhead = ((version_byte & 0x0F) >= 3) ? 15 : 13;
      frame_len = 5 + inner_overhead + payload_length;
    }

    if (buffer_.size() < frame_len) return;

    out.emplace_back(buffer_.begin(), buffer_.begin() + frame_len);
    buffer_.erase(buffer_.begin(), buffer_.begin() + frame_len);
  }
}

void FrameAssembler::feedRawHeader(std::vector<std::vector<uint8_t>> &out) {
  while (!buffer_.empty()) {
    size_t start = findPacketPrefix(buffer_, 0);
    if (start == SIZE_MAX) {
      buffer_.clear();
      return;
    }
    if (start > 0) buffer_.erase(buffer_.begin(), buffer_.begin() + start);

    if (buffer_.size() < 5) return;

    if (crc8(buffer_.data(), 4) != buffer_[4]) {
      buffer_.erase(buffer_.begin(), buffer_.begin() + 1);
      continue;
    }

    uint16_t payload_length = rd16le(buffer_.data() + 2);
    uint8_t version = buffer_[1];

    size_t inner_overhead;
    if (version == 4) {
      inner_overhead = 5;
    } else if (version >= 3) {
      inner_overhead = 15;
    } else {
      inner_overhead = 13;
    }
    size_t inner_len = inner_overhead + payload_length;
    size_t encrypted_len = (inner_len + 15) / 16 * 16;
    size_t frame_len = 5 + encrypted_len;

    if (buffer_.size() < frame_len) return;

    std::vector<uint8_t> plain(encrypted_len);
    size_t n = cipher_->decrypt(buffer_.data() + 5, encrypted_len, plain.data());

    std::vector<uint8_t> packet(buffer_.begin(), buffer_.begin() + 5);
    size_t body = inner_len < n ? inner_len : n;
    packet.insert(packet.end(), plain.begin(), plain.begin() + body);
    out.push_back(std::move(packet));

    buffer_.erase(buffer_.begin(), buffer_.begin() + frame_len);
  }
}

}  // namespace ef
