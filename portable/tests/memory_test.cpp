#include "comicchat/memory.hpp"

#include <catch2/catch_test_macros.hpp>
#include <memory_resource>
#include <string_view>

TEST_CASE("credentials live in explicitly clearable non-copyable storage") {
    auto secret = comicchat::LockedSecret::copy("correct horse battery staple");
    REQUIRE(secret.has_value());
    const auto bytes = secret->view();
    CHECK(std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()} ==
          "correct horse battery staple");
    secret->clear();
    CHECK(secret->view().empty());
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
