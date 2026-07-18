//! Bounded parser for the XKB text keymap a Wayland compositor sends over
//! `wl_keyboard.keymap` (format 1, XKB_V1_TEXT). No libxkbcommon, no C
//! interop: this reads the same brace-delimited text format compositors
//! already emit and extracts just enough to translate a physical key press
//! into the character or named key the user's configured layout produces.
//!
//! Scope, and why: a real XKB keymap also carries `xkb_types` (multi-level
//! group/shift-state rules, e.g. AltGr as a third level) and `xkb_compat`
//! (modifier-mapping semantics). Implementing those is a much larger,
//! separate undertaking — this parser covers `xkb_keycodes` (physical key
//! name <-> numeric code) and `xkb_symbols` (key name -> the keysym list for
//! levels 1 and 2, i.e. unshifted and Shift), which is what makes the base
//! and shifted character of a non-US layout actually correct. AltGr/ISO
//! Level3 and compose/dead-key sequences are not represented; a level-3+
//! keysym, if present, is ignored. IME input method integration is a
//! separate protocol (text-input-unstable-v3) and out of scope entirely.
//!
//! Wayland's wl_keyboard.key event reports the physical key as a raw evdev
//! scancode. XKB numeric keycodes are that scancode plus 8 (a fixed offset
//! inherited from X11, where keycodes below 8 were reserved) — see
//! `xkbKeycodeFromEvdev`.

const std = @import("std");

pub const ParseError = error{
    UnsupportedKeymapFormat,
    KeycodesSectionMissing,
    SymbolsSectionMissing,
    MalformedKeymap,
} || std.mem.Allocator.Error;

/// A physical key's Shift-level-1 (unshifted) and Shift-level-2 (shifted)
/// keysym names, e.g. .{ "a", "A" } or .{ "1", "exclam" }. A key with only
/// one level (rare; some symbol keys) repeats it in both slots.
const Levels = struct {
    base: []const u8,
    shifted: []const u8,
};

/// A parsed keymap: enough of `xkb_keycodes` and `xkb_symbols` to translate
/// an evdev scancode plus a shift state into the keysym name the
/// compositor's configured layout assigns it.
pub const Keymap = struct {
    gpa: std.mem.Allocator,
    /// xkb numeric keycode (evdev + 8) -> the bracket name that names it in
    /// both xkb_keycodes and xkb_symbols, e.g. 38 -> "AC01".
    code_to_name: std.AutoHashMapUnmanaged(u32, []const u8) = .empty,
    /// Bracket name -> its level-1/level-2 keysym names, e.g. "AC01" ->
    /// .{"a", "A"}.
    name_to_levels: std.StringHashMapUnmanaged(Levels) = .empty,
    /// Backing storage for every name/keysym slice held above, freed once as
    /// a whole on deinit rather than tracked per entry.
    arena: std.heap.ArenaAllocator,

    pub fn deinit(self: *Keymap) void {
        self.code_to_name.deinit(self.gpa);
        self.name_to_levels.deinit(self.gpa);
        self.arena.deinit();
        self.* = undefined;
    }

    /// The XKB numeric keycode for a raw evdev scancode (see module doc).
    pub fn xkbKeycodeFromEvdev(evdev_code: u32) u32 {
        return evdev_code + 8;
    }

    /// The keysym name a physical key (evdev scancode) produces at the given
    /// shift level, or null if this keymap has no entry for that key.
    pub fn keysymFor(self: *const Keymap, evdev_code: u32, shifted: bool) ?[]const u8 {
        const name = self.code_to_name.get(xkbKeycodeFromEvdev(evdev_code)) orelse return null;
        const levels = self.name_to_levels.get(name) orelse return null;
        return if (shifted) levels.shifted else levels.base;
    }
};

/// Parses a compositor-supplied XKB text keymap (the bytes mmap'd from the
/// wl_keyboard.keymap fd). `format` is the event's format field; only 1
/// (XKB_V1_TEXT) is understood.
pub fn parse(gpa: std.mem.Allocator, format: u32, text: []const u8) ParseError!Keymap {
    if (format != 1) return error.UnsupportedKeymapFormat;

    var keymap = Keymap{ .gpa = gpa, .arena = std.heap.ArenaAllocator.init(gpa) };
    errdefer keymap.deinit();
    const arena = keymap.arena.allocator();

    const keycodes_body = try extractSection(text, "xkb_keycodes") orelse return error.KeycodesSectionMissing;
    try parseKeycodes(arena, gpa, keycodes_body, &keymap.code_to_name);

    const symbols_body = try extractSection(text, "xkb_symbols") orelse return error.SymbolsSectionMissing;
    try parseSymbols(arena, gpa, symbols_body, &keymap.name_to_levels);

    return keymap;
}

/// Finds `name "<anything>" { ... };` (the section's own name string is
/// whatever the compositor labeled its component with, e.g.
/// "xkb_keycodes \"evdev+aliases(qwerty)\" { ... };") and returns the slice
/// between the outermost matched braces. XKB nests braces (indicator groups,
/// key symbol lists), so this counts depth rather than finding the first `}`.
fn extractSection(text: []const u8, name: []const u8) ParseError!?[]const u8 {
    var search_from: usize = 0;
    while (std.mem.indexOfPos(u8, text, search_from, name)) |name_at| {
        search_from = name_at + name.len;
        // Reject a match that is a substring of a longer identifier, e.g.
        // "xkb_keycodes" must not match inside some future "xkb_keycodes_v2".
        if (name_at > 0 and isIdentChar(text[name_at - 1])) continue;
        if (name_at + name.len < text.len and isIdentChar(text[name_at + name.len])) continue;

        const open = std.mem.indexOfScalarPos(u8, text, search_from, '{') orelse return null;
        // Everything between the name and the open brace must be whitespace
        // and/or a quoted label; reject a same-named identifier used for
        // something else (defensive, real keymaps never do this).
        var i = search_from;
        var saw_quote = false;
        while (i < open) : (i += 1) {
            const c = text[i];
            if (c == '"') {
                saw_quote = !saw_quote;
            } else if (!saw_quote and !std.ascii.isWhitespace(c)) {
                break;
            }
        }
        if (i != open) continue;

        var depth: usize = 1;
        var j = open + 1;
        while (j < text.len and depth > 0) : (j += 1) {
            switch (text[j]) {
                '{' => depth += 1,
                '}' => depth -= 1,
                else => {},
            }
        }
        if (depth != 0) return error.MalformedKeymap;
        return text[open + 1 .. j - 1];
    }
    return null;
}

fn isIdentChar(c: u8) bool {
    return std.ascii.isAlphanumeric(c) or c == '_';
}

/// Parses `<NAME> = NUMBER;` lines (ignores everything else in the section:
/// `minimum`/`maximum` bounds, `indicator` declarations, and `alias`
/// declarations, none of which affect the base+shift translation this
/// parser targets).
fn parseKeycodes(
    arena: std.mem.Allocator,
    gpa: std.mem.Allocator,
    body: []const u8,
    out: *std.AutoHashMapUnmanaged(u32, []const u8),
) ParseError!void {
    var i: usize = 0;
    while (i < body.len) {
        skipToNextToken(body, &i);
        if (i >= body.len) break;
        if (body[i] != '<') {
            skipStatement(body, &i);
            continue;
        }
        const name_start = i + 1;
        const name_end = std.mem.indexOfScalarPos(u8, body, name_start, '>') orelse return error.MalformedKeymap;
        const name = body[name_start..name_end];
        i = name_end + 1;

        skipToNextToken(body, &i);
        if (i >= body.len or body[i] != '=') {
            skipStatement(body, &i);
            continue;
        }
        i += 1;
        skipToNextToken(body, &i);

        const number_start = i;
        while (i < body.len and std.ascii.isDigit(body[i])) : (i += 1) {}
        if (i == number_start) return error.MalformedKeymap;
        const code = std.fmt.parseUnsigned(u32, body[number_start..i], 10) catch return error.MalformedKeymap;

        try out.put(gpa, code, try arena.dupe(u8, name));
        skipStatement(body, &i);
    }
}

/// Parses `key <NAME> { [ sym1, sym2, ... ] };` lines within `xkb_symbols`
/// (ignores `modifier_map`, virtual-modifier, and group declarations, none
/// of which this parser's level-1/level-2 scope needs).
fn parseSymbols(
    arena: std.mem.Allocator,
    gpa: std.mem.Allocator,
    body: []const u8,
    out: *std.StringHashMapUnmanaged(Levels),
) ParseError!void {
    var i: usize = 0;
    while (i < body.len) {
        skipToNextToken(body, &i);
        if (i >= body.len) break;

        if (!std.mem.startsWith(u8, body[i..], "key") or (i + 3 < body.len and isIdentChar(body[i + 3]))) {
            skipStatement(body, &i);
            continue;
        }
        i += 3;
        skipToNextToken(body, &i);
        if (i >= body.len or body[i] != '<') {
            skipStatement(body, &i);
            continue;
        }
        const name_start = i + 1;
        const name_end = std.mem.indexOfScalarPos(u8, body, name_start, '>') orelse return error.MalformedKeymap;
        const name = body[name_start..name_end];
        i = name_end + 1;

        skipToNextToken(body, &i);
        if (i >= body.len or body[i] != '{') {
            skipStatement(body, &i);
            continue;
        }
        // key <NAME> { ... } bodies can themselves contain nested
        // `symbols[Group1] = [ ... ]` or a bare `[ ... ]`; only the keysym
        // list is needed, wherever the first one appears.
        const brace_open = i;
        var depth: usize = 1;
        var j = brace_open + 1;
        while (j < body.len and depth > 0) : (j += 1) {
            switch (body[j]) {
                '{' => depth += 1,
                '}' => depth -= 1,
                else => {},
            }
        }
        if (depth != 0) return error.MalformedKeymap;
        const key_body = body[brace_open + 1 .. j - 1];
        i = j;

        if (try firstKeysymList(key_body)) |syms| {
            var it = std.mem.splitScalar(u8, syms, ',');
            var base: ?[]const u8 = null;
            var shifted: ?[]const u8 = null;
            while (it.next()) |raw| {
                const trimmed = std.mem.trim(u8, raw, " \t\r\n");
                if (trimmed.len == 0) continue;
                if (base == null) {
                    base = trimmed;
                } else if (shifted == null) {
                    shifted = trimmed;
                } else {
                    break; // level 3+ ignored, see module doc.
                }
            }
            if (base) |b| {
                try out.put(gpa, try arena.dupe(u8, name), .{
                    .base = try arena.dupe(u8, b),
                    .shifted = try arena.dupe(u8, shifted orelse b),
                });
            }
        }
        skipStatement(body, &i);
    }
}

/// Finds the contents of the first `[ ... ]` in a `key <NAME> { ... }` body
/// (the keysym list; may be preceded by `symbols[Group1] =` or nothing).
fn firstKeysymList(key_body: []const u8) ParseError!?[]const u8 {
    const open = std.mem.indexOfScalar(u8, key_body, '[') orelse return null;
    const close = std.mem.indexOfScalarPos(u8, key_body, open, ']') orelse return error.MalformedKeymap;
    return key_body[open + 1 .. close];
}

fn skipToNextToken(body: []const u8, i: *usize) void {
    while (i.* < body.len) {
        if (std.ascii.isWhitespace(body[i.*])) {
            i.* += 1;
        } else if (body[i.*] == '/' and i.* + 1 < body.len and body[i.* + 1] == '/') {
            while (i.* < body.len and body[i.*] != '\n') i.* += 1;
        } else {
            break;
        }
    }
}

/// Advances past the current statement's terminating `;` (or to the end of
/// the section if none remains), skipping any string literal's own `;`-free
/// content and respecting nested `{ }` so a `key <NAME> { ... };` this
/// parser did not recognize does not get truncated mid-body.
fn skipStatement(body: []const u8, i: *usize) void {
    var depth: usize = 0;
    var in_string = false;
    while (i.* < body.len) : (i.* += 1) {
        const c = body[i.*];
        if (in_string) {
            if (c == '"') in_string = false;
            continue;
        }
        switch (c) {
            '"' => in_string = true,
            '{' => depth += 1,
            '}' => {
                if (depth == 0) return;
                depth -= 1;
            },
            ';' => if (depth == 0) {
                i.* += 1;
                return;
            },
            else => {},
        }
    }
}

/// Named (non-printable-character) keys this parser recognizes by their XKB
/// keysym name. Mirrors the non-`char` variants of the platform `Key` union;
/// callers map these to their own Key type.
pub const NamedKey = enum {
    backspace,
    enter,
    escape,
    tab,
    left,
    right,
    up,
    down,
    home,
    end,
    page_up,
    page_down,
    delete,
};

const named_keysyms = std.StaticStringMap(NamedKey).initComptime(.{
    .{ "BackSpace", .backspace },
    .{ "Return", .enter },
    .{ "KP_Enter", .enter },
    .{ "Escape", .escape },
    .{ "Tab", .tab },
    .{ "ISO_Left_Tab", .tab },
    .{ "Left", .left },
    .{ "Right", .right },
    .{ "Up", .up },
    .{ "Down", .down },
    .{ "Home", .home },
    .{ "End", .end },
    .{ "Prior", .page_up },
    .{ "Next", .page_down },
    .{ "Delete", .delete },
});

/// keysym name -> the Latin-1/ASCII printable character it names, for the
/// named punctuation/space keysyms XKB uses instead of the literal
/// character. A single-character keysym name (letters, digits, and the
/// handful of ASCII symbols that are also valid identifier characters, like
/// bare "a" or "1") is its own translation and does not need a table entry
/// — see `charForKeysym`.
const named_char_keysyms = std.StaticStringMap(u8).initComptime(.{
    .{ "space", ' ' },
    .{ "exclam", '!' },
    .{ "quotedbl", '"' },
    .{ "numbersign", '#' },
    .{ "dollar", '$' },
    .{ "percent", '%' },
    .{ "ampersand", '&' },
    .{ "apostrophe", '\'' },
    .{ "quoteright", '\'' },
    .{ "parenleft", '(' },
    .{ "parenright", ')' },
    .{ "asterisk", '*' },
    .{ "plus", '+' },
    .{ "comma", ',' },
    .{ "minus", '-' },
    .{ "period", '.' },
    .{ "slash", '/' },
    .{ "colon", ':' },
    .{ "semicolon", ';' },
    .{ "less", '<' },
    .{ "equal", '=' },
    .{ "greater", '>' },
    .{ "question", '?' },
    .{ "at", '@' },
    .{ "bracketleft", '[' },
    .{ "backslash", '\\' },
    .{ "bracketright", ']' },
    .{ "asciicircum", '^' },
    .{ "underscore", '_' },
    .{ "grave", '`' },
    .{ "quoteleft", '`' },
    .{ "braceleft", '{' },
    .{ "bar", '|' },
    .{ "braceright", '}' },
    .{ "asciitilde", '~' },
});

/// Resolves a keysym name to a plain character, if it is one. Covers the
/// bare-letter/digit case ("a", "A", "5") and the named-punctuation table
/// above; returns null for anything else (a named key, or an unrecognized
/// keysym this bounded parser does not translate).
pub fn charForKeysym(name: []const u8) ?u8 {
    if (name.len == 1 and std.ascii.isPrint(name[0])) return name[0];
    return named_char_keysyms.get(name);
}

/// Resolves a keysym name to a named (non-character) key, if it is one.
pub fn namedKeyForKeysym(name: []const u8) ?NamedKey {
    return named_keysyms.get(name);
}

test "extractSection finds a brace-balanced body and ignores an unrelated prefix match" {
    const text =
        \\xkb_keymap {
        \\  xkb_keycodes "evdev" {
        \\      minimum = 8;
        \\      <AE01> = 10;
        \\  };
        \\  xkb_symbols "pc" {
        \\      key <AE01> { [ 1, exclam ] };
        \\  };
        \\};
    ;
    const keycodes = (try extractSection(text, "xkb_keycodes")).?;
    try std.testing.expect(std.mem.indexOf(u8, keycodes, "<AE01> = 10;") != null);
    const symbols = (try extractSection(text, "xkb_symbols")).?;
    try std.testing.expect(std.mem.indexOf(u8, symbols, "key <AE01>") != null);
    try std.testing.expectEqual(@as(?[]const u8, null), try extractSection(text, "xkb_geometry"));
}

test "parse translates a realistic US-shaped fragment for base and shifted levels" {
    const text =
        \\xkb_keymap {
        \\  xkb_keycodes "evdev+aliases(qwerty)" {
        \\      minimum = 8;
        \\      maximum = 255;
        \\      <AE02> = 11;
        \\      <AC01> = 38;
        \\      <RTRN> = 36;
        \\      indicator 1 = "Caps Lock";
        \\  };
        \\  xkb_types "complete" {
        \\      // deliberately unparsed by this bounded parser
        \\  };
        \\  xkb_compat "complete" {
        \\  };
        \\  xkb_symbols "pc+us+inet(evdev)" {
        \\      key <AE02> {  [ 2, at ] };
        \\      key <AC01> {        [       a,      A       ]       };
        \\      key <RTRN> { [ Return ] };
        \\      modifier_map Shift { <LFSH> };
        \\  };
        \\};
    ;
    var keymap = try parse(std.testing.allocator, 1, text);
    defer keymap.deinit();

    // evdev code 3 = XKB keycode 11 = <AE02> = "2"/"at".
    try std.testing.expectEqualStrings("2", keymap.keysymFor(3, false).?);
    try std.testing.expectEqualStrings("at", keymap.keysymFor(3, true).?);
    try std.testing.expectEqual(@as(u8, '@'), charForKeysym(keymap.keysymFor(3, true).?).?);

    // evdev code 30 = XKB keycode 38 = <AC01> = "a"/"A".
    try std.testing.expectEqualStrings("a", keymap.keysymFor(30, false).?);
    try std.testing.expectEqualStrings("A", keymap.keysymFor(30, true).?);
    try std.testing.expectEqual(@as(u8, 'a'), charForKeysym("a").?);
    try std.testing.expectEqual(@as(u8, 'A'), charForKeysym("A").?);

    // A single-level key (<RTRN>) repeats its only keysym at both levels,
    // and resolves through the named-key table, not charForKeysym.
    try std.testing.expectEqualStrings("Return", keymap.keysymFor(28, false).?);
    try std.testing.expectEqualStrings("Return", keymap.keysymFor(28, true).?);
    try std.testing.expectEqual(NamedKey.enter, namedKeyForKeysym("Return").?);
    try std.testing.expectEqual(@as(?u8, null), charForKeysym("Return"));

    // A key with no entry at all (never declared).
    try std.testing.expectEqual(@as(?[]const u8, null), keymap.keysymFor(999, false));
}

test "parse rejects an unsupported keymap format" {
    try std.testing.expectError(error.UnsupportedKeymapFormat, parse(std.testing.allocator, 0, ""));
}

test "parse fails closed on a missing section rather than returning a partial keymap" {
    try std.testing.expectError(
        error.KeycodesSectionMissing,
        parse(std.testing.allocator, 1, "xkb_keymap { xkb_symbols \"x\" { }; };"),
    );
    try std.testing.expectError(
        error.SymbolsSectionMissing,
        parse(std.testing.allocator, 1, "xkb_keymap { xkb_keycodes \"x\" { }; };"),
    );
}

test "charForKeysym and namedKeyForKeysym cover the documented tables" {
    try std.testing.expectEqual(@as(u8, ' '), charForKeysym("space").?);
    try std.testing.expectEqual(@as(u8, '!'), charForKeysym("exclam").?);
    try std.testing.expectEqual(@as(u8, '5'), charForKeysym("5").?);
    try std.testing.expectEqual(@as(?u8, null), charForKeysym("nonexistent_keysym_name"));
    try std.testing.expectEqual(NamedKey.backspace, namedKeyForKeysym("BackSpace").?);
    try std.testing.expectEqual(NamedKey.page_up, namedKeyForKeysym("Prior").?);
    try std.testing.expectEqual(@as(?NamedKey, null), namedKeyForKeysym("nonexistent_keysym_name"));
}
