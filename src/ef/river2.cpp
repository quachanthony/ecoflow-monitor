#include "river2.h"

#include <string.h>

namespace ef {
namespace river2 {

bool matchesSerialNumber(const char *sn) {
  static const char *prefixes[] = {"R601", "R603", "R611",
                                   "R613", "R621", "R623"};
  for (const char *p : prefixes) {
    if (strncmp(sn, p, 4) == 0) return true;
  }
  return false;
}

// Bounds-checked little-endian field readers. The reference decoder tolerates
// truncated heartbeats (older firmware omits trailing fields), so a field is
// only applied when the payload actually covers it.
static bool rdU8(const std::vector<uint8_t> &p, size_t off, int &out) {
  if (off + 1 > p.size()) return false;
  out = p[off];
  return true;
}

static bool rdU16(const std::vector<uint8_t> &p, size_t off, int &out) {
  if (off + 2 > p.size()) return false;
  out = p[off] | (p[off + 1] << 8);
  return true;
}

static bool rdU32(const std::vector<uint8_t> &p, size_t off, uint32_t &out) {
  if (off + 4 > p.size()) return false;
  out = (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) |
        ((uint32_t)p[off + 2] << 16) | ((uint32_t)p[off + 3] << 24);
  return true;
}

static bool rdF32(const std::vector<uint8_t> &p, size_t off, float &out) {
  uint32_t raw;
  if (!rdU32(p, off, raw)) return false;
  memcpy(&out, &raw, 4);
  return true;
}

bool parseTelemetry(const Packet &pkt, DeviceState &s, uint32_t now_ms) {
  const std::vector<uint8_t> &p = pkt.payload;
  bool processed = false;
  int v;
  uint32_t u;

  if (pkt.src == 0x02 && pkt.cmd_set == 0x20 && pkt.cmd_id == 0x02) {
    // DirectPdHeartbeatPack
    if (rdU16(p, 15, v)) s.watts_out = v;
    if (rdU16(p, 17, v)) s.watts_in = v;
    if (rdU8(p, 25, v)) s.usba_w = v;
    if (rdU8(p, 29, v)) s.usbc_w = v;
    if (rdU8(p, 34, v)) s.dc12v_out_w = v;
    processed = true;
  } else if (pkt.src == 0x03 && pkt.cmd_set == 0x20 && pkt.cmd_id == 0x02) {
    // DirectEmsDeltaHeartbeatPack
    if (rdU32(p, 17, u)) s.chg_remain_min = (int32_t)u;
    if (rdU32(p, 21, u)) s.dsg_remain_min = (int32_t)u;
    processed = true;
  } else if (pkt.src == 0x03 && pkt.cmd_set == 0x20 && pkt.cmd_id == 0x32) {
    // DirectBmsMDeltaHeartbeatPack
    float soc;
    if (rdF32(p, 53, soc) && soc >= 0.0f && soc <= 100.0f) {
      s.battery_pct = soc;
    }
    if (rdU8(p, 20, v)) s.battery_temp_c = v;
    processed = true;
  } else if (pkt.src == 0x04 && pkt.cmd_id == 0x02) {
    // DirectInvDeltaHeartbeatPack (any cmd_set, matching the reference)
    if (rdU16(p, 9, v)) s.ac_in_w = v;
    if (rdU16(p, 11, v)) s.ac_out_w = v;
    processed = true;
  } else if (pkt.src == 0x05 && pkt.cmd_set == 0x20 && pkt.cmd_id == 0x02) {
    // Mr330MpptHeart — source of truth for the AC/DC port switches
    if (rdU16(p, 16, v)) s.dc_in_w = v;
    if (rdU8(p, 56, v)) s.dc_on = (v == 1) ? 1 : 0;
    if (rdU8(p, 66, v)) s.ac_on = (v == 1) ? 1 : 0;
    s.last_port_state_ms = now_ms;
    processed = true;
  }

  if (processed) s.last_telemetry_ms = now_ms;
  return processed;
}

Packet buildAcToggle(bool on) {
  std::vector<uint8_t> payload = {(uint8_t)(on ? 0x01 : 0x00), 0xFF, 0xFF,
                                  0xFF, 0xFF, 0xFF, 0xFF};
  return Packet(0x21, 0x05, 0x20, 0x42, std::move(payload), 2);
}

Packet buildDcToggle(bool on) {
  std::vector<uint8_t> payload = {(uint8_t)(on ? 0x01 : 0x00)};
  return Packet(0x21, 0x05, 0x20, 0x51, std::move(payload), 2);
}

}  // namespace river2
}  // namespace ef
