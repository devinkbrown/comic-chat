#include "comicchat/net/sts_policy_store.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace comicchat::net {
namespace {

constexpr std::string_view file_header = "comicchat-sts-v1\n";
constexpr std::string_view hexadecimal = "0123456789abcdef";
constexpr unsigned int maximum_temporary_attempts = 32;

auto TemporarySequenceSeed() noexcept -> std::uint64_t {
    const auto wall = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto monotonic = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return static_cast<std::uint64_t>(wall) ^ static_cast<std::uint64_t>(monotonic);
}

// Seeding the process-local sequence prevents residues from an earlier process
// that happened to use the same PID from exhausting the bounded retry loop.
std::atomic_uint64_t temporary_sequence{TemporarySequenceSeed()};

struct ReadResult final {
    std::string bytes;
    bool missing{};
};

struct AtomicWriteResult final {
    std::optional<StsStoreError> error;
    bool replaced{};
};

auto CanonicalHostname(const std::string_view input) -> std::expected<std::string, StsStoreError> {
    if (input.empty() || input.size() > StsPolicyStore::maximum_hostname_bytes) {
        return std::unexpected(StsStoreError::invalid_hostname);
    }
    std::string result;
    result.reserve(input.size());
    for (const char raw : input) {
        const auto byte = static_cast<unsigned char>(raw);
        if (byte <= 0x20U || byte >= 0x7fU || raw == '/' || raw == '\\' || raw == '@' ||
            raw == '?' || raw == '#') {
            return std::unexpected(StsStoreError::invalid_hostname);
        }
        result.push_back(byte >= 'A' && byte <= 'Z'
                             ? static_cast<char>(byte + static_cast<unsigned char>('a' - 'A'))
                             : raw);
    }
    if (result.ends_with('.')) result.pop_back();
    if (result.empty() || result.ends_with('.')) return std::unexpected(StsStoreError::invalid_hostname);
    return result;
}

auto HexEncode(const std::string_view input) -> std::string {
    std::string encoded;
    encoded.reserve(input.size() * 2U);
    for (const char raw : input) {
        const auto byte = static_cast<unsigned char>(raw);
        encoded.push_back(hexadecimal.at(byte >> 4U));
        encoded.push_back(hexadecimal.at(byte & 0x0fU));
    }
    return encoded;
}

auto HexDigit(const char value) -> std::optional<unsigned int> {
    if (value >= '0' && value <= '9') return static_cast<unsigned int>(value - '0');
    if (value >= 'a' && value <= 'f') return static_cast<unsigned int>(value - 'a' + 10);
    return std::nullopt;
}

auto HexDecode(const std::string_view input) -> std::optional<std::string> {
    if (input.empty() || input.size() % 2U != 0 ||
        input.size() > StsPolicyStore::maximum_hostname_bytes * 2U) {
        return std::nullopt;
    }
    std::string decoded;
    decoded.reserve(input.size() / 2U);
    for (std::size_t index = 0; index < input.size(); index += 2U) {
        const auto high = HexDigit(input[index]);
        const auto low = HexDigit(input[index + 1U]);
        if (!high || !low) return std::nullopt;
        decoded.push_back(static_cast<char>((*high << 4U) | *low));
    }
    return decoded;
}

template <typename Integer>
auto ParseInteger(const std::string_view input) -> std::optional<Integer> {
    if (input.empty()) return std::nullopt;
    Integer result{};
    const auto [end, error] = std::from_chars(input.begin(), input.end(), result);
    if (error != std::errc{} || end != input.end()) return std::nullopt;
    return result;
}

auto SplitFields(const std::string_view line) -> std::optional<std::array<std::string_view, 5>> {
    std::array<std::string_view, 5> fields;
    std::size_t cursor{};
    for (std::size_t index = 0; index < fields.size(); ++index) {
        const auto end = line.find('\t', cursor);
        if (index + 1U == fields.size()) {
            if (end != std::string_view::npos) return std::nullopt;
            fields[index] = line.substr(cursor);
            return fields;
        }
        if (end == std::string_view::npos) return std::nullopt;
        fields[index] = line.substr(cursor, end - cursor);
        cursor = end + 1U;
    }
    return std::nullopt;
}

auto ParsePolicies(const std::string_view bytes, const StsTimePoint now,
                   const std::string_view preserved_expired_hostname = {})
    -> std::expected<std::map<std::string, StsPolicyRecord, std::less<>>, StsStoreError> {
    using Policies = std::map<std::string, StsPolicyRecord, std::less<>>;
    if (!bytes.starts_with(file_header)) return std::unexpected(StsStoreError::malformed_file);
    Policies policies;
    std::size_t cursor = file_header.size();
    while (cursor < bytes.size()) {
        const auto newline = bytes.find('\n', cursor);
        if (newline == std::string_view::npos || newline == cursor) {
            return std::unexpected(StsStoreError::malformed_file);
        }
        const auto fields = SplitFields(bytes.substr(cursor, newline - cursor));
        if (!fields) return std::unexpected(StsStoreError::malformed_file);
        const auto decoded = HexDecode((*fields)[0]);
        if (!decoded) return std::unexpected(StsStoreError::malformed_file);
        const auto canonical = CanonicalHostname(*decoded);
        if (!canonical || *canonical != *decoded) {
            return std::unexpected(StsStoreError::malformed_file);
        }
        const auto port = ParseInteger<unsigned int>((*fields)[1]);
        const auto duration = ParseInteger<std::uint64_t>((*fields)[2]);
        const auto expiry = ParseInteger<std::int64_t>((*fields)[3]);
        const auto preload = ParseInteger<unsigned int>((*fields)[4]);
        if (!port || *port == 0 || *port > 65'535U || !duration || *duration == 0 ||
            !expiry || *expiry < 0 || !preload || *preload > 1U) {
            return std::unexpected(StsStoreError::malformed_file);
        }
        if (policies.size() >= StsPolicyStore::maximum_policies) {
            return std::unexpected(StsStoreError::too_many_policies);
        }
        StsPolicyRecord record{
            *canonical,
            static_cast<std::uint16_t>(*port),
            *duration,
            StsTimePoint{std::chrono::seconds{*expiry}},
            *preload != 0,
        };
        if (!policies.emplace(record.hostname, std::move(record)).second) {
            return std::unexpected(StsStoreError::malformed_file);
        }
        cursor = newline + 1U;
    }
    if (cursor != bytes.size()) return std::unexpected(StsStoreError::malformed_file);

    // Expired records loaded at process start cannot belong to a currently
    // connected session, so discarding them is safe and keeps memory bounded.
    std::erase_if(policies, [now, preserved_expired_hostname](const auto& item) {
        return item.second.expires_at <= now && item.first != preserved_expired_hostname;
    });
    return policies;
}

auto SerializePolicies(const std::map<std::string, StsPolicyRecord, std::less<>>& policies)
    -> std::expected<std::string, StsStoreError> {
    if (policies.size() > StsPolicyStore::maximum_policies) {
        return std::unexpected(StsStoreError::too_many_policies);
    }
    std::string bytes{file_header};
    for (const auto& [hostname, policy] : policies) {
        const auto canonical = CanonicalHostname(hostname);
        const auto expiry = policy.expires_at.time_since_epoch().count();
        if (!canonical || *canonical != hostname || policy.secure_port == 0 ||
            policy.duration_seconds == 0 || expiry < 0) {
            return std::unexpected(StsStoreError::invalid_update);
        }
        const auto line = HexEncode(hostname) + '\t' + std::to_string(policy.secure_port) + '\t' +
                          std::to_string(policy.duration_seconds) + '\t' + std::to_string(expiry) + '\t' +
                          (policy.preload ? "1\n" : "0\n");
        if (bytes.size() > StsPolicyStore::maximum_file_bytes ||
            line.size() > StsPolicyStore::maximum_file_bytes - bytes.size()) {
            return std::unexpected(StsStoreError::file_too_large);
        }
        bytes += line;
    }
    return bytes;
}

#if defined(_WIN32)

auto TemporaryPath(const std::filesystem::path& file) -> std::filesystem::path {
    auto temporary = file;
    temporary += L".tmp.";
    temporary += std::to_wstring(GetCurrentProcessId());
    temporary += L".";
    temporary += std::to_wstring(temporary_sequence.fetch_add(1, std::memory_order_relaxed));
    return temporary;
}

class WindowsHandle final {
public:
    explicit WindowsHandle(const HANDLE handle = INVALID_HANDLE_VALUE) : handle_(handle) {}
    ~WindowsHandle() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    WindowsHandle(const WindowsHandle&) = delete;
    auto operator=(const WindowsHandle&) -> WindowsHandle& = delete;
    WindowsHandle(WindowsHandle&& other) noexcept : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
    auto operator=(WindowsHandle&& other) noexcept -> WindowsHandle& {
        if (this == &other) return *this;
        if (handle_ != INVALID_HANDLE_VALUE) (void)CloseHandle(handle_);
        handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        return *this;
    }
    [[nodiscard]] auto get() const noexcept -> HANDLE { return handle_; }
    [[nodiscard]] auto valid() const noexcept -> bool { return handle_ != INVALID_HANDLE_VALUE; }
    auto close() noexcept -> bool {
        if (!valid()) return true;
        const bool result = CloseHandle(handle_) != FALSE;
        handle_ = INVALID_HANDLE_VALUE;
        return result;
    }
private:
    HANDLE handle_;
};

class StoreLock final {
public:
    explicit StoreLock(WindowsHandle handle) : handle_(std::move(handle)) {}
    ~StoreLock() {
        if (!handle_.valid()) return;
        OVERLAPPED range{};
        (void)UnlockFileEx(handle_.get(), 0, 1, 0, &range);
    }
    StoreLock(const StoreLock&) = delete;
    auto operator=(const StoreLock&) -> StoreLock& = delete;
    StoreLock(StoreLock&&) noexcept = default;
    auto operator=(StoreLock&&) noexcept -> StoreLock& = default;

private:
    // Closing this handle remains the fallback release if UnlockFileEx fails.
    WindowsHandle handle_;
};

auto AcquireStoreLock(const std::filesystem::path& file) -> std::expected<StoreLock, StsStoreError> {
    auto lock_file = file;
    lock_file += L".lock";
    WindowsHandle handle{CreateFileW(lock_file.c_str(), GENERIC_READ | GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (!handle.valid()) return std::unexpected(StsStoreError::io_error);

    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &attributes, sizeof(attributes))) {
        return std::unexpected(StsStoreError::io_error);
    }
    if ((attributes.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        return std::unexpected(StsStoreError::unsafe_file);
    }

    OVERLAPPED range{};
    if (!LockFileEx(handle.get(), LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &range)) {
        return std::unexpected(StsStoreError::io_error);
    }
    return StoreLock{std::move(handle)};
}

auto ReadFileBounded(const std::filesystem::path& file) -> std::expected<ReadResult, StsStoreError> {
    WindowsHandle handle{CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (!handle.valid()) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return ReadResult{{}, true};
        return std::unexpected(StsStoreError::io_error);
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
        (attributes.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        return std::unexpected(StsStoreError::unsafe_file);
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle.get(), &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > StsPolicyStore::maximum_file_bytes) {
        return std::unexpected(size.QuadPart > static_cast<LONGLONG>(StsPolicyStore::maximum_file_bytes)
                                   ? StsStoreError::file_too_large
                                   : StsStoreError::io_error);
    }
    std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
    std::size_t offset{};
    while (offset < bytes.size()) {
        const auto remaining = std::span{bytes}.subspan(offset);
        DWORD read{};
        const auto requested = static_cast<DWORD>((std::min)(
            remaining.size(), static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        if (!ReadFile(handle.get(), remaining.data(), requested, &read, nullptr) || read == 0) {
            return std::unexpected(StsStoreError::io_error);
        }
        offset += read;
    }
    return ReadResult{std::move(bytes), false};
}

auto AtomicWriteFile(const std::filesystem::path& file, const std::string_view bytes) -> AtomicWriteResult {
    std::filesystem::path temporary;
    WindowsHandle handle;
    for (unsigned int attempt = 0; attempt < maximum_temporary_attempts; ++attempt) {
        temporary = TemporaryPath(file);
        WindowsHandle candidate{CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                            FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (candidate.valid()) {
            handle = std::move(candidate);
            break;
        }
        const auto error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            return {StsStoreError::io_error, false};
        }
    }
    if (!handle.valid()) return {StsStoreError::io_error, false};
    bool cleanup = true;
    const auto remove_temporary = [&] { if (cleanup) DeleteFileW(temporary.c_str()); };
    std::size_t offset{};
    while (offset < bytes.size()) {
        const auto remaining = std::span{bytes}.subspan(offset);
        DWORD written{};
        const auto requested = static_cast<DWORD>((std::min)(
            remaining.size(), static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        if (!WriteFile(handle.get(), remaining.data(), requested, &written, nullptr) || written == 0) {
            (void)handle.close();
            remove_temporary();
            return {StsStoreError::io_error, false};
        }
        offset += written;
    }
    if (!FlushFileBuffers(handle.get()) || !handle.close()) {
        remove_temporary();
        return {StsStoreError::io_error, false};
    }
    if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        remove_temporary();
        return {StsStoreError::io_error, false};
    }
    cleanup = false;
    return {std::nullopt, true};
}

#else

auto TemporaryPath(const std::filesystem::path& file) -> std::filesystem::path {
    auto temporary = file;
    temporary += ".tmp.";
    temporary += std::to_string(static_cast<std::uintmax_t>(::getpid()));
    temporary += ".";
    temporary += std::to_string(temporary_sequence.fetch_add(1, std::memory_order_relaxed));
    return temporary;
}

class PosixFd final {
public:
    explicit PosixFd(const int fd = -1) : fd_(fd) {}
    ~PosixFd() { if (fd_ >= 0) (void)::close(fd_); }
    PosixFd(const PosixFd&) = delete;
    auto operator=(const PosixFd&) -> PosixFd& = delete;
    PosixFd(PosixFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    auto operator=(PosixFd&& other) noexcept -> PosixFd& {
        if (this == &other) return *this;
        if (fd_ >= 0) (void)::close(fd_);
        fd_ = std::exchange(other.fd_, -1);
        return *this;
    }
    [[nodiscard]] auto get() const noexcept -> int { return fd_; }
    [[nodiscard]] auto valid() const noexcept -> bool { return fd_ >= 0; }
    auto close() noexcept -> bool {
        if (!valid()) return true;
        const int fd = std::exchange(fd_, -1);
        int result;
        do result = ::close(fd); while (result < 0 && errno == EINTR);
        return result == 0;
    }
private:
    int fd_;
};

class StoreLock final {
public:
    explicit StoreLock(PosixFd handle) : handle_(std::move(handle)) {}
    StoreLock(const StoreLock&) = delete;
    auto operator=(const StoreLock&) -> StoreLock& = delete;
    StoreLock(StoreLock&&) noexcept = default;
    auto operator=(StoreLock&&) noexcept -> StoreLock& = default;

private:
    // flock ownership follows this open file description and is released when
    // the descriptor closes, including after process termination.
    PosixFd handle_;
};

constexpr auto LockFlags() -> int {
    // O_NONBLOCK prevents a substituted FIFO/device from hanging before fstat
    // can reject it. flock remains blocking unless LOCK_NB is requested.
    int flags = O_RDWR | O_CREAT | O_NONBLOCK;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#endif
    return flags;
}

auto AcquireStoreLock(const std::filesystem::path& file) -> std::expected<StoreLock, StsStoreError> {
    const auto parent = file.has_parent_path() ? file.parent_path() : std::filesystem::path{"."};
    struct stat parent_status{};
    if (::stat(parent.c_str(), &parent_status) != 0 || !S_ISDIR(parent_status.st_mode) ||
        parent_status.st_uid != ::geteuid() || (parent_status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        return std::unexpected(StsStoreError::unsafe_file);
    }

    auto lock_file = file;
    lock_file += ".lock";
    PosixFd handle{::open(lock_file.c_str(), LockFlags(), S_IRUSR | S_IWUSR)};
    if (!handle.valid()) {
#if defined(ELOOP)
        if (errno == ELOOP) return std::unexpected(StsStoreError::unsafe_file);
#endif
        return std::unexpected(StsStoreError::io_error);
    }
    struct stat status{};
    if (::fstat(handle.get(), &status) != 0) return std::unexpected(StsStoreError::io_error);
    if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid() ||
        (status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        return std::unexpected(StsStoreError::unsafe_file);
    }

    int locked;
    do locked = ::flock(handle.get(), LOCK_EX); while (locked != 0 && errno == EINTR);
    if (locked != 0) return std::unexpected(StsStoreError::io_error);
    return StoreLock{std::move(handle)};
}

constexpr auto ReadFlags() -> int {
    // O_NONBLOCK prevents an attacker-controlled FIFO/device substitution from
    // hanging before fstat can reject every non-regular file.
    int flags = O_RDONLY | O_NONBLOCK;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#endif
    return flags;
}

auto ReadFileBounded(const std::filesystem::path& file) -> std::expected<ReadResult, StsStoreError> {
    PosixFd handle{::open(file.c_str(), ReadFlags())};
    if (!handle.valid()) {
        if (errno == ENOENT) return ReadResult{{}, true};
#if defined(ELOOP)
        if (errno == ELOOP) return std::unexpected(StsStoreError::unsafe_file);
#endif
        return std::unexpected(StsStoreError::io_error);
    }
    struct stat status{};
    if (::fstat(handle.get(), &status) != 0) return std::unexpected(StsStoreError::io_error);
    if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid() ||
        (status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        return std::unexpected(StsStoreError::unsafe_file);
    }
    if (status.st_size < 0 || static_cast<std::uintmax_t>(status.st_size) > StsPolicyStore::maximum_file_bytes) {
        return std::unexpected(status.st_size > 0 ? StsStoreError::file_too_large : StsStoreError::io_error);
    }
    std::string bytes(static_cast<std::size_t>(status.st_size), '\0');
    std::size_t offset{};
    while (offset < bytes.size()) {
        const auto remaining = std::span{bytes}.subspan(offset);
        const auto count = ::read(handle.get(), remaining.data(), remaining.size());
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return std::unexpected(StsStoreError::io_error);
        offset += static_cast<std::size_t>(count);
    }
    return ReadResult{std::move(bytes), false};
}

auto AtomicWriteFile(const std::filesystem::path& file, const std::string_view bytes) -> AtomicWriteResult {
    const auto parent = file.has_parent_path() ? file.parent_path() : std::filesystem::path{"."};
    struct stat parent_status{};
    if (::stat(parent.c_str(), &parent_status) != 0 || !S_ISDIR(parent_status.st_mode) ||
        parent_status.st_uid != ::geteuid() || (parent_status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        return {StsStoreError::unsafe_file, false};
    }
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#endif
    std::filesystem::path temporary;
    PosixFd handle;
    for (unsigned int attempt = 0; attempt < maximum_temporary_attempts; ++attempt) {
        temporary = TemporaryPath(file);
        PosixFd candidate{::open(temporary.c_str(), flags, S_IRUSR | S_IWUSR)};
        if (candidate.valid()) {
            handle = std::move(candidate);
            break;
        }
        if (errno != EEXIST) return {StsStoreError::io_error, false};
    }
    if (!handle.valid()) return {StsStoreError::io_error, false};
    bool cleanup = true;
    const auto remove_temporary = [&] { if (cleanup) (void)::unlink(temporary.c_str()); };
    std::size_t offset{};
    while (offset < bytes.size()) {
        const auto remaining = std::span{bytes}.subspan(offset);
        const auto count = ::write(handle.get(), remaining.data(), remaining.size());
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            (void)handle.close();
            remove_temporary();
            return AtomicWriteResult{StsStoreError::io_error, false};
        }
        offset += static_cast<std::size_t>(count);
    }
    if (::fsync(handle.get()) != 0 || !handle.close()) {
        remove_temporary();
        return {StsStoreError::io_error, false};
    }
    if (::rename(temporary.c_str(), file.c_str()) != 0) {
        remove_temporary();
        return {StsStoreError::io_error, false};
    }
    cleanup = false;

    int directory_flags = O_RDONLY;
#if defined(O_DIRECTORY)
    directory_flags |= O_DIRECTORY;
#endif
#if defined(O_CLOEXEC)
    directory_flags |= O_CLOEXEC;
#endif
    PosixFd directory{::open(parent.c_str(), directory_flags)};
    if (!directory.valid() || ::fsync(directory.get()) != 0 || !directory.close()) {
        return {StsStoreError::io_error, true};
    }
    return {std::nullopt, true};
}

#endif

auto ReadPoliciesSnapshot(const std::filesystem::path& file, const StsTimePoint now,
                          const std::string_view preserved_expired_hostname = {})
    -> std::expected<std::map<std::string, StsPolicyRecord, std::less<>>, StsStoreError> {
    const auto read = ReadFileBounded(file);
    if (!read) return std::unexpected(read.error());
    if (read->missing) return std::map<std::string, StsPolicyRecord, std::less<>>{};
    return ParsePolicies(read->bytes, now, preserved_expired_hostname);
}

auto ExpiryFrom(const StsTimePoint now, const std::uint64_t duration)
    -> std::expected<StsTimePoint, StsStoreError> {
    const auto now_seconds = now.time_since_epoch().count();
    if (now_seconds < 0 || duration == 0 ||
        duration > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)() - now_seconds)) {
        return std::unexpected(StsStoreError::clock_overflow);
    }
    return StsTimePoint{std::chrono::seconds{now_seconds + static_cast<std::int64_t>(duration)}};
}

} // namespace

StsPolicyStore::StsPolicyStore(std::filesystem::path file) : file_(std::move(file)) {}

auto StsPolicyStore::load(const StsTimePoint now) -> std::expected<void, StsStoreError> {
    if (file_.empty()) return std::unexpected(StsStoreError::io_error);
    auto next = ReadPoliciesSnapshot(file_, now);
    if (!next) return std::unexpected(next.error());
    policies_.swap(*next);
    loaded_ = true;
    return {};
}

auto StsPolicyStore::find(const std::string_view requested_hostname, const StsTimePoint now) const
    -> std::expected<std::optional<StsPolicyRecord>, StsStoreError> {
    if (!loaded_) return std::unexpected(StsStoreError::not_loaded);
    const auto hostname = CanonicalHostname(requested_hostname);
    if (!hostname) return std::unexpected(hostname.error());
    const auto found = policies_.find(*hostname);
    if (found == policies_.end() || found->second.expires_at <= now) return std::nullopt;
    return found->second;
}

auto StsPolicyStore::plan(ConnectionOptions requested, const StsTimePoint now) const
    -> std::expected<StsConnectionPlan, StsStoreError> {
    const auto requested_hostname = requested.server_name.empty()
                                        ? std::string_view{requested.endpoint.host}
                                        : std::string_view{requested.server_name};
    const auto policy = find(requested_hostname, now);
    if (!policy) return std::unexpected(policy.error());
    StsConnectionPlan result{std::move(requested), false};
    if (*policy) {
        result.options.security = Security::tls;
        result.options.endpoint.port = (*policy)->secure_port;
        result.enforced = true;
    }
    return result;
}

auto StsPolicyStore::persist(const Policies& policies) -> PersistResult {
    const auto serialized = SerializePolicies(policies);
    if (!serialized) return {serialized.error(), false};
    const auto written = AtomicWriteFile(file_, *serialized);
    return {written.error, written.replaced};
}

auto StsPolicyStore::apply_verified_update(
    const std::string_view requested_hostname,
    const std::uint16_t connected_secure_port,
    const bool tls_verified,
    const GenerationId connection_generation,
    const comic_chat::ircv3::StsPolicyUpdate& update,
    const StsTimePoint now) -> std::expected<std::optional<StsPolicyReceipt>, StsStoreError> {
    if (!loaded_) return std::unexpected(StsStoreError::not_loaded);
    if (!tls_verified || connection_generation == 0 ||
        update.action == comic_chat::ircv3::StsPolicyAction::Upgrade) {
        return std::unexpected(StsStoreError::invalid_update);
    }
    const auto hostname = CanonicalHostname(requested_hostname);
    if (!hostname) return std::unexpected(hostname.error());
    if (connected_secure_port == 0) return std::unexpected(StsStoreError::invalid_port);

    std::optional<StsPolicyRecord> replacement;
    if (update.action == comic_chat::ircv3::StsPolicyAction::Remove) {
        if (update.duration != 0 || update.port != 0 || update.preload) {
            return std::unexpected(StsStoreError::invalid_update);
        }
    } else {
        if (update.action != comic_chat::ircv3::StsPolicyAction::Persist || update.duration == 0 ||
            update.port != 0) {
            return std::unexpected(StsStoreError::invalid_update);
        }
        const auto expiry = ExpiryFrom(now, update.duration);
        if (!expiry) return std::unexpected(expiry.error());
        replacement.emplace(StsPolicyRecord{
            *hostname, connected_secure_port, update.duration, *expiry, update.preload});
    }

    // The companion lock is stable across atomic replacements of the data
    // file. Re-read while holding it so this object never writes a stale
    // process-local snapshot over another process's hostname.
    const auto transaction_lock = AcquireStoreLock(file_);
    if (!transaction_lock) return std::unexpected(transaction_lock.error());
    auto latest = ReadPoliciesSnapshot(file_, now);
    if (!latest) return std::unexpected(latest.error());
    Policies next = std::move(*latest);
    if (replacement) {
        if (!next.contains(*hostname) && next.size() >= maximum_policies) {
            return std::unexpected(StsStoreError::too_many_policies);
        }
        next[*hostname] = std::move(*replacement);
    } else {
        next.erase(*hostname);
    }

    const auto saved = persist(next);
    if (!saved.error || saved.replaced) policies_.swap(next);
    if (saved.error) return std::unexpected(*saved.error);
    if (update.action == comic_chat::ircv3::StsPolicyAction::Persist) {
        return std::optional<StsPolicyReceipt>{
            StsPolicyReceipt{policies_.at(*hostname), connection_generation}};
    }
    return std::nullopt;
}

auto StsPolicyStore::reschedule_on_verified_disconnect(
    const std::string_view requested_hostname,
    const bool tls_verified,
    const GenerationId closing_generation,
    const std::optional<StsPolicyReceipt>& last_persistence_from_this_connection,
    const StsTimePoint now) -> std::expected<void, StsStoreError> {
    if (!loaded_) return std::unexpected(StsStoreError::not_loaded);
    if (!tls_verified || closing_generation == 0) return std::unexpected(StsStoreError::invalid_update);
    const auto hostname = CanonicalHostname(requested_hostname);
    if (!hostname) return std::unexpected(hostname.error());
    if (!last_persistence_from_this_connection) return {};
    if (last_persistence_from_this_connection->hostname_ != *hostname ||
        last_persistence_from_this_connection->duration_seconds_ == 0 ||
        last_persistence_from_this_connection->generation_ != closing_generation) {
        return std::unexpected(StsStoreError::invalid_update);
    }
    const auto expiry = ExpiryFrom(now, last_persistence_from_this_connection->duration_seconds_);
    if (!expiry) return std::unexpected(expiry.error());

    const auto transaction_lock = AcquireStoreLock(file_);
    if (!transaction_lock) return std::unexpected(transaction_lock.error());
    // Preserve only the receipt-bound target past its old expiry. A live TLS
    // session is allowed to rebase that policy at disconnect; unrelated stale
    // entries remain pruned during the transaction reload.
    auto latest = ReadPoliciesSnapshot(file_, now, *hostname);
    if (!latest) return std::unexpected(latest.error());
    const auto found = latest->find(*hostname);
    if (found == latest->end() ||
        found->second.duration_seconds != last_persistence_from_this_connection->duration_seconds_) {
        return std::unexpected(StsStoreError::invalid_update);
    }
    Policies next = std::move(*latest);
    next[*hostname].expires_at = *expiry;
    const auto saved = persist(next);
    if (!saved.error || saved.replaced) policies_.swap(next);
    if (saved.error) return std::unexpected(*saved.error);
    return {};
}

} // namespace comicchat::net
