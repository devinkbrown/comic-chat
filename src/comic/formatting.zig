//! Microsoft Comic Chat inline text-format codec.
//!
//! This is a direct, bounds-safe port of `SzSkipOneFormat` and
//! `SzControlLess` from the released `format.cpp`. The wire control bytes are
//! removed and each resulting format state is recorded at the offset of the
//! first ordinary byte which follows it. Consecutive controls therefore
//! collapse to one change, exactly as the source `bNewFormatInPlace` flag did.

const std = @import("std");

pub const control = struct {
    pub const bold: u8 = 0x02;
    pub const color: u8 = 0x03;
    pub const link: u8 = 0x0c;
    pub const fixed_pitch: u8 = 0x11;
    pub const symbol: u8 = 0x12;
    pub const italic: u8 = 0x16;
    pub const underline: u8 = 0x1f;
};

pub const effect = struct {
    pub const bold: u16 = 0x0100;
    pub const italic: u16 = 0x0200;
    pub const underline: u16 = 0x0400;
    pub const fixed_pitch: u16 = 0x0800;
    pub const symbol: u16 = 0x1000;
    pub const foreground: u16 = 0x2000;
    pub const background: u16 = 0x4000;
    pub const link: u16 = 0x8000;
};

pub const Change = struct {
    offset: usize,
    format: u16,

    pub fn has(self: Change, flag: u16) bool {
        return self.format & flag != 0;
    }

    pub fn foreground(self: Change) ?u4 {
        return if (self.has(effect.foreground)) @truncate(self.format >> 4) else null;
    }

    pub fn background(self: Change) ?u4 {
        return if (self.has(effect.background)) @truncate(self.format) else null;
    }
};

pub const Parsed = struct {
    text: []u8,
    changes: []Change,

    pub fn deinit(self: *Parsed, gpa: std.mem.Allocator) void {
        gpa.free(self.text);
        gpa.free(self.changes);
        self.* = undefined;
    }
};

pub fn formatAt(changes: []const Change, offset: usize) u16 {
    var value: u16 = 0;
    for (changes) |change| {
        if (change.offset > offset) break;
        value = change.format;
    }
    return value;
}

/// `CutFormattingArray(..., cut)` plus the source's normal-format ellipsis.
pub fn beforeContinuation(gpa: std.mem.Allocator, changes: []const Change, cut: usize) ![]Change {
    var count: usize = 0;
    for (changes) |change| {
        if (change.offset >= cut) break;
        count += 1;
    }
    if (count == 0) return gpa.alloc(Change, 0);
    const result = try gpa.alloc(Change, count + 1);
    @memcpy(result[0..count], changes[0..count]);
    result[count] = .{ .offset = cut, .format = 0 };
    return result;
}

/// Literal `PullFormattingOffsets` followed by `PushFormattingOffsets`; the
/// continuation's leading ellipsis remains unformatted and the inherited
/// state begins on its first original character.
pub fn afterContinuation(
    gpa: std.mem.Allocator,
    changes: []const Change,
    start: usize,
    continuation_prefix_len: usize,
) ![]Change {
    var latest: u16 = 0;
    var first: usize = changes.len;
    for (changes, 0..) |change, index| {
        if (change.offset >= start) {
            first = index;
            break;
        }
        latest = change.format;
    }
    const inherited: usize = if (latest != 0) 1 else 0;
    if (first == changes.len and inherited == 0) return gpa.alloc(Change, 0);
    const result = try gpa.alloc(Change, inherited + changes.len - first);
    var out: usize = 0;
    if (latest != 0) {
        result[out] = .{ .offset = continuation_prefix_len, .format = latest };
        out += 1;
    }
    for (changes[first..]) |change| {
        result[out] = .{
            .offset = change.offset - start + continuation_prefix_len,
            .format = change.format,
        };
        out += 1;
    }
    return result;
}

fn isDigit(byte: u8) bool {
    return byte >= '0' and byte <= '9';
}

fn decimalAt(input: []const u8, at: usize) ?u8 {
    if (at >= input.len or !isDigit(input[at])) return null;
    var value = input[at] - '0';
    if (at + 1 < input.len and isDigit(input[at + 1]))
        value = value * 10 + input[at + 1] - '0';
    return value % 16;
}

/// Apply the control at `at` and return the first unconsumed byte. As in the
/// original, a comma is consumed only when followed by a background number.
fn skipOne(input: []const u8, at: usize, format: *u16) usize {
    std.debug.assert(at < input.len);
    switch (input[at]) {
        control.bold => format.* ^= effect.bold,
        control.italic => format.* ^= effect.italic,
        control.underline => format.* ^= effect.underline,
        control.fixed_pitch => format.* ^= effect.fixed_pitch,
        control.symbol => format.* ^= effect.symbol,
        control.color => {
            var cursor = at + 1;
            if (cursor >= input.len or !isDigit(input[cursor])) {
                if (cursor < input.len and input[cursor] == ',' and
                    cursor + 1 < input.len and isDigit(input[cursor + 1]))
                {
                    format.* &= ~effect.foreground;
                    format.* |= effect.background;
                    format.* &= 0xff00;
                    cursor += 1;
                    const color_value = decimalAt(input, cursor).?;
                    format.* |= color_value;
                    cursor += if (cursor + 1 < input.len and isDigit(input[cursor + 1])) 2 else 1;
                    return cursor;
                }

                format.* &= ~effect.foreground;
                format.* &= ~effect.background;
                format.* &= 0xff00;
                return at + 1;
            }

            format.* |= effect.foreground;
            format.* &= 0xff0f;
            const foreground = decimalAt(input, cursor).?;
            format.* |= @as(u16, foreground) << 4;
            cursor += if (cursor + 1 < input.len and isDigit(input[cursor + 1])) 2 else 1;

            if (cursor < input.len and input[cursor] == ',' and
                cursor + 1 < input.len and isDigit(input[cursor + 1]))
            {
                format.* |= effect.background;
                format.* &= 0xfff0;
                cursor += 1;
                const background = decimalAt(input, cursor).?;
                format.* |= background;
                cursor += if (cursor + 1 < input.len and isDigit(input[cursor + 1])) 2 else 1;
            }
            return cursor;
        },
        else => unreachable,
    }
    return at + 1;
}

fn recognized(byte: u8) bool {
    return switch (byte) {
        control.color,
        control.bold,
        control.italic,
        control.fixed_pitch,
        control.underline,
        control.symbol,
        => true,
        else => false,
    };
}

pub fn parse(gpa: std.mem.Allocator, input: []const u8) !Parsed {
    var text: std.ArrayList(u8) = .empty;
    defer text.deinit(gpa);
    var changes: std.ArrayList(Change) = .empty;
    defer changes.deinit(gpa);

    try text.ensureTotalCapacity(gpa, input.len);
    var format: u16 = 0;
    var changed_in_place = false;
    var at: usize = 0;
    while (at < input.len) {
        if (recognized(input[at])) {
            at = skipOne(input, at, &format);
            changed_in_place = true;
            continue;
        }

        const clean_offset = text.items.len;
        try text.append(gpa, input[at]);
        if (changed_in_place) {
            try changes.append(gpa, .{ .offset = clean_offset, .format = format });
            changed_in_place = false;
        }
        at += 1;
    }

    const owned_text = try text.toOwnedSlice(gpa);
    errdefer gpa.free(owned_text);
    const control_changes = try changes.toOwnedSlice(gpa);
    defer gpa.free(control_changes);
    return .{
        .text = owned_text,
        .changes = try identifyUrls(gpa, owned_text, control_changes),
    };
}

const UrlRange = struct { start: usize, end: usize };

const url_prefixes = [_][]const u8{
    "http", "ftp", "https", "gopher", "mic", "news", "mailto", "nntp", "telnet", "wais", "prospero",
};

fn urlFlags(byte: u8) u8 {
    if (byte >= 128) return 0x01;
    return switch (byte) {
        '!', ')', ',', '.', '=', '?', '~' => 0x11,
        '#',
        => 0x13,
        '%',
        '/',
        '\\',
        '^',
        '`',
        '|',
        => 0x03,
        '$',
        '&',
        '\'',
        '(',
        '*',
        '+',
        '-',
        '0'...'9',
        ';',
        '@',
        'A'...'Z',
        '_',
        'a'...'z',
        => 0x01,
        else => 0,
    };
}

fn knownPrefix(value: []const u8) bool {
    for (url_prefixes) |prefix| {
        if (std.ascii.eqlIgnoreCase(prefix, value)) return true;
    }
    return false;
}

fn validSuffix(value: []const u8) bool {
    if (value.len == 0) return false;
    var last_non_terminator: usize = 0;
    for (value, 0..) |byte, index| {
        const flags = urlFlags(byte);
        if (flags & 0x01 == 0) return false;
        if (flags & 0x10 == 0) last_non_terminator = index + 1;
    }
    return last_non_terminator != 1;
}

fn identifyUrlRanges(gpa: std.mem.Allocator, text: []const u8) ![]UrlRange {
    var ranges: std.ArrayList(UrlRange) = .empty;
    defer ranges.deinit(gpa);
    var search: usize = 0;
    while (search < text.len and ranges.items.len < 16) {
        const relative = std.mem.indexOfScalar(u8, text[search..], ':') orelse break;
        const colon = search + relative;
        var start = colon;
        while (start > search and std.ascii.isAlphanumeric(text[start - 1])) start -= 1;
        if (!knownPrefix(text[start..colon])) {
            search = colon + 1;
            continue;
        }

        var end = colon + 1;
        while (end < text.len and urlFlags(text[end]) & 0x01 != 0) end += 1;
        while (end > colon + 1) {
            const byte = text[end - 1];
            if (byte == '/' or byte == '\\' or !std.ascii.isPunctuation(byte)) break;
            end -= 1;
        }
        if (validSuffix(text[colon + 1 .. end]))
            try ranges.append(gpa, .{ .start = start, .end = end });
        search = @max(colon + 1, end);
    }
    return ranges.toOwnedSlice(gpa);
}

/// Portable fallback for the source `IdentifyURLs`: it uses the exact prefix
/// list and legal/terminating-character table from `urlutil.h`. Windows'
/// optional WinInet canonicalization is deliberately not a renderer input.
fn identifyUrls(gpa: std.mem.Allocator, text: []const u8, base: []const Change) ![]Change {
    const ranges = try identifyUrlRanges(gpa, text);
    defer gpa.free(ranges);
    if (ranges.len == 0) return gpa.dupe(Change, base);

    var offsets: std.ArrayList(usize) = .empty;
    defer offsets.deinit(gpa);
    try offsets.ensureTotalCapacity(gpa, base.len + ranges.len * 2);
    for (base) |change| offsets.appendAssumeCapacity(change.offset);
    for (ranges) |range| {
        offsets.appendAssumeCapacity(range.start);
        offsets.appendAssumeCapacity(range.end);
    }
    std.mem.sort(usize, offsets.items, {}, std.sort.asc(usize));

    var result: std.ArrayList(Change) = .empty;
    defer result.deinit(gpa);
    var previous_offset: ?usize = null;
    for (offsets.items) |offset| {
        if (previous_offset != null and previous_offset.? == offset) continue;
        previous_offset = offset;
        var state = formatAt(base, offset);
        for (ranges) |range| {
            if (offset >= range.start and offset < range.end) {
                state |= effect.link;
                break;
            }
        }
        try result.append(gpa, .{ .offset = offset, .format = state });
    }
    return result.toOwnedSlice(gpa);
}

pub const Rgb = struct { r: u8, g: u8, b: u8 };

/// The exact 16-entry palette returned by the source's misspelled
/// `GetRBGColor`. Unknown values cannot occur after the `% 16` wire parse.
pub fn palette(code: u4) Rgb {
    return switch (code) {
        0 => .{ .r = 255, .g = 255, .b = 255 },
        1 => .{ .r = 0, .g = 0, .b = 0 },
        2 => .{ .r = 0, .g = 0, .b = 128 },
        3 => .{ .r = 0, .g = 128, .b = 0 },
        4 => .{ .r = 255, .g = 0, .b = 0 },
        5 => .{ .r = 128, .g = 0, .b = 0 },
        6 => .{ .r = 128, .g = 0, .b = 128 },
        7 => .{ .r = 128, .g = 128, .b = 0 },
        8 => .{ .r = 255, .g = 255, .b = 0 },
        9 => .{ .r = 0, .g = 255, .b = 0 },
        10 => .{ .r = 0, .g = 128, .b = 128 },
        11 => .{ .r = 0, .g = 255, .b = 255 },
        12 => .{ .r = 0, .g = 0, .b = 255 },
        13 => .{ .r = 255, .g = 0, .b = 255 },
        14 => .{ .r = 128, .g = 128, .b = 128 },
        15 => .{ .r = 192, .g = 192, .b = 192 },
    };
}

test "SzControlLess toggles effects at clean text offsets" {
    var parsed = try parse(std.testing.allocator, "plain \x02bold\x16 both\x02 italic\x16 done");
    defer parsed.deinit(std.testing.allocator);

    try std.testing.expectEqualStrings("plain bold both italic done", parsed.text);
    try std.testing.expectEqualSlices(Change, &.{
        .{ .offset = 6, .format = effect.bold },
        .{ .offset = 10, .format = effect.bold | effect.italic },
        .{ .offset = 15, .format = effect.italic },
        .{ .offset = 22, .format = 0 },
    }, parsed.changes);
}

test "consecutive and trailing controls match bNewFormatInPlace" {
    var parsed = try parse(std.testing.allocator, "A\x02\x16B\x1f");
    defer parsed.deinit(std.testing.allocator);
    try std.testing.expectEqualStrings("AB", parsed.text);
    try std.testing.expectEqualSlices(Change, &.{
        .{ .offset = 1, .format = effect.bold | effect.italic },
    }, parsed.changes);
}

test "source color grammar supports foreground background reset and modulo sixteen" {
    var parsed = try parse(std.testing.allocator, "\x03" ++ "04,12red\x03,03green-bg\x03reset");
    defer parsed.deinit(std.testing.allocator);
    try std.testing.expectEqualStrings("redgreen-bgreset", parsed.text);
    try std.testing.expectEqual(@as(usize, 3), parsed.changes.len);
    try std.testing.expectEqual(Change{ .offset = 0, .format = effect.foreground | effect.background | 0x004c }, parsed.changes[0]);
    try std.testing.expectEqual(Change{ .offset = 3, .format = effect.background | 0x0003 }, parsed.changes[1]);
    try std.testing.expectEqual(Change{ .offset = 11, .format = 0 }, parsed.changes[2]);

    var modulo = try parse(std.testing.allocator, "\x03" ++ "99X");
    defer modulo.deinit(std.testing.allocator);
    try std.testing.expectEqual(Change{ .offset = 0, .format = effect.foreground | 0x0030 }, modulo.changes[0]);
}

test "invalid color comma remains ordinary text like SzSkipOneFormat" {
    var parsed = try parse(std.testing.allocator, "x\x03,y");
    defer parsed.deinit(std.testing.allocator);
    try std.testing.expectEqualStrings("x,y", parsed.text);
    try std.testing.expectEqualSlices(Change, &.{
        .{ .offset = 1, .format = 0 },
    }, parsed.changes);
}

test "link byte is not stripped by SzControlLess" {
    var parsed = try parse(std.testing.allocator, "a\x0cb");
    defer parsed.deinit(std.testing.allocator);
    try std.testing.expectEqualStrings("a\x0cb", parsed.text);
    try std.testing.expectEqual(@as(usize, 0), parsed.changes.len);
}

test "IdentifyURLs preserves inline state and adds source link bounds" {
    var parsed = try parse(std.testing.allocator, "A \x02http://example.test/path.\x02 Z");
    defer parsed.deinit(std.testing.allocator);
    try std.testing.expectEqualStrings("A http://example.test/path. Z", parsed.text);
    try std.testing.expectEqualSlices(Change, &.{
        .{ .offset = 2, .format = effect.bold | effect.link },
        .{ .offset = 26, .format = effect.bold },
        .{ .offset = 27, .format = 0 },
    }, parsed.changes);
}

test "continuation formatting resets old ellipsis and inherits after new ellipsis" {
    const changes = [_]Change{
        .{ .offset = 2, .format = effect.italic },
        .{ .offset = 12, .format = effect.italic | effect.foreground | 0x0040 },
        .{ .offset = 20, .format = 0 },
    };
    const first = try beforeContinuation(std.testing.allocator, &changes, 15);
    defer std.testing.allocator.free(first);
    try std.testing.expectEqualSlices(Change, &.{
        .{ .offset = 2, .format = effect.italic },
        .{ .offset = 12, .format = effect.italic | effect.foreground | 0x0040 },
        .{ .offset = 15, .format = 0 },
    }, first);

    const rest = try afterContinuation(std.testing.allocator, &changes, 15, 3);
    defer std.testing.allocator.free(rest);
    try std.testing.expectEqualSlices(Change, &.{
        .{ .offset = 3, .format = effect.italic | effect.foreground | 0x0040 },
        .{ .offset = 8, .format = 0 },
    }, rest);
}
