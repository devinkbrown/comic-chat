#include "comicchat/urldetect.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>

using comicchat::find_url_end;
using comicchat::find_urls;
using comicchat::is_url_prefix;
using comicchat::is_url_suffix;
using comicchat::UrlSpan;

namespace {
// Small helper so test bodies read like the spec's prose ("one span covering
// the URL") instead of index arithmetic.
[[nodiscard]] auto span_text(std::string_view text, const UrlSpan& span) -> std::string_view {
    return text.substr(span.offset, span.length);
}
} // namespace

TEST_CASE("find_urls locates a single http URL inside prose") {
    const std::string_view text = "see http://example.com/path here";
    const auto spans = find_urls(text);
    REQUIRE(spans.size() == 1);
    CHECK(span_text(text, spans[0]) == "http://example.com/path");
}

TEST_CASE("find_urls accepts the non-browser mic: scheme too") {
    const std::string_view text = "join mic:foo/bar now";
    const auto spans = find_urls(text);
    REQUIRE(spans.size() == 1);
    CHECK(span_text(text, spans[0]) == "mic:foo/bar");
}

TEST_CASE("find_urls returns no spans without a legal known scheme prefix") {
    CHECK(find_urls("no url here").empty());
    // "notascheme:" is not in either prefix list.
    CHECK(find_urls("notascheme:foo").empty());
}

TEST_CASE("find_urls includes a www. host as part of a scheme-prefixed URL") {
    const std::string_view text = "http://www.foo.com/page";
    const auto spans = find_urls(text);
    REQUIRE(spans.size() == 1);
    CHECK(span_text(text, spans[0]) == "http://www.foo.com/page");
}

TEST_CASE("find_urls does not detect a bare www. host with no scheme prefix") {
    // Faithful to the original: CUrlRec::HrIdentifyUrls only ever recognizes
    // "scheme:" candidates (szURLPREFIXSBROWSER/szURLPREFIXS); a bare
    // "www.foo.com" with no colon is never scanned as a URL at all.
    CHECK(find_urls("www.foo.com").empty());
    CHECK(find_urls("visit www.foo.com today").empty());
}

TEST_CASE("find_urls excludes trailing sentence punctuation") {
    const std::string_view text = "check http://example.com/page, ok?";
    const auto spans = find_urls(text);
    REQUIRE(spans.size() == 1);
    CHECK(span_text(text, spans[0]) == "http://example.com/page");
}

TEST_CASE("find_urls keeps a trailing slash") {
    const std::string_view text = "http://example.com/";
    const auto spans = find_urls(text);
    REQUIRE(spans.size() == 1);
    CHECK(span_text(text, spans[0]) == "http://example.com/");
}

TEST_CASE("find_urls keeps a trailing slash even with sentence punctuation right after") {
    const std::string_view text = "visit http://example.com/dir/. thanks";
    const auto spans = find_urls(text);
    REQUIRE(spans.size() == 1);
    // The '.' right after the URL is trimmed, but the '/' immediately before
    // it is preserved (urlutil.cpp:274's "still accept trailing slash").
    CHECK(span_text(text, spans[0]) == "http://example.com/dir/");
}

TEST_CASE("find_urls finds multiple URLs left to right") {
    const std::string_view text = "http://a.example and https://b.example";
    const auto spans = find_urls(text);
    REQUIRE(spans.size() == 2);
    CHECK(span_text(text, spans[0]) == "http://a.example");
    CHECK(span_text(text, spans[1]) == "https://b.example");
}

TEST_CASE("find_urls rejects a scheme with no suffix content") {
    // "http:" alone has nothing after the colon: bIsUrlSuffix rejects it.
    CHECK(find_urls("http:").empty());
    CHECK(find_urls("say http: to nobody").empty());
}

TEST_CASE("find_urls does not treat an unrelated colon as a URL") {
    // "foo" is not a known scheme, so this must not be misdetected.
    CHECK(find_urls("time is 10:30 sharp").empty());
}

TEST_CASE("find_urls never loops forever or reads out of bounds on malformed input") {
    // Regression coverage for the loop-progress invariant documented at
    // urldetect.cpp's find_urls: url_end is always > colon, so scanning a
    // string that is nothing but scheme-looking colons must still terminate.
    CHECK_NOTHROW(find_urls(":::::"));
    CHECK_NOTHROW(find_urls("http:http:http:"));
    CHECK_NOTHROW(find_urls(""));
    CHECK_NOTHROW(find_urls(std::string(4096, ':')));
}

TEST_CASE("is_url_prefix is case-insensitive and exact") {
    bool for_browser = false;
    CHECK(is_url_prefix("HTTP", &for_browser));
    CHECK(for_browser);

    for_browser = true;
    CHECK(is_url_prefix("MailTo", &for_browser));
    CHECK_FALSE(for_browser);

    CHECK_FALSE(is_url_prefix("htt"));
    CHECK_FALSE(is_url_prefix("http2"));
}

TEST_CASE("is_url_suffix rejects empty suffixes and lone-meaningful-byte suffixes") {
    CHECK_FALSE(is_url_suffix(""));
    CHECK(is_url_suffix("//example.com"));
    // A single legal, non-"trailing" byte followed by nothing (or only
    // decorative trailing-flagged punctuation) is not enough content:
    // bIsUrlSuffix's `iLast != 1` check treats iLast==1 (the sole
    // non-trailing byte sitting at index 0) as insufficient (urlutil.cpp:183-208).
    CHECK_FALSE(is_url_suffix("x"));
    CHECK_FALSE(is_url_suffix("x!!!"));
}

TEST_CASE("is_url_suffix has a faithfully-ported upstream quirk: pure trailing-punctuation passes") {
    // Documented quirk, not something introduced by this port: when every
    // byte is flagged "trailing" (0x10 set, e.g. '.', '!'), `last` never
    // advances off its initial 0, and the final `0 != 1` check is true, so
    // bIsUrlSuffix *accepts* strings with no real content at all. This
    // mirrors urlutil.cpp:183-208 exactly. In practice find_urls() never
    // reaches this path with such input: FindPreceedingWord/bIsUrlPrefix
    // already require a real known scheme word before the colon.
    CHECK(is_url_suffix("!"));
    CHECK(is_url_suffix("..."));
}

TEST_CASE("find_url_end stops scanning at the first illegal byte") {
    const std::string_view text = "http://example.com/path here";
    const std::size_t colon = text.find(':');
    const std::size_t end = find_url_end(text, colon);
    CHECK(text.substr(0, end) == "http://example.com/path");
}
