#pragma once

#include "comicchat/cpp26.hpp"
#include "comicchat/net/connection_engine.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

namespace comicchat {

enum class SecretError { allocation, invalid_size, lock_failed };

class LockedSecret final {
public:
    static auto copy(std::string_view value) -> std::expected<LockedSecret, SecretError>;
    LockedSecret();
    ~LockedSecret();
    LockedSecret(const LockedSecret&) = delete;
    auto operator=(const LockedSecret&) -> LockedSecret& = delete;
    LockedSecret(LockedSecret&&) noexcept;
    auto operator=(LockedSecret&&) noexcept -> LockedSecret&;

    [[nodiscard]] auto view() const noexcept -> std::span<const std::byte>;
    [[nodiscard]] auto is_locked() const noexcept -> bool;
    void clear() noexcept;

private:
    class Impl;
    explicit LockedSecret(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

class FrameArena final {
public:
    explicit FrameArena(std::size_t capacity = 256U * 1024U);
    ~FrameArena();
    FrameArena(const FrameArena&) = delete;
    auto operator=(const FrameArena&) -> FrameArena& = delete;

    [[nodiscard]] auto resource() noexcept -> std::pmr::memory_resource*;
    [[nodiscard]] auto capacity() const noexcept -> std::size_t;
    void reset() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

enum class PrimitiveKind : std::uint8_t { solid, image, glyph_run };

struct RenderPrimitive final {
    PrimitiveKind kind{};
    std::uint8_t flags{};
    std::uint16_t texture{};
    float left{};
    float top{};
    float right{};
    float bottom{};
    std::uint32_t color{0xffffffffU};
    std::uint32_t payload_offset{};
};

class RenderSnapshot final {
public:
    [[nodiscard]] auto generation() const noexcept -> net::GenerationId;
    [[nodiscard]] auto primitives() const noexcept -> std::span<const RenderPrimitive>;

private:
    friend class RenderBatchBuilder;
    RenderSnapshot(net::GenerationId generation, std::vector<RenderPrimitive> primitives);
    net::GenerationId generation_{};
    std::vector<RenderPrimitive> primitives_;
};

enum class BatchError { full, allocation };

class RenderBatchBuilder final {
public:
    RenderBatchBuilder(FrameArena& arena, std::size_t maximum_primitives);
    [[nodiscard]] auto push(RenderPrimitive primitive) -> std::expected<void, BatchError>;
    [[nodiscard]] auto finalize(net::GenerationId generation)
        -> std::expected<std::shared_ptr<const RenderSnapshot>, BatchError>;
    [[nodiscard]] auto size() const noexcept -> std::size_t;

private:
    std::size_t maximum_{};
    std::pmr::vector<RenderPrimitive> primitives_;
};

} // namespace comicchat

namespace comicchat::testing {

// One-shot fault injection used by the native memory/connection tests.
void fail_next_secret_lock() noexcept;

} // namespace comicchat::testing
