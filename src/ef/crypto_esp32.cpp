// ESP32 implementations of the crypto primitives, backed by the mbedTLS build
// bundled with the Arduino core plus vendored micro-ecc for SECP160r1.
#ifdef ARDUINO

#include <esp_random.h>
#include <mbedtls/aes.h>
#include <mbedtls/md5.h>
#include <string.h>
#include <uECC.h>

#include "crypto.h"

namespace ef {

void md5(const uint8_t *data, size_t len, uint8_t out[16]) {
  // the mbedTLS bundled with arduino-esp32 exports only the _ret variants
  mbedtls_md5_context ctx;
  mbedtls_md5_init(&ctx);
  mbedtls_md5_starts_ret(&ctx);
  mbedtls_md5_update_ret(&ctx, data, len);
  mbedtls_md5_finish_ret(&ctx, out);
  mbedtls_md5_free(&ctx);
}

void aes128CbcCrypt(bool encrypt, const uint8_t key[16], const uint8_t iv[16],
                    const uint8_t *in, size_t len, uint8_t *out) {
  uint8_t iv_copy[16];
  memcpy(iv_copy, iv, 16);

  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  if (encrypt) {
    mbedtls_aes_setkey_enc(&ctx, key, 128);
  } else {
    mbedtls_aes_setkey_dec(&ctx, key, 128);
  }
  mbedtls_aes_crypt_cbc(&ctx, encrypt ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT,
                        len, iv_copy, in, out);
  mbedtls_aes_free(&ctx);
}

static int uecc_rng(uint8_t *dest, unsigned size) {
  esp_fill_random(dest, size);
  return 1;
}

bool EcdhSecp160r1::generateKeypair() {
  uECC_set_rng(uecc_rng);
  return uECC_make_key(public_key_, private_key_, uECC_secp160r1()) == 1;
}

bool EcdhSecp160r1::computeSharedSecret(
    const uint8_t peer_public[PUBLIC_KEY_SIZE],
    uint8_t out_secret[SHARED_SECRET_SIZE]) const {
  if (uECC_valid_public_key(peer_public, uECC_secp160r1()) != 1) {
    return false;
  }
  return uECC_shared_secret(peer_public, private_key_, out_secret,
                            uECC_secp160r1()) == 1;
}

}  // namespace ef

#endif  // ARDUINO
