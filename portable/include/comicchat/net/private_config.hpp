#pragma once

#include "comicchat/cpp26.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string_view>

namespace comicchat::net {

enum class PrivateConfigError : std::uint8_t {
    invalid_root,
    invalid_filename,
    platform_unavailable,
    create_failed,
    unsafe_directory,
};

// Frontends which already know their platform's trusted per-user configuration
// root can use this seam without relying on process-global HOME state. The
// application directory is created private to the current user and is rejected
// if an existing path is a link/reparse point or is exposed on Unix.
[[nodiscard]] auto private_config_file_from_root(
    const std::filesystem::path& trusted_per_user_root,
    std::string_view filename) -> std::expected<std::filesystem::path, PrivateConfigError>;

// The Windows implementation obtains FOLDERID_LocalAppData from the shell and
// creates Comic Chat Reinked's private application directory below it. Unix and
// BSD frontends must supply their native/XDG root through the explicit seam
// above; this function deliberately never guesses HOME.
[[nodiscard]] auto native_private_config_file(std::string_view filename)
    -> std::expected<std::filesystem::path, PrivateConfigError>;

} // namespace comicchat::net
