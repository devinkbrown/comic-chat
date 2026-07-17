#include "comicchat/net/private_config.hpp"

#include <cctype>
#include <cerrno>
#include <filesystem>
#include <string>
#include <system_error>

#if defined(_WIN32)
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#include <knownfolders.h>
#include <objbase.h>
#include <shlobj.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace comicchat::net {
namespace {

constexpr std::string_view application_directory = "Comic Chat Reinked";

auto ValidFilename(const std::string_view filename) noexcept -> bool {
    if (filename.empty() || filename.size() > 64U || filename.front() == '.') return false;
    for (const char raw : filename) {
        const auto byte = static_cast<unsigned char>(raw);
        if (!(std::isalnum(byte) != 0 || byte == '.' || byte == '-' || byte == '_')) return false;
    }
    return filename != "." && filename != "..";
}

#if defined(_WIN32)

auto PrepareDirectory(const std::filesystem::path& directory)
    -> std::expected<void, PrivateConfigError> {
    if (!CreateDirectoryW(directory.c_str(), nullptr)) {
        const auto error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS) {
            return std::unexpected(PrivateConfigError::create_failed);
        }
    }
    const auto attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return std::unexpected(PrivateConfigError::create_failed);
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return std::unexpected(PrivateConfigError::unsafe_directory);
    }
    return {};
}

#else

auto PrepareDirectory(const std::filesystem::path& directory)
    -> std::expected<void, PrivateConfigError> {
    if (::mkdir(directory.c_str(), S_IRWXU) != 0 && errno != EEXIST) {
        return std::unexpected(PrivateConfigError::create_failed);
    }
    struct stat status {};
    if (::lstat(directory.c_str(), &status) != 0) {
        return std::unexpected(PrivateConfigError::create_failed);
    }
    if (!S_ISDIR(status.st_mode) || S_ISLNK(status.st_mode) || status.st_uid != ::geteuid() ||
        (status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        return std::unexpected(PrivateConfigError::unsafe_directory);
    }
    return {};
}

#endif

} // namespace

auto private_config_file_from_root(
    const std::filesystem::path& trusted_per_user_root,
    const std::string_view filename) -> std::expected<std::filesystem::path, PrivateConfigError> {
    if (trusted_per_user_root.empty() || !trusted_per_user_root.is_absolute()) {
        return std::unexpected(PrivateConfigError::invalid_root);
    }
    if (!ValidFilename(filename)) {
        return std::unexpected(PrivateConfigError::invalid_filename);
    }
    const auto directory = trusted_per_user_root / std::filesystem::path{application_directory};
    const auto prepared = PrepareDirectory(directory);
    if (!prepared) return std::unexpected(prepared.error());
    return directory / std::filesystem::path{std::string{filename}};
}

auto native_private_config_file(const std::string_view filename)
    -> std::expected<std::filesystem::path, PrivateConfigError> {
    if (!ValidFilename(filename)) {
        return std::unexpected(PrivateConfigError::invalid_filename);
    }
#if defined(_WIN32)
    PWSTR raw_root{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw_root)) ||
        raw_root == nullptr) {
        if (raw_root != nullptr) CoTaskMemFree(raw_root);
        return std::unexpected(PrivateConfigError::platform_unavailable);
    }
    const std::filesystem::path root{raw_root};
    CoTaskMemFree(raw_root);
    return private_config_file_from_root(root, filename);
#else
    return std::unexpected(PrivateConfigError::platform_unavailable);
#endif
}

} // namespace comicchat::net
