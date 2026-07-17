#include "comicchat/memory.hpp"

#include <catch2/catch_test_macros.hpp>
#include <memory_resource>
#include <string_view>

TEST_CASE("credentials live in explicitly clearable non-copyable storage") {
    auto secret = comicchat::LockedSecret::copy("correct horse battery staple");
    REQUIRE(secret.has_value());
    CHECK(secret->is_locked());
    const auto bytes = secret->view();
    CHECK(std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()} ==
          "correct horse battery staple");
    secret->clear();
    CHECK(secret->view().empty());
    CHECK_FALSE(secret->is_locked());
}

TEST_CASE("locked secrets preserve native ownership across moves and fail closed") {
    auto source = comicchat::LockedSecret::copy("move-only");
    REQUIRE(source);
    auto moved = std::move(*source);
    CHECK(source->view().empty());
    CHECK(moved.is_locked());
    CHECK(std::string_view{reinterpret_cast<const char*>(moved.view().data()), moved.view().size()} == "move-only");

    comicchat::testing::fail_next_secret_lock();
    const auto rejected = comicchat::LockedSecret::copy("must-lock");
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error() == comicchat::SecretError::lock_failed);
}

TEST_CASE("shared locked secrets lease the original pages without copying bytes") {
    auto unique = comicchat::LockedSecret::copy("shared-without-copy");
    REQUIRE(unique);
    const auto* original = unique->view().data();

    auto shared = std::move(*unique).share();
    REQUIRE(shared);
    CHECK(unique->view().empty());
    CHECK(shared->is_locked());
    CHECK(shared->view().data() == original);

    const auto second_owner = *shared;
    CHECK(second_owner.view().data() == original);
    CHECK(std::string_view{reinterpret_cast<const char*>(second_owner.view().data()), second_owner.view().size()} ==
          "shared-without-copy");
}

TEST_CASE("shared locked secret allocation failure retains unique zeroizing ownership") {
    auto unique = comicchat::LockedSecret::copy("fail-closed-share");
    REQUIRE(unique);
    comicchat::testing::fail_next_secret_share();
    const auto rejected = std::move(*unique).share();
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error() == comicchat::SecretError::allocation);
    CHECK(unique->is_locked());
    unique->clear();
    CHECK(unique->view().empty());
}

TEST_CASE("frame PMR arena fails closed at its memory bound") {
    comicchat::FrameArena arena{128};
    {
        std::pmr::vector<std::byte> bytes{arena.resource()};
        bytes.resize(64);
        CHECK_THROWS_AS(bytes.resize(1024), std::bad_alloc);
    }
    arena.reset();
    CHECK(arena.capacity() == 128);
}

TEST_CASE("render batches are bounded contiguous immutable snapshots") {
    comicchat::FrameArena arena{4096};
    comicchat::RenderBatchBuilder builder{arena, 2};
    REQUIRE(builder.push({comicchat::PrimitiveKind::solid, 0, 0, 0, 0, 10, 10, 0xff000000U, 0}).has_value());
    REQUIRE(builder.push({comicchat::PrimitiveKind::image, 0, 1, 10, 0, 20, 10, 0xffffffffU, 4}).has_value());
    CHECK_FALSE(builder.push({}).has_value());
    const auto snapshot = builder.finalize(12);
    REQUIRE(snapshot.has_value());
    CHECK((*snapshot)->generation() == 12);
    CHECK((*snapshot)->primitives().size() == 2);
    CHECK((*snapshot)->primitives().data() + 1 == &(*snapshot)->primitives()[1]);
}
