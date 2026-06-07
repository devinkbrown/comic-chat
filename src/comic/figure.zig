//! Assemble a complete Comic Chat character figure from a decoded .avb.
//!
//! Humanoid avatars store the figure as two layers — a head/expression pose and
//! a (taller) body/gesture pose — composited at the neck using the pose-metadata
//! anchors. Creature/totem avatars (Jordan, Tiki) store a complete figure per
//! pose and are rendered directly. Returns an RGBA image with a TRANSPARENT
//! background (so it can be composited over a panel scene); the figure itself is
//! an opaque white "sticker" silhouette with black ink, matching the original.

const std = @import("std");
const bgb = @import("../assets/bgb.zig");
const Canvas = @import("../render/canvas.zig").Canvas;

pub const Image = bgb.Image;

/// Vertical seat applied to the metadata neck anchor (a single global value is a
/// compromise across body types; the record's d(x,y) field may hold the exact
/// per-pose offset — see docs).
pub const neck_seat: i32 = 10;

/// Assemble the figure for `avb` using head pose `emotion` and body pose `gesture`
/// (both 0-based; clamped to what's available — index 0 = neutral).
pub fn assemble(gpa: std.mem.Allocator, avb: []const u8, emotion: usize, gesture: usize) !Image {
    var body = bgb.decodePoseAuto(gpa, avb, gesture, true) catch
        try bgb.decodePoseAuto(gpa, avb, 0, true);
    defer body.deinit(gpa);

    var head_opt: ?Image = bgb.decodePoseAuto(gpa, avb, emotion, false) catch
        (bgb.decodePoseAuto(gpa, avb, 0, false) catch null);
    defer if (head_opt) |*h| h.deinit(gpa);

    // Real head/body split only when the body is much taller than the head.
    const real_split = if (head_opt) |h| body.height * 10 > h.height * 16 else false;
    if (!real_split) return try solo(gpa, body);

    const head = head_opt.?;
    var dx: i32 = undefined;
    var dy: i32 = undefined;
    if (bgb.neckAnchors(avb)) |a| {
        dx = a.body.x - a.head.x;
        dy = a.body.y - a.head.y + neck_seat;
    } else {
        const bt = topInkRow(body);
        dx = centroidX(body, bt, bt + 18) - centroidX(head, @as(i32, @intCast(head.height)) - 25, @as(i32, @intCast(head.height)));
        dy = (bt + 26) - headNeckBottom(head, centroidX(head, @as(i32, @intCast(head.height)) - 25, @as(i32, @intCast(head.height))));
    }

    const bw: i32 = @intCast(body.width);
    const bh: i32 = @intCast(body.height);
    const hw: i32 = @intCast(head.width);
    const hh: i32 = @intCast(head.height);
    const body_x = @max(@as(i32, 0), -dx);
    const body_y = @max(@as(i32, 0), -dy);
    const head_x = body_x + dx;
    const head_y = body_y + dy;
    const W: u32 = @intCast(@max(body_x + bw, head_x + hw));
    const H: u32 = @intCast(@max(body_y + bh, head_y + hh));

    var c = try Canvas.init(gpa, W, H);
    defer c.deinit(gpa);
    c.clear(0x00000000);
    composite(&c, body.pixels, body.width, body.height, body_x, body_y, 14);
    composite(&c, head.pixels, head.width, head.height, head_x, head_y, 12);
    return dupe(gpa, &c);
}

fn solo(gpa: std.mem.Allocator, img: Image) !Image {
    var c = try Canvas.init(gpa, img.width, img.height);
    defer c.deinit(gpa);
    c.clear(0x00000000);
    composite(&c, img.pixels, img.width, img.height, 0, 0, 0);
    return dupe(gpa, &c);
}

fn dupe(gpa: std.mem.Allocator, c: *const Canvas) !Image {
    const px = try gpa.dupe(u32, c.px);
    return .{ .width = c.width, .height = c.height, .pixels = px };
}

/// Composite a transparent-keyed image onto `c` (opaque; upper layer occludes),
/// skipping the right `crop_r` columns (trailing-strip cleanup).
pub fn composite(c: *Canvas, src: []const u32, sw: u32, sh: u32, dx: i32, dy: i32, crop_r: u32) void {
    var y: u32 = 0;
    while (y < sh) : (y += 1) {
        var x: u32 = 0;
        while (x + crop_r < sw) : (x += 1) {
            const p = src[y * sw + x];
            if (p >> 24 == 0) continue;
            const ox = dx + @as(i32, @intCast(x));
            const oy = dy + @as(i32, @intCast(y));
            if (ox < 0 or oy < 0 or ox >= c.width or oy >= c.height) continue;
            c.px[@as(usize, @intCast(oy)) * c.width + @as(usize, @intCast(ox))] = p;
        }
    }
}

fn topInkRow(img: Image) i32 {
    var y: u32 = 0;
    while (y < img.height) : (y += 1) {
        var x: u32 = 0;
        while (x < img.width) : (x += 1) if (img.pixels[y * img.width + x] >> 24 != 0) return @intCast(y);
    }
    return 0;
}

fn headNeckBottom(img: Image, nx: i32) i32 {
    const x0: u32 = @intCast(@max(@as(i32, 0), nx - 18));
    const x1: u32 = @intCast(@min(@as(i32, @intCast(img.width)), nx + 18));
    var y: i32 = @as(i32, @intCast(img.height)) - 1;
    while (y >= 0) : (y -= 1) {
        const row = @as(usize, @intCast(y)) * img.width;
        var x: u32 = x0;
        while (x < x1) : (x += 1) if (img.pixels[row + x] >> 24 != 0) return y;
    }
    return @as(i32, @intCast(img.height)) - 1;
}

fn centroidX(img: Image, y0: i32, y1: i32) i32 {
    var sum: i64 = 0;
    var cnt: i64 = 0;
    var y: i32 = @max(0, y0);
    const ye: i32 = @min(@as(i32, @intCast(img.height)), y1);
    while (y < ye) : (y += 1) {
        const row = @as(usize, @intCast(y)) * img.width;
        var x: u32 = 0;
        while (x < img.width) : (x += 1) if (img.pixels[row + x] >> 24 != 0) {
            sum += x;
            cnt += 1;
        };
    }
    if (cnt == 0) return @intCast(img.width / 2);
    return @intCast(@divTrunc(sum, cnt));
}

test "assemble produces a figure with transparent margins and opaque ink" {
    const gpa = std.testing.allocator;
    const anna = @embedFile("../assets/testdata/anna.avb");
    var fig = try assemble(gpa, anna, 0, 0);
    defer fig.deinit(gpa);
    try std.testing.expect(fig.width > 0 and fig.height > 0);
    var opaque_px: usize = 0;
    for (fig.pixels) |p| {
        if (p >> 24 != 0) opaque_px += 1;
    }
    try std.testing.expect(opaque_px > 1000);
}
