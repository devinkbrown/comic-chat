#ifndef COMICCHAT_MBEDTLS_SHIM_H
#define COMICCHAT_MBEDTLS_SHIM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cc_tls_context cc_tls_context;

enum cc_tls_stage {
    CC_TLS_OK = 0,
    CC_TLS_ALLOC = 1,
    CC_TLS_PSA = 2,
    CC_TLS_ENTROPY = 3,
    CC_TLS_CA = 4,
    CC_TLS_CONFIG = 5,
    CC_TLS_HOSTNAME = 6,
    CC_TLS_NETWORK = 7,
    CC_TLS_HANDSHAKE = 8,
    CC_TLS_VERIFY = 9
};

int cc_tls_connect(const char *host, const char *port, const char *ca_file,
                   cc_tls_context **out_context, int *native_error);
/* Takes ownership of an already-connected stream socket, including on error. */
int cc_tls_connect_fd(const char *host, intptr_t socket_fd, const char *ca_file,
                      uint32_t handshake_timeout_ms,
                      cc_tls_context **out_context, int *native_error);
void cc_tls_free(cc_tls_context *context);
int cc_tls_fd(const cc_tls_context *context);
int cc_tls_write(cc_tls_context *context, const unsigned char *bytes, size_t length);
int cc_tls_read(cc_tls_context *context, unsigned char *bytes, size_t length);
int cc_tls_read_timeout(cc_tls_context *context, unsigned char *bytes, size_t length,
                        uint32_t milliseconds);
int cc_tls_is_timeout(int result);
void cc_tls_error_string(int result, char *buffer, size_t length);
uint32_t cc_tls_version_number(void);

#ifdef __cplusplus
}
#endif

#endif
