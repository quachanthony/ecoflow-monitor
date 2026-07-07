# EcoFlow River 2 (Pro) BLE Protocol Notes

Phase 3 deliverable: everything protocol-specific in this firmware, extracted
from the reference implementations (protocol source of truth):

- https://github.com/rabits/ef-ble-reverse — reverse-engineering notes
- https://github.com/rabits/ha-ef-ble — working Python implementation
  (`custom_components/ef_ble/eflib/` — file references below are to this tree)

If EcoFlow firmware changes ever break this box, re-check these repos and
update this file plus the matching code.

## Device discovery (`__init__.py`, `devicebase.py`)

- Advertisement carries **manufacturer data for company id `0xB5B5`**.
- Manufacturer payload: `proto_version(1) | serial[16] (zero-padded ASCII) |
  status(1) | product_type(1) | ... | capability_flags @ byte 22`.
- `encrypt_type = (capability_flags & 0b0111000) >> 3`; assume 7 when the
  payload is too short. River 2 units use type 7 (ECDH + AES session).
- River 2 series SN prefixes: `R601/R603` (River 2), `R611/R613` (Max),
  `R621/R623` (**Pro**). Advertised name starts with `EF-R2`.

## GATT (`connection.py:60`)

Two possible characteristic pairs; use whichever the device exposes:

| Profile | Write | Notify |
|---|---|---|
| "rfcomm" | `00000002-0000-1000-8000-00805f9b34fb` | `00000003-...` |
| Nordic UART | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` | `6e400003-...` |

Only one BLE client can be connected at a time — the EcoFlow app and this box
compete for the slot.

## Handshake (encrypt_type 7, `connection.py:908-1090`)

All handshake frames use the plaintext "SimplePacket" wrapper (below).

1. **ECDH exchange** — send payload `01 00 || our_pubkey(40)`.
   Curve is **SECP160r1**; public keys are raw `X||Y` (20+20 bytes).
   Response payload: `[cmd, status, curve_type, device_pubkey(40)]`.
2. **Shared secret** — 20-byte ECDH X coordinate.
   `iv = MD5(shared_20B)`, initial AES key = `shared[0:16]` (AES-128-CBC).
3. **Key info** — send payload `02`. Response: `02 || AES(sRand[16] || seed[2])`.
4. **Session key** — `pos = seed[0]*0x10 + ((seed[1]-1)&0xFF)*0x100`;
   `session_key = MD5(keydata[pos:pos+16] || sRand)` where `keydata` is the
   65280-byte table from `keydata.py` (vendored in `src/ef/keydata.cpp`).
   All further traffic is AES-128-CBC (PKCS7) with `session_key`/`iv`.
   **Each frame restarts from the same IV** — no CBC chaining across frames.
5. **Auth status** — send Packet `src=0x21 dst=0x35 cmd_set=0x35 cmd_id=0x89`,
   empty payload. Reply content is ignored; it just gates the next step.
6. **Authenticate** — send Packet `... cmd_id=0x86`, payload = 32 ASCII chars:
   uppercase hex of `MD5(user_id + serial_number)`. `user_id` is the numeric
   EcoFlow account id (see `tools/get_user_id.py`); the device learned it when
   paired via the app.
   Reply payload `0x00` = success; `0x01..0x07` = failure (wrong key, bound to
   another account, ...; `exceptions.py:81`). Any *other* packet arriving
   while authenticating also means success.

Types 0/1 skip ECDH: type 0 is plaintext; type 1 uses
`key = MD5(sn)`, `iv = MD5(reversed(sn))`, zero-padding, and frames are
`5-byte plaintext packet header + AES(rest)`.

## Framing

**SimplePacket / EncPacket** (`encpacket.py`, handshake + type-7 traffic):

```
5A 5A | frame_type<<4 | 01 | len(payload)+2 (LE16) | payload | CRC16 (LE)
```
frame_type 0 = handshake command (plaintext payload), 1 = protocol
(payload = AES-encrypted inner packet). CRC16 covers everything before it.

**Inner Packet V2** (`packet.py` — the River 2 speaks version 2):

```
AA | 02 | payload_len (LE16) | CRC8(hdr) | 0D | seq[4] | 00 00 |
src | dst | cmd_set | cmd_id | payload | CRC16 (LE)
```
- CRC8: poly 0x07, init 0, unreflected — over the first 4 bytes.
- CRC16: poly 0x8005 reflected (0xA001), init 0 (CRC-16/ARC) — over the rest.
- seq can stay `00 00 00 00` for sent packets.
- V3 adds `dsrc | ddst` (both 0x01) before cmd_set; version byte `0x1X` is a
  "sentinel" variant: no trailing CRC16, payload ends with `BB BB`.

## Telemetry (River 2: `devices/river2.py`, `model/*.py`)

Fixed-offset little-endian structs — **no protobuf** (nanopb not needed).
Firmware may truncate packs; only parse fields the payload covers.
Match on `(src, cmd_set, cmd_id)`:

| Pack | src | cmd_set | cmd_id | Fields we use (offset, type) |
|---|---|---|---|---|
| PD | 0x02 | 0x20 | 0x02 | watts_out_sum (15,u16), watts_in_sum (17,u16), usb1_watts (25,u8), typec1_watts (29,u8), car_watts (34,u8) |
| EMS | 0x03 | 0x20 | 0x02 | chg_remain_time (17,u32,min), dsg_remain_time (21,u32,min) |
| BMS | 0x03 | 0x20 | 0x32 | temp (20,u8,°C), **f32_show_soc (53,f32)** ← battery % |
| INV | 0x04 | any | 0x02 | input_watts (9,u16), output_watts (11,u16) |
| MPPT | 0x05 | 0x20 | 0x02 | in_watts (16,u16), **car_state (56,u8)** ← 12V on/off, **cfg_ac_enabled (66,u8)** ← AC on/off |

Full offset tables were derived mechanically from the `Annotated` struct
definitions in `model/` (see git history / the structs in `river2.cpp`).

## Commands (River 2, all `src=0x21`, V2)

| Action | dst | cmd_set | cmd_id | Payload |
|---|---|---|---|---|
| AC output on/off | 0x05 | 0x20 | 0x42 | `[01/00, FF, FF, FF, FF, FF, FF]` |
| 12V (DC) output on/off | 0x05 | 0x20 | 0x51 | `[01/00]` |

(X-Boost is byte 1 of the 0x42 payload; charge-rate and charge-limit commands
exist too — see `river2.py` — but are out of scope for this box.)

Confirmation: don't trust the write — the next MPPT heartbeat reports the
actual port state (`car_state` / `cfg_ac_enabled`), which is what flips the
button color.

## Notes

- No keep-alive needed; River 2 heartbeats arrive unsolicited every few
  seconds once authenticated. (`replyPacket` mirroring is only needed for
  Smart Home Panel devices.)
- Writes use write-with-response for type 7.
- Frames can span multiple BLE notifications and notifications can contain
  multiple frames — hence the reassembly buffers in `packet.cpp`.
