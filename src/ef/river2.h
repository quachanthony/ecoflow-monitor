// River 2 / River 2 Max / River 2 Pro telemetry parsing and control commands.
// Field offsets come from the fixed-width little-endian structs in
// ha-ef-ble eflib/model/*.py (verified against the source — see PROTOCOL.md).
#pragma once

#include <stdint.h>

#include "packet.h"

namespace ef {
namespace river2 {

// Serial-number prefixes: R601/R603 (River 2), R611/R613 (Max), R621/R623 (Pro)
bool matchesSerialNumber(const char *sn);

// Everything the UI needs. -1 / NAN-like sentinels mean "not received yet";
// each subsystem keeps its own last-update timestamp for staleness checks.
struct DeviceState {
  // BMS (src 0x03, cmd_set 0x20, cmd_id 0x32)
  float battery_pct = -1.0f;  // f32_show_soc, 0.0-100.0
  int battery_temp_c = -1000;

  // PD (src 0x02, cmd_set 0x20, cmd_id 0x02)
  int watts_in = -1;   // total input W
  int watts_out = -1;  // total output W
  int dc12v_out_w = -1;
  int usbc_w = -1;
  int usba_w = -1;

  // INV (src 0x04, any cmd_set, cmd_id 0x02)
  int ac_in_w = -1;
  int ac_out_w = -1;

  // MPPT (src 0x05, cmd_set 0x20, cmd_id 0x02) — authoritative for port state
  int ac_on = -1;  // -1 unknown, 0 off, 1 on
  int dc_on = -1;
  int dc_in_w = -1;  // solar/car input

  // EMS (src 0x03, cmd_set 0x20, cmd_id 0x02)
  int32_t chg_remain_min = -1;
  int32_t dsg_remain_min = -1;

  uint32_t last_telemetry_ms = 0;   // any heartbeat
  uint32_t last_port_state_ms = 0;  // MPPT heartbeat (ac_on/dc_on refresh)
};

// Parse one telemetry packet into `state`. `now_ms` stamps the update times.
// Returns true when the packet was a recognized River 2 heartbeat.
bool parseTelemetry(const Packet &packet, DeviceState &state, uint32_t now_ms);

// Control packets (src 0x21 → dst 0x05, cmd_set 0x20, V2).
Packet buildAcToggle(bool on);   // cmd_id 0x42, payload [on, 0xFF x6]
Packet buildDcToggle(bool on);   // cmd_id 0x51, payload [on]

}  // namespace river2
}  // namespace ef
