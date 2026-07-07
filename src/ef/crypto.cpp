// Portable part of the crypto layer: session-key derivation and the AES-CBC
// session cipher's padding/framing rules. The raw primitives (MD5, the AES
// block cipher, ECDH) live in crypto_esp32.cpp on target.
#include <string.h>

#include "crypto.h"
#include "keydata.h"

namespace ef {

void Aes128Cbc::setKey(const uint8_t key[16], const uint8_t iv[16],
                       Padding padding) {
  memcpy(key_, key, 16);
  memcpy(iv_, iv, 16);
  padding_ = padding;
}

size_t Aes128Cbc::encrypt(const uint8_t *in, size_t len, uint8_t *out) {
  // PKCS7 always pads (even at a block boundary); zero-padding only fills up
  // to the next boundary.
  size_t padded_len =
      (padding_ == PKCS7) ? (len / 16 + 1) * 16 : (len + 15) / 16 * 16;
  uint8_t pad_byte =
      (padding_ == PKCS7) ? (uint8_t)(padded_len - len) : (uint8_t)0x00;
  uint8_t *buf = new uint8_t[padded_len ? padded_len : 16];
  memcpy(buf, in, len);
  memset(buf + len, pad_byte, padded_len - len);

  // fresh IV per frame — the protocol never chains CBC state across frames
  aes128CbcCrypt(true, key_, iv_, buf, padded_len, out);

  delete[] buf;
  return padded_len;
}

size_t Aes128Cbc::decrypt(const uint8_t *in, size_t len, uint8_t *out) {
  // Whole AES blocks only; device firmware drops trailing partial-block bytes
  size_t aligned = len - len % 16;
  if (aligned == 0) {
    memcpy(out, in, len);
    return len;
  }

  aes128CbcCrypt(false, key_, iv_, in, aligned, out);

  if (padding_ == ZERO) return aligned;  // type 1 never unpads

  // Strip PKCS7 only when valid; otherwise return the raw blocks (matches the
  // reference, which tolerates unpadded device payloads).
  uint8_t pad = out[aligned - 1];
  if (pad >= 1 && pad <= 16 && pad <= aligned) {
    for (size_t i = aligned - pad; i < aligned; i++) {
      if (out[i] != pad) return aligned;
    }
    return aligned - pad;
  }
  return aligned;
}

bool gen_session_key(const uint8_t seed[2], const uint8_t srand[16],
                     uint8_t out_key[16]) {
  size_t pos = (size_t)seed[0] * 0x10 + (size_t)((seed[1] - 1) & 0xFF) * 0x100;
  if (pos + 16 > KEYDATA_SIZE) {
    return false;
  }

  uint8_t data[32];
  memcpy(data, &KEYDATA[pos], 16);
  memcpy(data + 16, srand, 16);
  md5(data, sizeof(data), out_key);
  return true;
}

}  // namespace ef
