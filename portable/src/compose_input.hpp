#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// Phase 2.5c — the message-composition text-input widget for the *nix SDL3
// frontend (app.cpp). This is deliberately app-local (portable/src/, not
// include/comicchat/): it is pure UTF-8 buffer/cursor bookkeeping with no SDL,
// Cairo, or rendering dependency, so it stays trivially unit-testable and does
// not grow into a second page/render surface. app.cpp owns turning SDL3
// SDL_EVENT_TEXT_INPUT/key events into calls on this buffer and drawing it.

namespace comicchat {

// A single-line, UTF-8 aware text-editing buffer with a byte-offset cursor
// that always lands on a codepoint boundary. Backed by a bounded std::string
// so a very long paste (or held key) cannot grow the compose line without
// limit.
class ComposeBuffer final {
public:
    // Upper bound on the buffer's UTF-8 byte length. Comic Chat balloons wrap
    // well before this; the cap exists purely to bound worst-case memory/CPU
    // from a single compose line, not to model a realistic message length.
    static constexpr std::size_t max_bytes = 4096;

    // Inserts `chunk` (assumed valid UTF-8 — SDL_EVENT_TEXT_INPUT always
    // delivers valid UTF-8) at the cursor and advances the cursor past it.
    // Silently truncates at a codepoint boundary if `chunk` would overflow
    // max_bytes; never splits a multi-byte codepoint and never throws.
    void insert(std::string_view chunk) {
        if (chunk.empty() || text_.size() >= max_bytes) return;
        const auto capacity = max_bytes - text_.size();
        if (chunk.size() > capacity) {
            auto trimmed = capacity;
            while (trimmed > 0 && is_continuation(chunk[trimmed])) --trimmed;
            chunk = chunk.substr(0, trimmed);
        }
        if (chunk.empty()) return;
        text_.insert(cursor_, chunk);
        cursor_ += chunk.size();
    }

    // Removes the codepoint immediately before the cursor, if any.
    void backspace() {
        if (cursor_ == 0) return;
        const auto start = previous_boundary(cursor_);
        text_.erase(start, cursor_ - start);
        cursor_ = start;
    }

    void move_left() noexcept {
        if (cursor_ != 0) cursor_ = previous_boundary(cursor_);
    }

    void move_right() noexcept {
        if (cursor_ != text_.size()) cursor_ = next_boundary(cursor_);
    }

    void move_home() noexcept { cursor_ = 0; }
    void move_end() noexcept { cursor_ = text_.size(); }

    void clear() noexcept {
        text_.clear();
        cursor_ = 0;
    }

    // Extracts and clears the buffer (used on Enter-to-submit).
    [[nodiscard]] auto take() -> std::string {
        auto result = std::move(text_);
        text_.clear();
        cursor_ = 0;
        return result;
    }

    [[nodiscard]] auto text() const noexcept -> const std::string& { return text_; }
    [[nodiscard]] auto cursor_offset() const noexcept -> std::size_t { return cursor_; }
    [[nodiscard]] auto empty() const noexcept -> bool { return text_.empty(); }

private:
    // Whether `byte` is a UTF-8 continuation byte (10xxxxxx), i.e. not the
    // start of a codepoint.
    [[nodiscard]] static auto is_continuation(char byte) noexcept -> bool {
        return (static_cast<unsigned char>(byte) & 0xC0U) == 0x80U;
    }

    // The byte offset of the codepoint starting immediately before `offset`
    // (`offset` must be > 0 and <= text_.size()).
    [[nodiscard]] auto previous_boundary(std::size_t offset) const noexcept -> std::size_t {
        do {
            --offset;
        } while (offset > 0 && is_continuation(text_[offset]));
        return offset;
    }

    // The byte offset of the codepoint starting immediately after `offset`
    // (`offset` must be < text_.size()).
    [[nodiscard]] auto next_boundary(std::size_t offset) const noexcept -> std::size_t {
        ++offset;
        while (offset < text_.size() && is_continuation(text_[offset])) ++offset;
        return offset;
    }

    std::string text_;
    std::size_t cursor_{};
};

} // namespace comicchat
