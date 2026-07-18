#include "comicchat/expression.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <optional>
#include <string_view>

namespace {

using comicchat::emotion_from_wheel_point;
using comicchat::emotions_from_text;
using comicchat::EmotionOpts;
namespace wheel = comicchat::emotion_wheel;

// Locate the option matching a target angle (exact-equality candidates share the
// authored constant, so a tight tolerance is enough).
[[nodiscard]] auto option_for(const EmotionOpts& opts, const double angle) -> std::optional<EmotionOpts::Opt> {
    for (const auto& opt : opts.opts) {
        if (std::fabs(opt.angle - angle) < 1e-9) return opt;
    }
    return std::nullopt;
}

[[nodiscard]] auto has_emotion(const EmotionOpts& opts, const double angle) -> bool {
    return option_for(opts, angle).has_value();
}

} // namespace

TEST_CASE("emotions_from_text infers the signature emoticons", "[expression]") {
    SECTION(":) smiles (happy)") {
        REQUIRE(has_emotion(emotions_from_text(":)"), wheel::happy));
    }
    SECTION(":( frowns (sad)") {
        REQUIRE(has_emotion(emotions_from_text(":("), wheel::sad));
    }
    SECTION("HELLO!!! shouts") {
        REQUIRE(has_emotion(emotions_from_text("HELLO!!!"), wheel::shout));
    }
    SECTION("LOL laughs") {
        REQUIRE(has_emotion(emotions_from_text("LOL"), wheel::laugh));
    }
    SECTION(";) is coy") {
        REQUIRE(has_emotion(emotions_from_text(";)"), wheel::coy));
    }
    SECTION("Hi there waves") {
        REQUIRE(has_emotion(emotions_from_text("Hi there"), wheel::wave));
    }
    SECTION("You there? points at the other") {
        REQUIRE(has_emotion(emotions_from_text("You there?"), wheel::point_other));
    }
}

TEST_CASE("emotions_from_text applies rule details faithfully", "[expression]") {
    SECTION("HELLO!!! matches both the AllCaps and !!! rules for shout") {
        const auto opts = emotions_from_text("HELLO!!!");
        const auto shout = option_for(opts, wheel::shout);
        REQUIRE(shout.has_value());
        // AllCaps and FindString(\"!!!\") both carry strength 9 and dedupe.
        REQUIRE(shout->priority == 9);
    }

    SECTION("CheckStart is anchored, not a substring: internal 'I' does not point-self") {
        // "team" contains no sentence-initial "I"; only whole-word/anchored hits count.
        const auto opts = emotions_from_text("the team ships");
        REQUIRE_FALSE(has_emotion(opts, wheel::point_self));
    }

    SECTION("CheckStart iterates every sentence, not just the first") {
        // The wave trigger begins the SECOND sentence; per-sentence iteration
        // must reach it (textpose.cpp:300-314).
        const auto opts = emotions_from_text("Nice weather. Hi everyone");
        REQUIRE(has_emotion(opts, wheel::wave));
    }

    SECTION("case-insensitive '*' rules match lowercase input") {
        REQUIRE(has_emotion(emotions_from_text("rotfl that was great"), wheel::laugh));
    }

    SECTION("a lone capital is not shouting") {
        // CheckForUppers needs more than one uppercase letter (textpose.cpp:32).
        REQUIRE_FALSE(has_emotion(emotions_from_text("A dog"), wheel::shout));
    }
}

TEST_CASE("EmotionOpts::add dedupes by angle keeping the max priority", "[expression]") {
    EmotionOpts opts;
    opts.add(wheel::happy, 0.5, 3);
    opts.add(wheel::happy, 0.9, 7); // higher priority overrides
    opts.add(wheel::happy, 0.1, 2); // lower priority ignored

    REQUIRE(opts.opts.size() == 1);
    REQUIRE(opts.opts[0].priority == 7);
    REQUIRE(opts.opts[0].intensity == Catch::Approx(0.9));
}

TEST_CASE("EmotionOpts::add honours the MAXEMOPTS cap", "[expression]") {
    EmotionOpts opts;
    for (int i = 0; i < 15; ++i) {
        opts.add(static_cast<double>(i) + 0.5, 1.0, i + 1); // distinct angles
    }
    REQUIRE(opts.opts.size() == EmotionOpts::max_opts);
}

TEST_CASE("emotion_from_wheel_point snaps the centre detente to neutral", "[expression]") {
    SECTION("inside the 0.2 detente -> neutral") {
        const auto emotion = emotion_from_wheel_point(0.1, 0.0, 10.0);
        REQUIRE(emotion.intensity == Catch::Approx(0.0));
        REQUIRE(emotion.angle == Catch::Approx(0.0));
    }
    SECTION("radius edge -> full intensity along the happy axis") {
        const auto emotion = emotion_from_wheel_point(10.0, 0.0, 10.0);
        REQUIRE(emotion.intensity == Catch::Approx(1.0));
        REQUIRE(emotion.angle == Catch::Approx(wheel::happy));
    }
    SECTION("straight up maps to atan2(dy, dx)") {
        const auto emotion = emotion_from_wheel_point(0.0, 5.0, 10.0);
        REQUIRE(emotion.intensity == Catch::Approx(0.5));
        REQUIRE(emotion.angle == Catch::Approx(std::atan2(5.0, 0.0)));
    }
}
