//! Platform-independent interactive client view.
//!
//! This renders the complete application chrome into a software framebuffer;
//! X11, Wayland, and Win32 backends present the pixels and translate input
//! events. Keeping UI rendering here prevents each platform
//! backend from becoming a separate Comic Chat implementation.

const std = @import("std");
const session = @import("../comic/session.zig");
const strip = @import("../comic/strip.zig");
const canvas_mod = @import("../render/canvas.zig");

const Canvas = canvas_mod.Canvas;
const black = canvas_mod.black;
const white = canvas_mod.white;

pub const min_width: u32 = 320;
pub const min_height: u32 = 240;
pub const header_height: i32 = 38;
pub const input_height: i32 = 42;

pub const View = struct {
    gpa: std.mem.Allocator,
    canvas: Canvas,

    pub fn init(gpa: std.mem.Allocator, initial_width: u32, initial_height: u32) !View {
        return .{
            .gpa = gpa,
            .canvas = try Canvas.init(gpa, @max(initial_width, min_width), @max(initial_height, min_height)),
        };
    }

    pub fn deinit(self: *View) void {
        self.canvas.deinit(self.gpa);
    }

    pub fn resize(self: *View, new_width: u32, new_height: u32) !void {
        const w = @max(new_width, min_width);
        const h = @max(new_height, min_height);
        if (w == self.canvas.width and h == self.canvas.height) return;
        const replacement = try Canvas.init(self.gpa, w, h);
        self.canvas.deinit(self.gpa);
        self.canvas = replacement;
    }

    pub fn pixels(self: *const View) []const u32 {
        return self.canvas.px;
    }

    pub fn width(self: *const View) u32 {
        return self.canvas.width;
    }

    pub fn height(self: *const View) u32 {
        return self.canvas.height;
    }

    /// Render the title/status, newest conversation panels, and editable input.
    pub fn render(
        self: *View,
        title: []const u8,
        status: []const u8,
        transcript: *const session.Transcript,
        input: []const u8,
        cursor: usize,
    ) !void {
        const c = &self.canvas;
        c.clear(0xffd7d4cb);
        drawChrome(c, title, status, input, cursor);

        if (transcript.lines.items.len == 0) {
            const hint = "No messages yet - type below and press Enter";
            const x = @max(12, @divTrunc(@as(i32, @intCast(c.width)) - Canvas.textWidth(hint), 2));
            _ = c.drawText(hint, x, header_height + 28, 0xff4c4a45);
            return;
        }

        // The strip renderer is intentionally independent of viewport size.
        // Select only as many newest lines as are useful on screen, then fit
        // the resulting comic page into the available application surface.
        const max_visible: usize = 9;
        const all = transcript.lines.items;
        const visible = all[if (all.len > max_visible) all.len - max_visible else 0..];
        const lines = try self.gpa.alloc(strip.Line, visible.len);
        defer self.gpa.free(lines);
        const target_views = try self.gpa.alloc([]strip.Participant, visible.len);
        var target_views_count: usize = 0;
        defer {
            for (target_views[0..target_views_count]) |targets| self.gpa.free(targets);
            self.gpa.free(target_views);
        }
        for (visible, 0..) |line, i| {
            const targets = try self.gpa.alloc(strip.Participant, line.talk_targets.len);
            target_views[i] = targets;
            target_views_count += 1;
            for (line.talk_targets, 0..) |target, target_index| targets[target_index] = .{
                .identity = target.nick,
                .display_name = target.nick,
                .avatar = target.avatar,
            };
            lines[i] = .{
                .identity = line.nick,
                .display_name = line.nick,
                .avatar = line.avatar,
                .text = line.text,
                .formatting = line.formatting,
                .pose_text = line.pose_text,
                .pose_state = line.pose_state,
                .talk_targets = targets,
                .modes = line.modes,
            };
        }

        const title_roster = try self.gpa.alloc(strip.TitleParticipant, transcript.roster.items.len);
        defer self.gpa.free(title_roster);
        for (transcript.roster.items, 0..) |member, index| title_roster[index] = .{
            .identity = member.nick,
            .display_name = member.nick,
            .avatar = member.avatar,
            .is_self = member.is_self,
            .sends = member.sends,
            .departed = member.departed,
        };

        var page = try strip.renderWithOptions(self.gpa, lines, .{ .title_roster = title_roster });
        defer page.deinit(self.gpa);

        const dst_x: i32 = 8;
        const dst_y: i32 = header_height + 8;
        const dst_w: i32 = @as(i32, @intCast(c.width)) - 16;
        const dst_h: i32 = @as(i32, @intCast(c.height)) - header_height - input_height - 16;
        blitFit(c, page.pixels, page.width, page.height, dst_x, dst_y, dst_w, dst_h);
    }
};

fn drawChrome(c: *Canvas, title: []const u8, status: []const u8, input: []const u8, cursor: usize) void {
    const w: i32 = @intCast(c.width);
    const h: i32 = @intCast(c.height);

    c.fillRect(0, 0, w, header_height, 0xff292823);
    _ = c.drawText(title, 12, 7, white);
    const status_w = Canvas.textWidth(status);
    _ = c.drawText(status, @max(12, w - status_w - 12), 7, 0xffb9d99f);

    const input_y = h - input_height;
    c.fillRect(0, input_y, w, input_height, 0xff292823);
    c.fillRect(8, input_y + 6, w - 16, input_height - 12, white);
    c.fillRect(8, input_y + 6, w - 16, 2, black);
    c.fillRect(8, input_y + input_height - 8, w - 16, 2, black);
    _ = c.drawText(">", 15, input_y + 9, black);

    const text_x: i32 = 34;
    _ = c.drawText(input, text_x, input_y + 9, black);
    const safe_cursor = @min(cursor, input.len);
    const caret_x = text_x + Canvas.textWidth(input[0..safe_cursor]);
    c.fillRect(caret_x, input_y + 9, 2, 23, 0xff315b92);
}

/// Aspect-fit nearest-neighbour blit. Downscaling as well as upscaling is
/// supported, unlike Canvas.blitScaled's integer-only enlargement.
fn blitFit(c: *Canvas, src: []const u32, sw: u32, sh: u32, x: i32, y: i32, max_w: i32, max_h: i32) void {
    if (sw == 0 or sh == 0 or max_w <= 0 or max_h <= 0) return;
    var dw = max_w;
    var dh: i32 = @intCast(@divTrunc(@as(i64, sh) * dw, sw));
    if (dh > max_h) {
        dh = max_h;
        dw = @intCast(@divTrunc(@as(i64, sw) * dh, sh));
    }
    dw = @max(dw, 1);
    dh = @max(dh, 1);
    const dx = x + @divTrunc(max_w - dw, 2);
    const dy = y + @divTrunc(max_h - dh, 2);

    var oy: i32 = 0;
    while (oy < dh) : (oy += 1) {
        const sy: u32 = @intCast(@divTrunc(@as(i64, oy) * sh, dh));
        var ox: i32 = 0;
        while (ox < dw) : (ox += 1) {
            const sx: u32 = @intCast(@divTrunc(@as(i64, ox) * sw, dw));
            c.set(dx + ox, dy + oy, src[@as(usize, sy) * sw + sx]);
        }
    }
}

test "view renders empty state, title, input, and caret" {
    const gpa = std.testing.allocator;
    var view = try View.init(gpa, 400, 300);
    defer view.deinit();
    var transcript = session.Transcript.init(gpa);
    defer transcript.deinit();

    try view.render("Comic Chat - #zig", "connected", &transcript, "hello", 3);
    try std.testing.expectEqual(@as(usize, 400 * 300), view.pixels().len);
    try std.testing.expect(view.pixels()[0] == 0xff292823);

    var non_background: usize = 0;
    for (view.pixels()) |p| if (p != 0xffd7d4cb) {
        non_background += 1;
    };
    try std.testing.expect(non_background > 1000);
}

test "view renders a live transcript and resizes" {
    const gpa = std.testing.allocator;
    var view = try View.init(gpa, 320, 240);
    defer view.deinit();
    var transcript = session.Transcript.init(gpa);
    defer transcript.deinit();
    try transcript.add("alice", "Hello from the channel");
    try transcript.add("bob", "The page should contain two panels");

    try view.render("Comic Chat", "joined", &transcript, "", 0);
    try view.resize(640, 480);
    try view.render("Comic Chat", "joined", &transcript, "reply", 5);
    try std.testing.expectEqual(@as(u32, 640), view.width());
    try std.testing.expectEqual(@as(u32, 480), view.height());
}
