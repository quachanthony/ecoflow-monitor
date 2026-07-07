// 65280-byte lookup table used by the session-key derivation step of the
// EcoFlow BLE handshake. Extracted from ha-ef-ble (eflib/keydata.py).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ef {

extern const uint8_t KEYDATA[];
extern const size_t KEYDATA_SIZE;

}  // namespace ef
