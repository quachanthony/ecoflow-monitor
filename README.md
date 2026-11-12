# EcoFlow River 2 Pro — ESP32 BLE Monitor & Control

An ESP32 touchscreen box that connects directly to an EcoFlow River 2 / 2 Max /
2 Pro over Bluetooth LE, shows live battery & power stats, and toggles the AC
and 12V outputs. No phone, no internet, no cloud.

**Hardware:** ESP32-2432S028R "Cheap Yellow Display" (CYD) — ESP32-WROOM-32 +
2.8" 240x320 ILI9341 TFT + XPT2046 resistive touch, one board, no wiring.

**New to electronics?** Open [docs/setup-guide.html](docs/setup-guide.html) in a
browser — an illustrated, step-by-step version of everything below.

**Protocol:** ported from [rabits/ha-ef-ble](https://github.com/rabits/ha-ef-ble)
(see [PROTOCOL.md](PROTOCOL.md) for the full write-up).

## Status

Written **before hardware was in hand**. The protocol layer is a faithful port
of the working Python reference and is covered by host-side tests that compare
its byte output against that reference, but nothing has run against a real
CYD or River 2 yet. First-boot checklist below.

## Setup

1. Install [PlatformIO](https://platformio.org/) (VS Code extension or CLI).
2. Get your EcoFlow user id (needed for BLE auth — the device only talks to
   clients presenting the MD5 of `user_id + serial`):

   ```sh
   python3 tools/get_user_id.py you@example.com
   ```

3. Create your secrets file:

   ```sh
   cp -n src/secrets.h.example src/secrets.h
   # edit ECOFLOW_USER_ID (and optionally ECOFLOW_DEVICE_SN)
   ```

4. Build & flash (CYD plugged in over USB, EcoFlow app **closed** — only one
   BLE client can connect at a time):

   ```sh
   pio run -e cyd -t upload && pio device monitor
   ```

   Board has **two** USB ports (USB-C + micro-USB)? It has a different display
   panel — use `-e cyd2usb` instead. Wrong choice = inverted colours.

## Host-side protocol tests (no hardware needed)

```sh
pio test -e native
```

Verifies CRCs, packet/frame encoding, reassembly and session-key derivation
against byte-for-byte vectors generated from the Python reference
implementation (`test/test_protocol/`).

## First-boot checklist (when hardware arrives)

1. **Display**: boot → header bar + "Scanning" appear. If colors/mirroring are
   off, tweak `include/User_Setup.h` (some CYD batches differ).
2. **Touch**: tap the screen, watch `[touch] raw=(x,y) -> (px,py)` in the
   serial monitor. Tap all four corners and adjust `TOUCH_RAW_*` in
   `src/config.h` until mapped coordinates track your finger.
3. **Scan**: with the River 2 powered on, serial should log
   `[ble] found R621... (encrypt_type=7)`. If not, check the unit advertises
   (app closed!).
4. **Auth**: `[ble] authenticated` then live numbers. `Auth rejected` on
   screen → wrong `ECOFLOW_USER_ID` for the account that paired the unit.
5. **Toggles**: tap AC/12V — button shows `...` until the next telemetry
   confirms, then flips color. Cross-check against the unit + app.
6. **Robustness**: walk out of range, power-cycle the unit, open/close the
   app — the box must always recover by itself (backoff reconnect + watchdog).

## Layout

```
src/main.cpp          setup/loop, watchdog
src/config.h          pins, touch calibration, timings
src/secrets.h         your user id (gitignored; copy from .example)
src/ef/               protocol port: crc, crypto, packet framing,
                      connection state machine, River 2 telemetry/commands
src/ui/               TFT_eSPI + XPT2046 touchscreen UI
lib/micro_ecc/        vendored micro-ecc (SECP160r1 ECDH; BSD-2)
include/User_Setup.h  TFT_eSPI pin config for the CYD
tools/get_user_id.py  fetch your EcoFlow user id
test/test_protocol/   host-side tests against Python-generated vectors
```
