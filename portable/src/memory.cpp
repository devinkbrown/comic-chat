#include "comicchat/memory.hpp"

#include <cstring>
#include <new>
#include <stdexcept>
#include <utility>

#include <mbedtls/platform_util.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace comicchat {
namespace {

auto lock_pages(void* address, const std::size_t size) noexcept -> bool {
    if (address == nullptr || size == 0) return false;
#if defined(_WIN32)
    return VirtualLock(address, size) != 0;
#else
    return mlock(address, size) == 0;
#endif
}

void unlock_pages(void* address, const std::size_t size) noexcept {
    if (address == nullptr || size == 0) return;
#if defined(_WIN32)
    (void)VirtualUnlock(address, size);
#else
    (void)munlock(address, size);
#endif
}

} // namespace

class LockedSecret::Impl final {
public:
    ~Impl() { clear(); }

    void clear() noexcept {
        if (!bytes.empty()) mbedtls_platform_zeroize(bytes.data(), bytes.size());
        if (locked) unlock_pages(bytes.data(), bytes.size());
        locked = false;
        bytes.clear();
    }

    std::vector<std::byte> bytes;
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
        impl->bytes.resize(value.size());
        if (!value.empty()) std::memcpy(impl->bytes.data(), value.data(), value.size());
        impl->locked = lock_pages(impl->bytes.data(), impl->bytes.size());
        return LockedSecret{std::move(impl)};
    } catch (const std::bad_alloc&) {
        return std::unexpected{SecretError::allocation};
    }
}

auto LockedSecret::view() const noexcept -> std::span<const std::byte> {
    return impl_ ? std::span<const std::byte>{impl_->bytes} : std::span<const std::byte>{};
}
auto LockedSecret::is_locked() const noexcept -> bool { return impl_ && impl_->locked; }
void LockedSecret::clear() noexcept { if (impl_) impl_->clear(); }

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
