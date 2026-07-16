#pragma once

// mbedTLS includes this C-compatible definition when its Windows build uses
// MBEDTLS_THREADING_ALT.  The implementation is installed by
// connection_engine.cpp before any mbedTLS context is initialized.
typedef struct mbedtls_threading_mutex_t {
    void* mutex;
} mbedtls_threading_mutex_t;
