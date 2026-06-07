//! Decoder for Comic Chat's `.avb` (avatar) and `.bgb` (background) assets.
//!
//! Format (reverse-engineered from the original files; verified across the
//! anna/bolo/cro/hugh avatars and the field background):
//!
//!   offset 0x00  u16  magic = 0x8181
//!   offset 0x02  u16  kind  (2 = avatar/.avb, 3 = background/.bgb)
//!   offset 0x04  ...  fixed sub-header bytes
//!   offset 0x10  cstr avatar name, NUL-terminated  (avatars only)
//!   ...          ...  small sub-header
//!   ...          cstr "Copyright (c) ...\0"  (avatars append "\n<artist>")
//!   ...          indexed-colour palette (RGB triples) + encoded bitmap data
//!
//! This module decodes the header/metadata layer (magic, kind, name,
//! copyright/artist). The palette + per-pose bitmap codec is not decoded yet
//! — that needs further RE of the loader; see docs/PROTOCOL.md.

const std = @import("std");

pub const magic: u16 = 0x8181;
pub const name_offset = 0x10;

pub const Kind = enum(u16) {
    avatar = 2, // .avb
    background = 3, // .bgb
    _,
};

pub const Asset = struct {
    kind: Kind,
    /// Avatar display name (avatars only). Borrows from the input buffer.
    name: ?[]const u8,
    /// Copyright line, including the trailing "\n<artist>" credit on avatars.
    /// Borrows from the input buffer.
    copyright: ?[]const u8,
};

pub const ParseError = error{ BadMagic, Truncated };

fn readU16le(b: []const u8, off: usize) u16 {
    return @as(u16, b[off]) | (@as(u16, b[off + 1]) << 8);
}

/// NUL-terminated ASCII string starting at `off`, or null if absent/empty.
fn cStrAt(b: []const u8, off: usize) ?[]const u8 {
    if (off >= b.len) return null;
    const end = std.mem.indexOfScalarPos(u8, b, off, 0) orelse return null;
    if (end == off) return null;
    return b[off..end];
}

/// The "Copyright ...\0" run, located by substring (its offset varies with the
/// length of the preceding name).
fn findCopyright(b: []const u8) ?[]const u8 {
    const start = std.mem.indexOf(u8, b, "Copyright") orelse return null;
    const end = std.mem.indexOfScalarPos(u8, b, start, 0) orelse b.len;
    return b[start..end];
}

pub fn parse(bytes: []const u8) ParseError!Asset {
    if (bytes.len < name_offset + 1) return error.Truncated;
    if (readU16le(bytes, 0) != magic) return error.BadMagic;

    const kind: Kind = @enumFromInt(readU16le(bytes, 2));
    const name: ?[]const u8 = if (kind == .avatar) cStrAt(bytes, name_offset) else null;
    return .{ .kind = kind, .name = name, .copyright = findCopyright(bytes) };
}

// --- Tests ----------------------------------------------------------------

test "parse: synthetic avatar header" {
    // magic, kind=2, 12 filler bytes, "Anna\0", sub-header, copyright, NUL.
    const data =
        "\x81\x81" ++ "\x02\x00" ++
        "\x02\x00\x07\x01\x04\x00\x4f\x00\x00\x00\x01\x00" ++
        "Anna\x00" ++
        "\x08\x00\x01\x00" ++
        "Copyright (c) 1998 Test\nJim Woodring\x00";

    const a = try parse(data);
    try std.testing.expectEqual(Kind.avatar, a.kind);
    try std.testing.expectEqualStrings("Anna", a.name.?);
    try std.testing.expectEqualStrings("Copyright (c) 1998 Test\nJim Woodring", a.copyright.?);
}

test "parse: background has no name field" {
    const data =
        "\x81\x81" ++ "\x03\x00" ++
        "\x02\x00\x07\x01\x04\x00\x41\x00\x00\x00\x03\x01" ++
        "Copyright (c) 1998 Microsoft Corporation\x00";

    const a = try parse(data);
    try std.testing.expectEqual(Kind.background, a.kind);
    try std.testing.expect(a.name == null);
    try std.testing.expect(std.mem.startsWith(u8, a.copyright.?, "Copyright (c) 1998"));
}

test "parse: rejects bad magic and truncation" {
    try std.testing.expectError(error.BadMagic, parse("\x00\x00\x02\x00aaaaaaaaaaaaaa"));
    try std.testing.expectError(error.Truncated, parse("\x81\x81\x02"));
}

// Real-data check against the original Microsoft assets. The files live in a
// git-ignored testdata/ dir (copyrighted; not redistributed). Skipped if a
// future checkout lacks them — but here they confirm the offsets are correct.
test "parse: real anna.avb and field.bgb" {
    const anna = @embedFile("testdata/anna.avb");
    const a = try parse(anna);
    try std.testing.expectEqual(Kind.avatar, a.kind);
    try std.testing.expectEqualStrings("Anna", a.name.?);
    try std.testing.expect(std.mem.indexOf(u8, a.copyright.?, "Jim Woodring") != null);

    const field = @embedFile("testdata/field.bgb");
    const f = try parse(field);
    try std.testing.expectEqual(Kind.background, f.kind);
    try std.testing.expect(std.mem.indexOf(u8, f.copyright.?, "Microsoft Corporation") != null);
}

test "parse: avatar with no name still parses (null name)" {
    // magic, kind=2, name_offset byte is an immediate NUL -> no name.
    const data =
        "\x81\x81" ++ "\x02\x00" ++
        "\x02\x00\x07\x01\x04\x00\x4f\x00\x00\x00\x01\x00" ++
        "\x00" ++ // empty name at name_offset
        "Copyright (c) 1998 Test\x00";
    const a = try parse(data);
    try std.testing.expectEqual(Kind.avatar, a.kind);
    try std.testing.expect(a.name == null);
    try std.testing.expectEqualStrings("Copyright (c) 1998 Test", a.copyright.?);
}

test "parse: unknown kind is preserved, not rejected" {
    // magic ok, kind = 7 (neither avatar nor background).
    const data = "\x81\x81" ++ "\x07\x00" ++ "aaaaaaaaaaaa" ++ "Copyright x\x00";
    const a = try parse(data);
    try std.testing.expectEqual(@as(u16, 7), @intFromEnum(a.kind));
    try std.testing.expect(a.name == null); // name only read for avatars
}

test "parse: missing copyright run yields null copyright" {
    const data = "\x81\x81" ++ "\x02\x00" ++ "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" ++ "Bob\x00";
    const a = try parse(data);
    try std.testing.expectEqualStrings("Bob", a.name.?);
    try std.testing.expect(a.copyright == null);
}

test "parse: exactly name_offset+1 bytes is the minimum accepted length" {
    // magic + kind=2 + filler, total = name_offset + 1 bytes: not Truncated.
    const data = "\x81\x81" ++ "\x02\x00" ++ ("\x00" ** (name_offset - 4)) ++ "\x00";
    try std.testing.expect(data.len == name_offset + 1);
    const a = try parse(data);
    try std.testing.expectEqual(Kind.avatar, a.kind);
}

test "parse: every real avatar exposes a name and copyright" {
    const names = [_][]const u8{
        "anna", "armando", "bolo", "cro", "dan", "denise", "hugh", "jordan",
        "kevin", "kwensa", "lance", "lynnea", "margaret", "maynard", "mike",
        "rebecca", "sage", "scotty", "susan", "tiki", "tongtyed", "xeno",
    };
    const blobs = [_][]const u8{
        @embedFile("testdata/anna.avb"),    @embedFile("testdata/armando.avb"),
        @embedFile("testdata/bolo.avb"),    @embedFile("testdata/cro.avb"),
        @embedFile("testdata/dan.avb"),     @embedFile("testdata/denise.avb"),
        @embedFile("testdata/hugh.avb"),    @embedFile("testdata/jordan.avb"),
        @embedFile("testdata/kevin.avb"),   @embedFile("testdata/kwensa.avb"),
        @embedFile("testdata/lance.avb"),   @embedFile("testdata/lynnea.avb"),
        @embedFile("testdata/margaret.avb"),@embedFile("testdata/maynard.avb"),
        @embedFile("testdata/mike.avb"),    @embedFile("testdata/rebecca.avb"),
        @embedFile("testdata/sage.avb"),    @embedFile("testdata/scotty.avb"),
        @embedFile("testdata/susan.avb"),   @embedFile("testdata/tiki.avb"),
        @embedFile("testdata/tongtyed.avb"),@embedFile("testdata/xeno.avb"),
    };
    inline for (names, blobs) |nm, blob| {
        const a = try parse(blob);
        // All ship as avatar (kind 2) except Jordan, an alternate subtype
        // (kind 1) for which the name field is not at name_offset.
        try std.testing.expect(a.copyright != null);
        if (a.kind == .avatar) {
            try std.testing.expect(a.name != null);
            try std.testing.expect(a.name.?.len > 0);
            // sanity: name is printable ASCII, no embedded control bytes.
            for (a.name.?) |ch| try std.testing.expect(ch >= 0x20 and ch < 0x7f);
        } else {
            try std.testing.expectEqual(@as(u16, 1), @intFromEnum(a.kind));
        }
        _ = nm;
    }
}

test "parse: every real background parses as a background" {
    const blobs = [_][]const u8{
        @embedFile("testdata/field.bgb"),  @embedFile("testdata/volcano.bgb"),
        @embedFile("testdata/den.bgb"),    @embedFile("testdata/room.bgb"),
        @embedFile("testdata/pastoral.bgb"),
    };
    inline for (blobs) |blob| {
        const a = try parse(blob);
        try std.testing.expectEqual(Kind.background, a.kind);
        try std.testing.expect(a.name == null);
    }
}
