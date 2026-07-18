#pragma once

// Text -> emotion inference engine, ported from Microsoft Comic Chat's
// "Comic View" auto-expression feature. The signature behaviour "type :) and
// your avatar smiles" is driven entirely by the pure text rules transcribed
// here from the shipped resource table (chat.rc:2289-2303) and the matching
// logic in textpose.cpp:26-314. The wheel-point helper mirrors
// CBodyCam::GetEmotionFromPoint (bodycam.cpp:409-419) for a future pointer UI.
//
// This header is deliberately asset-free so the whole inference pipeline is
// headless-testable; pose resolution (avatar_assets.hpp) consumes the
// EmotionOpts this produces.

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace comicchat {

// Mirrors CEmotion (avatar.h:59-66). `angle` is in radians on the emotion
// wheel; gesture sentinels (> 2*pi, see emotion_wheel below) are kept as literal
// magic magnitudes exactly as the source stores them in a float. `intensity` is
// the 0..1 radial fraction.
struct Emotion final {
    double angle{};
    double intensity{};
};

// Named wheel angles and gesture sentinels (avatar.h:326-346). The eight wheel
// emotions are evenly spaced around a full turn; the gesture sentinels are
// out-of-band magnitudes the pose selector matches by exact equality rather
// than nearest-angle.
namespace emotion_wheel {

inline constexpr double pi = 3.14159265358979323846;

inline constexpr double happy = 0.0 * 2.0 * pi / 8.0;
inline constexpr double coy = 1.0 * 2.0 * pi / 8.0;
inline constexpr double bored = 2.0 * 2.0 * pi / 8.0;
inline constexpr double scared = 3.0 * 2.0 * pi / 8.0;
inline constexpr double sad = 4.0 * 2.0 * pi / 8.0;
inline constexpr double angry = 5.0 * 2.0 * pi / 8.0;
inline constexpr double shout = 6.0 * 2.0 * pi / 8.0;
inline constexpr double laugh = 7.0 * 2.0 * pi / 8.0;
inline constexpr double neutral = 0.0;

inline constexpr double wave = 1001.0;
inline constexpr double point_other = 1002.0;
inline constexpr double point_self = 1003.0;
inline constexpr double double_point = 1004.0;
inline constexpr double shrug = 1005.0;

// Angles above this are gesture sentinels, never wheel emotions. Mirrors the
// `emotion <= 2*PI` branch guard in GetBodyIndexFromEmotion (avatar.cpp:333).
inline constexpr double gesture_threshold = 2.0 * pi;

} // namespace emotion_wheel

// Weighted set of candidate emotions produced from a line of text, mirroring
// CEmotionOpts (avatar.h:68-81). Capped at MAXEMOPTS entries; add() dedupes by
// exact angle keeping the maximum priority (OVERRIDEBYPRIORITY, avatar.cpp:728).
struct EmotionOpts final {
    static constexpr std::size_t max_opts = 10; // MAXEMOPTS (avatar.h:68)

    struct Opt final {
        double angle{};
        double intensity{};
        int priority{};
    };

    std::vector<Opt> opts;

    void add(double angle, double intensity, int priority);
};

// A single transcribed rule. `arg` is stored in its authored case; when
// `case_insensitive` is set (the `*` variants in chat.rc) both text and arg are
// ASCII-lowercased before matching. Mirrors the RegisterRule dispatch
// (textpose.cpp:248-265).
struct EmotionRule final {
    enum class Kind { all_caps, find_string, check_word, check_start };

    Kind kind{};
    std::string arg;
    double angle{};
    int strength{};
    bool case_insensitive{};
};

// The shipped rule table (chat.rc:2289-2303). ANGRY/SCARED/BORED ship empty and
// are omitted. Rule name -> wheel angle / gesture sentinel per textpose.cpp:19-24.
[[nodiscard]] auto default_emotion_rules() -> std::vector<EmotionRule>;

// Infer weighted emotions from a line of text, porting GetEmotionsFromString
// (textpose.cpp:271-314): AllCaps over the whole string, FindString / CheckWord
// over the whole string, and CheckStart iterated per sentence.
[[nodiscard]] auto emotions_from_text(std::string_view text, std::span<const EmotionRule> rules) -> EmotionOpts;

// Convenience overload using default_emotion_rules().
[[nodiscard]] auto emotions_from_text(std::string_view text) -> EmotionOpts;

// Map a wheel pointer offset (dx, dy from the bulls-eye) at a given radius to an
// Emotion, porting CBodyCam::GetEmotionFromPoint (bodycam.cpp:409-419): the
// intensity is the clamped radial fraction with a 0.2 centre detente that snaps
// to neutral; the angle is atan2(dy, dx) (vector_to_angle, vector2d.cpp:74).
[[nodiscard]] auto emotion_from_wheel_point(double dx, double dy, double radius) -> Emotion;

} // namespace comicchat
