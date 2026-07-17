#pragma once

#include <cstddef>
#include <span>

namespace comicchat::crypto {

// Initializes the process-wide mbedTLS/PSA runtime exactly once. On Windows
// this also installs the std::mutex-backed MBEDTLS_THREADING_ALT adapter. The
// state intentionally lives until process exit so late static consumers can
// never observe prematurely freed PSA/threading globals.
[[nodiscard]] auto initialize_runtime() noexcept -> bool;

// Fills bytes from the OS-seeded PSA CSPRNG. No output is produced when the
// crypto runtime is unavailable.
[[nodiscard]] auto random_bytes(std::span<std::byte> output) noexcept -> bool;

} // namespace comicchat::crypto
