//! Decoder for Comic Chat `.bgb` background images (the raster part).
//!
//! Layout after the metadata header (see avb.zig):
//!   - palette: biClrUsed RGB triples, immediately *before* the bitmap header
//!   - a Windows BITMAPINFOHEADER (biSize = 40), here 4 bits/pixel
//!   - two u32 size fields (uncompressed, compressed)
//!   - a zlib stream of the bottom-up, 4-byte-row-padded indexed pixels
//!
//! Verified against field.bgb: 315x315, 4bpp, 16-colour palette, 50400 bytes
//! of pixel data zlib-compressed to ~25 KB.

const std = @import("std");
const flate = std.compress.flate;
const avb = @import("avb.zig");

pub const Image = struct {
    width: u32,
    height: u32,
    /// Top-down, 0xAARRGGBB.
    pixels: []u32,

    pub fn deinit(self: *Image, gpa: std.mem.Allocator) void {
        gpa.free(self.pixels);
        self.* = undefined;
    }
};

pub const Error = error{
    BadMagic,
    Truncated,
    NoBitmapHeader,
    UnsupportedDepth,
    NoZlibStream,
} || std.mem.Allocator.Error || std.Io.Reader.Error;

fn rdU16(b: []const u8, o: usize) u16 {
    return @as(u16, b[o]) | (@as(u16, b[o + 1]) << 8);
}
fn rdU32(b: []const u8, o: usize) u32 {
    return @as(u32, b[o]) | (@as(u32, b[o + 1]) << 8) |
        (@as(u32, b[o + 2]) << 16) | (@as(u32, b[o + 3]) << 24);
}

/// Locate the BITMAPINFOHEADER (biSize == 40 with a plausible width/height/
/// planes/bit-depth). Returns its offset.
fn findBih(b: []const u8) ?usize {
    if (b.len < 40) return null;
    var i: usize = 0;
    while (i + 40 <= b.len) : (i += 1) {
        if (rdU32(b, i) != 40) continue;
        const w = rdU32(b, i + 4);
        const h = rdU32(b, i + 8);
        const planes = rdU16(b, i + 12);
        const bits = rdU16(b, i + 14);
        if (planes == 1 and (bits == 1 or bits == 4 or bits == 8) and
            w > 0 and w <= 4096 and h > 0 and h <= 4096) return i;
    }
    return null;
}

/// Find a zlib stream header at or after `from` (CMF/FLG multiple of 31).
fn findZlib(b: []const u8, from: usize) ?usize {
    var i: usize = from;
    while (i + 1 < b.len) : (i += 1) {
        if (b[i] != 0x78) continue;
        const cmf_flg = (@as(u16, b[i]) << 8) | b[i + 1];
        if (cmf_flg % 31 == 0) return i;
    }
    return null;
}

pub fn decodeBackground(gpa: std.mem.Allocator, bytes: []const u8) Error!Image {
    _ = try avb.parse(bytes); // validate magic / shape

    const bih = findBih(bytes) orelse return error.NoBitmapHeader;
    const width = rdU32(bytes, bih + 4);
    const height = rdU32(bytes, bih + 8);
    const bits = rdU16(bytes, bih + 14);
    if (bits != 4 and bits != 8) return error.UnsupportedDepth;

    var clr = rdU32(bytes, bih + 32); // biClrUsed
    if (clr == 0) clr = @as(u32, 1) << @intCast(bits);

    const pal_len = clr * 3;
    if (pal_len > bih) return error.Truncated;
    const palette = bytes[bih - pal_len .. bih]; // RGB triples

    const zoff = findZlib(bytes, bih + 40) orelse return error.NoZlibStream;

    // Rows padded to a 4-byte boundary, stored bottom-up.
    const stride: usize = (((@as(usize, width) * bits) + 31) / 32) * 4;
    const raw_len = stride * height;
    const raw = try gpa.alloc(u8, raw_len);
    defer gpa.free(raw);

    var in: std.Io.Reader = .fixed(bytes[zoff..]);
    var window: [flate.max_window_len]u8 = undefined;
    var dec = flate.Decompress.init(&in, .zlib, &window);
    try dec.reader.readSliceAll(raw);

    const pixels = try gpa.alloc(u32, @as(usize, width) * height);
    errdefer gpa.free(pixels);

    var y: u32 = 0;
    while (y < height) : (y += 1) {
        const src_row = (@as(usize, height - 1 - y)) * stride; // bottom-up
        var x: u32 = 0;
        while (x < width) : (x += 1) {
            const idx: usize = switch (bits) {
                8 => raw[src_row + x],
                else => blk: { // 4bpp: two pixels per byte, high nibble first
                    const byte = raw[src_row + x / 2];
                    break :blk if (x % 2 == 0) (byte >> 4) else (byte & 0x0f);
                },
            };
            const p = idx * 3;
            const r = palette[p];
            const g = palette[p + 1];
            const b = palette[p + 2];
            pixels[@as(usize, y) * width + x] =
                (@as(u32, 0xff) << 24) | (@as(u32, r) << 16) |
                (@as(u32, g) << 8) | @as(u32, b);
        }
    }

    return .{ .width = width, .height = height, .pixels = pixels };
}

// --- Avatar poses (2bpp grayscale, bottom-up, no BITMAPINFOHEADER) ---------
//
// Beyond the 40x40 self-portrait, each .avb holds the character's expression
// poses as separate zlib streams: 2 bits/pixel (4 grey levels), width 192,
// bottom-up rows (stride 48), height = streamLen/48. Index 0 = transparent
// (paper); 1..3 = light/dark/black ink. Decoded directly (poses carry no
// BITMAPINFOHEADER — the dimensions are implicit).

const ink_idx: u8 = 3; // black ink (index 3 in the 2bpp pose palette)
const body_min_height: usize = 250; // full standing poses; heads/busts are shorter

fn rdI16(b: []const u8, o: usize) i32 {
    return @as(i16, @bitCast(rdU16(b, o)));
}

pub const Point = struct { x: i32, y: i32 };
pub const NeckAnchors = struct { head: Point, body: Point };

/// The pose-metadata table (after the copyright string) lists per-pose anchor
/// records terminated by the marker 01 01 01 04 03 03, preceded by six i16:
/// P1(x,y) d(x,y) P2(x,y). P2 is the neck-join point (P1, on head records, is
/// the word-balloon/mouth point). Head records have P1!=(0,0); body records
/// have P1=(0,0). Aligning the head's P2 to the body's P2 reproduces Comic
/// Chat's exact head-on-body registration.
pub fn neckAnchors(data: []const u8) ?NeckAnchors {
    const mark = [_]u8{ 1, 1, 1, 4, 3, 3 };
    var head: ?Point = null;
    var body: ?Point = null;
    var i: usize = 0x60;
    while (i + 6 <= data.len and i < 0x600) : (i += 1) {
        if (!std.mem.eql(u8, data[i .. i + 6], &mark)) continue;
        if (i < 12) continue;
        const p1 = Point{ .x = rdI16(data, i - 12), .y = rdI16(data, i - 10) };
        const p2 = Point{ .x = rdI16(data, i - 4), .y = rdI16(data, i - 2) };
        if (p1.x == 0 and p1.y == 0) {
            if (body == null) body = p2;
        } else {
            if (head == null) head = p1; // head neck base (P2 sinks too far)
        }
    }
    if (head == null or body == null) return null;
    return .{ .head = head.?, .body = body.? };
}

// --- Emotion-wheel pose table ---------------------------------------------
//
// Each marker record (see neckAnchors) carries an emotion-wheel code in the
// i16 at marker+18. Verified across anna/cro/bolo/hugh/tiki:
//
//   * Head poses (mouth anchor P1 != 0,0) use codes 1..8 — the eight spokes
//     of Comic Chat's emotion wheel — plus 9 for the neutral/centre face, which
//     is by far the most common head code in every avatar.
//   * Body poses (P1 == 0,0) use a wider gesture vocabulary, codes 1..12.
//   * A single sentinel record (code 0) terminates the table; it is skipped.
//
// The wheel's named spokes (from the original help index) are happy, laughing,
// sad, angry, shouting, afraid/scared, coy and shy/bored. The exact code->spoke
// assignment and the code->bitmap linkage (a monotonic pointer field that
// several wheel cells share) are not yet resolved, so this table is exposed for
// analysis rather than driving pose selection. See docs/PROTOCOL.md.

pub const PoseKind = enum { head, body };

pub const PoseMeta = struct {
    kind: PoseKind,
    /// Word-balloon / mouth anchor (head poses); (0,0) on body poses.
    mouth: Point,
    /// Neck-join anchor.
    neck: Point,
    /// Emotion-wheel code (heads 1..9, bodies 1..12). Never 0 (sentinel).
    code: i16,
};

/// Parse the per-pose emotion-wheel metadata table. Caller owns the slice.
pub fn poseTable(gpa: std.mem.Allocator, data: []const u8) ![]PoseMeta {
    const mark = [_]u8{ 1, 1, 1, 4, 3, 3 };
    var list: std.ArrayList(PoseMeta) = .empty;
    errdefer list.deinit(gpa);
    var i: usize = 0x60;
    while (i + 6 <= data.len and i < 0x800) : (i += 1) {
        if (!std.mem.eql(u8, data[i .. i + 6], &mark)) continue;
        if (i < 12 or i + 20 > data.len) continue;
        const code = @as(i16, @bitCast(rdU16(data, i + 18)));
        const p1 = Point{ .x = rdI16(data, i - 12), .y = rdI16(data, i - 10) };
        const p2 = Point{ .x = rdI16(data, i - 4), .y = rdI16(data, i - 2) };
        const is_head = !(p1.x == 0 and p1.y == 0);
        // Skip terminator/sentinel records, whose trailing word reads into
        // unrelated bytes (heads end with code 0; bodies with junk). Valid
        // codes are 1..9 for heads and 1..12 for bodies.
        const max_code: i16 = if (is_head) 9 else 12;
        if (code < 1 or code > max_code) continue;
        try list.append(gpa, .{
            .kind = if (is_head) .head else .body,
            .mouth = p1,
            .neck = p2,
            .code = code,
        });
    }
    return list.toOwnedSlice(gpa);
}

/// Decode the `index`-th pose. Each pose's zlib stream is preceded by a 40-byte
/// header carrying height (@-40) and bit depth (@-34), so the exact pose width
/// is (uncompressedSize / height) * 8 / bpp — no guessing. `tall` selects the
/// standing body layer; otherwise the head/expression layer. All poses share a
/// top-left origin, so head and body composite at (0,0).
pub fn decodePoseAuto(gpa: std.mem.Allocator, data: []const u8, index: usize, tall: bool) !Image {
    var found: usize = 0;
    var i: usize = 40;
    while (i + 1 < data.len) : (i += 1) {
        if (data[i] != 0x78) continue;
        if (((@as(u16, data[i]) << 8) | data[i + 1]) % 31 != 0) continue;
        const height: usize = rdU16(data, i - 40);
        const bpp: usize = rdU16(data, i - 34);
        if (bpp != 2 or height < 40 or height > 600) continue;
        if ((height >= body_min_height) != tall) continue; // body vs head

        var in: std.Io.Reader = .fixed(data[i..]);
        var window: [flate.max_window_len]u8 = undefined;
        var dec = flate.Decompress.init(&in, .zlib, &window);
        var aw: std.Io.Writer.Allocating = .init(gpa);
        defer aw.deinit();
        _ = dec.reader.streamRemaining(&aw.writer) catch continue;
        const raw = aw.written();

        if (raw.len == 0 or raw.len % height != 0) continue;
        const stride = raw.len / height;
        const width: u32 = @intCast(stride * 8 / bpp);
        if (width < 80 or width > 320) continue; // sanity (reject false 0x78)
        if (found != index) {
            found += 1;
            continue;
        }
        return rasterPose(gpa, raw, width, @intCast(height), stride);
    }
    return error.PoseNotFound;
}

fn rasterPose(gpa: std.mem.Allocator, raw: []const u8, w: u32, height: u32, stride: usize) !Image {
    // Comic Chat default characters are black ink on an opaque white "sticker"
    // silhouette (the white area pops the figure against any background). So:
    // index 0 = paper (transparent), index 3 = black ink, 1/2 = opaque white.
    const n = @as(usize, w) * height;
    const pixels = try gpa.alloc(u32, n);
    errdefer gpa.free(pixels);
    var y: u32 = 0;
    while (y < height) : (y += 1) {
        const src_row = @as(usize, height - 1 - y) * stride; // bottom-up source
        var x: u32 = 0;
        while (x < w) : (x += 1) {
            const byte = raw[src_row + x / 4];
            const idx: u8 = @intCast((byte >> @intCast(6 - 2 * (x % 4))) & 3);
            pixels[@as(usize, y) * w + x] = switch (idx) {
                0 => 0x00000000, // paper / transparent
                ink_idx => 0xff000000, // black ink
                else => 0xffffffff, // opaque white fill
            };
        }
    }
    return .{ .width = w, .height = height, .pixels = pixels };
}

// --- Tests ----------------------------------------------------------------

test "decodeBackground: real field.bgb -> 315x315 image" {
    const gpa = std.testing.allocator;
    const data = @embedFile("testdata/field.bgb");

    var img = try decodeBackground(gpa, data);
    defer img.deinit(gpa);

    try std.testing.expectEqual(@as(u32, 315), img.width);
    try std.testing.expectEqual(@as(u32, 315), img.height);
    try std.testing.expectEqual(@as(usize, 315 * 315), img.pixels.len);

    // Every pixel opaque; the scene must contain more than one colour.
    var distinct = std.AutoHashMap(u32, void).init(gpa);
    defer distinct.deinit();
    for (img.pixels) |px| {
        try std.testing.expectEqual(@as(u32, 0xff), px >> 24);
        try distinct.put(px, {});
    }
    try std.testing.expect(distinct.count() > 1);
    try std.testing.expect(distinct.count() <= 16); // 4bpp palette
}

test "poseTable: real avatars expose emotion-wheel codes in range" {
    const gpa = std.testing.allocator;
    const blobs = [_][]const u8{
        @embedFile("testdata/anna.avb"), @embedFile("testdata/cro.avb"),
        @embedFile("testdata/bolo.avb"), @embedFile("testdata/hugh.avb"),
        @embedFile("testdata/tiki.avb"),
    };
    inline for (blobs) |blob| {
        const table = try poseTable(gpa, blob);
        defer gpa.free(table);
        try std.testing.expect(table.len > 0);

        var head_count: usize = 0;
        var neutral_count: usize = 0;
        for (table) |p| {
            try std.testing.expect(p.code != 0); // sentinel excluded
            switch (p.kind) {
                .head => {
                    head_count += 1;
                    try std.testing.expect(p.code >= 1 and p.code <= 9);
                    if (p.code == 9) neutral_count += 1;
                    try std.testing.expect(p.mouth.x != 0 or p.mouth.y != 0);
                },
                .body => {
                    try std.testing.expect(p.code >= 1 and p.code <= 12);
                    try std.testing.expect(p.mouth.x == 0 and p.mouth.y == 0);
                },
            }
        }
        // Neutral (code 9) is the most common head code in every avatar that
        // ships head poses.
        if (head_count > 0) try std.testing.expect(neutral_count >= 1);
    }
}

test "decodePose: real anna.avb pose 0 is a 192-wide grayscale figure" {
    const gpa = std.testing.allocator;
    const anna = @embedFile("testdata/anna.avb");
    var pose = try decodePoseAuto(gpa, anna, 0, false);
    defer pose.deinit(gpa);

    try std.testing.expectEqual(@as(u32, 192), pose.width);
    try std.testing.expect(pose.height >= 60 and pose.height <= 180);

    var ink: usize = 0;
    var transparent: usize = 0;
    for (pose.pixels) |px| {
        if (px >> 24 == 0) transparent += 1 else ink += 1;
    }
    // A real figure: substantial ink, and a transparent background around it.
    try std.testing.expect(ink > 1000);
    try std.testing.expect(transparent > 1000);
}
