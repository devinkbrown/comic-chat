#pragma once

// build_info.h includes this after the upstream 3.6 LTS configuration.
// Immutable AES tables avoid the default lazy global writer when independent
// TLS contexts initialize concurrently.
#define MBEDTLS_AES_ROM_TABLES

// Avoid mbedTLS's lazy, writable default-suite cache. Keep a modern immutable
// client/server set for TLS 1.3 and authenticated forward-secret TLS 1.2.
#define MBEDTLS_SSL_CIPHERSUITES                                      \
    MBEDTLS_TLS1_3_AES_256_GCM_SHA384,                               \
    MBEDTLS_TLS1_3_CHACHA20_POLY1305_SHA256,                         \
    MBEDTLS_TLS1_3_AES_128_GCM_SHA256,                               \
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,                 \
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,                   \
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,           \
    MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,             \
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,                 \
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256

// PSA Crypto owns process-global key-slot state.  Enable the upstream locking
// layer so independent ConnectionEngine instances can handshake concurrently.
#define MBEDTLS_THREADING_C
#if defined(_WIN32)
#define MBEDTLS_THREADING_ALT
#else
#define MBEDTLS_THREADING_PTHREAD
#endif

// The upstream self-test-only ECP operation counter is intentionally global
// and is not thread-safe. It is not part of Comic Chat's production surface.
#undef MBEDTLS_SELF_TEST
