//! Multi-panel comic strip renderer.
//!
//! Each transcript line becomes one panel: a decoded Comic Chat background, the
//! speaker's assembled figure, and a speech balloon whose tail points at the
//! figure's head. Panels are arranged in a 3-column strip with a 2px black
//! border/gutter.

const std = @import("std");
const bgb = @import("../assets/bgb.zig");
const figure = @import("figure.zig");
const Canvas = @import("../render/canvas.zig").Canvas;
const black = @import("../render/canvas.zig").black;
const white = @import("../render/canvas.zig").white;

pub const Image = bgb.Image;

pub const Line = struct {
    speaker: []const u8,
    text: []const u8,
};

pub const Error = error{
    PoseNotFound,
    UnknownAvatar,
} || bgb.Error || std.mem.Allocator.Error;

const panel_w: u32 = 315;
const panel_h: u32 = 315;
const gutter: u32 = 2;
const columns: u32 = 3;

pub fn render(gpa: std.mem.Allocator, lines: []const Line) Error!Image {
    const rows: u32 = if (lines.len == 0) 0 else @intCast((lines.len + columns - 1) / columns);
    const out_w = columns * panel_w + (columns + 1) * gutter;
    const out_h = @max(@as(u32, 1), rows) * panel_h + (@max(@as(u32, 1), rows) + 1) * gutter;

    var out = try Canvas.init(gpa, out_w, out_h);
    defer out.deinit(gpa);
    out.clear(black);

    for (lines, 0..) |line, i| {
        var panel = try renderPanel(gpa, line, i);
        defer panel.deinit(gpa);

        const col: u32 = @intCast(i % columns);
        const row: u32 = @intCast(i / columns);
        const dx: i32 = @intCast(gutter + col * (panel_w + gutter));
        const dy: i32 = @intCast(gutter + row * (panel_h + gutter));
        out.blit(panel.pixels, panel.width, panel.height, dx, dy);
    }

    const pixels = try gpa.dupe(u32, out.px);
    return .{ .width = out.width, .height = out.height, .pixels = pixels };
}

fn renderPanel(gpa: std.mem.Allocator, line: Line, index: usize) Error!Image {
    var bg = try bgb.decodeBackground(gpa, bgByIndex(index));
    defer bg.deinit(gpa);

    var c = try Canvas.init(gpa, panel_w, panel_h);
    defer c.deinit(gpa);
    c.blit(bg.pixels, bg.width, bg.height, 0, 0);

    const pad: i32 = 14;
    const bx: i32 = 14;
    const by: i32 = 12;
    const bw: i32 = @as(i32, @intCast(panel_w)) - 2 * bx;
    const text_w = bw - 2 * pad;
    const bh = @min(Canvas.wrappedHeight(line.text, text_w) + 2 * pad, 122);

    const avb = avatarByName(line.speaker) orelse return error.UnknownAvatar;
    var fig = try figure.assemble(gpa, avb, 0, 0);
    defer fig.deinit(gpa);

    const max_fig_h: i32 = @as(i32, @intCast(panel_h)) - (by + bh) - 18;
    const max_fig_w: i32 = @as(i32, @intCast(panel_w)) - 42;
    const fit = fitSize(fig.width, fig.height, max_fig_w, max_fig_h);
    const fx = @divTrunc(@as(i32, @intCast(panel_w)) - fit.w, 2);
    const fy = @as(i32, @intCast(panel_h)) - fit.h - 6;
    blitFitAlpha(&c, fig.pixels, fig.width, fig.height, fx, fy, fit.w, fit.h);

    const head_x = fx + @divTrunc(fit.w, 2);
    const head_y = fy + @max(8, @divTrunc(fit.h, 7));
    c.speechBalloon(bx, by, bw, bh, head_x, head_y);
    _ = c.drawTextWrapped(line.text, bx + pad, by + pad, text_w, black);

    const name_w = Canvas.textWidth(line.speaker) + 12;
    const tag_x = @max(4, fx + @divTrunc(fit.w - name_w, 2));
    const tag_y = @min(@as(i32, @intCast(panel_h)) - 25, fy + fit.h - 22);
    c.fillRect(tag_x, tag_y, name_w, 20, white);
    _ = c.drawText(line.speaker, tag_x + 6, tag_y + 1, black);

    const pixels = try gpa.dupe(u32, c.px);
    return .{ .width = c.width, .height = c.height, .pixels = pixels };
}

const Size = struct { w: i32, h: i32 };

fn fitSize(sw: u32, sh: u32, max_w: i32, max_h: i32) Size {
    var h: i32 = @max(1, max_h);
    var w: i32 = @divTrunc(@as(i32, @intCast(sw)) * h, @as(i32, @intCast(sh)));
    if (w > max_w) {
        w = @max(1, max_w);
        h = @divTrunc(@as(i32, @intCast(sh)) * w, @as(i32, @intCast(sw)));
    }
    return .{ .w = @max(1, w), .h = @max(1, h) };
}

fn blitFitAlpha(c: *Canvas, src: []const u32, sw: u32, sh: u32, dx: i32, dy: i32, dw: i32, dh: i32) void {
    var y: i32 = 0;
    while (y < dh) : (y += 1) {
        const sy: u32 = @intCast(@divTrunc(@as(i64, y) * sh, @as(i64, dh)));
        var x: i32 = 0;
        while (x < dw) : (x += 1) {
            const sx: u32 = @intCast(@divTrunc(@as(i64, x) * sw, @as(i64, dw)));
            const p = src[sy * sw + sx];
            const a = p >> 24;
            if (a == 0) continue;
            if (a >= 255) {
                c.set(dx + x, dy + y, p);
            } else {
                c.blendPixel(dx + x, dy + y, p, a);
            }
        }
    }
}

fn bgByIndex(index: usize) []const u8 {
    return switch (index % 5) {
        0 => @embedFile("../assets/testdata/field.bgb"),
        1 => @embedFile("../assets/testdata/volcano.bgb"),
        2 => @embedFile("../assets/testdata/den.bgb"),
        3 => @embedFile("../assets/testdata/room.bgb"),
        else => @embedFile("../assets/testdata/pastoral.bgb"),
    };
}

fn avatarByName(name: []const u8) ?[]const u8 {
    const eql = std.ascii.eqlIgnoreCase;
    if (eql(name, "anna")) return @embedFile("../assets/testdata/anna.avb");
    if (eql(name, "armando")) return @embedFile("../assets/testdata/armando.avb");
    if (eql(name, "bolo")) return @embedFile("../assets/testdata/bolo.avb");
    if (eql(name, "cro")) return @embedFile("../assets/testdata/cro.avb");
    if (eql(name, "dan")) return @embedFile("../assets/testdata/dan.avb");
    if (eql(name, "denise")) return @embedFile("../assets/testdata/denise.avb");
    if (eql(name, "hugh")) return @embedFile("../assets/testdata/hugh.avb");
    if (eql(name, "jordan")) return @embedFile("../assets/testdata/jordan.avb");
    if (eql(name, "kevin")) return @embedFile("../assets/testdata/kevin.avb");
    if (eql(name, "kwensa")) return @embedFile("../assets/testdata/kwensa.avb");
    if (eql(name, "lance")) return @embedFile("../assets/testdata/lance.avb");
    if (eql(name, "lynnea")) return @embedFile("../assets/testdata/lynnea.avb");
    if (eql(name, "margaret")) return @embedFile("../assets/testdata/margaret.avb");
    if (eql(name, "maynard")) return @embedFile("../assets/testdata/maynard.avb");
    if (eql(name, "mike")) return @embedFile("../assets/testdata/mike.avb");
    if (eql(name, "rebecca")) return @embedFile("../assets/testdata/rebecca.avb");
    if (eql(name, "sage")) return @embedFile("../assets/testdata/sage.avb");
    if (eql(name, "scotty")) return @embedFile("../assets/testdata/scotty.avb");
    if (eql(name, "susan")) return @embedFile("../assets/testdata/susan.avb");
    if (eql(name, "tiki")) return @embedFile("../assets/testdata/tiki.avb");
    if (eql(name, "tongtyed")) return @embedFile("../assets/testdata/tongtyed.avb");
    if (eql(name, "xeno")) return @embedFile("../assets/testdata/xeno.avb");
    return null;
}

test "render lays out panels in a three-column strip" {
    const gpa = std.testing.allocator;
    const lines = [_]Line{
        .{ .speaker = "anna", .text = "The field looks different in panel one." },
        .{ .speaker = "kevin", .text = "Panel two should have its own figure and balloon." },
        .{ .speaker = "sage", .text = "Panel three completes the first row." },
        .{ .speaker = "mike", .text = "The fourth line starts a new row." },
    };

    var img = try render(gpa, &lines);
    defer img.deinit(gpa);

    try std.testing.expectEqual(@as(u32, columns * panel_w + (columns + 1) * gutter), img.width);
    try std.testing.expectEqual(@as(u32, 2 * panel_h + 3 * gutter), img.height);
    try std.testing.expectEqual(black, img.pixels[0]);
    try std.testing.expect(img.pixels[@as(usize, gutter) * img.width + gutter] != black);
}

test "render rejects unknown speakers" {
    const gpa = std.testing.allocator;
    const lines = [_]Line{.{ .speaker = "not-an-avatar", .text = "No avatar." }};
    try std.testing.expectError(error.UnknownAvatar, render(gpa, &lines));
}
