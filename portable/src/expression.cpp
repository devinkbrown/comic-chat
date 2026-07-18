#include "comicchat/expression.hpp"

#include <algorithm>
#include <cmath>

// Port of Microsoft Comic Chat's text-to-emotion inference. Citations point at
// the reference sources under v2.5-beta-1-modern/: textpose.cpp (matchers +
// GetEmotionsFromString), avatar.cpp / avatar.h (CEmotionOpts), chat.rc (rule
// data), bodycam.cpp / vector2d.cpp (wheel point math).

namespace comicchat {
namespace {

// ASCII classifiers matching the C-locale behaviour the source relies on for
// its all-ASCII rule arguments (cc_islower/cc_isupper/... in textpose.cpp).
[[nodiscard]] auto is_upper(const char ch) noexcept -> bool { return ch >= 'A' && ch <= 'Z'; }
[[nodiscard]] auto is_lower(const char ch) noexcept -> bool { return ch >= 'a' && ch <= 'z'; }
[[nodiscard]] auto is_digit(const char ch) noexcept -> bool { return ch >= '0' && ch <= '9'; }
[[nodiscard]] auto is_alnum(const char ch) noexcept -> bool { return is_upper(ch) || is_lower(ch) || is_digit(ch); }
[[nodiscard]] auto is_space(const char ch) noexcept -> bool {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\v' || ch == '\f' || ch == '\r';
}
[[nodiscard]] auto is_punct(const char ch) noexcept -> bool {
    const auto uch = static_cast<unsigned char>(ch);
    return uch > 32U && uch < 127U && !is_alnum(ch);
}

[[nodiscard]] auto to_lower(const char ch) noexcept -> char {
    return is_upper(ch) ? static_cast<char>(ch - 'A' + 'a') : ch;
}

[[nodiscard]] auto to_lower(const std::string_view text) -> std::string {
    std::string lowered;
    lowered.reserve(text.size());
    for (const auto ch : text) lowered.push_back(to_lower(ch));
    return lowered;
}

// CheckForUppers (textpose.cpp:26-35): bail on the first lowercase letter, and
// otherwise require more than one uppercase letter so an isolated capital (or a
// digits/punctuation-only line) is not treated as shouting.
[[nodiscard]] auto check_for_uppers(const std::string_view text) noexcept -> bool {
    int uppers = 0;
    for (const auto ch : text) {
        if (is_lower(ch)) return false;
        if (is_upper(ch)) ++uppers;
    }
    return uppers > 1;
}

// CheckWord (textpose.cpp:37-49): `sub` occurs as a whole word — it starts the
// string or follows whitespace, and is followed by end-of-string, whitespace,
// or punctuation.
[[nodiscard]] auto check_word(const std::string_view text, const std::string_view sub) noexcept -> bool {
    if (sub.empty()) return false;
    for (auto pos = text.find(sub); pos != std::string_view::npos; pos = text.find(sub, pos + 1U)) {
        const auto starts_word = pos == 0U || is_space(text[pos - 1U]);
        if (!starts_word) continue;
        const auto after_index = pos + sub.size();
        const auto after = after_index < text.size() ? text[after_index] : '\0';
        if (after == '\0' || is_space(after) || is_punct(after)) return true;
    }
    return false;
}

// GetNextSentenceStart (textpose.cpp:99-104): advance to the first word after
// the next sentence terminator. Returns npos when no terminator remains; may
// return text.size() when the terminator is trailing.
[[nodiscard]] auto next_sentence_start(const std::string_view text, const std::size_t from) noexcept -> std::size_t {
    auto pos = text.find_first_of(".!?", from);
    if (pos == std::string_view::npos) return std::string_view::npos;
    while (pos < text.size() && (is_punct(text[pos]) || is_space(text[pos]))) ++pos;
    return pos;
}

// StartCompare2 (textpose.cpp:267-269): the sentence beginning at `offset`
// matches `arg` and the following character is not alphanumeric (so "I" does
// not match "Internet").
[[nodiscard]] auto start_compare(const std::string_view text, const std::size_t offset,
    const std::string_view arg) noexcept -> bool {
    if (offset > text.size() || text.size() - offset < arg.size()) return false;
    if (text.compare(offset, arg.size(), arg) != 0) return false;
    const auto after_index = offset + arg.size();
    const auto after = after_index < text.size() ? text[after_index] : '\0';
    return !is_alnum(after);
}

[[nodiscard]] auto substring_found(const std::string_view text, const std::string_view arg) noexcept -> bool {
    return !arg.empty() && text.find(arg) != std::string_view::npos;
}

} // namespace

void EmotionOpts::add(const double angle, const double intensity, const int priority) {
    // OVERRIDEBYPRIORITY dedupe (avatar.cpp:728-743): one entry per exact angle,
    // keeping the maximum priority; the winning priority's intensity is kept.
    for (auto& opt : opts) {
        if (opt.angle == angle) {
            if (opt.priority < priority) {
                opt.priority = priority;
                opt.intensity = intensity;
            }
            return;
        }
    }
    if (opts.size() >= max_opts) return; // MAXEMOPTS cap (avatar.cpp:745)
    opts.push_back(Opt{angle, intensity, priority});
}

auto default_emotion_rules() -> std::vector<EmotionRule> {
    using Kind = EmotionRule::Kind;
    namespace wheel = emotion_wheel;
    // Transcribed verbatim from chat.rc:2289-2296 (ANGRY/SCARED/BORED at
    // chat.rc:2301-2303 ship empty and are omitted). Rule name -> angle per the
    // ruleIDs/ruleEMs pairing in textpose.cpp:19-24. Text matches always carry
    // intensity 1.0 (GetEmotionsFromString), so only the priority (strength)
    // varies here.
    return {
        // ID_RULE_SHOUT
        {Kind::all_caps, "", wheel::shout, 9, false},
        {Kind::find_string, "!!!", wheel::shout, 9, false},
        // ID_RULE_LAUGH
        {Kind::check_word, "ROTFL", wheel::laugh, 11, true},
        {Kind::check_word, "LOL", wheel::laugh, 11, true},
        {Kind::find_string, "HEHE", wheel::laugh, 11, true},
        // ID_RULE_HAPPY
        {Kind::find_string, ":)", wheel::happy, 10, false},
        {Kind::find_string, ":-)", wheel::happy, 10, false},
        // ID_RULE_SAD
        {Kind::find_string, ":(", wheel::sad, 10, false},
        {Kind::find_string, ":-(", wheel::sad, 10, false},
        // ID_RULE_POINTOTHER
        {Kind::check_start, "You", wheel::point_other, 4, true},
        {Kind::check_word, "are you", wheel::point_other, 8, true},
        {Kind::check_word, "will you", wheel::point_other, 8, true},
        {Kind::check_word, "did you", wheel::point_other, 8, true},
        {Kind::check_word, "aren't you", wheel::point_other, 8, true},
        {Kind::check_word, "don't you", wheel::point_other, 8, true},
        // ID_RULE_POINTSELF
        {Kind::check_start, "I", wheel::point_self, 3, true},
        {Kind::check_word, "i'm", wheel::point_self, 7, true},
        {Kind::check_word, "i will", wheel::point_self, 7, true},
        {Kind::check_word, "i'll", wheel::point_self, 7, true},
        {Kind::check_word, "i am", wheel::point_self, 7, true},
        // ID_RULE_WAVE
        {Kind::check_start, "Hi", wheel::wave, 2, true},
        {Kind::check_start, "Bye", wheel::wave, 3, true},
        {Kind::check_start, "Hello", wheel::wave, 5, true},
        {Kind::check_start, "Welcome", wheel::wave, 5, true},
        {Kind::check_start, "Howdy", wheel::wave, 5, true},
        // ID_RULE_COY
        {Kind::find_string, ";-)", wheel::coy, 10, false},
        {Kind::find_string, ";)", wheel::coy, 10, false},
    };
}

auto emotions_from_text(const std::string_view text, const std::span<const EmotionRule> rules) -> EmotionOpts {
    using Kind = EmotionRule::Kind;
    EmotionOpts opts;
    const auto lowered = to_lower(text);
    const std::string_view lower{lowered};

    // AllCaps over the whole string (textpose.cpp:277-279). Computed once.
    const auto shouting = check_for_uppers(text);
    for (const auto& rule : rules) {
        if (rule.kind == Kind::all_caps && rule.strength != 0 && shouting)
            opts.add(rule.angle, 1.0, rule.strength);
    }

    // General FindString over the whole string (textpose.cpp:282-288).
    for (const auto& rule : rules) {
        if (rule.kind != Kind::find_string) continue;
        const auto matched = rule.case_insensitive ? substring_found(lower, to_lower(rule.arg))
                                                    : substring_found(text, rule.arg);
        if (matched) opts.add(rule.angle, 1.0, rule.strength);
    }

    // Whole-word CheckWord over the whole string (textpose.cpp:291-297).
    for (const auto& rule : rules) {
        if (rule.kind != Kind::check_word) continue;
        const auto matched = rule.case_insensitive ? check_word(lower, to_lower(rule.arg))
                                                    : check_word(text, rule.arg);
        if (matched) opts.add(rule.angle, 1.0, rule.strength);
    }

    // CheckStart iterated per sentence (textpose.cpp:300-314). Prune leading
    // whitespace, then advance through each sentence start.
    std::size_t offset = 0;
    while (offset < text.size() && is_space(text[offset])) ++offset;
    while (offset != std::string_view::npos && offset < text.size()) {
        for (const auto& rule : rules) {
            if (rule.kind != Kind::check_start) continue;
            const auto matched = rule.case_insensitive
                ? start_compare(lower, offset, to_lower(rule.arg))
                : start_compare(text, offset, rule.arg);
            if (matched) opts.add(rule.angle, 1.0, rule.strength);
        }
        offset = next_sentence_start(text, offset);
    }

    return opts;
}

auto emotions_from_text(const std::string_view text) -> EmotionOpts {
    const auto rules = default_emotion_rules();
    return emotions_from_text(text, rules);
}

auto emotion_from_wheel_point(const double dx, const double dy, const double radius) -> Emotion {
    // GetEmotionFromPoint (bodycam.cpp:409-419): intensity is the clamped radial
    // fraction with a 0.2 centre detente snapping to neutral.
    const auto magnitude = std::sqrt(dx * dx + dy * dy);
    auto intensity = radius > 0.0 ? magnitude / radius : 0.0;
    intensity = std::min(intensity, 1.0);
    if (intensity < 0.2) intensity = 0.0;
    const auto angle = intensity == 0.0 ? 0.0 : std::atan2(dy, dx);
    return Emotion{angle, intensity};
}

} // namespace comicchat
