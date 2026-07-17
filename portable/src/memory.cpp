#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include "comicchat/memory.hpp"

#include <atomic>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

#include <mbedtls/platform_util.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace comicchat {
namespace {

std::atomic_bool fail_next_lock{};

auto page_size() noexcept -> std::size_t {
#if defined(_WIN32)
    SYSTEM_INFO information{};
    GetSystemInfo(&information);
    return information.dwPageSize;
#else
    const auto size = ::sysconf(_SC_PAGESIZE);
    return size > 0 ? static_cast<std::size_t>(size) : 0;
#endif
}

auto rounded_page_size(const std::size_t requested) noexcept -> std::optional<std::size_t> {
    const auto page = page_size();
    const auto minimum = std::max<std::size_t>(requested, 1);
    if (page == 0 || minimum > std::numeric_limits<std::size_t>::max() - (page - 1)) return std::nullopt;
    return ((minimum + page - 1) / page) * page;
}

auto allocate_pages(const std::size_t size) noexcept -> void* {
#if defined(_WIN32)
    return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    auto* memory = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return memory == MAP_FAILED ? nullptr : memory;
#endif
}

auto lock_pages(void* address, const std::size_t size) noexcept -> bool {
    if (fail_next_lock.exchange(false, std::memory_order_relaxed)) return false;
#if defined(_WIN32)
    return VirtualLock(address, size) != 0;
#else
#if defined(MADV_DONTDUMP)
    (void)::madvise(address, size, MADV_DONTDUMP);
#endif
    return ::mlock(address, size) == 0;
#endif
}

void free_pages(void* address, const std::size_t size, const bool locked) noexcept {
    if (address == nullptr || size == 0) return;
    mbedtls_platform_zeroize(address, size);
#if defined(_WIN32)
    if (locked) (void)VirtualUnlock(address, size);
    (void)VirtualFree(address, 0, MEM_RELEASE);
#else
    if (locked) (void)::munlock(address, size);
    (void)::munmap(address, size);
#endif
}

} // namespace

class LockedSecret::Impl final {
public:
    ~Impl() { clear(); }

    void clear() noexcept {
        free_pages(address, allocation_size, locked);
        address = nullptr;
        used_size = 0;
        allocation_size = 0;
        locked = false;
    }

    void* address{};
    std::size_t used_size{};
    std::size_t allocation_size{};
    bool locked{};
};

LockedSecret::LockedSecret() = default;
LockedSecret::LockedSecret(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
LockedSecret::~LockedSecret() = default;
LockedSecret::LockedSecret(LockedSecret&&) noexcept = default;
auto LockedSecret::operator=(LockedSecret&&) noexcept -> LockedSecret& = default;

auto LockedSecret::copy(const std::string_view value) -> std::expected<LockedSecret, SecretError> {
    try {
        auto impl = std::make_unique<Impl>();
        const auto allocation_size = rounded_page_size(value.size());
        if (!allocation_size) return std::unexpected{SecretError::invalid_size};
        impl->address = allocate_pages(*allocation_size);
        if (impl->address == nullptr) return std::unexpected{SecretError::allocation};
        impl->allocation_size = *allocation_size;
        impl->used_size = value.size();
        if (!value.empty()) std::memcpy(impl->address, value.data(), value.size());
        impl->locked = lock_pages(impl->address, impl->allocation_size);
        if (!impl->locked) return std::unexpected{SecretError::lock_failed};
        return LockedSecret{std::move(impl)};
    } catch (const std::bad_alloc&) {
        return std::unexpected{SecretError::allocation};
    }
}

auto LockedSecret::view() const noexcept -> std::span<const std::byte> {
    if (!impl_ || impl_->address == nullptr) return {};
    return {static_cast<const std::byte*>(impl_->address), impl_->used_size};
}
auto LockedSecret::is_locked() const noexcept -> bool { return impl_ && impl_->address != nullptr && impl_->locked; }
void LockedSecret::clear() noexcept { if (impl_) impl_->clear(); }

namespace testing {
void fail_next_secret_lock() noexcept { fail_next_lock.store(true, std::memory_order_relaxed); }
} // namespace testing

class FrameArena::Impl final {
public:
    explicit Impl(const std::size_t requested_capacity)
        : buffer(requested_capacity), arena(buffer.data(), buffer.size(), std::pmr::null_memory_resource()) {
        if (requested_capacity == 0) throw std::invalid_argument{"frame arena capacity must be positive"};
    }

    std::vector<std::byte> buffer;
    std::pmr::monotonic_buffer_resource arena;
};

FrameArena::FrameArena(const std::size_t capacity) : impl_{std::make_unique<Impl>(capacity)} {}
FrameArena::~FrameArena() = default;
auto FrameArena::resource() noexcept -> std::pmr::memory_resource* { return &impl_->arena; }
auto FrameArena::capacity() const noexcept -> std::size_t { return impl_->buffer.size(); }
void FrameArena::reset() noexcept { impl_->arena.release(); }

RenderSnapshot::RenderSnapshot(const net::GenerationId generation, std::vector<RenderPrimitive> primitives)
    : generation_{generation}, primitives_{std::move(primitives)} {}
auto RenderSnapshot::generation() const noexcept -> net::GenerationId { return generation_; }
auto RenderSnapshot::primitives() const noexcept -> std::span<const RenderPrimitive> { return primitives_; }

RenderBatchBuilder::RenderBatchBuilder(FrameArena& arena, const std::size_t maximum_primitives)
    : maximum_{maximum_primitives}, primitives_{arena.resource()} {
    primitives_.reserve(maximum_);
}

auto RenderBatchBuilder::push(const RenderPrimitive primitive) -> std::expected<void, BatchError> {
    if (primitives_.size() >= maximum_) return std::unexpected{BatchError::full};
    try {
        primitives_.push_back(primitive);
        return {};
    } catch (const std::bad_alloc&) {
        return std::unexpected{BatchError::allocation};
    }
}

auto RenderBatchBuilder::finalize(const net::GenerationId generation)
    -> std::expected<std::shared_ptr<const RenderSnapshot>, BatchError> {
    try {
        std::vector<RenderPrimitive> stable{primitives_.begin(), primitives_.end()};
        return std::shared_ptr<const RenderSnapshot>{new RenderSnapshot{generation, std::move(stable)}};
    } catch (const std::bad_alloc&) {
        return std::unexpected{BatchError::allocation};
    }
}

auto RenderBatchBuilder::size() const noexcept -> std::size_t { return primitives_.size(); }

} // namespace comicchat
