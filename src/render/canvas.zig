//! Pure-Zig software rasterizer with alpha blending: an RGBA framebuffer plus
//! the primitives to composite a comic panel — image blit, rects, lines,
//! antialiased proportional text, and smooth (SDF, antialiased) rounded
//! speech balloons with a tail. No GPU, no C.

const std = @import("std");
const font = @import("font.zig");

pub const Color = u32; // 0xAARRGGBB

pub const black: Color = 0xff000000;
pub const white: Color = 0xffffffff;

fn chan(c: Color, comptime shift: u5) u32 {
    return (c >> shift) & 0xff;
}

pub const Canvas = struct {
    width: u32,
    height: u32,
    px: []Color,

    pub fn init(gpa: std.mem.Allocator, width: u32, height: u32) !Canvas {
        return .{ .width = width, .height = height, .px = try gpa.alloc(Color, @as(usize, width) * height) };
    }

    pub fn deinit(self: *Canvas, gpa: std.mem.Allocator) void {
        gpa.free(self.px);
        self.* = undefined;
    }

    pub fn clear(self: *Canvas, color: Color) void {
        @memset(self.px, color);
    }

    fn inBounds(self: *const Canvas, x: i32, y: i32) bool {
        return x >= 0 and y >= 0 and x < self.width and y < self.height;
    }

    pub fn set(self: *Canvas, x: i32, y: i32, color: Color) void {
        if (!self.inBounds(x, y)) return;
        self.px[@as(usize, @intCast(y)) * self.width + @as(usize, @intCast(x))] = color;
    }

    /// Blend `rgb` over the existing pixel with coverage `a` (0..255).
    pub fn blendPixel(self: *Canvas, x: i32, y: i32, rgb: Color, a: u32) void {
        if (a == 0 or !self.inBounds(x, y)) return;
        const i = @as(usize, @intCast(y)) * self.width + @as(usize, @intCast(x));
        if (a >= 255) {
            self.px[i] = 0xff000000 | (rgb & 0x00ffffff);
            return;
        }
        const d = self.px[i];
        const ia = 255 - a;
        const r = (chan(rgb, 16) * a + chan(d, 16) * ia) / 255;
        const g = (chan(rgb, 8) * a + chan(d, 8) * ia) / 255;
        const b = (chan(rgb, 0) * a + chan(d, 0) * ia) / 255;
        self.px[i] = 0xff000000 | (r << 16) | (g << 8) | b;
    }

    pub fn fillRect(self: *Canvas, x: i32, y: i32, w: i32, h: i32, color: Color) void {
        var yy: i32 = y;
        while (yy < y + h) : (yy += 1) {
            var xx: i32 = x;
            while (xx < x + w) : (xx += 1) self.set(xx, yy, color);
        }
    }

    /// Copy an opaque source image (top-down RGBA) at (dx,dy), with clipping.
    pub fn blit(self: *Canvas, src: []const Color, sw: u32, sh: u32, dx: i32, dy: i32) void {
        var sy: u32 = 0;
        while (sy < sh) : (sy += 1) {
            var sx: u32 = 0;
            while (sx < sw) : (sx += 1)
                self.set(dx + @as(i32, @intCast(sx)), dy + @as(i32, @intCast(sy)), src[sy * sw + sx]);
        }
    }

    /// Nearest-neighbour upscale blit by integer factor `s`.
    pub fn blitScaled(self: *Canvas, src: []const Color, sw: u32, sh: u32, dx: i32, dy: i32, s: i32) void {
        var sy: u32 = 0;
        while (sy < sh) : (sy += 1) {
            var sx: u32 = 0;
            while (sx < sw) : (sx += 1) {
                self.fillRect(dx + @as(i32, @intCast(sx)) * s, dy + @as(i32, @intCast(sy)) * s, s, s, src[sy * sw + sx]);
            }
        }
    }

    /// Like blitScaled but skips fully-transparent source pixels (alpha 0),
    /// blending partially-transparent ones — for compositing cut-out figures.
    pub fn blitScaledAlpha(self: *Canvas, src: []const Color, sw: u32, sh: u32, dx: i32, dy: i32, s: i32) void {
        var sy: u32 = 0;
        while (sy < sh) : (sy += 1) {
            var sx: u32 = 0;
            while (sx < sw) : (sx += 1) {
                const p = src[sy * sw + sx];
                const a = p >> 24;
                if (a == 0) continue;
                const ox = dx + @as(i32, @intCast(sx)) * s;
                const oy = dy + @as(i32, @intCast(sy)) * s;
                if (a >= 255) {
                    self.fillRect(ox, oy, s, s, p);
                } else {
                    var yy: i32 = 0;
                    while (yy < s) : (yy += 1) {
                        var xx: i32 = 0;
                        while (xx < s) : (xx += 1) self.blendPixel(ox + xx, oy + yy, p, a);
                    }
                }
            }
        }
    }

    pub fn drawLine(self: *Canvas, x0i: i32, y0i: i32, x1: i32, y1: i32, color: Color) void {
        var x0 = x0i;
        var y0 = y0i;
        const dx: i32 = @intCast(@abs(x1 - x0));
        const dy: i32 = -@as(i32, @intCast(@abs(y1 - y0)));
        const sx: i32 = if (x0 < x1) 1 else -1;
        const sy: i32 = if (y0 < y1) 1 else -1;
        var err = dx + dy;
        while (true) {
            self.set(x0, y0, color);
            if (x0 == x1 and y0 == y1) break;
            const e2 = 2 * err;
            if (e2 >= dy) {
                err += dy;
                x0 += sx;
            }
            if (e2 <= dx) {
                err += dx;
                y0 += sy;
            }
        }
    }

    pub fn fillTriangle(self: *Canvas, ax: i32, ay: i32, bx: i32, by: i32, cx: i32, cy: i32, color: Color) void {
        const min_y = @min(ay, @min(by, cy));
        const max_y = @max(ay, @max(by, cy));
        var y: i32 = min_y;
        while (y <= max_y) : (y += 1) {
            var lo: i32 = std.math.maxInt(i32);
            var hi: i32 = std.math.minInt(i32);
            edgeX(ax, ay, bx, by, y, &lo, &hi);
            edgeX(bx, by, cx, cy, y, &lo, &hi);
            edgeX(cx, cy, ax, ay, y, &lo, &hi);
            if (lo <= hi) {
                var x = lo;
                while (x <= hi) : (x += 1) self.set(x, y, color);
            }
        }
    }

    fn edgeX(x0: i32, y0: i32, x1: i32, y1: i32, y: i32, lo: *i32, hi: *i32) void {
        if (y0 == y1) return;
        if ((y < @min(y0, y1)) or (y > @max(y0, y1))) return;
        const x = x0 + @divTrunc((x1 - x0) * (y - y0), (y1 - y0));
        lo.* = @min(lo.*, x);
        hi.* = @max(hi.*, x);
    }

    // --- Antialiased text -------------------------------------------------

    pub fn textWidth(text: []const u8) i32 {
        var w: i32 = 0;
        for (text) |c| {
            if (c < font.first or c >= font.first + font.count) continue;
            w += font.glyphs[c - font.first].advance;
        }
        return w;
    }

    /// Draw a single line of proportional, antialiased text. `y` is the line
    /// top; glyphs are placed on the baseline. Returns the advanced pen X.
    pub fn drawText(self: *Canvas, text: []const u8, x: i32, y: i32, color: Color) i32 {
        var pen = x;
        for (text) |c| {
            if (c < font.first or c >= font.first + font.count) continue;
            const g = font.glyphs[c - font.first];
            var row: u32 = 0;
            while (row < g.h) : (row += 1) {
                var col: u32 = 0;
                while (col < g.w) : (col += 1) {
                    const a = font.coverage[g.off + row * g.w + col];
                    self.blendPixel(pen + g.xoff + @as(i32, @intCast(col)), y + g.yoff + @as(i32, @intCast(row)), color, a);
                }
            }
            pen += g.advance;
        }
        return pen;
    }

    pub fn wrappedHeight(text: []const u8, max_w: i32) i32 {
        var lines: i32 = 1;
        var line_w: i32 = 0;
        const space = font.glyphs[' ' - font.first].advance;
        var it = std.mem.tokenizeScalar(u8, text, ' ');
        while (it.next()) |word| {
            const ww = textWidth(word);
            if (line_w > 0 and line_w + space + ww > max_w) {
                lines += 1;
                line_w = 0;
            }
            if (line_w > 0) line_w += space;
            line_w += ww;
        }
        return lines * font.line_height;
    }

    pub fn drawTextWrapped(self: *Canvas, text: []const u8, x: i32, y: i32, max_w: i32, color: Color) i32 {
        const space = font.glyphs[' ' - font.first].advance;
        var cy = y;
        var pen = x;
        var line_w: i32 = 0;
        var it = std.mem.tokenizeScalar(u8, text, ' ');
        while (it.next()) |word| {
            const ww = textWidth(word);
            if (line_w > 0 and line_w + space + ww > max_w) {
                cy += font.line_height;
                pen = x;
                line_w = 0;
            }
            if (line_w > 0) {
                pen += space;
                line_w += space;
            }
            _ = self.drawText(word, pen, cy, color);
            pen += ww;
            line_w += ww;
        }
        return cy + font.line_height;
    }

    // --- Smooth speech balloon (SDF, antialiased) -------------------------

    fn sdRoundRect(px: f32, py: f32, cx: f32, cy: f32, hx: f32, hy: f32, r: f32) f32 {
        const qx = @abs(px - cx) - (hx - r);
        const qy = @abs(py - cy) - (hy - r);
        const ax = @max(qx, 0.0);
        const ay = @max(qy, 0.0);
        return @min(@max(qx, qy), 0.0) + @sqrt(ax * ax + ay * ay) - r;
    }

    fn covFromDist(d: f32) u32 {
        // coverage ~ how far inside the boundary, antialiased over 1px
        const c = std.math.clamp(0.5 - d, 0.0, 1.0);
        return @intFromFloat(c * 255.0);
    }

    /// A white speech balloon with a smooth black outline and a tail toward
    /// (tx,ty). Rounded corners; antialiased body.
    pub fn speechBalloon(self: *Canvas, x: i32, y: i32, w: i32, h: i32, tx: i32, ty: i32) void {
        const fx: f32 = @floatFromInt(x);
        const fy: f32 = @floatFromInt(y);
        const fw: f32 = @floatFromInt(w);
        const fh: f32 = @floatFromInt(h);
        const cx = fx + fw / 2.0;
        const cy = fy + fh / 2.0;
        const hx = fw / 2.0;
        const hy = fh / 2.0;
        const radius: f32 = @min(22.0, @min(hx, hy) - 1.0);
        const stroke: f32 = 2.0;

        // Body: AA white fill, then AA black stroke straddling the boundary.
        var py: i32 = y - 2;
        while (py < y + h + 2) : (py += 1) {
            var px: i32 = x - 2;
            while (px < x + w + 2) : (px += 1) {
                const d = sdRoundRect(@floatFromInt(px), @floatFromInt(py), cx, cy, hx, hy, radius);
                const fillc = covFromDist(d); // inside -> 255
                if (fillc > 0) self.blendPixel(px, py, white, fillc);
                // stroke band centred on the boundary
                const sd = stroke - @abs(d);
                if (sd > -0.5) {
                    const sc: u32 = @intFromFloat(std.math.clamp(sd + 0.5, 0.0, 1.0) * 255.0);
                    self.blendPixel(px, py, black, sc);
                }
            }
        }

        // Tail: a filled white triangle that punches through the bottom edge,
        // with two black sides. Drawn after the body so the mouth stays open.
        const base = std.math.clamp(tx, x + @divTrunc(w, 4), x + @divTrunc(3 * w, 4));
        const bl = base - 11;
        const br = base + 11;
        const top = y + h - 3; // inside the body, below its lower stroke
        self.fillTriangle(bl, top, br, top, tx, ty, white);
        // re-open the mouth: cover the body's bottom stroke between bl..br
        self.fillRect(bl, y + h - 3, br - bl, 3, white);
        // black tail sides (2px)
        self.drawLine(bl, top, tx, ty, black);
        self.drawLine(bl + 1, top, tx, ty, black);
        self.drawLine(br, top, tx, ty, black);
        self.drawLine(br - 1, top, tx, ty, black);
    }
};

// --- Tests ----------------------------------------------------------------

test "blendPixel mixes toward the source colour" {
    const gpa = std.testing.allocator;
    var c = try Canvas.init(gpa, 2, 2);
    defer c.deinit(gpa);
    c.clear(black);
    c.blendPixel(0, 0, white, 128); // ~50% white over black
    const p = c.px[0] & 0xff;
    try std.testing.expect(p > 100 and p < 160);
}

test "drawText advances and lights pixels for visible glyphs" {
    const gpa = std.testing.allocator;
    var c = try Canvas.init(gpa, 120, 40);
    defer c.deinit(gpa);
    c.clear(black);
    const end = c.drawText("Ag", 4, 4, white);
    try std.testing.expect(end > 4); // pen advanced
    var lit: usize = 0;
    for (c.px) |p| {
        if ((p & 0xff) > 40) lit += 1;
    }
    try std.testing.expect(lit > 0);
}

test "wrappedHeight grows with narrower width" {
    const wide = Canvas.wrappedHeight("one two three four five six", 1000);
    const narrow = Canvas.wrappedHeight("one two three four five six", 40);
    try std.testing.expect(narrow > wide);
}
