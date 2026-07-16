#include "comicchat/assets.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string_view>

TEST_CASE("zlib assets inflate within an explicit bound") {
    constexpr std::array<std::byte, 18> compressed{
        std::byte{0x78}, std::byte{0x9c}, std::byte{0x73}, std::byte{0xce}, std::byte{0xcf},
        std::byte{0xcd}, std::byte{0x4c}, std::byte{0x56}, std::byte{0x70}, std::byte{0xce},
        std::byte{0x48}, std::byte{0x2c}, std::byte{0x01}, std::byte{0x00}, std::byte{0x13},
        std::byte{0x42}, std::byte{0x03}, std::byte{0x8c},
    };
    const auto inflated = comicchat::inflate_asset(compressed, 64);
    INFO("inflate error " << (inflated ? -1 : static_cast<int>(inflated.error())));
    REQUIRE(inflated.has_value());
    const auto text = std::string_view{reinterpret_cast<const char*>(inflated->data()), inflated->size()};
    CHECK(text == "Comic Chat");
    CHECK_FALSE(comicchat::inflate_asset(compressed, 4).has_value());
}
