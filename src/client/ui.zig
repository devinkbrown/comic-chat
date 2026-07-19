//! Reusable visual primitives for the Comic Chat desktop shell.
//!
//! The historical layout is deliberately retained by `geometry.zig`; this
//! module owns the modern skin so menus, dialogs, buffer states, and status
//! feedback share the same spacing, contrast, and focus language.

const std = @import("std");
const canvas_mod = @import("../render/canvas.zig");
const geometry = @import("geometry.zig");

const Canvas = canvas_mod.Canvas;
const Rect = geometry.Rect;

pub const Theme = struct {
    pub const ink: u32 = 0xff1f2933;
    pub const secondary: u32 = 0xff58636f;
    pub const chrome: u32 = 0xfff7f9fc;
    pub const layer: u32 = 0xffffffff;
    pub const subtle: u32 = 0xffe8edf3;
    pub const divider: u32 = 0xffcbd5e1;
    pub const accent: u32 = 0xff0f6cbd;
    pub const accent_soft: u32 = 0xffdbeafe;
    pub const focus: u32 = 0xff0b4f85;
    pub const success: u32 = 0xff107c10;
    pub const warning: u32 = 0xffca5010;
};

pub const ButtonKind = enum { primary, secondary, quiet };

/// Source-shaped dialog geometry, centralized so draw, accessibility, and
/// pointer handling cannot silently drift apart as dialogs grow.
pub const DialogLayout = struct {
    rect: Rect,
    body_y: i32,
    row_h: i32,
    field_count: usize,
    primary: Rect,
    cancel: Rect,

    pub fn init(canvas_width: u32, canvas_height: u32, source_width: u16, source_height: u16, field_count: usize, primary_width: i32) DialogLayout {
        const canvas_w: i32 = @intCast(canvas_width);
        const canvas_h: i32 = @intCast(canvas_height);
        const desired_w = @divTrunc(@as(i32, source_width) * 3, 2);
        const desired_h = @divTrunc(@as(i32, source_height) * 3, 2);
        const rect = Rect{
            .x = @divTrunc(canvas_w - @min(@max(300, desired_w), @max(240, canvas_w - 32)), 2),
            .y = @divTrunc(canvas_h - @min(@max(170, desired_h), @max(140, canvas_h - 32)), 2),
            .w = @min(@max(300, desired_w), @max(240, canvas_w - 32)),
            .h = @min(@max(170, desired_h), @max(140, canvas_h - 32)),
        };
        const body_y = rect.y + 80;
        const available_h = @max(43, rect.bottom() - 48 - body_y);
        const row_h = @min(54, @max(43, @divTrunc(available_h, @max(1, @as(i32, @intCast(field_count))))));
        return .{
            .rect = rect,
            .body_y = body_y,
            .row_h = row_h,
            .field_count = field_count,
            .primary = .{ .x = rect.right() - 96 - primary_width, .y = rect.bottom() - 36, .w = primary_width, .h = 28 },
            .cancel = .{ .x = rect.right() - 84, .y = rect.bottom() - 36, .w = 76, .h = 28 },
        };
    }

    pub fn fieldLabelY(self: DialogLayout, index: usize) i32 {
        return self.body_y + @as(i32, @intCast(index)) * self.row_h;
    }

    pub fn fieldRect(self: DialogLayout, index: usize) Rect {
        return .{ .x = self.rect.x + 20, .y = self.fieldLabelY(index) + 17, .w = self.rect.w - 40, .h = 24 };
    }

    pub fn fieldIndexAt(self: DialogLayout, y: i32) ?usize {
        if (y < self.body_y or y >= self.rect.bottom() - 43) return null;
        const raw = @divTrunc(y - self.body_y, self.row_h);
        if (raw < 0 or raw >= self.field_count) return null;
        return @intCast(raw);
    }
};

pub fn contains(rect: Rect, x: i32, y: i32) bool {
    return x >= rect.x and y >= rect.y and x < rect.right() and y < rect.bottom();
}

pub fn drawOutline(c: *Canvas, x: i32, y: i32, w: i32, h: i32, color: u32) void {
    if (w <= 0 or h <= 0) return;
    c.drawLine(x, y, x + w - 1, y, color);
    c.drawLine(x, y + h - 1, x + w - 1, y + h - 1, color);
    c.drawLine(x, y, x, y + h - 1, color);
    c.drawLine(x + w - 1, y, x + w - 1, y + h - 1, color);
}

pub fn drawButton(c: *Canvas, x: i32, y: i32, width: i32, label: []const u8, kind: ButtonKind, hovered: bool) void {
    const fill = switch (kind) {
        .primary => if (hovered) Theme.focus else Theme.accent,
        .secondary => if (hovered) Theme.layer else Theme.chrome,
        .quiet => if (hovered) Theme.subtle else Theme.chrome,
    };
    const border = switch (kind) {
        .primary => fill,
        .secondary, .quiet => if (hovered) Theme.focus else Theme.divider,
    };
    c.fillRect(x, y, width, 28, fill);
    drawOutline(c, x, y, width, 28, border);
    const text_w = Canvas.textWidth(label);
    const text_color = if (kind == .primary) Theme.layer else Theme.ink;
    _ = c.drawText(label, x + @divTrunc(width - text_w, 2), y + 3, text_color);
}

pub fn drawModalBackdrop(c: *Canvas) void {
    var y: i32 = 0;
    while (y < @as(i32, @intCast(c.height))) : (y += 1) {
        var x: i32 = 0;
        while (x < @as(i32, @intCast(c.width))) : (x += 1) c.blendPixel(x, y, 0xff000000, 0x66);
    }
}

pub fn drawDialogSurface(c: *Canvas, rect: Rect, title: []const u8, subtitle: []const u8) void {
    c.fillRect(rect.x + 3, rect.y + 4, rect.w, rect.h, 0xff8793a1);
    c.fillRect(rect.x, rect.y, rect.w, rect.h, Theme.layer);
    drawOutline(c, rect.x, rect.y, rect.w, rect.h, Theme.focus);
    c.fillRect(rect.x + 1, rect.y + 1, rect.w - 2, 38, Theme.accent);
    _ = c.drawText(title, rect.x + 12, rect.y + 6, Theme.layer);
    _ = c.drawText(subtitle, rect.x + 20, rect.y + 52, Theme.secondary);
}

pub fn drawField(c: *Canvas, x: i32, y: i32, width: i32, active: bool) void {
    c.fillRect(x, y, width, 24, Theme.layer);
    drawOutline(c, x, y, width, 24, if (active) Theme.accent else Theme.divider);
    if (active) c.fillRect(x + 1, y + 1, 2, 22, Theme.accent);
}

/// A compact command tile for icon-only tools.  The top ink mark is the
/// Comic Chat signature: selected modes read at a glance without a bulky
/// native-toolbar bevel.
pub fn drawCommandTile(c: *Canvas, x: i32, y: i32, selected: bool, hovered: bool) u32 {
    if (selected) {
        c.fillRect(x, y, 24, 24, Theme.accent_soft);
        c.fillRect(x, y, 24, 3, Theme.accent);
        return Theme.accent;
    }
    if (hovered) {
        c.fillRect(x, y, 24, 24, Theme.layer);
        drawOutline(c, x, y, 24, 24, Theme.divider);
        return Theme.focus;
    }
    return Theme.ink;
}

pub fn drawMenuItem(c: *Canvas, x: i32, y: i32, width: i32, label: []const u8, selected: bool) void {
    if (selected) {
        c.fillRect(x, y, width, 23, Theme.accent_soft);
        c.fillRect(x, y, 3, 23, Theme.accent);
    }
    _ = c.drawText(label, x + 8, y + 3, Theme.ink);
}

pub fn drawTab(c: *Canvas, x: i32, y: i32, width: i32, height: i32, selected: bool) void {
    c.fillRect(x, y, width, height, if (selected) Theme.layer else Theme.subtle);
    if (selected) c.fillRect(x, y, width, 3, Theme.accent);
    if (selected) drawOutline(c, x, y, width, height, Theme.divider);
}

pub fn drawActionTile(c: *Canvas, x: i32, y: i32, width: i32, height: i32, selected: bool) u32 {
    c.fillRect(x, y, width, height, if (selected) Theme.accent_soft else Theme.chrome);
    c.fillRect(x, y, 1, height, Theme.divider);
    if (selected) c.fillRect(x + 1, y + height - 3, @max(1, width - 1), 3, Theme.accent);
    return if (selected) Theme.accent else Theme.ink;
}

pub fn drawStatusBar(c: *Canvas, x: i32, y: i32, width: i32, height: i32, status: []const u8, member_count: usize) void {
    c.fillRect(x, y, width, height, Theme.chrome);
    c.fillRect(x, y, width, 1, Theme.divider);
    const is_connected = std.mem.indexOf(u8, status, "connected") != null and std.mem.indexOf(u8, status, "reconnecting") == null;
    const status_color = if (is_connected) Theme.success else Theme.warning;
    c.fillRect(x + 9, y + 8, 6, 6, status_color);
    var buf: [32]u8 = undefined;
    const members = std.fmt.bufPrint(&buf, "{d} members", .{member_count}) catch "members";
    const badge_w = Canvas.textWidth(members) + 16;
    const badge_x = x + @max(108, width - badge_w - 8);
    c.fillRect(badge_x, y + 3, badge_w, @max(1, height - 6), Theme.subtle);
    drawEllipsized(c, status, x + 22, y + 2, badge_x - x - 30, Theme.secondary);
    _ = c.drawText(members, badge_x + 8, y + 2, Theme.secondary);
}

pub fn drawEmptyState(c: *Canvas, x: i32, y: i32, width: i32, height: i32, detail: []const u8) void {
    c.fillRect(x, y, width, height, Theme.layer);
    const card_w = @min(360, @max(220, width - 48));
    const card_h = 132;
    const card_x = x + @divTrunc(width - card_w, 2);
    const card_y = y + @divTrunc(height - card_h, 2);
    c.fillRect(card_x + 4, card_y + 5, card_w, card_h, 0xffd5dce5);
    c.fillRect(card_x, card_y, card_w, card_h, Theme.chrome);
    drawOutline(c, card_x, card_y, card_w, card_h, Theme.divider);
    c.fillRect(card_x, card_y, 4, card_h, Theme.accent);
    c.fillRect(card_x + 20, card_y + 18, 16, 12, Theme.accent_soft);
    drawOutline(c, card_x + 20, card_y + 18, 16, 12, Theme.accent);
    c.drawLine(card_x + 26, card_y + 29, card_x + 24, card_y + 34, Theme.accent);
    _ = c.drawText("READY TO TALK", card_x + 48, card_y + 14, Theme.accent);
    _ = c.drawText("Your conversation starts here", card_x + 20, card_y + 43, Theme.ink);
    drawEllipsized(c, detail, card_x + 20, card_y + 66, card_w - 40, Theme.secondary);
    c.fillRect(card_x + 20, card_y + 96, card_w - 40, 22, Theme.accent_soft);
    c.fillRect(card_x + 20, card_y + 96, 3, 22, Theme.accent);
    _ = c.drawText("Type a message below to begin", card_x + 32, card_y + 99, Theme.focus);
}

fn drawEllipsized(c: *Canvas, text: []const u8, x: i32, y: i32, max_width: i32, color: u32) void {
    if (max_width <= 0) return;
    if (Canvas.textWidth(text) <= max_width) {
        _ = c.drawText(text, x, y, color);
        return;
    }
    const dots = "...";
    const dots_width = Canvas.textWidth(dots);
    var end = text.len;
    while (end > 0 and Canvas.textWidth(text[0..end]) + dots_width > max_width) end -= 1;
    _ = c.drawText(text[0..end], x, y, color);
    _ = c.drawText(dots, x + Canvas.textWidth(text[0..end]), y, color);
}

test "primary buttons and focused fields use the shared accent" {
    const testing = std.testing;
    var canvas = try Canvas.init(testing.allocator, 160, 64);
    defer canvas.deinit(testing.allocator);
    drawButton(&canvas, 4, 4, 90, "Save", .primary, false);
    drawField(&canvas, 4, 38, 120, true);
    try testing.expectEqual(Theme.accent, canvas.px[4 + 4 * 160]);
    try testing.expectEqual(Theme.accent, canvas.px[5 + 40 * 160]);
}

test "dialog layout keeps fields and actions inside the modal" {
    const layout = DialogLayout.init(640, 430, 252, 226, 3, 108);
    try std.testing.expect(layout.rect.w >= 300);
    try std.testing.expect(contains(layout.primary, layout.primary.x + 1, layout.primary.y + 1));
    try std.testing.expect(contains(layout.cancel, layout.cancel.x + 1, layout.cancel.y + 1));
    const last_field = layout.fieldRect(2);
    try std.testing.expect(last_field.y > layout.fieldRect(0).y);
    try std.testing.expect(last_field.y + last_field.h < layout.primary.y);
}
