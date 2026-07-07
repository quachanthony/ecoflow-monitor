// Crypto primitives for the EcoFlow BLE handshake (see PROTOCOL.md):
//   - ECDH on SECP160r1 (via vendored micro-ecc; mbedTLS lacks this curve)
//   - AES-128-CBC with PKCS7 padding ("Type 7" session encryption)
//   - MD5 (IV + session-key derivation, auth payload)
// Reference: ha-ef-ble eflib/connection.py, eflib/encryption.py
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ef {

// --- Platform primitives: crypto_esp32.cpp on target (mbedTLS), the test
// --- harness on native builds. Everything else in this header is portable
// --- and covered by the host-side tests.

// MD5 of `len` bytes into out[16].
void md5(const uint8_t *data, size_t len, uint8_t out[16]);

// Raw AES-128-CBC over `len` bytes (must be a multiple of 16), no padding.
void aes128CbcCrypt(bool encrypt, const uint8_t key[16], const uint8_t iv[16],
                    const uint8_t *in, size_t len, uint8_t *out);

// Session-key derivation: MD5 over (keydata[pos:pos+16] || srand[0:16]) where
// pos = seed[0]*0x10 + ((seed[1]-1) & 0xFF)*0x100.
// Returns false if pos would read past the keydata table.
bool gen_session_key(const uint8_t seed[2], const uint8_t srand[16],
                     uint8_t out_key[16]);

// AES-128-CBC session cipher. Every frame is encrypted independently: the
// cipher restarts from the stored IV on each call (the reference creates a
// fresh AES context per packet — the IV never chains across frames).
// Padding follows the encrypt_type: type 7 uses PKCS7 both ways, type 1 uses
// zero-padding on encrypt and no unpadding on decrypt.
class Aes128Cbc {
 public:
  enum Padding { PKCS7, ZERO };

  void setKey(const uint8_t key[16], const uint8_t iv[16],
              Padding padding = PKCS7);

  // Encrypt with the configured padding. out must hold len + 16 bytes;
  // returns the padded ciphertext length.
  size_t encrypt(const uint8_t *in, size_t len, uint8_t *out);

  // Decrypt whole AES blocks only (trailing partial-block bytes are dropped,
  // matching device firmware). In PKCS7 mode, padding is stripped when it is
  // valid. out must hold len bytes; returns the plaintext length.
  size_t decrypt(const uint8_t *in, size_t len, uint8_t *out);

 private:
  uint8_t key_[16];
  uint8_t iv_[16];
  Padding padding_ = PKCS7;
};

// ECDH key exchange on SECP160r1: 40-byte raw public keys (X||Y, 20+20),
// 20-byte shared secret (X coordinate). Matches python-ecdsa's wire sizes.
class EcdhSecp160r1 {
 public:
  static constexpr size_t PUBLIC_KEY_SIZE = 40;
  static constexpr size_t PRIVATE_KEY_SIZE = 21;  // order is 161 bits
  static constexpr size_t SHARED_SECRET_SIZE = 20;

  bool generateKeypair();
  const uint8_t *publicKey() const { return public_key_; }
  bool computeSharedSecret(const uint8_t peer_public[PUBLIC_KEY_SIZE],
                           uint8_t out_secret[SHARED_SECRET_SIZE]) const;

 private:
  uint8_t public_key_[PUBLIC_KEY_SIZE];
  uint8_t private_key_[PRIVATE_KEY_SIZE];
};

}  // namespace ef
