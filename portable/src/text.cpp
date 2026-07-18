#include "comicchat/text.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <mutex>
#include <utility>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb-ft.h>
#include <hb.h>
#include <unicode/normalizer2.h>
#include <unicode/ustring.h>
#include <unicode/unistr.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(COMICCHAT_HAS_FONTCONFIG)
#include <fontconfig/fontconfig.h>
#endif

namespace comicchat {

auto normalize_utf8_nfc(const std::string_view input) -> std::expected<std::string, TextError> {
    if (input.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        return std::unexpected{TextError::invalid_utf8};
    }
    UErrorCode status = U_ZERO_ERROR;
    std::int32_t utf16_size{};
    u_strFromUTF8(nullptr, 0, &utf16_size, input.data(), static_cast<std::int32_t>(input.size()), &status);
    if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status)) {
        return std::unexpected{TextError::invalid_utf8};
    }
    status = U_ZERO_ERROR;
    icu::UnicodeString source;
    auto* buffer = source.getBuffer(utf16_size);
    u_strFromUTF8(buffer, utf16_size, &utf16_size, input.data(), static_cast<std::int32_t>(input.size()), &status);
    source.releaseBuffer(utf16_size);
    if (U_FAILURE(status)) return std::unexpected{TextError::invalid_utf8};

    const auto* normalizer = icu::Normalizer2::getNFCInstance(status);
    if (U_FAILURE(status) || normalizer == nullptr) return std::unexpected{TextError::invalid_utf8};
    icu::UnicodeString normalized;
    normalizer->normalize(source, normalized, status);
    if (U_FAILURE(status)) return std::unexpected{TextError::invalid_utf8};
    std::string output;
    normalized.toUTF8String(output);
    return output;
}

auto find_portable_comic_font() -> std::expected<std::string, TextError> {
    std::vector<std::filesystem::path> candidates;
    if (const auto* override_path = std::getenv("COMICCHAT_FONT_PATH"); override_path != nullptr)
        candidates.emplace_back(override_path);
#if defined(COMICCHAT_INSTALL_FONT_PATH)
    candidates.emplace_back(COMICCHAT_INSTALL_FONT_PATH);
#endif
#if defined(COMICCHAT_SOURCE_FONT_PATH)
    candidates.emplace_back(COMICCHAT_SOURCE_FONT_PATH);
#endif
    std::error_code error;
    const auto working = std::filesystem::current_path(error);
    if (!error) {
        candidates.push_back(working / "comic.ttf");
        candidates.push_back(working / "assets" / "comic.ttf");
        candidates.push_back(working / "v1.0" / "shared" / "comic.ttf");
        candidates.push_back(working / ".." / "v1.0" / "shared" / "comic.ttf");
        candidates.push_back(working / ".." / ".." / "v1.0" / "shared" / "comic.ttf");
    }
#if defined(_WIN32)
    std::array<wchar_t, 32768> module{};
    const auto module_size = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (module_size != 0 && module_size < module.size()) {
        const auto directory = std::filesystem::path{module.data()}.parent_path();
        candidates.push_back(directory / L"comic.ttf");
        candidates.push_back(directory / L"assets" / L"comic.ttf");
    }
    std::array<wchar_t, MAX_PATH + 1> windows{};
    const auto windows_size = GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
    if (windows_size != 0 && windows_size < windows.size())
        candidates.push_back(std::filesystem::path{windows.data()} / L"Fonts" / L"comic.ttf");
#endif
    for (const auto& candidate : candidates) {
        error.clear();
        if (std::filesystem::is_regular_file(candidate, error) && !error) return candidate.string();
    }
#if defined(COMICCHAT_HAS_FONTCONFIG)
    for (const auto* family : {"Comic Sans MS", "Comic Sans", "DejaVu Sans"}) {
        auto* pattern = FcPatternCreate();
        if (pattern == nullptr) continue;
        (void)FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8*>(family));
        FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
        FcDefaultSubstitute(pattern);
        FcResult result{};
        auto* match = FcFontMatch(nullptr, pattern, &result);
        FcPatternDestroy(pattern);
        if (match == nullptr) continue;
        FcChar8* file{};
        const bool found = FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file != nullptr;
        const auto path = found ? std::string{reinterpret_cast<const char*>(file)} : std::string{};
        FcPatternDestroy(match);
        error.clear();
        if (found && std::filesystem::is_regular_file(path, error) && !error) return path;
    }
#endif
    return std::unexpected{TextError::font_open};
}

class TextEngine::Impl final {
public:
    ~Impl() {
        if (font != nullptr) hb_font_destroy(font);
        if (face != nullptr) FT_Done_Face(face);
        if (library != nullptr) FT_Done_FreeType(library);
    }

    FT_Library library{};
    FT_Face face{};
    hb_font_t* font{};
    std::mutex mutex;
};

TextEngine::TextEngine(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
TextEngine::~TextEngine() = default;

auto TextEngine::create(const std::string_view font_path)
    -> std::expected<std::unique_ptr<TextEngine>, TextError> {
    auto impl = std::make_unique<Impl>();
    if (FT_Init_FreeType(&impl->library) != 0) return std::unexpected{TextError::font_library};
    const std::string path{font_path};
    if (FT_New_Face(impl->library, path.c_str(), 0, &impl->face) != 0) {
        return std::unexpected{TextError::font_open};
    }
    impl->font = hb_ft_font_create_referenced(impl->face);
    if (impl->font == nullptr) return std::unexpected{TextError::font_open};
    return std::unique_ptr<TextEngine>{new TextEngine{std::move(impl)}};
}

auto TextEngine::shape(const std::string_view utf8, const double pixel_size)
    -> std::expected<std::vector<ShapedGlyph>, TextError> {
    if (!std::isfinite(pixel_size) || pixel_size <= 0.0 ||
        utf8.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected{TextError::font_size};
    }
    const auto normalized = normalize_utf8_nfc(utf8);
    if (!normalized) return std::unexpected{normalized.error()};
    std::scoped_lock lock{impl_->mutex};
    const auto rounded_size = static_cast<FT_UInt>(std::max(1.0, std::round(pixel_size)));
    if (FT_Set_Pixel_Sizes(impl_->face, 0, rounded_size) != 0) return std::unexpected{TextError::font_size};
    hb_ft_font_changed(impl_->font);
    auto* buffer = hb_buffer_create();
    if (buffer == nullptr) return std::unexpected{TextError::shaping};
    hb_buffer_add_utf8(buffer, normalized->data(), static_cast<int>(normalized->size()), 0, -1);
    hb_buffer_guess_segment_properties(buffer);
    hb_shape(impl_->font, buffer, nullptr, 0);
    unsigned int count{};
    const auto* infos = hb_buffer_get_glyph_infos(buffer, &count);
    const auto* positions = hb_buffer_get_glyph_positions(buffer, &count);
    std::vector<ShapedGlyph> result;
    result.reserve(count);
    for (unsigned int index = 0; index < count; ++index) {
        result.push_back({
            infos[index].codepoint,
            static_cast<double>(positions[index].x_advance) / 64.0,
            static_cast<double>(positions[index].y_advance) / 64.0,
            static_cast<double>(positions[index].x_offset) / 64.0,
            static_cast<double>(positions[index].y_offset) / 64.0,
        });
    }
    hb_buffer_destroy(buffer);
    return result;
}

auto TextEngine::native_face() const noexcept -> void* { return impl_->face; }

auto TextEngine::size_metrics(const double pixel_size) -> std::expected<SizeMetrics, TextError> {
    if (!std::isfinite(pixel_size) || pixel_size <= 0.0) return std::unexpected{TextError::font_size};
    std::scoped_lock lock{impl_->mutex};
    const auto rounded_size = static_cast<FT_UInt>(std::max(1.0, std::round(pixel_size)));
    if (FT_Set_Pixel_Sizes(impl_->face, 0, rounded_size) != 0) return std::unexpected{TextError::font_size};
    // FT_Size_Metrics are 26.6 fixed point; map to GDI TEXTMETRIC field names.
    const auto& metrics = impl_->face->size->metrics;
    const auto round26_6 = [](const FT_Pos value) -> std::int32_t {
        return static_cast<std::int32_t>((value + (value >= 0 ? 32 : -32)) / 64);
    };
    const std::int32_t ascent = round26_6(metrics.ascender);   // tm.tmAscent
    const std::int32_t descent = -round26_6(metrics.descender); // descender <= 0 -> positive tm.tmDescent
    const std::int32_t line_spacing = round26_6(metrics.height); // baseline-to-baseline advised advance
    const std::int32_t height = ascent + descent;                // tm.tmHeight
    const std::int32_t external_leading = std::max(0, line_spacing - height); // tm.tmExternalLeading
    return SizeMetrics{ascent, descent, height, external_leading};
}

auto TextEngine::measure_width(const std::string_view utf8, const double pixel_size)
    -> std::expected<std::int32_t, TextError> {
    const auto glyphs = shape(utf8, pixel_size); // takes the mutex itself; do not hold it here
    if (!glyphs) return std::unexpected{glyphs.error()};
    double total = 0.0;
    for (const auto& glyph : *glyphs) total += glyph.x_advance;
    return static_cast<std::int32_t>(std::llround(total));
}

auto build_font_metrics(TextEngine& engine, const double pixel_size, const std::int32_t n_leading,
                        const std::int32_t n_base_add) -> std::expected<FontMetrics, TextError> {
    const auto raw = engine.size_metrics(pixel_size);
    if (!raw) return std::unexpected{raw.error()};
    const auto continuation = engine.measure_width(continuation_ellipsis, pixel_size);
    if (!continuation) return std::unexpected{continuation.error()};

    // CFontInfo::CFontInfo (balloon.cpp:606-643), in source order.
    FontMetrics metrics{};
    metrics.leading = n_leading + raw->external_leading;    // m_leading (balloon.cpp:624)
    metrics.base_add = n_base_add - raw->external_leading;  // m_baseAdd (balloon.cpp:625)
    metrics.top_offset = (n_leading != 0) ? 0 : far_east_top_offset; // (balloon.cpp:635-638)
    metrics.line_height = raw->height + metrics.leading;    // m_lineHeight (balloon.cpp:640)
    metrics.continuation_width = *continuation;             // m_continuationWidth (balloon.cpp:642)
    return metrics;
}

auto measure_text_width(TextEngine& engine, const double pixel_size) -> TextMeasure {
    // Captures `engine` by reference: the returned measure must not outlive it.
    return [&engine, pixel_size](const std::string_view text) -> std::int32_t {
        if (text.empty()) return 0;
        const auto width = engine.measure_width(text, pixel_size);
        return width ? *width : 0;
    };
}

namespace {

// Advance past exactly one UTF-8 code point starting at byte `index`.
[[nodiscard]] auto utf8_next(const std::string_view text, const std::size_t index) -> std::size_t {
    if (index >= text.size()) return text.size();
    const auto lead = static_cast<unsigned char>(text[index]);
    std::size_t length = 1;
    if (lead >= 0xF0) {
        length = 4;
    } else if (lead >= 0xE0) {
        length = 3;
    } else if (lead >= 0xC0) {
        length = 2;
    }
    return std::min(text.size(), index + length);
}

} // namespace

auto widest_line_width(const std::vector<TextLine>& lines) noexcept -> std::int32_t {
    std::int32_t widest = 0;
    for (const auto& line : lines)
        if (line.width > widest) widest = line.width;
    return widest;
}

auto compute_label_bbox(const Rect& request, const FontMetrics& metrics, const std::int32_t n_lines,
                        const std::int32_t max_line_width, const LabelJustify justify) noexcept -> Rect {
    const std::int32_t desired_width = request.right - request.left; // iDesiredWidth (balloon.cpp:688)
    Rect out{};
    out.top = request.top; // fInfo.m_bbox.Top = m_bbox.Top (balloon.cpp:698)
    if (justify == LabelJustify::left) {                          // FT_LEFT_JUSTIFY (balloon.cpp:699)
        out.left = request.left;
    } else {                                                      // CENTER (balloon.cpp:705)
        out.left = (desired_width - max_line_width) / 2 + request.left;
    }
    out.right = out.left + max_line_width; // (balloon.cpp:702 / 707)
    // fInfo.m_bbox.Bottom = Top - nLines*m_lineHeight - m_baseAdd (balloon.cpp:711).
    out.bottom = out.top - n_lines * metrics.line_height - metrics.base_add;
    return out;
}

auto break_into_lines(const TextMeasure& measure, const std::int32_t max_width,
                      const std::string_view text) -> std::vector<TextLine> {
    std::vector<TextLine> lines;
    const auto full = [&] { return static_cast<std::int32_t>(lines.size()) >= max_label_lines; };
    const auto emit = [&](std::string line) {
        const std::int32_t width = line.empty() ? 0 : measure(line);
        lines.push_back(TextLine{std::move(line), width});
    };

    // Break a single word wider than the box at UTF-8 boundaries; emit the full
    // lines and return the trailing partial so following words can share it
    // (ForceLineBreak, balloon.cpp:407).
    const auto force_break = [&](const std::string_view word) -> std::string {
        std::string current;
        std::size_t index = 0;
        while (index < word.size()) {
            const std::size_t next = utf8_next(word, index);
            const std::string_view glyph = word.substr(index, next - index);
            std::string candidate = current + std::string(glyph);
            if (!current.empty() && measure(candidate) > max_width) {
                emit(std::move(current));
                if (full()) return {};
                current = std::string(glyph);
            } else {
                current = std::move(candidate);
            }
            index = next;
        }
        return current;
    };

    // Explicit '\n' is a hard break (UpcomingReturn, balloon.cpp:384); each
    // segment then greedily wraps its space-delimited words.
    std::size_t paragraph_start = 0;
    while (paragraph_start <= text.size() && !full()) {
        const std::size_t newline = text.find('\n', paragraph_start);
        const std::size_t paragraph_end = (newline == std::string_view::npos) ? text.size() : newline;
        const std::string_view paragraph = text.substr(paragraph_start, paragraph_end - paragraph_start);

        std::string current;
        std::size_t word_start = 0;
        while (word_start < paragraph.size() && !full()) {
            while (word_start < paragraph.size() && paragraph[word_start] == ' ') ++word_start; // skip spaces
            if (word_start >= paragraph.size()) break;
            std::size_t word_end = word_start;
            while (word_end < paragraph.size() && paragraph[word_end] != ' ') ++word_end;
            const std::string_view word = paragraph.substr(word_start, word_end - word_start);
            word_start = word_end;

            std::string candidate = current.empty() ? std::string(word) : current + " " + std::string(word);
            if (measure(candidate) <= max_width) {
                current = std::move(candidate);
                continue;
            }
            if (!current.empty()) {
                emit(std::move(current));
                current.clear();
                if (full()) break;
            }
            if (measure(std::string(word)) <= max_width) {
                current = std::string(word);
            } else {
                current = force_break(word); // emits complete lines, keeps the remainder
            }
        }
        if (!full()) emit(std::move(current)); // paragraph's final (or only/blank) line

        if (newline == std::string_view::npos) break;
        paragraph_start = newline + 1;
    }
    return lines;
}

auto break_into_lines_formatted(const TextMeasure& measure, const std::int32_t max_width,
                                const std::string_view text, const std::vector<TextRun>& runs)
    -> std::vector<TextLine> {
    // Delegate wrapping verbatim so text/width/count/PRNG behavior is byte-for-byte
    // identical to the plain path; we only add per-line runs on top.
    auto lines = break_into_lines(measure, max_width, text);
    if (runs.empty()) return lines; // nothing to slice; runs stay empty (plain path).

    // Locate each produced line back in `text` to get its [line_start, line_len)
    // byte span, tolerant of the wrapper's leading-trim and single-space collapse
    // (see the header's SPACE-COLLAPSE CAVEAT). Lines preserve character order, so
    // a forward cursor over `text` suffices.
    std::size_t cursor = 0;
    const auto is_wrap_space = [](const char byte) { return byte == ' ' || byte == '\n'; };
    for (auto& line : lines) {
        if (line.text.empty()) {
            // A blank line (e.g. from "\n\n") draws no glyphs; give it an empty,
            // zero-length slice and leave the cursor for the next non-empty line.
            line.runs = reslice_runs_for_line(runs, cursor, 0);
            continue;
        }
        // Skip separator whitespace (trimmed leading spaces / hard-break newlines)
        // that the wrapper dropped between the previous line and this one.
        while (cursor < text.size() && is_wrap_space(text[cursor])) ++cursor;
        const std::size_t line_start = cursor;
        for (const char glyph_byte : line.text) {
            if (glyph_byte == ' ') {
                // One line-local space maps to one-or-more original spaces (collapse).
                while (cursor < text.size() && text[cursor] == ' ') ++cursor;
            } else {
                // Non-space byte (incl. UTF-8 continuation bytes) matches one-to-one.
                if (cursor < text.size()) ++cursor;
            }
        }
        line.runs = reslice_runs_for_line(runs, line_start, cursor - line_start);
    }
    return lines;
}

auto ellipsize_single_line(const TextMeasure& measure, const std::int32_t max_width,
                           const std::string_view text, const std::string_view ellipsis) -> std::string {
    if (text.empty() || measure(text) <= max_width) return std::string{text};
    const std::string ell{ellipsis};
    if (measure(ell) > max_width) return {}; // not even the ellipsis fits
    std::string best = ell;                  // empty prefix + ellipsis
    std::string prefix;
    std::size_t index = 0;
    while (index < text.size()) {
        const std::size_t next = utf8_next(text, index);
        std::string candidate = prefix + std::string(text.substr(index, next - index));
        if (measure(candidate + ell) > max_width) break;
        prefix = std::move(candidate);
        best = prefix + ell;
        index = next;
    }
    return best;
}

} // namespace comicchat
