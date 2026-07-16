#include "comicchat/sound.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace comicchat::sound {
namespace {

constexpr std::size_t maximum_remote_name_bytes = 128;

auto ascii_lower(std::string_view value) -> std::string {
    std::string result{value};
    std::ranges::transform(result, result.begin(), [](const unsigned char byte) {
        return static_cast<char>(std::tolower(byte));
    });
    return result;
}

auto ascii_upper(std::string_view value) -> std::string {
    std::string result{value};
    std::ranges::transform(result, result.begin(), [](const unsigned char byte) {
        return static_cast<char>(std::toupper(byte));
    });
    return result;
}

auto reserved_dos_stem(std::string_view stem) -> bool {
    const auto name = ascii_upper(stem);
    if (name == "CON" || name == "PRN" || name == "AUX" || name == "NUL" ||
        name == "CLOCK$" || name == "CONIN$" || name == "CONOUT$") {
        return true;
    }
    if (name.size() == 4 && (name.starts_with("COM") || name.starts_with("LPT")) &&
        name[3] >= '1' && name[3] <= '9') {
        return true;
    }
    if ((name.starts_with("COM") || name.starts_with("LPT")) && name.size() == 5) {
        const auto suffix = std::string_view{name}.substr(3);
        if (suffix == "\xC2\xB9" || suffix == "\xC2\xB2" || suffix == "\xC2\xB3")
            return true;
    }
    return false;
}

auto component_equal(const std::filesystem::path& left, const std::filesystem::path& right) -> bool {
#if defined(_WIN32)
    return ascii_lower(left.string()) == ascii_lower(right.string());
#else
    return left == right;
#endif
}

auto contained_by(
    const std::filesystem::path& candidate,
    const std::filesystem::path& root) -> bool {
    auto candidate_component = candidate.begin();
    for (auto root_component = root.begin(); root_component != root.end(); ++root_component) {
        if (candidate_component == candidate.end() ||
            !component_equal(*candidate_component, *root_component)) {
            return false;
        }
        ++candidate_component;
    }
    return true;
}

} // namespace

auto validate_name(const std::string_view remote_name)
    -> std::expected<ValidatedName, ResolveError> {
    if (remote_name.empty()) return std::unexpected{ResolveError::empty_name};
    if (remote_name.size() > maximum_remote_name_bytes) {
        return std::unexpected{ResolveError::name_too_long};
    }
    if (std::ranges::any_of(remote_name, [](const unsigned char byte) {
            return byte < 0x20U || byte == 0x7fU;
        })) {
        return std::unexpected{ResolveError::control_character};
    }
    if (remote_name.front() == '/' || remote_name.front() == '\\' ||
        (remote_name.size() >= 2 && std::isalpha(static_cast<unsigned char>(remote_name[0])) &&
            remote_name[1] == ':')) {
        return std::unexpected{ResolveError::rooted_path};
    }
    if (remote_name.contains('/') || remote_name.contains('\\') || remote_name.contains(':')) {
        return std::unexpected{ResolveError::path_separator};
    }
    if (remote_name == "." || remote_name == ".." || remote_name.contains("..")) {
        return std::unexpected{ResolveError::traversal};
    }
    if (remote_name.back() == '.' || remote_name.back() == ' ') {
        return std::unexpected{ResolveError::trailing_dot_or_space};
    }

    const auto dot = remote_name.find('.');
    const auto stem = remote_name.substr(0, dot);
    if (!stem.empty() && (stem.back() == '.' || stem.back() == ' ')) {
        return std::unexpected{ResolveError::trailing_dot_or_space};
    }
    if (stem.empty() || reserved_dos_stem(stem)) {
        return std::unexpected{ResolveError::reserved_name};
    }
    const auto extension_dot = remote_name.rfind('.');
    if (extension_dot == std::string_view::npos || extension_dot + 1 == remote_name.size()) {
        return std::unexpected{ResolveError::unsupported_extension};
    }
    const auto extension = ascii_lower(remote_name.substr(extension_dot + 1));
    Format format{};
    if (extension == "wav") format = Format::wave;
    else if (extension == "mid" || extension == "midi") format = Format::midi;
    else if (extension == "rmi") format = Format::rmi;
    else return std::unexpected{ResolveError::unsupported_extension};
    return ValidatedName{std::string{remote_name}, format};
}

auto resolve(
    const std::filesystem::path& sound_root,
    const std::string_view remote_name) -> std::expected<ResolvedSound, ResolveError> {
    auto validated = validate_name(remote_name);
    if (!validated) return std::unexpected{validated.error()};

    std::error_code error;
    const auto canonical_root = std::filesystem::canonical(sound_root, error);
    if (error || !std::filesystem::is_directory(canonical_root, error) || error) {
        return std::unexpected{ResolveError::invalid_root};
    }
    const auto unresolved = canonical_root / std::filesystem::path{validated->value};
    const auto status = std::filesystem::symlink_status(unresolved, error);
    if (error || status.type() == std::filesystem::file_type::not_found) {
        return std::unexpected{ResolveError::not_found};
    }
    const auto canonical_candidate = std::filesystem::canonical(unresolved, error);
    if (error) return std::unexpected{ResolveError::not_found};
    if (!contained_by(canonical_candidate, canonical_root)) {
        return std::unexpected{ResolveError::outside_root};
    }
    if (!std::filesystem::is_regular_file(canonical_candidate, error) || error) {
        return std::unexpected{ResolveError::not_regular_file};
    }
    return ResolvedSound{
        .path = canonical_candidate,
        .display_name = validated->value,
        .format = validated->format,
    };
}

} // namespace comicchat::sound
