//! Minimal pure-Zig PNG encoder.
//!
//! Encodes top-down 0xAARRGGBB pixels as an 8-bit RGB PNG. IDAT uses a zlib
//! stream containing stored deflate blocks, so encoding is deterministic and
//! dependency-free.

const std = @import("std");

const signature = [_]u8{ 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };

pub const Error = error{
    DimensionOverflow,
    PixelCountMismatch,
} || std.mem.Allocator.Error;

fn writeU32(out: *std.ArrayList(u8), gpa: std.mem.Allocator, value: u32) !void {
    var buf: [4]u8 = undefined;
    std.mem.writeInt(u32, &buf, value, .big);
    try out.appendSlice(gpa, &buf);
}

fn appendChunk(out: *std.ArrayList(u8), gpa: std.mem.Allocator, typ: *const [4]u8, data: []const u8) !void {
    try writeU32(out, gpa, @intCast(data.len));
    try out.appendSlice(gpa, typ);
    try out.appendSlice(gpa, data);

    var crc = std.hash.Crc32.init();
    crc.update(typ);
    crc.update(data);
    try writeU32(out, gpa, crc.final());
}

fn buildRaw(gpa: std.mem.Allocator, pixels: []const u32, w: u32, h: u32) Error![]u8 {
    const row_pixels = @as(usize, w);
    const height = @as(usize, h);
    const expected = std.math.mul(usize, row_pixels, height) catch return error.DimensionOverflow;
    if (pixels.len != expected) return error.PixelCountMismatch;

    const rgb_bytes = std.math.mul(usize, expected, 3) catch return error.DimensionOverflow;
    const raw_len = std.math.add(usize, rgb_bytes, height) catch return error.DimensionOverflow;
    const raw = try gpa.alloc(u8, raw_len);
    errdefer gpa.free(raw);

    var src: usize = 0;
    var dst: usize = 0;
    var y: usize = 0;
    while (y < height) : (y += 1) {
        raw[dst] = 0; // filter type 0: none
        dst += 1;
        var x: usize = 0;
        while (x < row_pixels) : (x += 1) {
            const px = pixels[src];
            src += 1;
            raw[dst] = @intCast((px >> 16) & 0xff);
            raw[dst + 1] = @intCast((px >> 8) & 0xff);
            raw[dst + 2] = @intCast(px & 0xff);
            dst += 3;
        }
    }

    return raw;
}

fn zlibStored(gpa: std.mem.Allocator, raw: []const u8) Error![]u8 {
    const block_count = @max(@as(usize, 1), (raw.len -| 1) / 65535 + 1);
    const block_overhead = std.math.mul(usize, block_count, 5) catch return error.DimensionOverflow;
    const deflate_len = std.math.add(usize, raw.len, block_overhead) catch return error.DimensionOverflow;
    const out_len = std.math.add(usize, deflate_len, 2 + 4) catch return error.DimensionOverflow;
    const out = try gpa.alloc(u8, out_len);
    errdefer gpa.free(out);

    // CMF/FLG: deflate, 32 KiB window, fastest algorithm, valid FCHECK.
    out[0] = 0x78;
    out[1] = 0x01;

    var src: usize = 0;
    var dst: usize = 2;
    while (src < raw.len or (raw.len == 0 and src == 0)) {
        const remaining = raw.len - src;
        const n: u16 = @intCast(@min(remaining, 65535));
        const final = src + n == raw.len;

        out[dst] = if (final) 0x01 else 0x00; // BFINAL + stored block BTYPE
        dst += 1;
        std.mem.writeInt(u16, out[dst..][0..2], n, .little);
        dst += 2;
        std.mem.writeInt(u16, out[dst..][0..2], ~n, .little);
        dst += 2;
        @memcpy(out[dst..][0..n], raw[src..][0..n]);
        dst += n;
        src += n;

        if (raw.len == 0) break;
    }

    std.mem.writeInt(u32, out[dst..][0..4], std.hash.Adler32.hash(raw), .big);
    dst += 4;
    return out[0..dst];
}

/// Encode top-down 0xAARRGGBB pixels as an 8-bit RGB PNG.
pub fn encode(gpa: std.mem.Allocator, pixels: []const u32, w: u32, h: u32) Error![]u8 {
    if (w == 0 or h == 0) return error.DimensionOverflow;

    var out: std.ArrayList(u8) = .empty;
    errdefer out.deinit(gpa);

    try out.appendSlice(gpa, &signature);

    var ihdr: [13]u8 = undefined;
    std.mem.writeInt(u32, ihdr[0..4], w, .big);
    std.mem.writeInt(u32, ihdr[4..8], h, .big);
    ihdr[8] = 8; // bit depth
    ihdr[9] = 2; // truecolor RGB
    ihdr[10] = 0; // compression method
    ihdr[11] = 0; // filter method
    ihdr[12] = 0; // no interlace
    try appendChunk(&out, gpa, "IHDR", &ihdr);

    const raw = try buildRaw(gpa, pixels, w, h);
    defer gpa.free(raw);
    const idat = try zlibStored(gpa, raw);
    defer gpa.free(idat);
    try appendChunk(&out, gpa, "IDAT", idat);

    try appendChunk(&out, gpa, "IEND", &.{});
    return out.toOwnedSlice(gpa);
}

fn findChunk(png: []const u8, typ: []const u8) ?usize {
    var i: usize = 8;
    while (i + 12 <= png.len) {
        const len = std.mem.readInt(u32, png[i..][0..4], .big);
        if (std.mem.eql(u8, png[i + 4 .. i + 8], typ)) return i;
        i += 12 + len;
    }
    return null;
}

fn expectChunkCrc(png: []const u8, pos: usize) !void {
    const len = std.mem.readInt(u32, png[pos..][0..4], .big);
    const stored = std.mem.readInt(u32, png[pos + 8 + len ..][0..4], .big);
    try std.testing.expectEqual(std.hash.Crc32.hash(png[pos + 4 .. pos + 8 + len]), stored);
}

test "encode small RGB PNG" {
    const gpa = std.testing.allocator;
    const pixels = [_]u32{
        0xffff0000, 0xff00ff00,
        0xff0000ff, 0xffffffff,
    };

    const png = try encode(gpa, &pixels, 2, 2);
    defer gpa.free(png);

    try std.testing.expectEqualSlices(u8, &signature, png[0..8]);

    const ihdr = findChunk(png, "IHDR") orelse return error.TestExpectedEqual;
    const idat = findChunk(png, "IDAT") orelse return error.TestExpectedEqual;
    const iend = findChunk(png, "IEND") orelse return error.TestExpectedEqual;
    try std.testing.expect(ihdr < idat and idat < iend);
    try std.testing.expect(ihdr + 12 <= png.len);
    try std.testing.expect(idat + 12 <= png.len);
    try std.testing.expect(iend + 12 <= png.len);

    const ihdr_len = std.mem.readInt(u32, png[ihdr..][0..4], .big);
    const idat_len = std.mem.readInt(u32, png[idat..][0..4], .big);
    const iend_len = std.mem.readInt(u32, png[iend..][0..4], .big);
    try std.testing.expectEqual(@as(u32, 13), ihdr_len);
    try std.testing.expect(idat_len > 0);
    try std.testing.expectEqual(@as(u32, 0), iend_len);

    try expectChunkCrc(png, ihdr);
    try expectChunkCrc(png, idat);
    try expectChunkCrc(png, iend);

    var raw: [14]u8 = undefined;
    var in: std.Io.Reader = .fixed(png[idat + 8 .. idat + 8 + idat_len]);
    var window: [std.compress.flate.max_window_len]u8 = undefined;
    var dec = std.compress.flate.Decompress.init(&in, .zlib, &window);
    try dec.reader.readSliceAll(&raw);
    try std.testing.expectEqualSlices(u8, &.{
        0, 0xff, 0x00, 0x00, 0x00, 0xff, 0x00,
        0, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
    }, &raw);
}
