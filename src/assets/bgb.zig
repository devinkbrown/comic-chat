//! Exact raster decoder for Microsoft Comic Chat avatar/backdrop assets.
//!
//! `avb.zig` parses the released source's tagged record format. This module
//! follows each authored image offset, reads its palette/BITMAPINFOHEADER and
//! length-prefixed zlib payload, and rasterizes the actual bitmap width (not the
//! padded scan-line width).

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

pub const Error = avb.ParseError || error{
    UnsupportedFormat,
    UnsupportedPalette,
    UnsupportedDepth,
    InvalidBitmap,
    PoseNotFound,
} || std.mem.Allocator.Error || std.Io.Reader.Error;

fn rdU16(b: []const u8, o: usize) Error!u16 {
    if (o + 2 > b.len) return error.Truncated;
    return @as(u16, b[o]) | (@as(u16, b[o + 1]) << 8);
}

fn rdU32(b: []const u8, o: usize) Error!u32 {
    if (o + 4 > b.len) return error.Truncated;
    return @as(u32, b[o]) | (@as(u32, b[o + 1]) << 8) |
        (@as(u32, b[o + 2]) << 16) | (@as(u32, b[o + 3]) << 24);
}

fn rdI32(b: []const u8, o: usize) Error!i32 {
    return @bitCast(try rdU32(b, o));
}

const Palette = struct {
    values: []const u8 = &.{},
    count: usize = 0,
};

fn localPalette(bytes: []const u8, pos: *usize) Error!Palette {
    // GetProperPalette in avbfile.cpp expects AK_COLORPALETTE followed by its
    // new-tag size, then CAvatarPalette::Read's u16 count and RGB triples.
    if (try rdU16(bytes, pos.*) != 257) return error.UnsupportedPalette;
    const size = try rdU16(bytes, pos.* + 2);
    const payload = pos.* + 4;
    const end = std.math.add(usize, payload, size) catch return error.Truncated;
    if (end > bytes.len) return error.Truncated;
    const count = try rdU16(bytes, payload);
    const byte_len = std.math.mul(usize, count, 3) catch return error.InvalidBitmap;
    if (payload + 2 + byte_len > end) return error.Truncated;
    pos.* = end;
    return .{ .values = bytes[payload + 2 .. payload + 2 + byte_len], .count = count };
}

fn paletteColor(palette: Palette, index: usize) Error!u32 {
    if (index >= palette.count) return error.InvalidBitmap;
    const p = index * 3;
    return 0xff000000 | (@as(u32, palette.values[p]) << 16) |
        (@as(u32, palette.values[p + 1]) << 8) | palette.values[p + 2];
}

fn decodeZlibImageRef(gpa: std.mem.Allocator, bytes: []const u8, ref: avb.ImageRef) Error!Image {
    var pos: usize = ref.offset;
    if (pos >= bytes.len) return error.Truncated;

    const palette: Palette = switch (ref.palette) {
        .local => try localPalette(bytes, &pos),
        .masked_mono, .dual_mask => .{},
        .monochrome => .{},
        else => return error.UnsupportedPalette,
    };

    const header_size = try rdU32(bytes, pos);
    if (header_size < 40 or header_size > 240) return error.InvalidBitmap;
    if (pos + header_size > bytes.len) return error.Truncated;
    const signed_width = try rdI32(bytes, pos + 4);
    const signed_height = try rdI32(bytes, pos + 8);
    if (signed_width <= 0 or signed_height == 0 or signed_height == std.math.minInt(i32))
        return error.InvalidBitmap;
    const width: u32 = @intCast(signed_width);
    const height: u32 = @intCast(@abs(signed_height));
    if (width > 8192 or height > 8192) return error.InvalidBitmap;
    if (try rdU16(bytes, pos + 12) != 1) return error.InvalidBitmap;
    const bits = try rdU16(bytes, pos + 14);
    switch (ref.palette) {
        .local => if (bits != 1 and bits != 4 and bits != 8) return error.UnsupportedDepth,
        .masked_mono, .dual_mask => if (bits != 2) return error.UnsupportedDepth,
        .monochrome => if (bits != 1) return error.UnsupportedDepth,
        else => unreachable,
    }

    pos += header_size;
    const raw_len = try rdU32(bytes, pos);
    const compressed_len = try rdU32(bytes, pos + 4);
    pos += 8;
    const compressed_end = std.math.add(usize, pos, compressed_len) catch return error.Truncated;
    if (compressed_end > bytes.len) return error.Truncated;

    const stride: usize = ((@as(usize, width) * bits + 31) / 32) * 4;
    const expected_len = std.math.mul(usize, stride, height) catch return error.InvalidBitmap;
    if (raw_len != expected_len) return error.InvalidBitmap;
    const raw = try gpa.alloc(u8, expected_len);
    defer gpa.free(raw);

    var input: std.Io.Reader = .fixed(bytes[pos..compressed_end]);
    var window: [flate.max_window_len]u8 = undefined;
    var decoder = flate.Decompress.init(&input, .zlib, &window);
    try decoder.reader.readSliceAll(raw);

    const pixel_count = std.math.mul(usize, width, height) catch return error.InvalidBitmap;
    const pixels = try gpa.alloc(u32, pixel_count);
    errdefer gpa.free(pixels);

    var y: u32 = 0;
    while (y < height) : (y += 1) {
        const source_y: u32 = if (signed_height > 0) height - 1 - y else y;
        const row = @as(usize, source_y) * stride;
        var x: u32 = 0;
        while (x < width) : (x += 1) {
            const value: u8 = switch (bits) {
                1 => (raw[row + x / 8] >> @intCast(7 - x % 8)) & 1,
                2 => (raw[row + x / 4] >> @intCast(6 - 2 * (x % 4))) & 3,
                4 => if (x % 2 == 0) raw[row + x / 2] >> 4 else raw[row + x / 2] & 0x0f,
                8 => raw[row + x],
                else => unreachable,
            };
            pixels[@as(usize, y) * width + x] = switch (ref.palette) {
                .local => try paletteColor(palette, value),
                .monochrome => if (value == 0) 0xffffffff else 0xff000000,
                .masked_mono => switch (value) {
                    // CPose::ConvertMasksCommon splits each two-bit pair into
                    // drawing/mask/aura monochrome DIBs, then applies the
                    // source's `image &= mask` aura workaround. With its
                    // white=0, black=1 palette this leaves only 11 as black;
                    // 01 and 10 are opaque white sticker pixels.
                    0 => 0x00000000,
                    1 => 0xffffffff,
                    2 => 0xffffffff,
                    3 => 0xff000000,
                    else => unreachable,
                },
                .dual_mask => if (value == 0) 0x00000000 else 0xffffffff,
                else => unreachable,
            };
        }
    }
    return .{ .width = width, .height = height, .pixels = pixels };
}

fn dibPaletteColor(bytes: []const u8, table: usize, index: usize, entry_size: usize) Error!u32 {
    const off = std.math.add(usize, table, index * entry_size) catch return error.Truncated;
    if (off + entry_size > bytes.len or entry_size < 3) return error.Truncated;
    return 0xff000000 | (@as(u32, bytes[off + 2]) << 16) |
        (@as(u32, bytes[off + 1]) << 8) | bytes[off];
}

/// Decode the old `AIF_DIB` resource representation. `CAvatarDIB::Load` reads
/// a complete BMP stream at the authored offset (including BITMAPFILEHEADER),
/// accepts Windows BITMAPINFOHEADER and OS/2 BITMAPCOREHEADER, and obtains the
/// palette from that stream rather than an AVB palette record.
fn decodeDibImageRef(gpa: std.mem.Allocator, bytes: []const u8, ref: avb.ImageRef) Error!Image {
    if (ref.palette != .none) return error.UnsupportedPalette;
    const start: usize = ref.offset;
    if (start + 14 > bytes.len) return error.Truncated;
    if (try rdU16(bytes, start) != 0x4d42) return error.InvalidBitmap;
    const file_size: usize = try rdU32(bytes, start + 2);
    const bits_rel: usize = try rdU32(bytes, start + 10);
    if (file_size < 14 or bits_rel < 14) return error.InvalidBitmap;
    const file_end = std.math.add(usize, start, file_size) catch return error.Truncated;
    const bits_start = std.math.add(usize, start, bits_rel) catch return error.Truncated;
    if (file_end > bytes.len or bits_start > file_end) return error.Truncated;

    const info = start + 14;
    const header_size = try rdU32(bytes, info);
    var signed_width: i32 = 0;
    var signed_height: i32 = 0;
    var bits: u16 = 0;
    var compression: u32 = 0;
    var palette_count: usize = 0;
    var palette_entry_size: usize = 0;
    if (header_size == 40) {
        if (info + 40 > file_end) return error.Truncated;
        signed_width = try rdI32(bytes, info + 4);
        signed_height = try rdI32(bytes, info + 8);
        if (try rdU16(bytes, info + 12) != 1) return error.InvalidBitmap;
        bits = try rdU16(bytes, info + 14);
        compression = try rdU32(bytes, info + 16);
        palette_count = try rdU32(bytes, info + 32);
        palette_entry_size = 4;
    } else if (header_size == 12) {
        // BITMAPCOREHEADER uses unsigned dimensions and RGBTRIPLE entries.
        if (info + 12 > file_end) return error.Truncated;
        signed_width = try rdU16(bytes, info + 4);
        signed_height = try rdU16(bytes, info + 6);
        if (try rdU16(bytes, info + 8) != 1) return error.InvalidBitmap;
        bits = try rdU16(bytes, info + 10);
        palette_entry_size = 3;
    } else return error.InvalidBitmap;

    if (compression != 0) return error.UnsupportedFormat;
    if (signed_width <= 0 or signed_height == 0 or signed_height == std.math.minInt(i32))
        return error.InvalidBitmap;
    const width: u32 = @intCast(signed_width);
    const height: u32 = @intCast(@abs(signed_height));
    if (width > 8192 or height > 8192) return error.InvalidBitmap;
    if (bits != 1 and bits != 4 and bits != 8 and bits != 16 and bits != 24 and bits != 32)
        return error.UnsupportedDepth;

    const default_palette_count: usize = switch (bits) {
        1 => 2,
        4 => 16,
        8 => 256,
        else => 0,
    };
    if (palette_count == 0) palette_count = default_palette_count;
    // NumDIBColorEntries clamps bogus biClrUsed values to the bit-depth limit.
    if (palette_count > default_palette_count) palette_count = default_palette_count;
    const table_start = std.math.add(usize, info, header_size) catch return error.Truncated;
    const table_len = std.math.mul(usize, palette_count, palette_entry_size) catch return error.Truncated;
    const table_end = std.math.add(usize, table_start, table_len) catch return error.Truncated;
    if (table_end > bits_start) return error.InvalidBitmap;

    const stride: usize = ((@as(usize, width) * bits + 31) / 32) * 4;
    const bitmap_len = std.math.mul(usize, stride, height) catch return error.InvalidBitmap;
    if (bitmap_len > file_end - bits_start) return error.Truncated;
    const pixel_count = std.math.mul(usize, width, height) catch return error.InvalidBitmap;
    const pixels = try gpa.alloc(u32, pixel_count);
    errdefer gpa.free(pixels);

    var y: u32 = 0;
    while (y < height) : (y += 1) {
        const source_y: u32 = if (signed_height > 0) height - 1 - y else y;
        const row = bits_start + @as(usize, source_y) * stride;
        var x: u32 = 0;
        while (x < width) : (x += 1) {
            const color: u32 = switch (bits) {
                1 => try dibPaletteColor(bytes, table_start, (bytes[row + x / 8] >> @intCast(7 - x % 8)) & 1, palette_entry_size),
                4 => try dibPaletteColor(bytes, table_start, if (x % 2 == 0) bytes[row + x / 2] >> 4 else bytes[row + x / 2] & 0x0f, palette_entry_size),
                8 => try dibPaletteColor(bytes, table_start, bytes[row + x], palette_entry_size),
                16 => blk: {
                    const value = try rdU16(bytes, row + @as(usize, x) * 2);
                    const r = @as(u32, (value >> 10) & 0x1f) * 255 / 31;
                    const g = @as(u32, (value >> 5) & 0x1f) * 255 / 31;
                    const b = @as(u32, value & 0x1f) * 255 / 31;
                    break :blk 0xff000000 | (r << 16) | (g << 8) | b;
                },
                24 => blk: {
                    const p = row + @as(usize, x) * 3;
                    break :blk 0xff000000 | (@as(u32, bytes[p + 2]) << 16) |
                        (@as(u32, bytes[p + 1]) << 8) | bytes[p];
                },
                32 => blk: {
                    const p = row + @as(usize, x) * 4;
                    // StretchDIBits treats BI_RGB's high byte as reserved.
                    break :blk 0xff000000 | (@as(u32, bytes[p + 2]) << 16) |
                        (@as(u32, bytes[p + 1]) << 8) | bytes[p];
                },
                else => unreachable,
            };
            pixels[@as(usize, y) * width + x] = color;
        }
    }
    return .{ .width = width, .height = height, .pixels = pixels };
}

/// Decode one source-authored resource using the exact AVB image-format byte.
pub fn decodeImageRef(gpa: std.mem.Allocator, bytes: []const u8, ref: avb.ImageRef) Error!Image {
    return switch (ref.format) {
        .dib => decodeDibImageRef(gpa, bytes, ref),
        .zlib => decodeZlibImageRef(gpa, bytes, ref),
        else => error.UnsupportedFormat,
    };
}

pub fn decodeBackground(gpa: std.mem.Allocator, bytes: []const u8) Error!Image {
    const ref = try avb.backdropImage(bytes);
    return decodeImageRef(gpa, bytes, ref);
}

/// Decode the dedicated conversation-star icon authored in an AVB rather than
/// approximating it with a full neutral body (`CBodyUnary`, panel.cpp:1437).
pub fn decodeIcon(gpa: std.mem.Allocator, bytes: []const u8) Error!Image {
    return decodeImageRef(gpa, bytes, try avb.iconImage(bytes));
}

pub const Point = struct { x: i32, y: i32 };
pub const NeckAnchors = struct { head: Point, body: Point };

fn point(p: avb.Point) Point {
    return .{ .x = p.x, .y = p.y };
}

/// Return the old client's exact neutral head/torso join anchors. The resulting
/// displacement is `body - head`, equivalent to
/// `torso.center + face.delta - face.center` in `CBodyDouble::GetBodyBox`.
pub fn neckAnchors(data: []const u8) ?NeckAnchors {
    return neckAnchorsForEmotion(data, 9, 0, 9, 0);
}

/// Return the exact selected face/torso join anchors. `CBodyDouble::GetDimInfo`
/// computes the head displacement from the selected records, not from a fixed
/// neutral pose: torso.center + face.delta - face.center.
pub fn neckAnchorsForEmotion(
    data: []const u8,
    face_emotion: u16,
    face_intensity: u8,
    torso_emotion: u16,
    torso_intensity: u8,
) ?NeckAnchors {
    const gpa = std.heap.page_allocator;
    var table = avb.parsePoseTable(gpa, data) catch return null;
    defer table.deinit(gpa);
    const face = selectPose(table.records, .face, face_emotion, face_intensity) orelse return null;
    const torso = selectPose(table.records, .torso, torso_emotion, torso_intensity) orelse return null;
    return .{
        .head = .{
            .x = @as(i32, face.center.x) - face.delta.x,
            .y = @as(i32, face.center.y) - face.delta.y,
        },
        .body = point(torso.center),
    };
}

pub const PoseKind = enum { head, body };

pub const PoseMeta = struct {
    kind: PoseKind,
    layer: avb.PoseLayer,
    mouth: Point,
    neck: Point,
    delta: Point,
    /// Exact index from avatario.cpp (kept as `code` for API compatibility).
    code: i16,
    intensity: u8,
    image_offset: u32,
    pose_id: u32,
};

/// Parse the source-defined pose table. Caller owns the returned slice.
pub fn poseTable(gpa: std.mem.Allocator, data: []const u8) ![]PoseMeta {
    var parsed = try avb.parsePoseTable(gpa, data);
    defer parsed.deinit(gpa);
    const result = try gpa.alloc(PoseMeta, parsed.records.len);
    for (parsed.records, result) |record, *out| {
        out.* = .{
            .kind = if (record.layer == .face) .head else .body,
            .layer = record.layer,
            .mouth = point(record.face),
            .neck = point(record.center),
            .delta = point(record.delta),
            .code = @intCast(record.emotion_index),
            .intensity = record.intensity,
            .image_offset = record.images[0].offset,
            .pose_id = record.pose_id,
        };
    }
    return result;
}

fn emotionalSpoke(index: u16) ?u8 {
    return switch (index) {
        1...8 => @intCast(index - 1),
        9 => 0, // neutral shares happy's angle but has intensity zero
        else => null,
    };
}

fn angularDistance(a: u16, b: u16) u8 {
    const sa = emotionalSpoke(a).?;
    const sb = emotionalSpoke(b).?;
    const linear = if (sa > sb) sa - sb else sb - sa;
    return @min(linear, 8 - linear);
}

/// Match an authored pose using the original client's angle-first,
/// closest-intensity rule. Gesture indices (10+) require an exact match.
pub fn selectPose(records: []const avb.PoseRecord, layer: avb.PoseLayer, emotion_index: u16, intensity: u8) ?*const avb.PoseRecord {
    var best: ?*const avb.PoseRecord = null;
    var best_angle: u8 = 255;
    var best_intensity: u8 = 255;
    for (records) |*record| {
        if (record.layer != layer) continue;
        if (emotion_index >= 10) {
            if (record.emotion_index == emotion_index) return record;
            continue;
        }
        if (emotionalSpoke(record.emotion_index) == null) continue;
        const angle = angularDistance(record.emotion_index, emotion_index);
        const intensity_delta = if (record.intensity > intensity)
            record.intensity - intensity
        else
            intensity - record.intensity;
        if (angle < best_angle or (angle == best_angle and intensity_delta < best_intensity)) {
            best = record;
            best_angle = angle;
            best_intensity = intensity_delta;
        }
    }
    return best;
}

pub fn decodePoseForEmotion(
    gpa: std.mem.Allocator,
    data: []const u8,
    layer: avb.PoseLayer,
    emotion_index: u16,
    intensity: u8,
) Error!Image {
    var table = try avb.parsePoseTable(gpa, data);
    defer table.deinit(gpa);
    const record = selectPose(table.records, layer, emotion_index, intensity) orelse return error.PoseNotFound;
    return decodeImageRef(gpa, data, record.images[0]);
}

/// Decode the Nth distinct authored pose in a layer. Repeated AVB records that
/// share a pose ID do not shift the ordinal.
pub fn decodePoseByOrdinal(gpa: std.mem.Allocator, data: []const u8, index: usize, layer: avb.PoseLayer) Error!Image {
    var table = try avb.parsePoseTable(gpa, data);
    defer table.deinit(gpa);
    var found: usize = 0;
    var previous_pose_id: ?u32 = null;
    for (table.records) |record| {
        if (record.layer != layer) continue;
        if (previous_pose_id != null and previous_pose_id.? == record.pose_id) continue;
        previous_pose_id = record.pose_id;
        if (found == index) return decodeImageRef(gpa, data, record.images[0]);
        found += 1;
    }
    return error.PoseNotFound;
}

fn legacyHeadSelection(index: usize) struct { emotion: u16, intensity: u8 } {
    // Existing public Emotion.headIndex values are semantic selections, not AVB
    // record ordinals. Map them to Microsoft's exact avatario.cpp indices.
    return switch (index) {
        0 => .{ .emotion = 9, .intensity = 0 }, // neutral
        1 => .{ .emotion = 1, .intensity = 255 }, // happy
        2 => .{ .emotion = 9, .intensity = 0 }, // talking has no old pose
        3 => .{ .emotion = 4, .intensity = 255 }, // surprised -> scared
        4 => .{ .emotion = 5, .intensity = 255 }, // sad
        5 => .{ .emotion = 6, .intensity = 255 }, // angry
        6 => .{ .emotion = 7, .intensity = 255 }, // shouting
        7 => .{ .emotion = 2, .intensity = 255 }, // coy
        8 => .{ .emotion = 3, .intensity = 255 }, // bored
        9 => .{ .emotion = 8, .intensity = 255 }, // laughing
        else => .{ .emotion = 9, .intensity = 0 },
    };
}

/// Compatibility entry point used by `figure.zig`. Head values are semantic
/// `Emotion.headIndex` selections; body values remain distinct-pose ordinals.
pub fn decodePoseAuto(gpa: std.mem.Allocator, data: []const u8, index: usize, tall: bool) Error!Image {
    const header = try avb.parse(data);
    if (header.kind == .simple_avatar) {
        if (!tall) return error.PoseNotFound;
        return decodePoseByOrdinal(gpa, data, index, .body);
    }
    if (tall) return decodePoseByOrdinal(gpa, data, index, .torso);
    const selection = legacyHeadSelection(index);
    return decodePoseForEmotion(gpa, data, .face, selection.emotion, selection.intensity);
}

// --- Tests ----------------------------------------------------------------

test "decodeBackground follows backdrop record and local palette" {
    const gpa = std.testing.allocator;
    var image = try decodeBackground(gpa, @embedFile("testdata/field.bgb"));
    defer image.deinit(gpa);
    try std.testing.expectEqual(@as(u32, 315), image.width);
    try std.testing.expectEqual(@as(u32, 315), image.height);

    var distinct = std.AutoHashMap(u32, void).init(gpa);
    defer distinct.deinit();
    for (image.pixels) |pixel| {
        try std.testing.expectEqual(@as(u32, 0xff), pixel >> 24);
        try distinct.put(pixel, {});
    }
    try std.testing.expect(distinct.count() > 1);
    try std.testing.expect(distinct.count() <= 16);
}

test "old AIF_DIB decodes an embedded Windows BMP bottom-up" {
    const bmp =
        "BM\x46\x00\x00\x00\x00\x00\x00\x00\x36\x00\x00\x00" ++
        "\x28\x00\x00\x00\x02\x00\x00\x00\x02\x00\x00\x00" ++
        "\x01\x00\x18\x00\x00\x00\x00\x00\x10\x00\x00\x00" ++
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" ++
        "\x00\x00\x00\x00" ++
        // File rows are bottom-up: blue/white, then red/green.
        "\xff\x00\x00\xff\xff\xff\x00\x00" ++
        "\x00\x00\xff\x00\xff\x00\x00\x00";
    var image = try decodeImageRef(std.testing.allocator, bmp, .{
        .offset = 0,
        .format = .dib,
        .palette = .none,
    });
    defer image.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u32, 2), image.width);
    try std.testing.expectEqual(@as(u32, 2), image.height);
    try std.testing.expectEqualSlices(u32, &.{
        0xffff0000, 0xff00ff00,
        0xff0000ff, 0xffffffff,
    }, image.pixels);
}

test "old AIF_DIB converts an OS/2 core palette and scan line" {
    const bmp =
        "BM\x24\x00\x00\x00\x00\x00\x00\x00\x20\x00\x00\x00" ++
        "\x0c\x00\x00\x00\x02\x00\x01\x00\x01\x00\x01\x00" ++
        // OS/2 RGBTRIPLE palette: red, blue.
        "\x00\x00\xff\xff\x00\x00" ++
        // 0,1 then DWORD padding.
        "\x40\x00\x00\x00";
    var image = try decodeImageRef(std.testing.allocator, bmp, .{
        .offset = 0,
        .format = .dib,
        .palette = .none,
    });
    defer image.deinit(std.testing.allocator);
    try std.testing.expectEqualSlices(u32, &.{ 0xffff0000, 0xff0000ff }, image.pixels);
}

test "pose table is exact and keeps shared image linkage" {
    const gpa = std.testing.allocator;
    const table = try poseTable(gpa, @embedFile("testdata/anna.avb"));
    defer gpa.free(table);
    try std.testing.expectEqual(@as(usize, 34), table.len);
    try std.testing.expectEqual(PoseKind.head, table[0].kind);
    try std.testing.expectEqual(@as(i16, 9), table[0].code);
    try std.testing.expectEqual(@as(u8, 0), table[0].intensity);
    try std.testing.expectEqual(@as(u32, 0x603), table[0].image_offset);
    try std.testing.expectEqual(table[2].pose_id, table[3].pose_id);
    try std.testing.expectEqual(table[2].image_offset, table[3].image_offset);
}

test "selection uses authored emotion and closest intensity" {
    const gpa = std.testing.allocator;
    var table = try avb.parsePoseTable(gpa, @embedFile("testdata/anna.avb"));
    defer table.deinit(gpa);
    const neutral = selectPose(table.records, .face, 9, 0).?;
    try std.testing.expectEqual(@as(u16, 9), neutral.emotion_index);
    try std.testing.expectEqual(@as(u8, 0), neutral.intensity);
    const happy = selectPose(table.records, .face, 1, 255).?;
    try std.testing.expectEqual(@as(u16, 1), happy.emotion_index);
    try std.testing.expectEqual(@as(u8, 102), happy.intensity);
}

test "decode exact face dimensions and masked-mono colors" {
    const gpa = std.testing.allocator;
    const anna = @embedFile("testdata/anna.avb");
    var pose = try decodePoseForEmotion(gpa, anna, .face, 9, 0);
    defer pose.deinit(gpa);
    // The old scanner reported padded stride width 192. BITMAPINFOHEADER says
    // the actual authored width is 189.
    try std.testing.expectEqual(@as(u32, 189), pose.width);
    try std.testing.expectEqual(@as(u32, 135), pose.height);
    var transparent: usize = 0;
    var black: usize = 0;
    var white: usize = 0;
    for (pose.pixels) |pixel| switch (pixel) {
        0x00000000 => transparent += 1,
        0xff000000 => black += 1,
        0xffffffff => white += 1,
        else => return error.InvalidBitmap,
    };
    try std.testing.expect(transparent > 1000);
    try std.testing.expect(black > 100);
    try std.testing.expect(white > 100);
}

test "neutral join reproduces CBodyDouble offset formula" {
    const anchors = neckAnchors(@embedFile("testdata/anna.avb")).?;
    // first neutral face: center=(92,111), delta=(-4,3)
    // first neutral torso: center=(93,26)
    try std.testing.expectEqual(@as(i32, -3), anchors.body.x - anchors.head.x);
    try std.testing.expectEqual(@as(i32, -82), anchors.body.y - anchors.head.y);
}

test "simple avatars decode whole-body records without a fake head layer" {
    const gpa = std.testing.allocator;
    const jordan = @embedFile("testdata/jordan.avb");
    try std.testing.expectError(error.PoseNotFound, decodePoseAuto(gpa, jordan, 0, false));
    var body = try decodePoseAuto(gpa, jordan, 0, true);
    defer body.deinit(gpa);
    try std.testing.expect(body.width > 0 and body.height > 0);
    var white: usize = 0;
    var black: usize = 0;
    for (body.pixels) |pixel| switch (pixel) {
        0xffffffff => white += 1,
        0xff000000 => black += 1,
        else => {},
    };
    // Microsoft's published Jordan character sheet is predominantly white
    // with black ink and skirt, not the inverse silhouette.
    try std.testing.expect(white > black);
}

test "generated Tiki HD AVB decodes its icon and every authored body pose" {
    const data = @embedFile("generated/tiki-reimagined-hd-v1.avb");
    var icon = try decodeIcon(std.testing.allocator, data);
    defer icon.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u32, 64), icon.width);
    try std.testing.expectEqual(@as(u32, 64), icon.height);

    var table = try avb.parsePoseTable(std.testing.allocator, data);
    defer table.deinit(std.testing.allocator);
    for (table.records) |record| {
        var image = try decodeImageRef(std.testing.allocator, data, record.images[0]);
        defer image.deinit(std.testing.allocator);
        try std.testing.expectEqual(@as(u32, 240), image.width);
        try std.testing.expectEqual(@as(u32, 280), image.height);
    }
}

test "every distinct primary pose in bundled avatars decodes from its record offset" {
    const gpa = std.testing.allocator;
    const blobs = [_][]const u8{
        @embedFile("testdata/anna.avb"),     @embedFile("testdata/armando.avb"),
        @embedFile("testdata/bolo.avb"),     @embedFile("testdata/cro.avb"),
        @embedFile("testdata/dan.avb"),      @embedFile("testdata/denise.avb"),
        @embedFile("testdata/hugh.avb"),     @embedFile("testdata/jordan.avb"),
        @embedFile("testdata/kevin.avb"),    @embedFile("testdata/kwensa.avb"),
        @embedFile("testdata/lance.avb"),    @embedFile("testdata/lynnea.avb"),
        @embedFile("testdata/margaret.avb"), @embedFile("testdata/maynard.avb"),
        @embedFile("testdata/mike.avb"),     @embedFile("testdata/rebecca.avb"),
        @embedFile("testdata/sage.avb"),     @embedFile("testdata/scotty.avb"),
        @embedFile("testdata/susan.avb"),    @embedFile("testdata/tiki.avb"),
        @embedFile("testdata/tongtyed.avb"), @embedFile("testdata/xeno.avb"),
    };
    inline for (blobs) |blob| {
        var table = try avb.parsePoseTable(gpa, blob);
        defer table.deinit(gpa);
        var seen = std.AutoHashMap(u32, void).init(gpa);
        defer seen.deinit();
        for (table.records) |record| {
            const offset = record.images[0].offset;
            if (seen.contains(offset)) continue;
            try seen.put(offset, {});
            var image = try decodeImageRef(gpa, blob, record.images[0]);
            defer image.deinit(gpa);
            try std.testing.expect(image.width > 0 and image.height > 0);
            try std.testing.expectEqual(@as(usize, image.width) * image.height, image.pixels.len);
        }
    }
}
