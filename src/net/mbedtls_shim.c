#include "mbedtls_shim.h"

#include "mbedtls/build_info.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/pk.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/ssl.h"
#include "mbedtls/version.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <wincrypt.h>
#endif

struct cc_tls_context {
    mbedtls_net_context socket;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt ca;
    mbedtls_x509_crt client_cert;
    mbedtls_pk_context client_key;
    int last_error;
    int last_alert;
    int ready;
};

static int load_explicit_ca(mbedtls_x509_crt *ca, const char *path)
{
    int result = mbedtls_x509_crt_parse_file(ca, path);
    return result >= 0 ? 0 : result;
}

#if defined(_WIN32)
static int load_system_ca(mbedtls_x509_crt *ca)
{
    HCERTSTORE store = CertOpenSystemStoreA((HCRYPTPROV_LEGACY) 0, "ROOT");
    PCCERT_CONTEXT certificate = NULL;
    int loaded = 0;
    if (store == NULL) {
        return MBEDTLS_ERR_X509_FILE_IO_ERROR;
    }
    while ((certificate = CertEnumCertificatesInStore(store, certificate)) != NULL) {
        if (mbedtls_x509_crt_parse_der(ca, certificate->pbCertEncoded,
                                       certificate->cbCertEncoded) == 0) {
            loaded++;
        }
    }
    CertCloseStore(store, 0);
    return loaded > 0 ? 0 : MBEDTLS_ERR_X509_CERT_UNKNOWN_FORMAT;
}
#else
static int load_system_ca(mbedtls_x509_crt *ca)
{
    static const char *const files[] = {
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/ssl/ca-bundle.pem",
        "/etc/ssl/cert.pem",
        "/usr/local/share/certs/ca-root-nss.crt"
    };
    size_t index;
    for (index = 0; index < sizeof(files) / sizeof(files[0]); index++) {
        int result = mbedtls_x509_crt_parse_file(ca, files[index]);
        if (result >= 0) {
            return 0;
        }
    }
    {
        int result = mbedtls_x509_crt_parse_path(ca, "/etc/ssl/certs");
        return result >= 0 ? 0 : result;
    }
}
#endif

static void initialize_context(cc_tls_context *context)
{
    mbedtls_net_init(&context->socket);
    mbedtls_entropy_init(&context->entropy);
    mbedtls_ctr_drbg_init(&context->drbg);
    mbedtls_ssl_init(&context->ssl);
    mbedtls_ssl_config_init(&context->config);
    mbedtls_x509_crt_init(&context->ca);
    mbedtls_x509_crt_init(&context->client_cert);
    mbedtls_pk_init(&context->client_key);
}

static int configure_context(cc_tls_context *context, const char *host,
                             const char *ca_file, const char *client_cert_file,
                             int *native_error)
{
    static const unsigned char personalization[] = "comicchat-mbedtls-3.6.6";
    int result;

    result = psa_crypto_init();
    if (result != PSA_SUCCESS) {
        *native_error = result;
        return CC_TLS_PSA;
    }
    result = mbedtls_ctr_drbg_seed(&context->drbg, mbedtls_entropy_func,
                                   &context->entropy, personalization,
                                   sizeof(personalization) - 1);
    if (result != 0) {
        *native_error = result;
        return CC_TLS_ENTROPY;
    }
    result = ca_file != NULL && ca_file[0] != '\0'
        ? load_explicit_ca(&context->ca, ca_file)
        : load_system_ca(&context->ca);
    if (result != 0) {
        *native_error = result;
        return CC_TLS_CA;
    }
    result = mbedtls_ssl_config_defaults(&context->config,
                                         MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
    if (result != 0) {
        *native_error = result;
        return CC_TLS_CONFIG;
    }
    mbedtls_ssl_conf_rng(&context->config, mbedtls_ctr_drbg_random, &context->drbg);
    mbedtls_ssl_conf_ca_chain(&context->config, &context->ca, NULL);
    mbedtls_ssl_conf_authmode(&context->config, MBEDTLS_SSL_VERIFY_REQUIRED);
    if (client_cert_file != NULL && client_cert_file[0] != '\0') {
        /* mbedTLS 3.6.6 client-auth records currently trigger a TLS 1.3
         * decode_error in the live Onyx Server listener. TLS 1.2 remains a
         * verified, server-supported path for certificate-bound EXTERNAL. */
        mbedtls_ssl_conf_max_tls_version(&context->config,
                                         MBEDTLS_SSL_VERSION_TLS1_2);
        result = mbedtls_x509_crt_parse_file(&context->client_cert, client_cert_file);
        if (result < 0) {
            *native_error = result;
            return CC_TLS_CLIENT_AUTH;
        }
        result = mbedtls_pk_parse_keyfile(&context->client_key, client_cert_file,
                                          NULL, mbedtls_ctr_drbg_random,
                                          &context->drbg);
        if (result != 0) {
            *native_error = result;
            return CC_TLS_CLIENT_AUTH;
        }
        result = mbedtls_ssl_conf_own_cert(&context->config,
                                           &context->client_cert,
                                           &context->client_key);
        if (result != 0) {
            *native_error = result;
            return CC_TLS_CLIENT_AUTH;
        }
    }
    result = mbedtls_ssl_setup(&context->ssl, &context->config);
    if (result != 0) {
        *native_error = result;
        return CC_TLS_CONFIG;
    }
    result = mbedtls_ssl_set_hostname(&context->ssl, host);
    if (result != 0) {
        *native_error = result;
        return CC_TLS_HOSTNAME;
    }
    return CC_TLS_OK;
}

static int finish_handshake(cc_tls_context *context, uint32_t timeout_ms,
                            cc_tls_context **out_context, int *native_error)
{
    int result;
    mbedtls_ssl_conf_read_timeout(&context->config, timeout_ms);
    mbedtls_ssl_set_bio(&context->ssl, &context->socket, mbedtls_net_send,
                        mbedtls_net_recv, mbedtls_net_recv_timeout);
    do {
        result = mbedtls_ssl_handshake(&context->ssl);
    } while (result == MBEDTLS_ERR_SSL_WANT_READ ||
             result == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (result != 0) {
        *native_error = result;
        return mbedtls_ssl_get_verify_result(&context->ssl) != 0
            ? CC_TLS_VERIFY
            : CC_TLS_HANDSHAKE;
    }
    if (mbedtls_ssl_get_verify_result(&context->ssl) != 0) {
        *native_error = MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
        return CC_TLS_VERIFY;
    }
    mbedtls_ssl_conf_read_timeout(&context->config, 0);
    context->ready = 1;
    *out_context = context;
    return CC_TLS_OK;
}

int cc_tls_connect(const char *host, const char *port, const char *ca_file,
                   const char *client_cert_file,
                   cc_tls_context **out_context, int *native_error)
{
    cc_tls_context *context;
    int result;
    int stage = CC_TLS_OK;

    if (out_context == NULL || native_error == NULL || host == NULL || port == NULL) {
        return CC_TLS_CONFIG;
    }
    *out_context = NULL;
    *native_error = 0;
    context = (cc_tls_context *) calloc(1, sizeof(*context));
    if (context == NULL) {
        return CC_TLS_ALLOC;
    }
    initialize_context(context);
    stage = configure_context(context, host, ca_file, client_cert_file, native_error);
    if (stage != CC_TLS_OK) goto fail;
    result = mbedtls_net_connect(&context->socket, host, port, MBEDTLS_NET_PROTO_TCP);
    if (result != 0) {
        *native_error = result;
        stage = CC_TLS_NETWORK;
        goto fail;
    }
    stage = finish_handshake(context, 15000, out_context, native_error);
    if (stage == CC_TLS_OK) return CC_TLS_OK;

fail:
    cc_tls_free(context);
    return stage;
}

int cc_tls_connect_fd(const char *host, intptr_t socket_fd, const char *ca_file,
                      const char *client_cert_file,
                      uint32_t handshake_timeout_ms,
                      cc_tls_context **out_context, int *native_error)
{
    cc_tls_context *context;
    int stage;
    if (out_context == NULL || native_error == NULL || host == NULL || socket_fd < 0) {
        if (socket_fd >= 0) {
            mbedtls_net_context owned_socket;
            mbedtls_net_init(&owned_socket);
            owned_socket.fd = (int) socket_fd;
            mbedtls_net_free(&owned_socket);
        }
        return CC_TLS_CONFIG;
    }
    *out_context = NULL;
    *native_error = 0;
    context = (cc_tls_context *) calloc(1, sizeof(*context));
    if (context == NULL) {
        mbedtls_net_context owned_socket;
        mbedtls_net_init(&owned_socket);
        owned_socket.fd = (int) socket_fd;
        mbedtls_net_free(&owned_socket);
        return CC_TLS_ALLOC;
    }
    initialize_context(context);
    context->socket.fd = (int) socket_fd;
    stage = configure_context(context, host, ca_file, client_cert_file, native_error);
    if (stage == CC_TLS_OK) {
        stage = finish_handshake(context, handshake_timeout_ms,
                                 out_context, native_error);
    }
    if (stage == CC_TLS_OK) return CC_TLS_OK;
    cc_tls_free(context);
    return stage;
}

void cc_tls_free(cc_tls_context *context)
{
    if (context == NULL) {
        return;
    }
    if (context->ready) {
        (void) mbedtls_ssl_close_notify(&context->ssl);
    }
    mbedtls_net_free(&context->socket);
    mbedtls_ssl_free(&context->ssl);
    mbedtls_ssl_config_free(&context->config);
    mbedtls_x509_crt_free(&context->ca);
    mbedtls_x509_crt_free(&context->client_cert);
    mbedtls_pk_free(&context->client_key);
    mbedtls_ctr_drbg_free(&context->drbg);
    mbedtls_entropy_free(&context->entropy);
    mbedtls_platform_zeroize(context, sizeof(*context));
    free(context);
}

int cc_tls_fd(const cc_tls_context *context)
{
    return context == NULL ? -1 : context->socket.fd;
}

int cc_tls_write(cc_tls_context *context, const unsigned char *bytes, size_t length)
{
    int result;
    size_t bounded = length > (size_t) INT_MAX ? (size_t) INT_MAX : length;
    do {
        result = mbedtls_ssl_write(&context->ssl, bytes, bounded);
    } while (result == MBEDTLS_ERR_SSL_WANT_READ ||
             result == MBEDTLS_ERR_SSL_WANT_WRITE);
    return result;
}

static int read_with_timeout(cc_tls_context *context, unsigned char *bytes,
                             size_t length, uint32_t milliseconds)
{
    int result;
    size_t bounded = length > (size_t) INT_MAX ? (size_t) INT_MAX : length;
    mbedtls_ssl_conf_read_timeout(&context->config, milliseconds);
    do {
        result = mbedtls_ssl_read(&context->ssl, bytes, bounded);
    } while (result == MBEDTLS_ERR_SSL_WANT_READ ||
             result == MBEDTLS_ERR_SSL_WANT_WRITE);
    mbedtls_ssl_conf_read_timeout(&context->config, 0);
    context->last_error = result < 0 ? result : 0;
    context->last_alert = result == MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE
        ? context->ssl.MBEDTLS_PRIVATE(in_msg)[1]
        : 0;
    if (result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        return 0;
    }
    return result;
}

int cc_tls_read(cc_tls_context *context, unsigned char *bytes, size_t length)
{
    return read_with_timeout(context, bytes, length, 0);
}

int cc_tls_read_timeout(cc_tls_context *context, unsigned char *bytes, size_t length,
                        uint32_t milliseconds)
{
    return read_with_timeout(context, bytes, length, milliseconds);
}

int cc_tls_is_timeout(int result)
{
    return result == MBEDTLS_ERR_SSL_TIMEOUT;
}

int cc_tls_last_error(const cc_tls_context *context)
{
    return context == NULL ? 0 : context->last_error;
}

int cc_tls_last_alert(const cc_tls_context *context)
{
    return context == NULL ? 0 : context->last_alert;
}

void cc_tls_error_string(int result, char *buffer, size_t length)
{
    mbedtls_strerror(result, buffer, length);
}

uint32_t cc_tls_version_number(void)
{
    return MBEDTLS_VERSION_NUMBER;
}
