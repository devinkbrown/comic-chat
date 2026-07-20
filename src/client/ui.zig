//! Reusable visual primitives for the Comic Chat desktop shell.
//!
//! `geometry.zig` preserves the established workspace proportions; this
//! module owns a fully modern presentation for menus, dialogs, buffer states,
//! and status feedback.

const std = @import("std");
const canvas_mod = @import("../render/canvas.zig");
const geometry = @import("geometry.zig");

const Canvas = canvas_mod.Canvas;
const Rect = geometry.Rect;

pub const Theme = struct {
    pub const ink: u32 = 0xff13243a;
    pub const secondary: u32 = 0xff52657a;
    pub const chrome: u32 = 0xfff4f7fb;
    pub const layer: u32 = 0xffffffff;
    pub const subtle: u32 = 0xffe4edf7;
    pub const divider: u32 = 0xffc7d4e2;
    pub const accent: u32 = 0xff0e6fcb;
    pub const accent_soft: u32 = 0xffd9edff;
    pub const focus: u32 = 0xff074d8f;
    pub const success: u32 = 0xff107c10;
    pub const warning: u32 = 0xffca5010;
    pub const comic_paper: u32 = 0xffe1edf8;
    pub const workspace: u32 = 0xffedf4fb;
    pub const navigation: u32 = 0xff102a43;
    pub const navigation_hover: u32 = 0xff1e4e79;
};

pub const ButtonKind = enum { primary, secondary, quiet };
pub const DialogButton = enum { primary, cancel };
pub const NoticeTone = enum { info, warning, failure, success };
pub const SurfaceKind = enum { canvas, panel, raised, accent };

pub const ControlState = struct {
    hovered: bool = false,
    selected: bool = false,
    focused: bool = false,
    pressed: bool = false,
    disabled: bool = false,
};

pub const ControlColors = struct { fill: u32, border: u32, content: u32 };

pub fn resolveControlColors(state: ControlState) ControlColors {
    if (state.disabled) return .{ .fill = Theme.chrome, .border = Theme.divider, .content = Theme.divider };
    if (state.pressed) return .{ .fill = Theme.accent, .border = Theme.focus, .content = Theme.layer };
    if (state.selected) return .{ .fill = Theme.accent_soft, .border = Theme.accent_soft, .content = Theme.accent };
    if (state.hovered) return .{ .fill = Theme.layer, .border = Theme.divider, .content = Theme.focus };
    return .{ .fill = Theme.chrome, .border = Theme.chrome, .content = Theme.ink };
}

/// Shared dialog geometry, centralized so draw, accessibility, and pointer
/// handling cannot silently drift apart as dialogs grow.
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

pub fn dialogButtonAt(layout: DialogLayout, x: i32, y: i32) ?DialogButton {
    if (contains(layout.primary, x, y)) return .primary;
    if (contains(layout.cancel, x, y)) return .cancel;
    return null;
}

pub fn drawOutline(c: *Canvas, x: i32, y: i32, w: i32, h: i32, color: u32) void {
    if (w <= 0 or h <= 0) return;
    c.drawLine(x, y, x + w - 1, y, color);
    c.drawLine(x, y + h - 1, x + w - 1, y + h - 1, color);
    c.drawLine(x, y, x, y + h - 1, color);
    c.drawLine(x + w - 1, y, x + w - 1, y + h - 1, color);
}

pub fn fillRoundedRect(c: *Canvas, x: i32, y: i32, w: i32, h: i32, radius: i32, color: u32) void {
    if (w <= 0 or h <= 0) return;
    const r = @min(@max(0, radius), @divTrunc(@min(w, h), 2));
    c.fillRect(x + r, y, @max(0, w - 2 * r), h, color);
    c.fillRect(x, y + r, w, @max(0, h - 2 * r), color);
    var oy: i32 = 0;
    while (oy < r) : (oy += 1) {
        var ox: i32 = 0;
        while (ox < r) : (ox += 1) {
            const dx = r - ox;
            const dy = r - oy;
            if (dx * dx + dy * dy <= r * r) {
                c.set(x + ox, y + oy, color);
                c.set(x + w - 1 - ox, y + oy, color);
                c.set(x + ox, y + h - 1 - oy, color);
                c.set(x + w - 1 - ox, y + h - 1 - oy, color);
            }
        }
    }
}

pub fn drawRoundedBorder(c: *Canvas, x: i32, y: i32, w: i32, h: i32, radius: i32, fill: u32, border: u32) void {
    fillRoundedRect(c, x, y, w, h, radius, border);
    fillRoundedRect(c, x + 1, y + 1, w - 2, h - 2, @max(0, radius - 1), fill);
}

pub fn drawSurface(c: *Canvas, rect: Rect, kind: SurfaceKind) void {
    const fill = switch (kind) {
        .canvas => Theme.workspace,
        .panel => Theme.layer,
        .raised => Theme.chrome,
        .accent => Theme.accent_soft,
    };
    if (kind == .raised) fillRoundedRect(c, rect.x + 3, rect.y + 4, rect.w, rect.h, 7, 0xffc3ceda);
    drawRoundedBorder(c, rect.x, rect.y, rect.w, rect.h, 7, fill, Theme.divider);
}

pub fn drawPill(c: *Canvas, rect: Rect, label: []const u8, active: bool) void {
    const fill = if (active) Theme.accent_soft else Theme.chrome;
    const color = if (active) Theme.accent else Theme.secondary;
    drawRoundedBorder(c, rect.x, rect.y, rect.w, rect.h, @divTrunc(rect.h, 2), fill, if (active) Theme.accent_soft else Theme.divider);
    drawEllipsized(c, label, rect.x + 8, rect.y + @max(1, @divTrunc(rect.h - 17, 2)), rect.w - 16, color);
}

pub fn drawTooltip(c: *Canvas, rect: Rect, label: []const u8) void {
    fillRoundedRect(c, rect.x + 3, rect.y + 4, rect.w, rect.h, 5, 0xffaeb9c5);
    drawRoundedBorder(c, rect.x, rect.y, rect.w, rect.h, 5, Theme.navigation, Theme.navigation_hover);
    drawEllipsized(c, label, rect.x + 9, rect.y + 5, rect.w - 18, Theme.layer);
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
    drawRoundedBorder(c, x, y, width, 28, 5, fill, border);
    const text_w = Canvas.uiTextWidth(label);
    const text_color = if (kind == .primary) Theme.layer else Theme.ink;
    _ = c.drawUiText(label, x + @divTrunc(width - text_w, 2), y + 3, text_color);
}

pub fn drawModalBackdrop(c: *Canvas) void {
    var y: i32 = 0;
    while (y < @as(i32, @intCast(c.height))) : (y += 1) {
        var x: i32 = 0;
        while (x < @as(i32, @intCast(c.width))) : (x += 1) c.blendPixel(x, y, 0xff000000, 0x66);
    }
}

pub fn drawDialogSurface(c: *Canvas, rect: Rect, title: []const u8, subtitle: []const u8) void {
    fillRoundedRect(c, rect.x + 5, rect.y + 7, rect.w, rect.h, 8, 0xffaeb9c5);
    drawRoundedBorder(c, rect.x, rect.y, rect.w, rect.h, 8, Theme.layer, Theme.divider);
    fillRoundedRect(c, rect.x + 1, rect.y + 1, rect.w - 2, 44, 7, Theme.accent);
    c.fillRect(rect.x + 1, rect.y + 30, rect.w - 2, 15, Theme.accent);
    _ = c.drawUiText(title, rect.x + 12, rect.y + 6, Theme.layer);
    _ = c.drawUiText(subtitle, rect.x + 20, rect.y + 52, Theme.secondary);
}

pub fn drawNotice(c: *Canvas, x: i32, y: i32, width: i32, label: []const u8, tone: NoticeTone) void {
    const colors = switch (tone) {
        .info => .{ Theme.accent_soft, Theme.accent },
        .warning => .{ 0xfffff4ce, Theme.warning },
        .failure => .{ 0xffffe5e5, 0xffc42b1c },
        .success => .{ 0xffdff6dd, Theme.success },
    };
    c.fillRect(x, y, width, 20, colors[0]);
    c.fillRect(x, y, 3, 20, colors[1]);
    drawEllipsized(c, label, x + 10, y + 2, width - 16, colors[1]);
}

pub fn drawField(c: *Canvas, x: i32, y: i32, width: i32, active: bool) void {
    drawRoundedBorder(c, x, y, width, 24, 4, Theme.layer, if (active) Theme.accent else Theme.divider);
    if (active) c.fillRect(x + 1, y + 1, 2, 22, Theme.accent);
}

/// A compact command tile for icon-only tools.  The top ink mark is the
/// Comic Chat signature: selected modes read at a glance without a bulky
/// native-toolbar bevel.
pub fn drawCommandTile(c: *Canvas, x: i32, y: i32, selected: bool, hovered: bool) u32 {
    const state: ControlState = .{ .selected = selected, .hovered = hovered };
    const colors = resolveControlColors(state);
    if (selected) {
        fillRoundedRect(c, x, y, 32, 32, 5, colors.fill);
        c.fillRect(x, y + 29, 32, 3, Theme.accent);
        return colors.content;
    }
    if (hovered) {
        drawRoundedBorder(c, x, y, 32, 32, 5, colors.fill, colors.border);
        return colors.content;
    }
    fillRoundedRect(c, x, y, 32, 32, 5, colors.fill);
    return colors.content;
}

pub fn drawMenuItem(c: *Canvas, x: i32, y: i32, width: i32, label: []const u8, selected: bool) void {
    if (selected) {
        fillRoundedRect(c, x, y, width, 25, 4, Theme.accent_soft);
        c.fillRect(x, y + 4, 3, 17, Theme.accent);
    }
    _ = c.drawUiText(label, x + 10, y + 4, Theme.ink);
}

pub fn drawMenuLabel(c: *Canvas, x: i32, y: i32, width: i32, label: []const u8, selected: bool) void {
    if (selected) c.fillRect(x - 8, y + 4, width, 26, Theme.navigation_hover);
    _ = c.drawUiText(label, x, y + 6, Theme.layer);
}

pub fn drawMenuBarSurface(c: *Canvas, rect: Rect) void {
    c.fillRect(rect.x, rect.y, rect.w, rect.h, Theme.navigation);
    c.fillRect(rect.x, rect.bottom() - 2, rect.w, 2, Theme.accent);
}

pub fn drawToolbarSurface(c: *Canvas, rect: Rect) void {
    c.fillRect(rect.x, rect.y, rect.w, rect.h, Theme.chrome);
    c.fillRect(rect.x, rect.bottom() - 1, rect.w, 1, Theme.divider);
}

pub fn drawPopupSurface(c: *Canvas, rect: Rect) void {
    fillRoundedRect(c, rect.x + 4, rect.y + 5, rect.w, rect.h, 6, 0xffb8c2cc);
    drawRoundedBorder(c, rect.x, rect.y, rect.w, rect.h, 6, Theme.layer, Theme.divider);
}

pub fn drawToolbarSeparator(c: *Canvas, x: i32, rect: Rect) i32 {
    c.fillRect(x + 5, rect.y + 9, 1, rect.h - 18, Theme.divider);
    return x + 12;
}

pub fn drawSplitter(c: *Canvas, rect: Rect) void {
    c.fillRect(rect.x, rect.y, rect.w, rect.h, Theme.divider);
}

pub fn drawContentSurface(c: *Canvas, rect: Rect, comic: bool) void {
    c.fillRect(rect.x, rect.y, rect.w, rect.h, if (comic) Theme.comic_paper else Theme.workspace);
}

pub fn drawTabStrip(c: *Canvas, rect: Rect) void {
    c.fillRect(rect.x, rect.y, rect.w, rect.h, Theme.subtle);
    c.fillRect(rect.x, rect.bottom() - 1, rect.w, 1, Theme.divider);
}

pub fn drawStatusTab(c: *Canvas, rect: Rect) void {
    c.fillRect(rect.x + 8, rect.y + 6, 96, rect.h - 8, Theme.accent_soft);
}

pub fn drawMemberCard(c: *Canvas, rect: Rect, selected: bool, departed: bool) void {
    const card = Rect{ .x = rect.x + 3, .y = rect.y + 3, .w = rect.w - 6, .h = rect.h - 6 };
    drawRoundedBorder(c, card.x, card.y, card.w, card.h, 5, Theme.layer, if (selected) Theme.accent else Theme.divider);
    fillRoundedRect(c, rect.x + 9, rect.y + 10, 7, 7, 4, if (departed) Theme.divider else Theme.success);
}

pub fn drawCharacterPane(c: *Canvas, rect: Rect) void {
    c.fillRect(rect.x, rect.y, rect.w, rect.h, Theme.workspace);
    drawOutline(c, rect.x, rect.y, rect.w, rect.h, Theme.divider);
}

/// The expression picker is an intentional control surface, rather than a
/// leftover slab beneath the character preview.  The caller draws the dial
/// and authored expression marks inside the returned interior.
pub fn drawExpressionPanel(c: *Canvas, rect: Rect, selection: []const u8) void {
    c.fillRect(rect.x, rect.y, rect.w, rect.h, Theme.subtle);
    c.fillRect(rect.x, rect.y, rect.w, 2, Theme.accent);
    c.fillRect(rect.x + 9, rect.y + 9, 4, 4, Theme.accent);
    _ = c.drawUiText("MOOD", rect.x + 20, rect.y + 5, Theme.ink);
    const label_w = Canvas.uiTextWidth(selection) + 20;
    const label_x = rect.right() - label_w - 8;
    drawPill(c, .{ .x = label_x, .y = rect.y + 4, .w = label_w, .h = 19 }, selection, true);
    c.fillRect(rect.x + 8, rect.y + 25, rect.w - 16, 1, Theme.divider);
}

pub fn drawComposerSurface(c: *Canvas, rect: Rect) void {
    c.fillRect(rect.x, rect.y, rect.w, rect.h, Theme.subtle);
    c.fillRect(rect.x, rect.y, rect.w, 2, Theme.accent);
}

pub fn drawHistoryBanner(c: *Canvas, rect: Rect, label: []const u8) void {
    const width = @min(rect.w - 12, Canvas.uiTextWidth(label) + 16);
    drawRoundedBorder(c, rect.x + 6, rect.y + 6, width, 25, 5, Theme.layer, Theme.divider);
    _ = c.drawUiText(label, rect.x + 12, rect.y + 8, Theme.secondary);
}

pub fn drawTab(c: *Canvas, x: i32, y: i32, width: i32, height: i32, selected: bool) void {
    fillRoundedRect(c, x, y, width, height + 5, 5, if (selected) Theme.layer else Theme.subtle);
    if (selected) c.fillRect(x, y, width, 3, Theme.accent);
    if (selected) drawOutline(c, x, y, width, height, Theme.accent_soft);
}

pub fn drawActionTile(c: *Canvas, x: i32, y: i32, width: i32, height: i32, selected: bool) u32 {
    drawRoundedBorder(c, x + 3, y + 7, width - 6, height - 14, 5, if (selected) Theme.accent_soft else Theme.layer, if (selected) Theme.accent_soft else Theme.divider);
    if (selected) c.fillRect(x + 1, y + height - 3, @max(1, width - 1), 3, Theme.accent);
    return if (selected) Theme.accent else Theme.ink;
}

pub fn drawFocusRing(c: *Canvas, rect: Rect) void {
    if (rect.w < 4 or rect.h < 4) return;
    c.fillRect(rect.x, rect.y, rect.w, 2, Theme.focus);
    c.fillRect(rect.x, rect.bottom() - 2, rect.w, 2, Theme.focus);
    c.fillRect(rect.x, rect.y, 2, rect.h, Theme.focus);
    c.fillRect(rect.right() - 2, rect.y, 2, rect.h, Theme.focus);
}

pub fn drawComposerField(c: *Canvas, rect: Rect, focused: bool) void {
    drawRoundedBorder(c, rect.x + 8, rect.y + 8, @max(0, rect.w - 16), @max(0, rect.h - 16), 6, Theme.layer, if (focused) Theme.accent else Theme.divider);
    if (focused) c.fillRect(rect.x + 8, rect.y + 8, 3, @max(0, rect.h - 16), Theme.accent);
}

pub fn drawMessageRow(c: *Canvas, rect: Rect, nick: []const u8, text: []const u8, alternate: bool) void {
    const nick_w = @min(112, @max(54, Canvas.uiTextWidth(nick) + 14));
    drawRoundedBorder(c, rect.x + 7, rect.y - 2, rect.w - 14, rect.h - 3, 5, if (alternate) Theme.chrome else Theme.layer, Theme.divider);
    c.fillRect(rect.x + 7, rect.y + 3, 3, rect.h - 13, Theme.accent);
    fillRoundedRect(c, rect.x + 16, rect.y + 2, nick_w - 8, 18, 4, Theme.accent_soft);
    drawEllipsized(c, nick, rect.x + 20, rect.y + 3, nick_w - 16, Theme.accent);
    drawEllipsized(c, text, rect.x + nick_w + 14, rect.y + 3, rect.w - nick_w - 24, Theme.ink);
}

pub fn drawMemberRow(c: *Canvas, rect: Rect, label: []const u8, selected: bool, departed: bool) void {
    if (selected) fillRoundedRect(c, rect.x + 3, rect.y - 1, rect.w - 6, 23, 4, Theme.accent_soft);
    fillRoundedRect(c, rect.x + 8, rect.y + 5, 8, 8, 4, if (departed) Theme.divider else Theme.success);
    drawEllipsized(c, label, rect.x + 24, rect.y, rect.w - 30, if (departed) Theme.secondary else Theme.ink);
}

pub fn drawPaneHeader(c: *Canvas, rect: Rect, title: []const u8) void {
    c.fillRect(rect.x, rect.y, rect.w, 24, Theme.layer);
    c.fillRect(rect.x, rect.y, 3, 24, Theme.accent);
    c.fillRect(rect.x, rect.y + 23, rect.w, 1, Theme.divider);
    _ = c.drawUiText(title, rect.x + 13, rect.y + 6, Theme.ink);
}

pub fn drawStatusBar(c: *Canvas, x: i32, y: i32, width: i32, height: i32, status: []const u8, member_count: usize) void {
    c.fillRect(x, y, width, height, Theme.layer);
    c.fillRect(x, y, width, 1, Theme.divider);
    const status_color = switch (statusTone(status)) {
        .success => Theme.success,
        .warning => Theme.warning,
        .failure => 0xffc42b1c,
        .info => Theme.accent,
    };
    c.fillRect(x + 9, y + 8, 6, 6, status_color);
    var buf: [32]u8 = undefined;
    const members = if (member_count == 1)
        "1 member"
    else
        std.fmt.bufPrint(&buf, "{d} members", .{member_count}) catch "members";
    const badge_w = Canvas.uiTextWidth(members) + 16;
    const badge_x = x + @max(108, width - badge_w - 8);
    c.fillRect(badge_x, y + 5, badge_w, @max(1, height - 10), Theme.accent_soft);
    drawEllipsized(c, status, x + 22, y + 4, badge_x - x - 30, Theme.secondary);
    _ = c.drawUiText(members, badge_x + 8, y + 4, Theme.secondary);
}

pub fn statusTone(status: []const u8) NoticeTone {
    if (std.mem.indexOf(u8, status, "connected") != null and std.mem.indexOf(u8, status, "reconnecting") == null) return .success;
    if (std.mem.indexOf(u8, status, "error") != null or std.mem.indexOf(u8, status, "failed") != null) return .failure;
    if (std.mem.indexOf(u8, status, "reconnect") != null or std.mem.indexOf(u8, status, "offline") != null) return .warning;
    return .info;
}

pub fn drawEmptyState(c: *Canvas, x: i32, y: i32, width: i32, height: i32, detail: []const u8) void {
    c.fillRect(x, y, width, height, Theme.workspace);
    const card_w = @min(380, @max(240, width - 64));
    const card_h = 144;
    const card_x = x + @divTrunc(width - card_w, 2);
    const card_y = y + @divTrunc(height - card_h, 2);
    drawSurface(c, .{ .x = card_x, .y = card_y, .w = card_w, .h = card_h }, .raised);
    c.fillRect(card_x, card_y, card_w, 34, Theme.subtle);
    c.fillRect(card_x, card_y, 4, 34, Theme.accent);
    c.fillRect(card_x, card_y + 33, card_w, 1, Theme.divider);
    c.fillRect(card_x + 18, card_y + 13, 6, 6, Theme.accent);
    _ = c.drawUiText("Ready to talk", card_x + 34, card_y + 9, Theme.ink);
    _ = c.drawUiText("Your conversation starts here", card_x + 20, card_y + 51, Theme.ink);
    drawEllipsized(c, detail, card_x + 20, card_y + 74, card_w - 40, Theme.secondary);
    c.fillRect(card_x + 20, card_y + 108, card_w - 40, 23, Theme.accent_soft);
    c.fillRect(card_x + 20, card_y + 108, 3, 23, Theme.accent);
    _ = c.drawUiText("Type a message below to begin", card_x + 32, card_y + 111, Theme.focus);
}

fn drawEllipsized(c: *Canvas, text: []const u8, x: i32, y: i32, max_width: i32, color: u32) void {
    if (max_width <= 0) return;
    if (Canvas.uiTextWidth(text) <= max_width) {
        _ = c.drawUiText(text, x, y, color);
        return;
    }
    const dots = "...";
    const dots_width = Canvas.uiTextWidth(dots);
    var end = text.len;
    while (end > 0 and Canvas.uiTextWidth(text[0..end]) + dots_width > max_width) end -= 1;
    _ = c.drawUiText(text[0..end], x, y, color);
    _ = c.drawUiText(dots, x + Canvas.uiTextWidth(text[0..end]), y, color);
}

test "primary buttons and focused fields use the shared accent" {
    const testing = std.testing;
    var canvas = try Canvas.init(testing.allocator, 160, 64);
    defer canvas.deinit(testing.allocator);
    drawButton(&canvas, 4, 4, 90, "Save", .primary, false);
    drawField(&canvas, 4, 38, 120, true);
    try testing.expectEqual(Theme.accent, canvas.px[10 + 10 * 160]);
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

test "semantic feedback primitives classify status and button targets" {
    const layout = DialogLayout.init(640, 430, 252, 218, 2, 96);
    try std.testing.expectEqual(NoticeTone.success, statusTone("connected"));
    try std.testing.expectEqual(NoticeTone.warning, statusTone("reconnecting"));
    try std.testing.expectEqual(NoticeTone.failure, statusTone("connection failed"));
    try std.testing.expectEqual(DialogButton.primary, dialogButtonAt(layout, layout.primary.x + 1, layout.primary.y + 1).?);
    try std.testing.expectEqual(DialogButton.cancel, dialogButtonAt(layout, layout.cancel.x + 1, layout.cancel.y + 1).?);
}

test "control states resolve selected pressed and disabled colors consistently" {
    try std.testing.expectEqual(Theme.accent, resolveControlColors(.{ .selected = true }).content);
    try std.testing.expectEqual(Theme.layer, resolveControlColors(.{ .pressed = true }).content);
    try std.testing.expectEqual(Theme.divider, resolveControlColors(.{ .disabled = true }).content);
}
