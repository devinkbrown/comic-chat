#include "comicchat/urldetect.hpp"

#include <cctype>

namespace comicchat {

namespace {

// Case-insensitive exact match, mirroring _tcsicmp against a known scheme
// word (urlutil.cpp:134, 149).
[[nodiscard]] auto equals_ignore_case(std::string_view a, std::string_view b) noexcept -> bool {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

} // namespace

auto is_url_prefix(std::string_view word, bool* for_browser) -> bool {
    // urlutil.cpp:127-140: known browser-launching schemes.
    for (const std::string_view prefix : url_prefixes_browser) {
        if (equals_ignore_case(word, prefix)) {
            if (for_browser) *for_browser = true;
            return true;
        }
    }
    // urlutil.cpp:142-155: known non-browser schemes.
    for (const std::string_view prefix : url_prefixes_other) {
        if (equals_ignore_case(word, prefix)) {
            if (for_browser) *for_browser = false;
            return true;
        }
    }
    return false;
}

auto is_url_suffix(std::string_view suffix) noexcept -> bool {
    // urlutil.cpp:180-181.
    if (suffix.empty()) return false;

    // urlutil.cpp:183-206. `last` mirrors `iLast`: the index (1-based, via
    // "+1") of the most recent byte that was legal-for-URL AND not itself a
    // "trailing" (0x10) byte. The DBCS `iBytesofChar(ch) == 2` branch
    // (urlutil.cpp:188-189) is intentionally not ported; see urldetect.hpp's
    // file-level DBCS note.
    std::size_t last = 0;
    for (std::size_t end = 0; end < suffix.size(); ++end) {
        const auto ch = static_cast<unsigned char>(suffix[end]);
        const std::uint8_t flags = legal_for_url[ch];
        if (flags == 0 || (flags & 0x01) == 0) {
            // urlutil.cpp:194-198: an illegal byte anywhere aborts the scan.
            return false;
        }
        if ((flags & 0x10) == 0) last = end + 1;
    }

    // urlutil.cpp:208.
    return last != 1;
}

auto find_preceding_word(std::string_view text, std::size_t start, std::size_t colon) noexcept -> std::size_t {
    // urlutil.cpp:230: `CharPrev(cszStart, cszColon)`, clamped so it never
    // steps before `start` (colon is always >= start: the caller finds it via
    // a forward scan from `start`).
    std::size_t pos = (colon > start) ? colon - 1 : start;

    // urlutil.cpp:232: walk backward while the byte is URL-legal and not
    // itself punctuation, stopping at `start`.
    for (;;) {
        const auto ch = static_cast<unsigned char>(text[pos]);
        if (!is_url_char(ch) || std::ispunct(ch) != 0) {
            // Loop condition failed without moving: original leaves szTmp
            // unchanged and returns ++szTmp.
            return pos + 1;
        }
        if (pos == start) {
            // One further decrement would step szTmp below cszStart; the
            // original's guard (`szTmp >= cszStart`) then fails and
            // `++szTmp` restores it to exactly `start`.
            return start;
        }
        --pos;
    }
}

auto find_url_end(std::string_view text, std::size_t colon) noexcept -> std::size_t {
    // urlutil.cpp:267-272: forward scan while URL-legal (':' itself passes,
    // legal_for_url[':'] has bit 0x01 set, so the colon is always consumed).
    std::size_t end = colon;
    while (end < text.size() && is_url_char(static_cast<unsigned char>(text[end]))) ++end;

    // urlutil.cpp:274-276: trim trailing punctuation, but keep a trailing '/'
    // or '\\'. `end` is guaranteed > colon on loop entry (the forward scan
    // above always consumes at least the colon byte), and every decrement
    // below is only taken after that invariant holds, so `end - 1` never
    // underflows past `colon`.
    for (;;) {
        const unsigned char cur = (end < text.size()) ? static_cast<unsigned char>(text[end]) : '\0';
        if (cur == '\\' || cur == '/') break;
        --end;
        const auto trimmed = static_cast<unsigned char>(text[end]);
        if (std::ispunct(trimmed) == 0 || end <= colon) break;
    }

    // urlutil.cpp:277: CharNext(szEnd).
    return end + 1;
}

auto find_urls(std::string_view text) -> std::vector<UrlSpan> {
    std::vector<UrlSpan> result;
    std::size_t search_from = 0;

    // urlutil.cpp:304-387.
    for (;;) {
        const std::size_t colon = text.find(':', search_from);
        if (colon == std::string_view::npos) break;

        const std::size_t word_start = find_preceding_word(text, search_from, colon);
        const std::string_view word = text.substr(word_start, colon - word_start);

        if (!is_url_prefix(word)) {
            // urlutil.cpp:314-318.
            search_from = colon + 1;
            continue;
        }

        const std::size_t url_end = find_url_end(text, colon);

        // urlutil.cpp:362-364 (non-wininet branch): validate the text right
        // after the colon, up to the tentative URL end.
        const std::string_view suffix = text.substr(colon + 1, url_end - (colon + 1));
        if (is_url_suffix(suffix)) {
            result.push_back(UrlSpan{.offset = word_start, .length = url_end - word_start});
        }

        // urlutil.cpp:383: unconditional, whether or not this candidate
        // qualified. `url_end` is always > colon (see find_url_end), so
        // `search_from` strictly advances and the loop always terminates.
        search_from = url_end;
    }

    return result;
}

} // namespace comicchat
