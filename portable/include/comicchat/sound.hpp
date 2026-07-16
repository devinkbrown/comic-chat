#pragma once

#include "comicchat/cpp26.hpp"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace comicchat::sound {

enum class Format {
    wave,
    midi,
    rmi,
};

enum class ResolveError {
    empty_name,
    name_too_long,
    control_character,
    rooted_path,
    path_separator,
    traversal,
    reserved_name,
    trailing_dot_or_space,
    unsupported_extension,
    invalid_root,
    not_found,
    not_regular_file,
    outside_root,
};

struct ValidatedName final {
    std::string value;
    Format format{Format::wave};
};

struct ResolvedSound final {
    std::filesystem::path path;
    std::string display_name;
    Format format{Format::wave};
};

[[nodiscard]] auto validate_name(std::string_view remote_name)
    -> std::expected<ValidatedName, ResolveError>;

// This performs filesystem work and must be called from a UI/worker context,
// never from the network callback. The returned path is canonical and was
// verified to remain within the configured sound root at resolution time.
[[nodiscard]] auto resolve(
    const std::filesystem::path& sound_root,
    std::string_view remote_name) -> std::expected<ResolvedSound, ResolveError>;

} // namespace comicchat::sound
