#include "comicchat/text.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("ICU normalization is strict and deterministic") {
    const auto normalized = comicchat::normalize_utf8_nfc("Cafe\xCC\x81");
    REQUIRE(normalized.has_value());
    CHECK(*normalized == "Caf\xC3\xA9");

    const char invalid[]{static_cast<char>(0xc3), static_cast<char>(0x28)};
    CHECK_FALSE(comicchat::normalize_utf8_nfc(std::string_view{invalid, 2}).has_value());
}

TEST_CASE("FreeType and HarfBuzz shape Unicode text") {
    const auto font = comicchat::find_portable_comic_font();
    REQUIRE(font.has_value());
    auto engine = comicchat::TextEngine::create(*font);
    REQUIRE(engine.has_value());
    const auto glyphs = (*engine)->shape("Comic Chat \xE2\x98\x85", 28.0);
    REQUIRE(glyphs.has_value());
    CHECK(glyphs->size() >= 11);
    CHECK(glyphs->front().x_advance > 0.0);
}
