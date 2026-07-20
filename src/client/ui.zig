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
    pub const ink: u32 = 0xff182033;
    pub const secondary: u32 = 0xff697386;
    pub const chrome: u32 = 0xfff7f8fb;
    pub const layer: u32 = 0xffffffff;
    pub const subtle: u32 = 0xffedf0f5;
    pub const divider: u32 = 0xffd8dde7;
    pub const accent: u32 = 0xff3563e9;
    pub const accent_soft: u32 = 0xffe8edff;
    pub const focus: u32 = 0xff2147bd;
    pub const success: u32 = 0xff169b62;
    pub const warning: u32 = 0xffd97706;
    pub const comic_paper: u32 = 0xffe9edf4;
    pub const workspace: u32 = 0xfff1f3f7;
    pub const navigation: u32 = 0xff171d2d;
    pub const navigation_hover: u32 = 0xff2a334b;
    pub const shadow: u32 = 0xffc5cbd6;
    pub const paper_ink: u32 = 0xff252936;
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
    if (state.focused) return .{ .fill = Theme.layer, .border = Theme.accent, .content = Theme.focus };
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
            .primary = .{ .x = rect.right() - 100 - primary_width, .y = rect.bottom() - 42, .w = primary_width, .h = 32 },
            .cancel = .{ .x = rect.right() - 88, .y = rect.bottom() - 42, .w = 78, .h = 32 },
        };
    }

    pub fn fieldLabelY(self: DialogLayout, index: usize) i32 {
        return self.body_y + @as(i32, @intCast(index)) * self.row_h;
    }

    pub fn fieldRect(self: DialogLayout, index: usize) Rect {
        return .{ .x = self.rect.x + 24, .y = self.fieldLabelY(index) + 18, .w = self.rect.w - 48, .h = 30 };
    }

    pub fn fieldIndexAt(self: DialogLayout, x: i32, y: i32) ?usize {
        if (y < self.body_y or y >= self.rect.bottom() - 43) return null;
        const raw = @divTrunc(y - self.body_y, self.row_h);
        if (raw < 0 or raw >= self.field_count) return null;
        const index: usize = @intCast(raw);
        if (!contains(self.fieldRect(index), x, y)) return null;
        return index;
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

/// Four-by-four supersampling for compact vector-like icon artwork. These
/// primitives stay deterministic across every native framebuffer backend.
pub fn drawAaDisc(c: *Canvas, cx: i32, cy: i32, radius: f64, color: u32) void {
    const extent: i32 = @intFromFloat(@ceil(radius + 1.0));
    var py = cy - extent;
    while (py <= cy + extent) : (py += 1) {
        var px = cx - extent;
        while (px <= cx + extent) : (px += 1) {
            var covered: u32 = 0;
            for (0..4) |sample_y| for (0..4) |sample_x| {
                const sx = @as(f64, @floatFromInt(px)) + (@as(f64, @floatFromInt(sample_x)) + 0.5) / 4.0;
                const sy = @as(f64, @floatFromInt(py)) + (@as(f64, @floatFromInt(sample_y)) + 0.5) / 4.0;
                const dx = sx - (@as(f64, @floatFromInt(cx)) + 0.5);
                const dy = sy - (@as(f64, @floatFromInt(cy)) + 0.5);
                if (dx * dx + dy * dy <= radius * radius) covered += 1;
            };
            if (covered != 0) c.blendPixel(px, py, color, @divTrunc(covered * 255, 16));
        }
    }
}

pub fn drawAaLine(c: *Canvas, x1: i32, y1: i32, x2: i32, y2: i32, width: f64, color: u32) void {
    const extent: i32 = @intFromFloat(@ceil(width));
    var py = @min(y1, y2) - extent;
    while (py <= @max(y1, y2) + extent) : (py += 1) {
        var px = @min(x1, x2) - extent;
        while (px <= @max(x1, x2) + extent) : (px += 1) {
            var covered: u32 = 0;
            for (0..4) |sample_y| for (0..4) |sample_x| {
                const sx = @as(f64, @floatFromInt(px)) + (@as(f64, @floatFromInt(sample_x)) + 0.5) / 4.0;
                const sy = @as(f64, @floatFromInt(py)) + (@as(f64, @floatFromInt(sample_y)) + 0.5) / 4.0;
                if (pointSegmentDistanceSquared(sx, sy, @floatFromInt(x1), @floatFromInt(y1), @floatFromInt(x2), @floatFromInt(y2)) <= width * width / 4.0) covered += 1;
            };
            if (covered != 0) c.blendPixel(px, py, color, @divTrunc(covered * 255, 16));
        }
    }
}

fn pointSegmentDistanceSquared(px: f64, py: f64, x1: f64, y1: f64, x2: f64, y2: f64) f64 {
    const dx = x2 - x1;
    const dy = y2 - y1;
    if (dx == 0 and dy == 0) return (px - x1) * (px - x1) + (py - y1) * (py - y1);
    const t = std.math.clamp(((px - x1) * dx + (py - y1) * dy) / (dx * dx + dy * dy), 0.0, 1.0);
    const nearest_x = x1 + t * dx;
    const nearest_y = y1 + t * dy;
    return (px - nearest_x) * (px - nearest_x) + (py - nearest_y) * (py - nearest_y);
}

pub fn drawSurface(c: *Canvas, rect: Rect, kind: SurfaceKind) void {
    const fill = switch (kind) {
        .canvas => Theme.workspace,
        .panel => Theme.layer,
        .raised => Theme.chrome,
        .accent => Theme.accent_soft,
    };
    if (kind == .raised) fillRoundedRect(c, rect.x + 4, rect.y + 6, rect.w, rect.h, 10, Theme.shadow);
    drawRoundedBorder(c, rect.x, rect.y, rect.w, rect.h, 10, fill, Theme.divider);
}

pub fn drawPill(c: *Canvas, rect: Rect, label: []const u8, active: bool) void {
    const fill = if (active) Theme.accent_soft else Theme.chrome;
    const color = if (active) Theme.accent else Theme.secondary;
    drawRoundedBorder(c, rect.x, rect.y, rect.w, rect.h, @divTrunc(rect.h, 2), fill, if (active) Theme.accent_soft else Theme.divider);
    drawEllipsized(c, label, rect.x + 8, rect.y + @max(1, @divTrunc(rect.h - 17, 2)), rect.w - 16, color);
}

pub fn drawTooltip(c: *Canvas, rect: Rect, label: []const u8) void {
    fillRoundedRect(c, rect.x + 3, rect.y + 5, rect.w, rect.h, 7, Theme.shadow);
    drawRoundedBorder(c, rect.x, rect.y, rect.w, rect.h, 7, Theme.navigation, Theme.navigation_hover);
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
    drawRoundedBorder(c, x, y, width, 32, 7, fill, border);
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
    fillRoundedRect(c, rect.x + 7, rect.y + 10, rect.w, rect.h, 12, Theme.shadow);
    drawRoundedBorder(c, rect.x, rect.y, rect.w, rect.h, 12, Theme.layer, Theme.divider);
    fillRoundedRect(c, rect.x + 20, rect.y + 18, 28, 28, 8, Theme.accent);
    _ = c.drawUiText("C", rect.x + 29, rect.y + 21, Theme.layer);
    _ = c.drawUiText(title, rect.x + 60, rect.y + 17, Theme.ink);
    _ = c.drawUiText(subtitle, rect.x + 24, rect.y + 52, Theme.secondary);
    c.fillRect(rect.x + 24, rect.y + 74, rect.w - 48, 1, Theme.divider);
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
    drawRoundedBorder(c, x, y, width, 30, 7, if (active) 0xfffbfcff else Theme.chrome, if (active) Theme.accent else Theme.divider);
    if (active) c.fillRect(x + 1, y + 7, 3, 16, Theme.accent);
}

pub fn drawChoiceField(c: *Canvas, x: i32, y: i32, width: i32, active: bool) void {
    drawRoundedBorder(c, x, y, width, 30, 7, if (active) Theme.layer else Theme.chrome, if (active) Theme.accent else Theme.divider);
    c.fillRect(x + width - 34, y + 5, 1, 20, Theme.divider);
    const cx = x + width - 17;
    c.drawLine(cx - 4, y + 12, cx, y + 16, Theme.secondary);
    c.drawLine(cx, y + 16, cx + 4, y + 12, Theme.secondary);
}

pub fn drawListField(c: *Canvas, x: i32, y: i32, width: i32, active: bool) void {
    drawRoundedBorder(c, x, y, width, 30, 7, if (active) Theme.layer else Theme.chrome, if (active) Theme.accent else Theme.divider);
    fillRoundedRect(c, x + 8, y + 8, 14, 14, 4, Theme.accent_soft);
    c.fillRect(x + 12, y + 11, 7, 2, Theme.accent);
    c.fillRect(x + 12, y + 16, 7, 2, Theme.accent);
}

pub fn drawPreviewField(c: *Canvas, x: i32, y: i32, width: i32) void {
    drawRoundedBorder(c, x, y, width, 30, 7, Theme.chrome, Theme.divider);
    fillRoundedRect(c, x + 8, y + 7, 30, 16, 5, Theme.accent);
    fillRoundedRect(c, x + 43, y + 7, 20, 16, 5, Theme.accent_soft);
}

pub fn drawReadonlyField(c: *Canvas, x: i32, y: i32, width: i32) void {
    fillRoundedRect(c, x, y, width, 30, 7, Theme.subtle);
    fillRoundedRect(c, x + 9, y + 11, 8, 8, 4, Theme.success);
}

/// A compact command tile for icon-only tools.  The top ink mark is the
/// Comic Chat signature: selected modes read at a glance without a bulky
/// native-toolbar bevel.
pub fn drawCommandTile(c: *Canvas, x: i32, y: i32, selected: bool, hovered: bool) u32 {
    const state: ControlState = .{ .selected = selected, .hovered = hovered };
    const colors = resolveControlColors(state);
    if (selected) {
        fillRoundedRect(c, x, y, 32, 32, 8, colors.fill);
        c.fillRect(x + 8, y + 29, 16, 3, Theme.accent);
        return colors.content;
    }
    if (hovered) {
        drawRoundedBorder(c, x, y, 32, 32, 8, colors.fill, colors.border);
        return colors.content;
    }
    fillRoundedRect(c, x, y, 32, 32, 8, colors.fill);
    return colors.content;
}

pub fn drawMenuItem(c: *Canvas, x: i32, y: i32, width: i32, label: []const u8, selected: bool) void {
    if (selected) {
        fillRoundedRect(c, x, y, width, 25, 7, Theme.accent_soft);
        fillRoundedRect(c, x + 5, y + 8, 4, 9, 2, Theme.accent);
    }
    _ = c.drawUiText(label, x + 16, y + 4, Theme.ink);
}

pub fn drawMenuLabel(c: *Canvas, x: i32, y: i32, width: i32, label: []const u8, selected: bool) void {
    if (selected) fillRoundedRect(c, x - 8, y + 4, width, 26, 7, Theme.navigation_hover);
    _ = c.drawUiText(label, x, y + 6, Theme.layer);
}

pub fn drawMenuBarSurface(c: *Canvas, rect: Rect) void {
    c.fillRect(rect.x, rect.y, rect.w, rect.h, Theme.navigation);
    c.fillRect(rect.x, rect.bottom() - 1, rect.w, 1, 0xff30384d);
}

pub fn drawToolbarSurface(c: *Canvas, rect: Rect) void {
    c.fillRect(rect.x, rect.y, rect.w, rect.h, Theme.chrome);
    c.fillRect(rect.x, rect.bottom() - 1, rect.w, 1, Theme.divider);
}

pub fn drawToolbarGroup(c: *Canvas, rect: Rect) void {
    fillRoundedRect(c, rect.x + 1, rect.y + 2, rect.w, rect.h, 9, Theme.shadow);
    drawRoundedBorder(c, rect.x, rect.y, rect.w, rect.h, 9, Theme.layer, Theme.divider);
}

pub fn drawPopupSurface(c: *Canvas, rect: Rect) void {
    fillRoundedRect(c, rect.x + 6, rect.y + 8, rect.w, rect.h, 10, Theme.shadow);
    drawRoundedBorder(c, rect.x, rect.y, rect.w, rect.h, 10, Theme.layer, Theme.divider);
}

pub fn drawToolbarSeparator(c: *Canvas, x: i32, rect: Rect) i32 {
    c.fillRect(x + 5, rect.y + 9, 1, rect.h - 18, Theme.divider);
    return x + 12;
}

pub fn drawSplitter(c: *Canvas, rect: Rect) void {
    c.fillRect(rect.x, rect.y, rect.w, rect.h, Theme.workspace);
    if (rect.w > rect.h) fillRoundedRect(c, rect.x + @divTrunc(rect.w - 32, 2), rect.y + @divTrunc(rect.h - 3, 2), @min(32, rect.w), 3, 2, Theme.divider);
    if (rect.h > rect.w) fillRoundedRect(c, rect.x + @divTrunc(rect.w - 3, 2), rect.y + @divTrunc(rect.h - 32, 2), 3, @min(32, rect.h), 2, Theme.divider);
}

pub fn drawContentSurface(c: *Canvas, rect: Rect, comic: bool) void {
    c.fillRect(rect.x, rect.y, rect.w, rect.h, if (comic) Theme.comic_paper else Theme.workspace);
}

pub fn drawTabStrip(c: *Canvas, rect: Rect) void {
    c.fillRect(rect.x, rect.y, rect.w, rect.h, Theme.chrome);
    c.fillRect(rect.x, rect.bottom() - 1, rect.w, 1, Theme.divider);
}

pub fn drawStatusTab(c: *Canvas, rect: Rect) void {
    fillRoundedRect(c, rect.x + 8, rect.y + 6, 96, rect.h - 12, 8, Theme.subtle);
}

pub fn drawMemberCard(c: *Canvas, rect: Rect, selected: bool, departed: bool, hovered: bool) void {
    const card = Rect{ .x = rect.x + 4, .y = rect.y + 4, .w = rect.w - 8, .h = rect.h - 8 };
    if (selected) fillRoundedRect(c, card.x + 2, card.y + 3, card.w, card.h, 9, Theme.shadow);
    drawRoundedBorder(c, card.x, card.y, card.w, card.h, 9, if (selected) Theme.accent_soft else if (hovered) Theme.chrome else Theme.layer, if (selected) Theme.accent else if (hovered) Theme.secondary else Theme.divider);
    fillRoundedRect(c, rect.x + 10, rect.y + 10, 7, 7, 4, if (departed) Theme.divider else Theme.success);
}

pub fn drawCharacterPane(c: *Canvas, rect: Rect) void {
    c.fillRect(rect.x, rect.y, rect.w, rect.h, Theme.workspace);
    drawRoundedBorder(c, rect.x + 5, rect.y + 5, rect.w - 10, rect.h - 10, 10, Theme.layer, Theme.divider);
}

/// The expression picker is an intentional control surface, rather than a
/// leftover slab beneath the character preview.  The caller draws the dial
/// and authored expression marks inside the returned interior.
pub fn drawExpressionPanel(c: *Canvas, rect: Rect, selection: []const u8) void {
    c.fillRect(rect.x, rect.y, rect.w, rect.h, Theme.workspace);
    drawRoundedBorder(c, rect.x + 5, rect.y + 2, rect.w - 10, rect.h - 7, 10, Theme.layer, Theme.divider);
    fillRoundedRect(c, rect.x + 13, rect.y + 11, 5, 5, 3, Theme.accent);
    _ = c.drawUiText("MOOD", rect.x + 26, rect.y + 6, Theme.ink);
    const label_w = Canvas.uiTextWidth(selection) + 20;
    const label_x = rect.right() - label_w - 13;
    drawPill(c, .{ .x = label_x, .y = rect.y + 5, .w = label_w, .h = 20 }, selection, true);
    c.fillRect(rect.x + 13, rect.y + 31, rect.w - 26, 1, Theme.divider);
}

pub fn drawComposerSurface(c: *Canvas, rect: Rect) void {
    c.fillRect(rect.x, rect.y, rect.w, rect.h, Theme.workspace);
    c.fillRect(rect.x, rect.y, rect.w, 1, Theme.divider);
}

pub fn drawHistoryBanner(c: *Canvas, rect: Rect, label: []const u8) void {
    const width = @min(rect.w - 12, Canvas.uiTextWidth(label) + 16);
    drawRoundedBorder(c, rect.x + 6, rect.y + 6, width, 25, 5, Theme.layer, Theme.divider);
    _ = c.drawUiText(label, rect.x + 12, rect.y + 8, Theme.secondary);
}

pub fn drawTab(c: *Canvas, x: i32, y: i32, width: i32, height: i32, selected: bool) void {
    fillRoundedRect(c, x, y, width, height - 2, 8, if (selected) Theme.layer else Theme.chrome);
    if (selected) {
        drawRoundedBorder(c, x, y, width, height - 2, 8, Theme.layer, Theme.divider);
        fillRoundedRect(c, x + 12, y + height - 6, width - 24, 3, 2, Theme.accent);
    }
}

pub fn drawActionTile(c: *Canvas, x: i32, y: i32, width: i32, height: i32, selected: bool, hovered: bool) u32 {
    const inset = Rect{ .x = x + 5, .y = y + 7, .w = width - 10, .h = height - 14 };
    if (selected) {
        fillRoundedRect(c, inset.x + 2, inset.y + 3, inset.w, inset.h, 9, Theme.shadow);
        drawRoundedBorder(c, inset.x, inset.y, inset.w, inset.h, 9, Theme.accent, Theme.accent);
        return Theme.layer;
    }
    drawRoundedBorder(c, inset.x, inset.y, inset.w, inset.h, 9, if (hovered) Theme.accent_soft else Theme.layer, if (hovered) Theme.accent else Theme.divider);
    return if (hovered) Theme.accent else Theme.secondary;
}

pub fn drawFocusRing(c: *Canvas, rect: Rect) void {
    if (rect.w < 4 or rect.h < 4) return;
    c.fillRect(rect.x, rect.y, rect.w, 2, Theme.focus);
    c.fillRect(rect.x, rect.bottom() - 2, rect.w, 2, Theme.focus);
    c.fillRect(rect.x, rect.y, 2, rect.h, Theme.focus);
    c.fillRect(rect.right() - 2, rect.y, 2, rect.h, Theme.focus);
}

pub fn drawComposerField(c: *Canvas, rect: Rect, focused: bool) void {
    const field = Rect{ .x = rect.x + 9, .y = rect.y + 7, .w = @max(0, rect.w - 18), .h = @max(0, rect.h - 14) };
    if (focused) fillRoundedRect(c, field.x + 2, field.y + 3, field.w, field.h, 10, Theme.shadow);
    drawRoundedBorder(c, field.x, field.y, field.w, field.h, 10, Theme.layer, if (focused) Theme.accent else Theme.divider);
    if (focused) fillRoundedRect(c, field.x + 1, field.y + 9, 3, field.h - 18, 2, Theme.accent);
}

pub fn drawStepper(c: *Canvas, rect: Rect, decrease_hovered: bool, increase_hovered: bool) void {
    drawRoundedBorder(c, rect.x, rect.y, rect.w, rect.h, 8, Theme.layer, Theme.divider);
    if (decrease_hovered) fillRoundedRect(c, rect.x + 1, rect.y + 1, 29, rect.h - 2, 7, Theme.accent_soft);
    if (increase_hovered) fillRoundedRect(c, rect.right() - 30, rect.y + 1, 29, rect.h - 2, 7, Theme.accent_soft);
    c.fillRect(rect.x + 30, rect.y + 5, 1, rect.h - 10, Theme.divider);
    c.fillRect(rect.right() - 31, rect.y + 5, 1, rect.h - 10, Theme.divider);
}

pub fn drawMessageRow(c: *Canvas, rect: Rect, nick: []const u8, text: []const u8, alternate: bool) void {
    const nick_w = @min(112, @max(54, Canvas.uiTextWidth(nick) + 14));
    drawRoundedBorder(c, rect.x + 7, rect.y - 2, rect.w - 14, rect.h - 3, 5, if (alternate) Theme.chrome else Theme.layer, Theme.divider);
    c.fillRect(rect.x + 7, rect.y + 3, 3, rect.h - 13, Theme.accent);
    fillRoundedRect(c, rect.x + 16, rect.y + 2, nick_w - 8, 18, 4, Theme.accent_soft);
    drawEllipsized(c, nick, rect.x + 20, rect.y + 3, nick_w - 16, Theme.accent);
    drawEllipsized(c, text, rect.x + nick_w + 14, rect.y + 3, rect.w - nick_w - 24, Theme.ink);
}

pub fn drawMemberRow(c: *Canvas, rect: Rect, label: []const u8, selected: bool, departed: bool, hovered: bool) void {
    if (selected or hovered) fillRoundedRect(c, rect.x + 3, rect.y - 1, rect.w - 6, 23, 6, if (selected) Theme.accent_soft else Theme.chrome);
    fillRoundedRect(c, rect.x + 8, rect.y + 5, 8, 8, 4, if (departed) Theme.divider else Theme.success);
    drawEllipsized(c, label, rect.x + 24, rect.y, rect.w - 30, if (departed) Theme.secondary else Theme.ink);
}

pub fn drawPaneHeader(c: *Canvas, rect: Rect, title: []const u8) void {
    c.fillRect(rect.x, rect.y, rect.w, 30, Theme.workspace);
    fillRoundedRect(c, rect.x + 10, rect.y + 11, 5, 5, 3, Theme.accent);
    _ = c.drawUiText(title, rect.x + 23, rect.y + 7, Theme.ink);
}

pub fn drawStatusBar(c: *Canvas, x: i32, y: i32, width: i32, height: i32, status: []const u8, member_count: usize) void {
    c.fillRect(x, y, width, height, Theme.navigation);
    c.fillRect(x, y, width, 1, Theme.divider);
    const status_color = switch (statusTone(status)) {
        .success => Theme.success,
        .warning => Theme.warning,
        .failure => 0xffc42b1c,
        .info => Theme.accent,
    };
    fillRoundedRect(c, x + 10, y + 9, 6, 6, 3, status_color);
    var buf: [32]u8 = undefined;
    const members = if (member_count == 1)
        "1 member"
    else
        std.fmt.bufPrint(&buf, "{d} members", .{member_count}) catch "members";
    const badge_w = Canvas.uiTextWidth(members) + 16;
    const badge_x = x + @max(108, width - badge_w - 8);
    fillRoundedRect(c, badge_x, y + 4, badge_w, @max(1, height - 8), 7, Theme.navigation_hover);
    drawEllipsized(c, status, x + 24, y + 4, badge_x - x - 32, 0xffcbd3df);
    _ = c.drawUiText(members, badge_x + 8, y + 4, Theme.layer);
}

pub fn statusTone(status: []const u8) NoticeTone {
    if (std.mem.indexOf(u8, status, "connected") != null and std.mem.indexOf(u8, status, "reconnecting") == null) return .success;
    if (std.mem.indexOf(u8, status, "error") != null or std.mem.indexOf(u8, status, "failed") != null) return .failure;
    if (std.mem.indexOf(u8, status, "reconnect") != null or std.mem.indexOf(u8, status, "offline") != null) return .warning;
    return .info;
}

pub fn drawEmptyState(c: *Canvas, x: i32, y: i32, width: i32, height: i32, detail: []const u8, requested_columns: u8) void {
    c.fillRect(x, y, width, height, Theme.workspace);
    if (width < 360 or height < 170) {
        const label = "Type a message to start the scene";
        drawEllipsized(c, label, x + 16, y + @max(8, @divTrunc(height - 17, 2)), width - 32, Theme.secondary);
        return;
    }
    const page_w = @min(620, @max(280, width - 56));
    const page_h = @min(390, @max(170, height - 32));
    const page_x = x + @divTrunc(width - page_w, 2);
    const page_y = y + @divTrunc(height - page_h, 2);
    drawSurface(c, .{ .x = page_x, .y = page_y, .w = page_w, .h = page_h }, .raised);

    c.fillRect(page_x + 1, page_y + 1, page_w - 2, 42, Theme.chrome);
    c.fillRect(page_x + 1, page_y + 42, page_w - 2, 1, Theme.divider);
    fillRoundedRect(c, page_x + 16, page_y + 13, 16, 16, 5, Theme.accent);
    _ = c.drawUiText("New scene", page_x + 44, page_y + 11, Theme.ink);
    const columns: i32 = std.math.clamp(@as(i32, requested_columns), 1, 6);
    var layout_buf: [20]u8 = undefined;
    const layout_label = std.fmt.bufPrint(&layout_buf, "{d} panels across", .{columns}) catch "4 panels across";
    const layout_w = Canvas.uiTextWidth(layout_label) + 20;
    drawPill(c, .{ .x = page_x + page_w - layout_w - 12, .y = page_y + 10, .w = layout_w, .h = 23 }, layout_label, true);

    const gutter: i32 = 10;
    const inner_x = page_x + 16;
    const inner_y = page_y + 58;
    const inner_w = page_w - 32;
    const available_panel_h = page_h - 132;
    const panel_w = @divTrunc(inner_w - (columns - 1) * gutter, columns);
    const panel_h = @min(available_panel_h, panel_w);
    const panels_y = inner_y + @divTrunc(available_panel_h - panel_h, 2);
    var column: i32 = 0;
    while (column < columns) : (column += 1) {
        const panel_x = inner_x + column * (panel_w + gutter);
        const actual_w = if (column == columns - 1) inner_x + inner_w - panel_x else panel_w;
        drawRoundedBorder(c, panel_x, panels_y, actual_w, panel_h, 5, if (column == 0) Theme.layer else Theme.chrome, Theme.paper_ink);
        fillRoundedRect(c, panel_x + 10, panels_y + 10, 20, 5, 3, if (column == 0) Theme.accent else Theme.divider);
        var number_buf: [8]u8 = undefined;
        const number = std.fmt.bufPrint(&number_buf, "{d}", .{column + 1}) catch "1";
        _ = c.drawUiText(number, panel_x + actual_w - Canvas.uiTextWidth(number) - 9, panels_y + 6, Theme.secondary);
    }

    const prompt_y = page_y + page_h - 57;
    fillRoundedRect(c, inner_x, prompt_y, inner_w, 40, 8, Theme.accent_soft);
    fillRoundedRect(c, inner_x + 10, prompt_y + 11, 18, 18, 6, Theme.accent);
    _ = c.drawUiText("+", inner_x + 15, prompt_y + 11, Theme.layer);
    _ = c.drawUiText("Start the scene", inner_x + 40, prompt_y + 4, Theme.ink);
    drawEllipsized(c, detail, inner_x + 40, prompt_y + 21, inner_w - 52, Theme.secondary);
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
    try testing.expectEqual(Theme.accent, canvas.px[11 + 38 * 160]);
}

test "dialog layout keeps fields and actions inside the modal" {
    const layout = DialogLayout.init(640, 430, 252, 226, 3, 108);
    try std.testing.expect(layout.rect.w >= 300);
    try std.testing.expect(contains(layout.primary, layout.primary.x + 1, layout.primary.y + 1));
    try std.testing.expect(contains(layout.cancel, layout.cancel.x + 1, layout.cancel.y + 1));
    const last_field = layout.fieldRect(2);
    try std.testing.expect(last_field.y > layout.fieldRect(0).y);
    try std.testing.expect(last_field.y + last_field.h < layout.primary.y);
    try std.testing.expectEqual(@as(?usize, 2), layout.fieldIndexAt(last_field.x + 2, last_field.y + 2));
    try std.testing.expectEqual(@as(?usize, null), layout.fieldIndexAt(layout.rect.x + 2, last_field.y + 2));
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
    try std.testing.expectEqual(Theme.accent, resolveControlColors(.{ .focused = true }).border);
    try std.testing.expectEqual(Theme.divider, resolveControlColors(.{ .disabled = true }).content);
}

test "supersampled icon primitives produce smooth partial edge coverage" {
    var canvas = try Canvas.init(std.testing.allocator, 32, 32);
    defer canvas.deinit(std.testing.allocator);
    canvas.clear(Theme.layer);
    drawAaDisc(&canvas, 16, 16, 8.4, Theme.accent);
    drawAaLine(&canvas, 9, 16, 23, 16, 1.8, Theme.ink);
    const center = canvas.px[16 * 32 + 16];
    try std.testing.expect(center != Theme.layer and center != Theme.accent);
    var has_partial_coverage = false;
    for (canvas.px) |pixel| {
        if (pixel != Theme.layer and pixel != Theme.accent and pixel != Theme.ink) {
            has_partial_coverage = true;
            break;
        }
    }
    try std.testing.expect(has_partial_coverage);
}
