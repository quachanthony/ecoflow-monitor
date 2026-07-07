/* NimBLE-Arduino bundles tinycrypt, whose ECC exports the same uECC_* symbol
 * names as micro-ecc. Prefix this vendored copy's public API so both can be
 * linked into one firmware image. (Included from uECC.h, so callers keep
 * using the normal uECC_* names.) */
#ifndef _UECC_RENAME_H_
#define _UECC_RENAME_H_

#define uECC_set_rng                ef_uECC_set_rng
#define uECC_get_rng                ef_uECC_get_rng
#define uECC_curve_private_key_size ef_uECC_curve_private_key_size
#define uECC_curve_public_key_size  ef_uECC_curve_public_key_size
#define uECC_secp160r1              ef_uECC_secp160r1
#define uECC_secp192r1              ef_uECC_secp192r1
#define uECC_secp224r1              ef_uECC_secp224r1
#define uECC_secp256r1              ef_uECC_secp256r1
#define uECC_secp256k1              ef_uECC_secp256k1
#define uECC_make_key               ef_uECC_make_key
#define uECC_shared_secret          ef_uECC_shared_secret
#define uECC_valid_public_key       ef_uECC_valid_public_key
#define uECC_compute_public_key     ef_uECC_compute_public_key
#define uECC_sign                   ef_uECC_sign
#define uECC_sign_deterministic     ef_uECC_sign_deterministic
#define uECC_verify                 ef_uECC_verify
#define uECC_compress               ef_uECC_compress
#define uECC_decompress             ef_uECC_decompress

#endif /* _UECC_RENAME_H_ */
