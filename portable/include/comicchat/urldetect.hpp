#pragma once

#include "comicchat/cpp26.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace comicchat {

// Port of CUrlRec::HrIdentifyUrls (artifacts-modern/core/urlutil.cpp:294-390)
// and its helpers (artifacts/inc/urlutil.h, artifacts-modern/core/urlutil.cpp).
//
// This is a pure, allocation-light, Linux-testable scanner: no Win32, no
// Cairo, no InternetCanonicalizeUrl/wininet. The original HrIdentifyUrls had
// two branches depending on whether wininet was loaded (urlutil.cpp:333-380):
// a canonicalizing path via InternetCanonicalizeUrl/HrCanonicalizeUrl
// (urlutil.cpp:37-103, 335-361) and a non-wininet fallback that validates the
// candidate purely with bIsUrlSuffix (urlutil.cpp:362-380). Per the porting
// brief we implement ONLY the non-wininet fallback: it only *rejects*
// malformed candidates that a clean byte-level scan already excludes (no
// scheme-specific canonicalization, no network access), so nothing
// Windows-only is lost for our purposes.
//
// DBCS note: the original tracks `iBytesofChar(ch) == 2` (double-byte
// character set lead bytes, e.g. Shift-JIS) via a Win32 codepage API we do
// not have and do not need: every mIRC-style control/URL byte this file
// cares about lives in 0x00-0x7F, and UTF-8 continuation/lead bytes
// (0x80-0xFF) are already marked "legal for URL" in bLegalForURL (see
// urlutil.h:49-73, the 128-255 rows are all 0x01), so a byte-wise scan over
// UTF-8 text is safe here without any DBCS special-casing.

// One URL match: `[offset, offset + length)` into the scanned text, in bytes.
struct UrlSpan final {
    std::size_t offset{};
    std::size_t length{};
    auto operator==(const UrlSpan&) const -> bool = default;
};

// bLegalForURL (urlutil.h:21-73): per-byte legality flags used while scanning
// a candidate URL.
//   bit 0x01 - the byte may appear inside a URL (bIsUrlChar, urlutil.h:120-121)
//   bit 0x02 - the byte is URL-illegal punctuation (unused directly by the
//              ported functions below; kept for table fidelity)
//   bit 0x10 - the byte is a "trailing" character that should not, by
//              itself, count as a URL's meaningful suffix (bIsUrlSuffix,
//              urlutil.cpp:174-209)
inline constexpr std::array<std::uint8_t, 256> legal_for_url = {{
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 0
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 16
    0x02, 0x11, 0x02, 0x13, 0x01, 0x03, 0x01, 0x01, 0x01, 0x11, 0x01, 0x01, 0x11, 0x01, 0x11, 0x03, // 32
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x01, 0x02, 0x13, // 48
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, // 64
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x03, 0x02, 0x03, 0x01, // 80
    0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, // 96
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x03, 0x02, 0x13, 0x00, // 112
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, // 128
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, // 144
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, // 160
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, // 176
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, // 192
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, // 208
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, // 224
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, // 240
}};

// szURLPREFIXSBROWSER (urlutil.h:86-90): known schemes that launch a browser,
// ordered by match probability, as upstream. ("file" stayed commented out
// upstream too, urlutil.h:93; not ported.)
inline constexpr std::array<std::string_view, 4> url_prefixes_browser = {
    "http",
    "ftp",
    "https",
    "gopher",
};

// szURLPREFIXS (urlutil.h:92-100): known non-browser schemes.
inline constexpr std::array<std::string_view, 7> url_prefixes_other = {
    "mic", "news", "mailto", "nntp", "telnet", "wais", "prospero",
};

// bIsUrlChar (urlutil.h:120-121).
[[nodiscard]] constexpr auto is_url_char(unsigned char ch) noexcept -> bool {
    const std::uint8_t flags = legal_for_url[ch];
    return flags != 0 && (flags & 0x01) != 0;
}

// bIsUrlPrefix (urlutil.cpp:119-158): case-insensitive exact match of `word`
// against the known scheme lists. `for_browser`, if non-null, is set to true
// when the match came from url_prefixes_browser.
[[nodiscard]] auto is_url_prefix(std::string_view word, bool* for_browser = nullptr) -> bool;

// bIsUrlSuffix (urlutil.cpp:174-209): does `suffix` (the text right after the
// scheme's colon, up to the tentative URL end) contain enough real content to
// be worth linking?
[[nodiscard]] auto is_url_suffix(std::string_view suffix) noexcept -> bool;

// FindPreceedingWord (urlutil.cpp:228-235): walk backward from `colon` (not
// past `start`) over URL-legal, non-punctuation bytes to find where the
// scheme word begins.
[[nodiscard]] auto find_preceding_word(std::string_view text, std::size_t start, std::size_t colon) noexcept
    -> std::size_t;

// FindUrlEnd (urlutil.cpp:251-278): scan forward from `colon` while bytes are
// URL-legal, then trim trailing punctuation (but keep a trailing '/' or
// '\\'). Returns the index just past the URL.
[[nodiscard]] auto find_url_end(std::string_view text, std::size_t colon) noexcept -> std::size_t;

// HrIdentifyUrls (urlutil.cpp:294-390), non-wininet fallback branch only
// (urlutil.cpp:362-380): scan `text` for scheme-prefixed URLs and return
// their `[offset, offset+length)` spans in left-to-right order.
[[nodiscard]] auto find_urls(std::string_view text) -> std::vector<UrlSpan>;

} // namespace comicchat
