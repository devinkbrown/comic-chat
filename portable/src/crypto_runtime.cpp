#include "comicchat/crypto_runtime.hpp"

#include <array>
#include <mutex>
#include <new>

#include <mbedtls/aes.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/ssl.h>
#include <mbedtls/threading.h>
#include <mbedtls/version.h>
#include <psa/crypto.h>

static_assert(MBEDTLS_VERSION_NUMBER == 0x03060700,
              "Comic Chat requires the pinned mbedTLS 3.6.7 ABI");
#if !defined(MBEDTLS_AES_ROM_TABLES) || !defined(MBEDTLS_SSL_CIPHERSUITES) || \
    !defined(MBEDTLS_THREADING_C) || defined(MBEDTLS_SELF_TEST)
#error "Comic Chat requires immutable tables/suites and thread-safe production mbedTLS"
#endif

namespace comicchat::crypto {
namespace {

#if defined(_WIN32)
void windows_mutex_init(mbedtls_threading_mutex_t* mutex) noexcept {
    if (mutex != nullptr) mutex->mutex = new (std::nothrow) std::mutex;
}

void windows_mutex_free(mbedtls_threading_mutex_t* mutex) noexcept {
    if (mutex == nullptr) return;
    delete static_cast<std::mutex*>(mutex->mutex);
    mutex->mutex = nullptr;
}

auto windows_mutex_lock(mbedtls_threading_mutex_t* mutex) noexcept -> int {
    if (mutex == nullptr || mutex->mutex == nullptr) return MBEDTLS_ERR_THREADING_MUTEX_ERROR;
    try {
        static_cast<std::mutex*>(mutex->mutex)->lock();
        return 0;
    } catch (...) {
        return MBEDTLS_ERR_THREADING_MUTEX_ERROR;
    }
}

auto windows_mutex_unlock(mbedtls_threading_mutex_t* mutex) noexcept -> int {
    if (mutex == nullptr || mutex->mutex == nullptr) return MBEDTLS_ERR_THREADING_MUTEX_ERROR;
    try {
        static_cast<std::mutex*>(mutex->mutex)->unlock();
        return 0;
    } catch (...) {
        return MBEDTLS_ERR_THREADING_MUTEX_ERROR;
    }
}
#endif

struct RuntimeState final {
    std::once_flag once;
    bool ready{};
};

auto runtime_state() -> RuntimeState& {
    // Intentionally process-lifetime. Explicit teardown cannot be ordered
    // safely against arbitrary late static crypto consumers, while the OS can
    // reclaim PSA state and Windows alternate mutexes atomically at exit.
    static RuntimeState state;
    return state;
}

} // namespace

auto initialize_runtime() noexcept -> bool {
    auto& state = runtime_state();
    try {
        std::call_once(state.once, [&state] {
#if defined(_WIN32)
            mbedtls_threading_set_alt(windows_mutex_init, windows_mutex_free,
                                      windows_mutex_lock, windows_mutex_unlock);
#endif
            if (mbedtls_ssl_list_ciphersuites() == nullptr || psa_crypto_init() != PSA_SUCCESS) return;
            mbedtls_aes_context aes;
            mbedtls_aes_init(&aes);
            std::array<unsigned char, 16> key{};
            state.ready = mbedtls_aes_setkey_enc(&aes, key.data(), 128) == 0;
            mbedtls_aes_free(&aes);
            mbedtls_platform_zeroize(key.data(), key.size());
        });
    } catch (...) {
        return false;
    }
    return state.ready;
}

auto random_bytes(const std::span<std::byte> output) noexcept -> bool {
    if (!initialize_runtime()) return false;
    return psa_generate_random(reinterpret_cast<std::uint8_t*>(output.data()), output.size()) == PSA_SUCCESS;
}

} // namespace comicchat::crypto
