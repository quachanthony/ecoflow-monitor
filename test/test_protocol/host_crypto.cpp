// Native (host) implementations of the platform crypto primitives so the
// portable protocol code can run in `pio test -e native`. Uses macOS
// CommonCrypto; on other hosts, port to OpenSSL or a vendored implementation.
#ifdef __APPLE__

#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonDigest.h>

#include "../../src/ef/crypto.h"

namespace ef {

void md5(const uint8_t *data, size_t len, uint8_t out[16]) {
  CC_MD5(data, (CC_LONG)len, out);
}

void aes128CbcCrypt(bool encrypt, const uint8_t key[16], const uint8_t iv[16],
                    const uint8_t *in, size_t len, uint8_t *out) {
  size_t moved = 0;
  CCCrypt(encrypt ? kCCEncrypt : kCCDecrypt, kCCAlgorithmAES, 0 /* no padding */,
          key, kCCKeySizeAES128, iv, in, len, out, len, &moved);
}

}  // namespace ef

#else
#error "Host crypto shim currently supports macOS (CommonCrypto) only"
#endif
