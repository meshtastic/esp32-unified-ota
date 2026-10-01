/*
 * mbedTLS / TF-PSA-Crypto user config, included at the end of ESP-IDF's
 * mbedtls/esp_config.h via MBEDTLS_USER_CONFIG_FILE (set in CMakeLists.txt).
 *
 * In ESP-IDF 6.x all crypto goes through PSA, whose core references every
 * enabled algorithm, so anything left enabled is linked even if unused.
 * esp_config.h doesn't map all of them to Kconfig; this file turns off the rest
 * (keyed to the Kconfig option that should have controlled each one) so the
 * loader fits the 640 KiB ota_1 partition.
 *
 * Still needed: WPA2-PSK STA (HMAC/PBKDF2-SHA1, AES-ECB for key wrap and the
 * supplicant's own CMAC), NimBLE LE Secure Connections (AES-ECB, AES-CMAC, P-256
 * ECDH) and the OTA SHA-256. AES and CMAC run on the ESP hardware drivers.
 *
 * No include guard on purpose: these overrides must follow every inclusion of
 * esp_config.h. The build doesn't track this header (it's included through a
 * macro), so clean the build after editing it.
 */

/* RSA: CONFIG_MBEDTLS_RSA_C=n drops the key types, but esp_config.h leaves
 * PSA_WANT_ALG_RSA_PSS on, which check_crypto_config.h rejects. */
#ifndef CONFIG_MBEDTLS_RSA_C
#undef PSA_WANT_ALG_RSA_PSS
#undef PSA_WANT_ALG_RSA_OAEP
#undef PSA_WANT_ALG_RSA_PKCS1V15_CRYPT
#undef PSA_WANT_ALG_RSA_PKCS1V15_SIGN
#endif

/* Finite-field DH isn't mapped to Kconfig at all */
#ifndef CONFIG_MBEDTLS_DHM_C
#undef PSA_WANT_ALG_FFDH
#undef PSA_WANT_KEY_TYPE_DH_PUBLIC_KEY
#undef PSA_WANT_KEY_TYPE_DH_KEY_PAIR_BASIC
#undef PSA_WANT_KEY_TYPE_DH_KEY_PAIR_IMPORT
#undef PSA_WANT_KEY_TYPE_DH_KEY_PAIR_EXPORT
#undef PSA_WANT_KEY_TYPE_DH_KEY_PAIR_GENERATE
#undef PSA_WANT_DH_RFC7919_2048
#undef PSA_WANT_DH_RFC7919_3072
#undef PSA_WANT_DH_RFC7919_4096
#undef PSA_WANT_DH_RFC7919_6144
#undef PSA_WANT_DH_RFC7919_8192
#endif

/* SHA-224 follows CONFIG_MBEDTLS_SHA256_C, not CONFIG_MBEDTLS_SHA224_C; on
 * targets without SHA-224 hardware it links the software SHA-256 */
#ifndef CONFIG_MBEDTLS_SHA224_C
#undef PSA_WANT_ALG_SHA_224
#endif

/* SHAKE isn't mapped to Kconfig; it belongs with SHA-3 */
#ifndef CONFIG_MBEDTLS_SHA3_C
#undef PSA_WANT_ALG_SHAKE128
#undef PSA_WANT_ALG_SHAKE256
#endif

/* TLS 1.2 key derivations are only used by TLS */
#ifndef CONFIG_MBEDTLS_TLS_ENABLED
#undef PSA_WANT_ALG_TLS12_PRF
#undef PSA_WANT_ALG_TLS12_PSK_TO_MS
#endif

/* No persistent PSA keys (Wi-Fi and the OTA hash use volatile keys only), so
 * skip the NVS-backed key store, as IDF does when ITS isn't available */
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C

/* PBKDF2 with an AES-CMAC PRF is for Thread; Wi-Fi uses PBKDF2-HMAC-SHA1 */
#undef PSA_WANT_ALG_PBKDF2_AES_CMAC_PRF_128

#ifdef CONFIG_MBEDTLS_HARDWARE_AES
/* The ESP AES driver handles ECB/CBC/CTR (one-shot and multi-part) and the ESP
 * CMAC driver CMAC, but esp_config.h never marks the AES key type or ECB as
 * accelerated, so TF-PSA also builds software AES and the cipher layer. */
#define MBEDTLS_PSA_ACCEL_KEY_TYPE_AES
#define MBEDTLS_PSA_ACCEL_ALG_ECB_NO_PADDING
/* esp_config.h un-accelerates these again; nothing here uses them, and any
 * soft AES mode would bring software AES back */
#undef PSA_WANT_ALG_CBC_PKCS7
#ifndef CONFIG_MBEDTLS_CCM_C
#undef PSA_WANT_ALG_CCM_STAR_NO_TAG
#endif
#endif
