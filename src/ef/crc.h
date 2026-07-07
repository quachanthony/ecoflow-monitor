// CRC variants used by the EcoFlow BLE protocol.
// crc8:  width=8,  poly=0x07,   init=0x00, no reflection
// crc16: width=16, poly=0x8005, init=0x0000, reflected in/out (CRC-16/ARC)
// Reference: ha-ef-ble custom_components/ef_ble/eflib/crc.py
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ef {

uint8_t crc8(const uint8_t *data, size_t len);
uint16_t crc16(const uint8_t *data, size_t len);

}  // namespace ef
