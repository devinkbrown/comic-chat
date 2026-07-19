//! Software raster bridge for Microsoft Comic Chat 2.5 panel geometry.
//!
//! `original_balloon.zig` deliberately keeps the released renderer's y-up
//! TWIP coordinates.  This module is the single device boundary: it maps those
//! coordinates to a top-down `Canvas`, clips to the target viewport, fills the
//! Woodring path, and reproduces the source pen/dash ordering.
//!
//! Source correspondence:
//! - `panel.cpp:664-707`: bodies first, panel elements tail-to-head.
//! - `balloon.cpp:97-98,1777-1807`: 28 TWIP black pen; whispers first use a
//!   100 TWIP white nimbus, then a continuous 100/100 black source dash.
//! - `balloon.cpp:1819-1867`: thought-tail ellipses.
//! - `balloon.cpp:1872-1900`: four-sided action box.
//! - `arc.cpp:97-125` and `traj.cpp:8-111`: circular `DrawArc2` geometry and
//!   Manhattan-distance dash progression.
//! - `defines.h:118`: `STRETCH_HALFTONE`; `blitImage` supplies the portable
//!   premultiplied-alpha bilinear counterpart, including mirrored bodies.

const std = @import("std");
const balloon = @import("original_balloon.zig");
const formatting = @import("formatting.zig");
const canvas_mod = @import("../render/canvas.zig");
const font = @import("../render/font.zig");
const whisper_font = @import("../render/font_italic.zig");

pub const Canvas = canvas_mod.Canvas;
pub const Color = canvas_mod.Color;

pub const source_pen_twips: f64 = 28.0;
pub const source_nimbus_twips: f64 = 100.0;
pub const source_dash_twips: i32 = 100;
pub const source_panel_size: i32 = 2300;
/// 12 points at 1440 TWIPs/inch (`IDS_DFLT_COMICSPNTSIZE`, `chat.rc:2336`).
pub const source_font_height_twips: i32 = 240;

/// The vertical values that Win32's `CFontInfo` derives from a selected face.
/// Keeping the intermediate values visible makes it possible to verify the
/// checked-in atlas against `fonts.cpp:82-92` and `balloon.cpp:584-627` instead
/// of collapsing the LOGFONT request and resulting TEXTMETRIC height together.
pub const AtlasVerticalMetrics = struct {
    request_height: i32,
    ascent: i32,
    descent: i32,
    external_leading: i32,
    leading: i32,
    base_add: i32,
    top_offset: i32,
    line_height: i32,
};

const HeightBasis = enum {
    /// `logical_height` is the source LOGFONT character-height request.
    source_request,
    /// Compatibility path for title labels, whose CFontInfo construction is
    /// separate from the balloon metrics fixed here.
    line_height,
};

/// Metric adapter shared by balloon layout and rasterization. Microsoft uses
/// 12pt bold Comic Sans MS and derives an italic Western whisper face in
/// `fonts.cpp:72-92`. The portable build substitutes checked-in Comic Neue
/// Bold/Bold Italic atlases. Their nominal 20px request produces an 18px
/// ascent, 5px descent, and 23px TEXTMETRIC height; source balloons request
/// 240 TWIPs and retain the default Comic-Sans vertical-kern adjustment.
pub const AtlasMetrics = struct {
    logical_height: i32 = source_font_height_twips,
    height_basis: HeightBasis = .source_request,
    style: canvas_mod.FontStyle = .normal,

    /// Preserve the pre-existing title-label scale until its independent
    /// `UpdateTitleFonts` CFontInfo path is ported. Balloon callers use the
    /// default source-request basis.
    pub fn fromLogicalLineHeight(logical_line_height: i32) AtlasMetrics {
        return .{ .logical_height = logical_line_height, .height_basis = .line_height };
    }

    pub fn verticalMetrics(self: AtlasMetrics) AtlasVerticalMetrics {
        if (self.height_basis == .line_height) return .{
            .request_height = self.logical_height,
            .ascent = self.logical_height,
            .descent = 0,
            .external_leading = 0,
            .leading = 0,
            .base_add = 0,
            .top_offset = 0,
            .line_height = self.logical_height,
        };

        const ascent = self.scaleNativeMetric(nativeAscent(self.style));
        const descent = self.scaleNativeMetric(nativeDescent(self.style));
        const external_leading = self.scaleNativeMetric(nativeExternalLeading(self.style));

        // `fonts.cpp:82-92`: the shipped Comic Sans face receives this compact
        // vertical kernel. Comic Neue is the portable replacement for that
        // face, so retain the same branch rather than treating the substitute
        // as an unrelated fallback font.
        const requested_leading = @divTrunc(-40 * self.logical_height, 180);
        const requested_base_add = @divTrunc(30 * self.logical_height, 180);
        const leading = requested_leading + external_leading;
        const base_add = requested_base_add - external_leading;
        return .{
            .request_height = self.logical_height,
            .ascent = ascent,
            .descent = descent,
            .external_leading = external_leading,
            .leading = leading,
            .base_add = base_add,
            // `balloon.cpp:618-621` tests the requested leading, not the value
            // after adding tmExternalLeading.
            .top_offset = if (requested_leading != 0) 0 else 50,
            .line_height = ascent + descent + leading,
        };
    }

    pub fn fontInfo(self: AtlasMetrics) balloon.FontInfo {
        const vertical = self.verticalMetrics();
        return .{
            .line_height = vertical.line_height,
            .base_add = vertical.base_add,
            .top_offset = vertical.top_offset,
        };
    }

    pub fn textMeasurer(self: *const AtlasMetrics) balloon.TextMeasurer {
        return .{
            .context = self,
            .measure_fn = measureText,
            .measure_formatted_fn = measureFormattedText,
        };
    }

    fn measureText(raw_context: *const anyopaque, text: []const u8) balloon.Size {
        const self: *const AtlasMetrics = @ptrCast(@alignCast(raw_context));
        const native_width = Canvas.textWidthStyled(text, self.style);
        return .{
            .width = sourceRound(@as(f64, @floatFromInt(native_width)) *
                @as(f64, @floatFromInt(self.logical_height)) /
                @as(f64, @floatFromInt(self.nativeScaleDenominator()))),
            .height = if (self.height_basis == .source_request)
                self.verticalMetrics().ascent + self.verticalMetrics().descent
            else
                self.logical_height,
        };
    }

    fn measureFormattedText(
        raw_context: *const anyopaque,
        text: []const u8,
        changes: []const formatting.Change,
        source_offset: usize,
    ) balloon.Size {
        const self: *const AtlasMetrics = @ptrCast(@alignCast(raw_context));
        var width: i32 = 0;
        var cursor = source_offset;
        const end = source_offset + text.len;
        while (cursor < end) {
            const next = nextFormatOffset(changes, cursor, end);
            const state = formatting.formatAt(changes, cursor);
            const style = inlineStyle(self.style, state);
            const native_width = Canvas.textWidthStyled(
                text[cursor - source_offset .. next - source_offset],
                style,
            );
            var styled_metrics = self.*;
            styled_metrics.style = style;
            width += sourceRound(@as(f64, @floatFromInt(native_width)) *
                @as(f64, @floatFromInt(self.logical_height)) /
                @as(f64, @floatFromInt(styled_metrics.nativeScaleDenominator())));
            cursor = next;
        }
        return .{
            .width = width,
            .height = if (self.height_basis == .source_request)
                self.verticalMetrics().ascent + self.verticalMetrics().descent
            else
                self.logical_height,
        };
    }

    fn scaleNativeMetric(self: AtlasMetrics, native_value: i32) i32 {
        return sourceRound(@as(f64, @floatFromInt(native_value)) *
            @as(f64, @floatFromInt(self.logical_height)) /
            @as(f64, @floatFromInt(nativeRequestSize(self.style))));
    }

    fn nativeScaleDenominator(self: AtlasMetrics) i32 {
        if (self.height_basis == .source_request) return nativeRequestSize(self.style);
        return nativeLineHeight(self.style);
    }

    fn nativeRequestSize(style: canvas_mod.FontStyle) i32 {
        return switch (style) {
            .normal => font.request_size,
            .whisper => whisper_font.request_size,
        };
    }

    fn nativeAscent(style: canvas_mod.FontStyle) i32 {
        return switch (style) {
            .normal => font.ascent,
            .whisper => whisper_font.ascent,
        };
    }

    fn nativeDescent(style: canvas_mod.FontStyle) i32 {
        return switch (style) {
            .normal => font.descent,
            .whisper => whisper_font.descent,
        };
    }

    fn nativeExternalLeading(style: canvas_mod.FontStyle) i32 {
        return switch (style) {
            .normal => font.external_leading,
            .whisper => whisper_font.external_leading,
        };
    }

    fn nativeLineHeight(style: canvas_mod.FontStyle) i32 {
        return switch (style) {
            .normal => font.line_height,
            .whisper => whisper_font.line_height,
        };
    }
};

pub const default_atlas_metrics = AtlasMetrics{};

fn nextFormatOffset(changes: []const formatting.Change, cursor: usize, end: usize) usize {
    for (changes) |change| {
        if (change.offset > cursor) return @min(change.offset, end);
    }
    return end;
}

fn inlineStyle(base: canvas_mod.FontStyle, format_state: u16) canvas_mod.FontStyle {
    // The source starts from the balloon's selected LOGFONT. Setting italic
    // therefore selects Bold Italic, while a whisper remains italic whether
    // or not the inline bit is present. Bold is already the base face.
    if (base == .whisper or format_state & formatting.effect.italic != 0) return .whisper;
    return .normal;
}

fn transparentFormat(format_state: u16) bool {
    return format_state & formatting.effect.foreground != 0 and
        format_state & formatting.effect.background != 0 and
        @as(u4, @truncate(format_state >> 4)) == @as(u4, @truncate(format_state));
}

pub const DeviceRect = struct {
    x: i32,
    y: i32,
    width: i32,
    height: i32,
};

const DPoint = struct { x: f64, y: f64 };

/// Affine conversion from the original logical y-up panel to a top-down
/// framebuffer viewport.  The source and target may be partially outside the
/// canvas; every raster operation intersects both the viewport and framebuffer.
pub const Transform = struct {
    source: balloon.Rect,
    target: DeviceRect,

    pub fn init(source: balloon.Rect, target: DeviceRect) error{InvalidTransform}!Transform {
        if (source.width() <= 0 or source.height() <= 0 or target.width <= 0 or target.height <= 0)
            return error.InvalidTransform;
        return .{ .source = source, .target = target };
    }

    /// Canonical one-panel conversion used by the current 315px strip.
    pub fn panel315() Transform {
        return .{
            .source = .{ .left = 0, .top = 0, .right = source_panel_size, .bottom = -source_panel_size },
            .target = .{ .x = 0, .y = 0, .width = 315, .height = 315 },
        };
    }

    pub fn map(self: Transform, point: balloon.Point) struct { x: i32, y: i32 } {
        const mapped = self.mapD(.{ .x = @floatFromInt(point.x), .y = @floatFromInt(point.y) });
        return .{ .x = sourceRound(mapped.x), .y = sourceRound(mapped.y) };
    }

    fn mapD(self: Transform, point: DPoint) DPoint {
        const sx = @as(f64, @floatFromInt(self.target.width)) /
            @as(f64, @floatFromInt(self.source.width()));
        const sy = @as(f64, @floatFromInt(self.target.height)) /
            @as(f64, @floatFromInt(self.source.height()));
        return .{
            .x = @as(f64, @floatFromInt(self.target.x)) +
                (point.x - @as(f64, @floatFromInt(self.source.left))) * sx,
            .y = @as(f64, @floatFromInt(self.target.y)) +
                (@as(f64, @floatFromInt(self.source.top)) - point.y) * sy,
        };
    }

    fn penScale(self: Transform) f64 {
        const sx = @as(f64, @floatFromInt(self.target.width)) /
            @as(f64, @floatFromInt(self.source.width()));
        const sy = @as(f64, @floatFromInt(self.target.height)) /
            @as(f64, @floatFromInt(self.source.height()));
        return (@abs(sx) + @abs(sy)) / 2.0;
    }
};

pub const Error = error{InvalidTransform} || std.mem.Allocator.Error;

const Clip = struct { left: i32, top: i32, right: i32, bottom: i32 };

fn clipFor(canvas: *const Canvas, transform: Transform) Clip {
    const target_right = transform.target.x + transform.target.width;
    const target_bottom = transform.target.y + transform.target.height;
    return .{
        .left = @max(0, transform.target.x),
        .top = @max(0, transform.target.y),
        .right = @min(@as(i32, @intCast(canvas.width)), target_right),
        .bottom = @min(@as(i32, @intCast(canvas.height)), target_bottom),
    };
}

fn clippedBlend(canvas: *Canvas, clip: Clip, x: i32, y: i32, color: Color, coverage: u32) void {
    if (x < clip.left or x >= clip.right or y < clip.top or y >= clip.bottom) return;
    canvas.blendPixel(x, y, color, coverage);
}

fn sourceRound(value: f64) i32 {
    // `vector2d.h:46-52`: halves are rounded away from zero.
    return @intFromFloat(if (value > 0) value + 0.5 else value - 0.5);
}

fn toD(point: balloon.Point) DPoint {
    return .{ .x = @floatFromInt(point.x), .y = @floatFromInt(point.y) };
}

fn addOrigin(point: DPoint, origin: balloon.Point) DPoint {
    return .{ .x = point.x + @as(f64, @floatFromInt(origin.x)), .y = point.y + @as(f64, @floatFromInt(origin.y)) };
}

fn appendPoint(list: *std.ArrayList(DPoint), allocator: std.mem.Allocator, point: DPoint) !void {
    if (list.items.len != 0) {
        const previous = list.items[list.items.len - 1];
        if (previous.x == point.x and previous.y == point.y) return;
    }
    try list.append(allocator, point);
}

const DBezier = struct { p0: DPoint, p1: DPoint, p2: DPoint, p3: DPoint };

fn midpoint(a: DPoint, b: DPoint) DPoint {
    return .{ .x = (a.x + b.x) * 0.5, .y = (a.y + b.y) * 0.5 };
}

fn splitBezier(curve: DBezier) struct { left: DBezier, right: DBezier } {
    const p01 = midpoint(curve.p0, curve.p1);
    const p12 = midpoint(curve.p1, curve.p2);
    const p23 = midpoint(curve.p2, curve.p3);
    const p012 = midpoint(p01, p12);
    const p123 = midpoint(p12, p23);
    const center = midpoint(p012, p123);
    return .{
        .left = .{ .p0 = curve.p0, .p1 = p01, .p2 = p012, .p3 = center },
        .right = .{ .p0 = center, .p1 = p123, .p2 = p23, .p3 = curve.p3 },
    };
}

/// Literal epsilon-1 predicate from `splinutl.cpp:54-86`.
fn flatBezier(curve: DBezier) bool {
    const xmin = @min(curve.p0.x, curve.p3.x);
    const xmax = @max(curve.p0.x, curve.p3.x);
    const ymin = @min(curve.p0.y, curve.p3.y);
    const ymax = @max(curve.p0.y, curve.p3.y);
    for ([_]DPoint{ curve.p1, curve.p2 }) |point| {
        if (point.x + 0.5 < xmin or point.x - 0.5 > xmax or
            point.y + 0.5 < ymin or point.y - 0.5 > ymax) return false;
    }
    const d1 = DPoint{ .x = curve.p1.x - curve.p0.x, .y = curve.p1.y - curve.p0.y };
    const d2 = DPoint{ .x = curve.p2.x - curve.p0.x, .y = curve.p2.y - curve.p0.y };
    const d = DPoint{ .x = curve.p3.x - curve.p0.x, .y = curve.p3.y - curve.p0.y };
    const dx = @abs(d.x);
    const dy = @abs(d.y);
    if (dx + dy < 1.0) return true;
    if (dy < dx) {
        const slope = d.y / d.x;
        return @abs(d2.y - d2.x * slope) < 1.0 and @abs(d1.y - d1.x * slope) < 1.0;
    }
    const slope = d.x / d.y;
    return @abs(d2.x - d2.y * slope) < 1.0 and @abs(d1.x - d1.y * slope) < 1.0;
}

fn flattenBezier(
    list: *std.ArrayList(DPoint),
    allocator: std.mem.Allocator,
    curve: DBezier,
    depth: u8,
) !void {
    if (flatBezier(curve) or depth == 32) {
        try appendPoint(list, allocator, curve.p3);
        return;
    }
    const halves = splitBezier(curve);
    try flattenBezier(list, allocator, halves.left, depth + 1);
    try flattenBezier(list, allocator, halves.right, depth + 1);
}

fn truncScaled(point: balloon.Point, scalar: f64) balloon.Point {
    // The POINT overload of `point_scalmult` truncates (`vector2d.cpp:99-104`).
    return .{
        .x = @intFromFloat(@as(f64, @floatFromInt(point.x)) * scalar),
        .y = @intFromFloat(@as(f64, @floatFromInt(point.y)) * scalar),
    };
}

fn arcCenter(arc: balloon.Arc) ?balloon.Point {
    if (arc.altitude == 0) return null;
    const sum = balloon.Point{ .x = arc.start.x + arc.end.x, .y = arc.start.y + arc.end.y };
    const mid = truncScaled(sum, 0.5);
    const end_to_mid = balloon.Point{ .x = mid.x - arc.end.x, .y = mid.y - arc.end.y };
    const half_chord = @sqrt(
        @as(f64, @floatFromInt(end_to_mid.x * end_to_mid.x + end_to_mid.y * end_to_mid.y)),
    );
    if (half_chord <= 1.0e-24) return null;
    const altitude: f64 = @floatFromInt(arc.altitude);
    const radius = (half_chord * half_chord + altitude * altitude) / (2.0 * altitude);
    const center_distance = radius - altitude;
    const perpendicular = balloon.Point{ .x = end_to_mid.y, .y = -end_to_mid.x };
    const center_delta = truncScaled(perpendicular, center_distance / half_chord);
    return .{ .x = arc.end.x + end_to_mid.x + center_delta.x, .y = arc.end.y + end_to_mid.y + center_delta.y };
}

/// Append the exact circle selected by `DrawArc2`; 0.02 radians is the
/// original `DashArc2` maximum step and is finer than a device pixel here.
fn appendArc(
    list: *std.ArrayList(DPoint),
    allocator: std.mem.Allocator,
    arc: balloon.Arc,
) !void {
    const center_i = arcCenter(arc) orelse {
        try appendPoint(list, allocator, toD(arc.end));
        return;
    };
    const center = toD(center_i);
    const start = toD(arc.start);
    const finish = toD(arc.end);
    const radius = @sqrt((start.x - center.x) * (start.x - center.x) +
        (start.y - center.y) * (start.y - center.y));
    if (radius <= 1.0e-24) {
        try appendPoint(list, allocator, finish);
        return;
    }
    const start_angle = std.math.atan2(start.y - center.y, start.x - center.x);
    const end_angle = std.math.atan2(finish.y - center.y, finish.x - center.x);
    var sweep = if (arc.altitude > 0) end_angle - start_angle else start_angle - end_angle;
    while (sweep <= 0) sweep += 2.0 * std.math.pi;
    const count: usize = @max(1, @as(usize, @intFromFloat(@ceil(sweep / 0.02))));
    var index: usize = 1;
    while (index < count) : (index += 1) {
        const fraction = @as(f64, @floatFromInt(index)) / @as(f64, @floatFromInt(count));
        const angle = if (arc.altitude > 0)
            start_angle + sweep * fraction
        else
            start_angle - sweep * fraction;
        try appendPoint(list, allocator, .{
            .x = center.x + radius * @cos(angle),
            .y = center.y + radius * @sin(angle),
        });
    }
    try appendPoint(list, allocator, finish);
}

fn buildLogicalPath(
    allocator: std.mem.Allocator,
    geometry: *const balloon.BalloonGeometry,
) !std.ArrayList(DPoint) {
    var path: std.ArrayList(DPoint) = .empty;
    errdefer path.deinit(allocator);

    if (geometry.outline_beziers.len != 0) {
        try appendPoint(&path, allocator, toD(geometry.outline_beziers[0].p0));
        for (geometry.outline_beziers) |bezier| {
            try flattenBezier(&path, allocator, .{
                .p0 = toD(bezier.p0),
                .p1 = toD(bezier.p1),
                .p2 = toD(bezier.p2),
                .p3 = toD(bezier.p3),
            }, 0);
        }
    } else {
        for (geometry.outline_points) |point| try appendPoint(&path, allocator, toD(point));
    }

    if (geometry.tail) |tail| {
        try appendArc(&path, allocator, tail.first_arc);
        try appendArc(&path, allocator, tail.second_arc);
    }
    if (path.items.len != 0) try appendPoint(&path, allocator, path.items[0]);

    for (path.items) |*point| point.* = addOrigin(point.*, geometry.origin);
    return path;
}

fn mapPath(
    allocator: std.mem.Allocator,
    logical: []const DPoint,
    transform: Transform,
) !std.ArrayList(DPoint) {
    var device: std.ArrayList(DPoint) = .empty;
    errdefer device.deinit(allocator);
    try device.ensureTotalCapacity(allocator, logical.len);
    for (logical) |point| device.appendAssumeCapacity(transform.mapD(point));
    return device;
}

fn fillPath(
    allocator: std.mem.Allocator,
    canvas: *Canvas,
    clip: Clip,
    points: []const DPoint,
    color: Color,
) !void {
    if (points.len < 3 or clip.left >= clip.right or clip.top >= clip.bottom) return;
    var min_y = points[0].y;
    var max_y = points[0].y;
    for (points[1..]) |point| {
        min_y = @min(min_y, point.y);
        max_y = @max(max_y, point.y);
    }
    const first_y = @max(clip.top, @as(i32, @intFromFloat(@floor(min_y))));
    const last_y = @min(clip.bottom - 1, @as(i32, @intFromFloat(@ceil(max_y))));
    if (first_y > last_y) return;

    var intersections: std.ArrayList(f64) = .empty;
    defer intersections.deinit(allocator);
    try intersections.ensureTotalCapacity(allocator, points.len);

    var y = first_y;
    while (y <= last_y) : (y += 1) {
        intersections.clearRetainingCapacity();
        const scan_y = @as(f64, @floatFromInt(y)) + 0.5;
        for (0..points.len - 1) |index| {
            const a = points[index];
            const b = points[index + 1];
            if (!((a.y <= scan_y and b.y > scan_y) or (b.y <= scan_y and a.y > scan_y))) continue;
            const x = a.x + (scan_y - a.y) * (b.x - a.x) / (b.y - a.y);
            intersections.appendAssumeCapacity(x);
        }
        std.mem.sort(f64, intersections.items, {}, std.sort.asc(f64));
        var pair: usize = 0;
        while (pair + 1 < intersections.items.len) : (pair += 2) {
            var x = @max(clip.left, @as(i32, @intFromFloat(@ceil(intersections.items[pair] - 0.5))));
            const end = @min(clip.right - 1, @as(i32, @intFromFloat(@floor(intersections.items[pair + 1] - 0.5))));
            while (x <= end) : (x += 1) clippedBlend(canvas, clip, x, y, color, 255);
        }
    }
}

fn pointSegmentDistance(px: f64, py: f64, a: DPoint, b: DPoint) f64 {
    const dx = b.x - a.x;
    const dy = b.y - a.y;
    const length_sq = dx * dx + dy * dy;
    if (length_sq <= 1.0e-24) return @sqrt((px - a.x) * (px - a.x) + (py - a.y) * (py - a.y));
    const fraction = std.math.clamp(((px - a.x) * dx + (py - a.y) * dy) / length_sq, 0.0, 1.0);
    const nearest_x = a.x + fraction * dx;
    const nearest_y = a.y + fraction * dy;
    return @sqrt((px - nearest_x) * (px - nearest_x) + (py - nearest_y) * (py - nearest_y));
}

fn strokeSegment(canvas: *Canvas, clip: Clip, a: DPoint, b: DPoint, width: f64, color: Color) void {
    if (width <= 0) return;
    const half = width / 2.0;
    const left = @max(clip.left, @as(i32, @intFromFloat(@floor(@min(a.x, b.x) - half - 1.0))));
    const right = @min(clip.right - 1, @as(i32, @intFromFloat(@ceil(@max(a.x, b.x) + half + 1.0))));
    const top = @max(clip.top, @as(i32, @intFromFloat(@floor(@min(a.y, b.y) - half - 1.0))));
    const bottom = @min(clip.bottom - 1, @as(i32, @intFromFloat(@ceil(@max(a.y, b.y) + half + 1.0))));
    if (left > right or top > bottom) return;
    var y = top;
    while (y <= bottom) : (y += 1) {
        var x = left;
        while (x <= right) : (x += 1) {
            const distance = pointSegmentDistance(
                @as(f64, @floatFromInt(x)) + 0.5,
                @as(f64, @floatFromInt(y)) + 0.5,
                a,
                b,
            );
            const coverage = std.math.clamp(half + 0.5 - distance, 0.0, 1.0);
            if (coverage > 0) clippedBlend(canvas, clip, x, y, color, @intFromFloat(coverage * 255.0));
        }
    }
}

fn strokePath(canvas: *Canvas, clip: Clip, points: []const DPoint, width: f64, color: Color) void {
    if (points.len < 2) return;
    for (0..points.len - 1) |index| strokeSegment(canvas, clip, points[index], points[index + 1], width, color);
}

const IPoint = struct { x: i32, y: i32 };

fn roundLogical(point: DPoint) IPoint {
    return .{ .x = sourceRound(point.x), .y = sourceRound(point.y) };
}

fn mapIPoint(transform: Transform, point: IPoint) DPoint {
    return transform.mapD(.{ .x = @floatFromInt(point.x), .y = @floatFromInt(point.y) });
}

/// Literal `DashSeg` progression (`traj.cpp:8-31`): distance is Manhattan,
/// interpolation is normalized by that same distance, and equality toggles.
fn dashPath(canvas: *Canvas, clip: Clip, logical: []const DPoint, transform: Transform, width: f64) void {
    if (logical.len < 2) return;
    var last = roundLogical(logical[0]);
    var in_dash = true;
    var partial_distance: i32 = 0;
    for (logical[1..]) |raw_point| {
        const this_point = roundLogical(raw_point);
        while (true) {
            const next_distance: i32 = @intCast(@abs(this_point.x - last.x) + @abs(this_point.y - last.y));
            if (next_distance + partial_distance < source_dash_twips) {
                partial_distance += next_distance;
                if (in_dash) strokeSegment(canvas, clip, mapIPoint(transform, last), mapIPoint(transform, this_point), width, canvas_mod.black);
                last = this_point;
                break;
            }
            if (next_distance == 0) {
                in_dash = !in_dash;
                partial_distance = 0;
                break;
            }
            const distance_left = source_dash_twips - partial_distance;
            const intermediate = IPoint{
                .x = last.x + sourceRound(@as(f64, @floatFromInt(distance_left * (this_point.x - last.x))) /
                    @as(f64, @floatFromInt(next_distance))),
                .y = last.y + sourceRound(@as(f64, @floatFromInt(distance_left * (this_point.y - last.y))) /
                    @as(f64, @floatFromInt(next_distance))),
            };
            if (in_dash) strokeSegment(canvas, clip, mapIPoint(transform, last), mapIPoint(transform, intermediate), width, canvas_mod.black);
            last = intermediate;
            in_dash = !in_dash;
            partial_distance = 0;
        }
    }
}

fn ellipsePath(
    allocator: std.mem.Allocator,
    rect: balloon.Rect,
    transform: Transform,
) !std.ArrayList(DPoint) {
    var result: std.ArrayList(DPoint) = .empty;
    errdefer result.deinit(allocator);
    const cx = @as(f64, @floatFromInt(rect.left + rect.right)) / 2.0;
    const cy = @as(f64, @floatFromInt(rect.top + rect.bottom)) / 2.0;
    const rx = @as(f64, @floatFromInt(rect.right - rect.left)) / 2.0;
    const ry = @as(f64, @floatFromInt(rect.top - rect.bottom)) / 2.0;
    const count: usize = 96;
    try result.ensureTotalCapacity(allocator, count + 1);
    for (0..count + 1) |index| {
        const angle = 2.0 * std.math.pi * @as(f64, @floatFromInt(index)) / @as(f64, @floatFromInt(count));
        result.appendAssumeCapacity(transform.mapD(.{ .x = cx + rx * @cos(angle), .y = cy + ry * @sin(angle) }));
    }
    return result;
}

fn atlasCoverage(comptime atlas: type, glyph: atlas.Glyph, x: f64, y: f64) f64 {
    const x0: i32 = @intFromFloat(@floor(x));
    const y0: i32 = @intFromFloat(@floor(y));
    const fx = x - @as(f64, @floatFromInt(x0));
    const fy = y - @as(f64, @floatFromInt(y0));
    var result: f64 = 0;
    const xs = [_]i32{ x0, x0 + 1, x0, x0 + 1 };
    const ys = [_]i32{ y0, y0, y0 + 1, y0 + 1 };
    const weights = [_]f64{ (1.0 - fx) * (1.0 - fy), fx * (1.0 - fy), (1.0 - fx) * fy, fx * fy };
    for (0..4) |index| {
        if (xs[index] < 0 or ys[index] < 0 or xs[index] >= glyph.w or ys[index] >= glyph.h) continue;
        const offset = glyph.off + @as(u32, @intCast(ys[index])) * glyph.w + @as(u32, @intCast(xs[index]));
        result += @as(f64, @floatFromInt(atlas.coverage[offset])) * weights[index];
    }
    return result;
}

fn drawScaledGlyph(
    comptime atlas: type,
    canvas: *Canvas,
    clip: Clip,
    transform: Transform,
    line_x: i32,
    line_y: i32,
    native_pen: i32,
    glyph: atlas.Glyph,
    metrics: AtlasMetrics,
    color: Color,
) void {
    if (glyph.w == 0 or glyph.h == 0) return;
    const logical_per_native = @as(f64, @floatFromInt(metrics.logical_height)) /
        @as(f64, @floatFromInt(metrics.nativeScaleDenominator()));
    const logical_left = @as(f64, @floatFromInt(line_x)) +
        @as(f64, @floatFromInt(native_pen + glyph.xoff)) * logical_per_native;
    const logical_right = logical_left + @as(f64, @floatFromInt(glyph.w)) * logical_per_native;
    const logical_top = @as(f64, @floatFromInt(line_y)) -
        @as(f64, @floatFromInt(glyph.yoff)) * logical_per_native;
    const logical_bottom = logical_top - @as(f64, @floatFromInt(glyph.h)) * logical_per_native;
    const top_left = transform.mapD(.{ .x = logical_left, .y = logical_top });
    const bottom_right = transform.mapD(.{ .x = logical_right, .y = logical_bottom });
    const left_f = @min(top_left.x, bottom_right.x);
    const right_f = @max(top_left.x, bottom_right.x);
    const top_f = @min(top_left.y, bottom_right.y);
    const bottom_f = @max(top_left.y, bottom_right.y);
    if (right_f <= left_f or bottom_f <= top_f) return;
    const left = @max(clip.left, @as(i32, @intFromFloat(@floor(left_f))));
    const right = @min(clip.right, @as(i32, @intFromFloat(@ceil(right_f))));
    const top = @max(clip.top, @as(i32, @intFromFloat(@floor(top_f))));
    const bottom = @min(clip.bottom, @as(i32, @intFromFloat(@ceil(bottom_f))));
    var y = top;
    while (y < bottom) : (y += 1) {
        const source_y = (@as(f64, @floatFromInt(y)) + 0.5 - top_f) /
            (bottom_f - top_f) * @as(f64, @floatFromInt(glyph.h)) - 0.5;
        var x = left;
        while (x < right) : (x += 1) {
            const source_x = (@as(f64, @floatFromInt(x)) + 0.5 - left_f) /
                (right_f - left_f) * @as(f64, @floatFromInt(glyph.w)) - 0.5;
            const coverage = atlasCoverage(atlas, glyph, source_x, source_y);
            if (coverage > 0) clippedBlend(canvas, clip, x, y, color, @intFromFloat(@min(255.0, coverage)));
        }
    }
}

fn drawText(
    canvas: *Canvas,
    geometry: *const balloon.BalloonGeometry,
    transform: Transform,
    clip: Clip,
    metrics: AtlasMetrics,
) void {
    var text_metrics = metrics;
    // `CBWoodringWhisper` alone resets `m_fontI` to the italic face. The
    // `BM_ACTION|BM_WHISPER` constructor creates a `CBWoodringBox`, so its
    // dashed action box deliberately retains the normal face.
    text_metrics.style = if (geometry.kind == .whisper) .whisper else .normal;
    for (geometry.lines) |line| {
        drawFormattedTextLine(canvas, geometry, line, transform, clip, text_metrics);
    }
}

fn formatColor(format_state: u16) Color {
    if (format_state & formatting.effect.link != 0) return 0xff0000ff;
    if (format_state & formatting.effect.foreground == 0) return canvas_mod.black;
    const rgb = formatting.palette(@truncate(format_state >> 4));
    // `iDrawFormattedTextLine` retains the default black when foreground is
    // COLOR_WINDOW (white), except for the same-FG/BG transparent branch.
    if (rgb.r == 255 and rgb.g == 255 and rgb.b == 255) return canvas_mod.black;
    return 0xff000000 | @as(u32, rgb.r) << 16 | @as(u32, rgb.g) << 8 | rgb.b;
}

fn logicalTextWidth(text: []const u8, metrics: AtlasMetrics) i32 {
    return sourceRound(@as(f64, @floatFromInt(Canvas.textWidthStyled(text, metrics.style))) *
        @as(f64, @floatFromInt(metrics.logical_height)) /
        @as(f64, @floatFromInt(metrics.nativeScaleDenominator())));
}

fn drawFormattedTextLine(
    canvas: *Canvas,
    geometry: *const balloon.BalloonGeometry,
    line: balloon.TextLine,
    transform: Transform,
    clip: Clip,
    base_metrics: AtlasMetrics,
) void {
    var cursor = line.start;
    const end = line.start + line.len;
    var logical_pen: i32 = 0;
    while (cursor < end) {
        const next = nextFormatOffset(geometry.formatting, cursor, end);
        const state = formatting.formatAt(geometry.formatting, cursor);
        var metrics = base_metrics;
        metrics.style = inlineStyle(base_metrics.style, state);
        const chunk = geometry.text[cursor..next];
        const width = logicalTextWidth(chunk, metrics);
        if (!transparentFormat(state)) {
            const color = formatColor(state);
            drawAtlasTextColor(
                canvas,
                chunk,
                line.x + logical_pen,
                line.y,
                transform,
                metrics,
                color,
            );
            if (state & formatting.effect.underline != 0 and width > 0) {
                const underline_y = line.y - metrics.verticalMetrics().ascent;
                const from = transform.map(.{ .x = line.x + logical_pen, .y = underline_y });
                const to = transform.map(.{ .x = line.x + logical_pen + width, .y = underline_y });
                if (from.y >= clip.top and from.y < clip.bottom)
                    canvas.drawLine(from.x, from.y, to.x, to.y, color);
            }
        }
        logical_pen += width;
        cursor = next;
    }
}

fn drawAtlasTextFor(
    comptime atlas: type,
    canvas: *Canvas,
    text: []const u8,
    logical_x: i32,
    logical_y: i32,
    transform: Transform,
    metrics: AtlasMetrics,
    color: Color,
) void {
    const clip = clipFor(canvas, transform);
    const position = transform.map(.{ .x = logical_x, .y = logical_y });
    if (position.x >= clip.right or position.y >= clip.bottom) return;
    var pen: i32 = 0;
    for (text) |character| {
        if (character < atlas.first or character >= atlas.first + atlas.count) continue;
        const glyph = atlas.glyphs[character - atlas.first];
        drawScaledGlyph(atlas, canvas, clip, transform, logical_x, logical_y, pen, glyph, metrics, color);
        pen += glyph.advance;
    }
}

/// Draw one already-positioned source text line using the same logical metric
/// scale as balloon layout. Title and participant labels use this entry point
/// with their role-specific LOGFONT heights.
pub fn drawAtlasText(
    canvas: *Canvas,
    text: []const u8,
    logical_x: i32,
    logical_y: i32,
    transform: Transform,
    metrics: AtlasMetrics,
) void {
    drawAtlasTextColor(canvas, text, logical_x, logical_y, transform, metrics, canvas_mod.black);
}

fn drawAtlasTextColor(
    canvas: *Canvas,
    text: []const u8,
    logical_x: i32,
    logical_y: i32,
    transform: Transform,
    metrics: AtlasMetrics,
    color: Color,
) void {
    switch (metrics.style) {
        .normal => drawAtlasTextFor(font, canvas, text, logical_x, logical_y, transform, metrics, color),
        .whisper => drawAtlasTextFor(whisper_font, canvas, text, logical_x, logical_y, transform, metrics, color),
    }
}

/// Draw one source-derived balloon. Geometry is filled before stroking, as in
/// `StrokeAndFillPath`; text follows the cloud, then thought-tail ellipses.
pub fn drawGeometry(
    allocator: std.mem.Allocator,
    canvas: *Canvas,
    geometry: *const balloon.BalloonGeometry,
    transform: Transform,
) Error!void {
    return drawGeometryWithMetrics(allocator, canvas, geometry, transform, default_atlas_metrics);
}

pub fn drawGeometryWithMetrics(
    allocator: std.mem.Allocator,
    canvas: *Canvas,
    geometry: *const balloon.BalloonGeometry,
    transform: Transform,
    metrics: AtlasMetrics,
) Error!void {
    _ = try Transform.init(transform.source, transform.target);
    if (metrics.logical_height <= 0) return error.InvalidTransform;
    const clip = clipFor(canvas, transform);
    if (clip.left >= clip.right or clip.top >= clip.bottom) return;

    var logical = try buildLogicalPath(allocator, geometry);
    defer logical.deinit(allocator);
    var device = try mapPath(allocator, logical.items, transform);
    defer device.deinit(allocator);

    try fillPath(allocator, canvas, clip, device.items, canvas_mod.white);
    const black_width = @max(1.0, source_pen_twips * transform.penScale());
    if (geometry.dashed) {
        // Source first strokes/fills with its white 100-TWIP nimbus, then
        // selects the normal black pen and dashes the same complete trajectory.
        strokePath(canvas, clip, device.items, @max(1.0, source_nimbus_twips * transform.penScale()), canvas_mod.white);
        dashPath(canvas, clip, logical.items, transform, black_width);
    } else {
        strokePath(canvas, clip, device.items, black_width, canvas_mod.black);
    }

    drawText(canvas, geometry, transform, clip, metrics);

    for (geometry.thought_bubbles) |bubble_rect| {
        var ellipse = try ellipsePath(allocator, bubble_rect, transform);
        defer ellipse.deinit(allocator);
        try fillPath(allocator, canvas, clip, ellipse.items, canvas_mod.white);
        strokePath(canvas, clip, ellipse.items, black_width, canvas_mod.black);
    }
}

pub fn drawBalloon(
    allocator: std.mem.Allocator,
    canvas: *Canvas,
    geometry: *const balloon.BalloonGeometry,
    transform: Transform,
) Error!void {
    return drawGeometry(allocator, canvas, geometry, transform);
}

/// `CUnitPanel::Draw` traverses panel elements tail-to-head. The layout slice
/// is head-to-tail, so render in reverse to retain the released overlap order.
pub fn drawPanelBalloons(
    allocator: std.mem.Allocator,
    canvas: *Canvas,
    geometries: []const balloon.BalloonGeometry,
    transform: Transform,
) Error!void {
    var index = geometries.len;
    while (index != 0) {
        index -= 1;
        try drawGeometry(allocator, canvas, &geometries[index], transform);
    }
}

fn channel(color: Color, shift: u5) f64 {
    return @floatFromInt((color >> shift) & 0xff);
}

fn bilinearPremultiplied(pixels: []const Color, width: u32, height: u32, x: f64, y: f64) struct { rgb: Color, alpha: u32 } {
    const x0_i: i32 = @intFromFloat(@floor(x));
    const y0_i: i32 = @intFromFloat(@floor(y));
    const x1_i = x0_i + 1;
    const y1_i = y0_i + 1;
    const fx = x - @as(f64, @floatFromInt(x0_i));
    const fy = y - @as(f64, @floatFromInt(y0_i));
    const xs = [_]i32{ x0_i, x1_i, x0_i, x1_i };
    const ys = [_]i32{ y0_i, y0_i, y1_i, y1_i };
    const weights = [_]f64{ (1.0 - fx) * (1.0 - fy), fx * (1.0 - fy), (1.0 - fx) * fy, fx * fy };
    var alpha: f64 = 0;
    var red: f64 = 0;
    var green: f64 = 0;
    var blue: f64 = 0;
    for (0..4) |index| {
        const sx: u32 = @intCast(std.math.clamp(xs[index], 0, @as(i32, @intCast(width)) - 1));
        const sy: u32 = @intCast(std.math.clamp(ys[index], 0, @as(i32, @intCast(height)) - 1));
        const pixel = pixels[@as(usize, sy) * width + sx];
        const a = channel(pixel, 24) / 255.0;
        const weight = weights[index];
        alpha += a * weight;
        red += channel(pixel, 16) * a * weight;
        green += channel(pixel, 8) * a * weight;
        blue += channel(pixel, 0) * a * weight;
    }
    if (alpha <= 1.0e-12) return .{ .rgb = 0, .alpha = 0 };
    const r: u32 = @intFromFloat(std.math.clamp(red / alpha, 0.0, 255.0));
    const g: u32 = @intFromFloat(std.math.clamp(green / alpha, 0.0, 255.0));
    const b: u32 = @intFromFloat(std.math.clamp(blue / alpha, 0.0, 255.0));
    return .{
        .rgb = 0xff000000 | (r << 16) | (g << 8) | b,
        .alpha = @intFromFloat(std.math.clamp(alpha * 255.0, 0.0, 255.0)),
    };
}

/// Pixel-edge source crop. `(0,0,width,height)` selects the whole image. This
/// matches the backdrop source rectangle produced by `AdjustArtToCoord`.
pub const ImageRegion = struct {
    left: f64,
    top: f64,
    right: f64,
    bottom: f64,

    pub fn full(width: u32, height: u32) ImageRegion {
        return .{ .left = 0, .top = 0, .right = @floatFromInt(width), .bottom = @floatFromInt(height) };
    }

    pub fn normalized(left: f64, top: f64, right: f64, bottom: f64, width: u32, height: u32) ImageRegion {
        return .{
            .left = left * @as(f64, @floatFromInt(width)),
            .top = top * @as(f64, @floatFromInt(height)),
            .right = right * @as(f64, @floatFromInt(width)),
            .bottom = bottom * @as(f64, @floatFromInt(height)),
        };
    }

    /// Convert `CBackDrop::m_bbox` exactly like `backdrop.cpp:339-350`.
    /// Both rectangles use y-up panel coordinates; the resulting crop uses
    /// top-down source pixel edges.
    pub fn fromBackdropBBox(bbox: balloon.Rect, panel: balloon.Rect, width: u32, height: u32) ImageRegion {
        const panel_width: f64 = @floatFromInt(panel.right - panel.left);
        const panel_height_signed: f64 = @floatFromInt(panel.bottom - panel.top);
        return .{
            .left = @floatFromInt(sourceRound(@as(f64, @floatFromInt(bbox.left - panel.left)) / panel_width * @as(f64, @floatFromInt(width)))),
            .top = @floatFromInt(sourceRound(@as(f64, @floatFromInt(bbox.top - panel.top)) / panel_height_signed * @as(f64, @floatFromInt(height)))),
            .right = @floatFromInt(sourceRound(@as(f64, @floatFromInt(bbox.right - panel.left)) / panel_width * @as(f64, @floatFromInt(width)))),
            .bottom = @floatFromInt(sourceRound(@as(f64, @floatFromInt(bbox.bottom - panel.top)) / panel_height_signed * @as(f64, @floatFromInt(height)))),
        };
    }
};

/// Clipped, alpha-correct software equivalent of the old `STRETCH_HALFTONE`
/// bitmap path. Source pixels are top-down ARGB. `flip_x` mirrors avatar bodies
/// without requiring a second decoded asset.
pub fn blitImage(
    canvas: *Canvas,
    pixels: []const Color,
    source_width: u32,
    source_height: u32,
    logical_destination: balloon.Rect,
    transform: Transform,
    flip_x: bool,
) void {
    return blitImageRegion(
        canvas,
        pixels,
        source_width,
        source_height,
        ImageRegion.full(source_width, source_height),
        logical_destination,
        transform,
        flip_x,
    );
}

/// Cropped variant used by backdrops and AVB sub-images. The region uses
/// source pixel edges and is intersected with the decoded image.
pub fn blitImageRegion(
    canvas: *Canvas,
    pixels: []const Color,
    source_width: u32,
    source_height: u32,
    source_region_unclipped: ImageRegion,
    logical_destination: balloon.Rect,
    transform: Transform,
    flip_x: bool,
) void {
    if (source_width == 0 or source_height == 0 or
        pixels.len < @as(usize, source_width) * source_height) return;
    const source_region = ImageRegion{
        .left = std.math.clamp(source_region_unclipped.left, 0.0, @as(f64, @floatFromInt(source_width))),
        .top = std.math.clamp(source_region_unclipped.top, 0.0, @as(f64, @floatFromInt(source_height))),
        .right = std.math.clamp(source_region_unclipped.right, 0.0, @as(f64, @floatFromInt(source_width))),
        .bottom = std.math.clamp(source_region_unclipped.bottom, 0.0, @as(f64, @floatFromInt(source_height))),
    };
    if (source_region.right <= source_region.left or source_region.bottom <= source_region.top) return;
    const upper_left = transform.mapD(.{
        .x = @floatFromInt(logical_destination.left),
        .y = @floatFromInt(logical_destination.top),
    });
    const lower_right = transform.mapD(.{
        .x = @floatFromInt(logical_destination.right),
        .y = @floatFromInt(logical_destination.bottom),
    });
    const left_f = @min(upper_left.x, lower_right.x);
    const right_f = @max(upper_left.x, lower_right.x);
    const top_f = @min(upper_left.y, lower_right.y);
    const bottom_f = @max(upper_left.y, lower_right.y);
    if (right_f <= left_f or bottom_f <= top_f) return;

    const clip = clipFor(canvas, transform);
    const left = @max(clip.left, @as(i32, @intFromFloat(@floor(left_f))));
    const right = @min(clip.right, @as(i32, @intFromFloat(@ceil(right_f))));
    const top = @max(clip.top, @as(i32, @intFromFloat(@floor(top_f))));
    const bottom = @min(clip.bottom, @as(i32, @intFromFloat(@ceil(bottom_f))));
    var y = top;
    while (y < bottom) : (y += 1) {
        const v = (@as(f64, @floatFromInt(y)) + 0.5 - top_f) / (bottom_f - top_f);
        const source_y_unclipped = source_region.top + v * (source_region.bottom - source_region.top) - 0.5;
        const source_y = std.math.clamp(source_y_unclipped, source_region.top, @max(source_region.top, source_region.bottom - 1.0));
        var x = left;
        while (x < right) : (x += 1) {
            var u = (@as(f64, @floatFromInt(x)) + 0.5 - left_f) / (right_f - left_f);
            if (flip_x) u = 1.0 - u;
            const source_x_unclipped = source_region.left + u * (source_region.right - source_region.left) - 0.5;
            const source_x = std.math.clamp(source_x_unclipped, source_region.left, @max(source_region.left, source_region.right - 1.0));
            const sample = bilinearPremultiplied(pixels, source_width, source_height, source_x, source_y);
            clippedBlend(canvas, clip, x, y, sample.rgb, sample.alpha);
        }
    }
}

fn testBoxGeometry(dashed: bool, bubbles: []balloon.ThoughtBubble) balloon.BalloonGeometry {
    return .{
        .input_index = 0,
        .kind = if (bubbles.len == 0) .action else .think,
        .dashed = dashed,
        .text = @constCast(&[_]u8{}),
        .formatting = @constCast(&[_]formatting.Change{}),
        .lines = @constCast(&[_]balloon.TextLine{}),
        .cloud_bbox = .{ .left = 300, .top = -300, .right = 2000, .bottom = -1200 },
        .route_region = .{ .left = 0, .top = 0, .right = 2300, .bottom = -2300 },
        .origin = .{ .x = 0, .y = 0 },
        .outline_points = @constCast(&[_]balloon.Point{
            .{ .x = 300, .y = -1200 },
            .{ .x = 300, .y = -300 },
            .{ .x = 2000, .y = -300 },
            .{ .x = 2000, .y = -1200 },
        }),
        .outline_beziers = @constCast(&[_]balloon.CubicBezier{}),
        .tail = null,
        .thought_bubbles = bubbles,
    };
}

test "Transform maps canonical y-up panel into 315px top-down coordinates" {
    const transform = Transform.panel315();
    try std.testing.expectEqual(@as(i32, 0), transform.map(.{ .x = 0, .y = 0 }).x);
    try std.testing.expectEqual(@as(i32, 0), transform.map(.{ .x = 0, .y = 0 }).y);
    try std.testing.expectEqual(@as(i32, 315), transform.map(.{ .x = 2300, .y = -2300 }).x);
    try std.testing.expectEqual(@as(i32, 315), transform.map(.{ .x = 2300, .y = -2300 }).y);
}

test "portable atlas metrics reproduce source CFontInfo vertical geometry" {
    const metrics = AtlasMetrics{};
    const whisper = AtlasMetrics{ .style = .whisper };
    const expected = AtlasVerticalMetrics{
        .request_height = 240,
        .ascent = 216,
        .descent = 60,
        .external_leading = 0,
        .leading = -53,
        .base_add = 40,
        .top_offset = 0,
        .line_height = 223,
    };
    try std.testing.expectEqual(expected, metrics.verticalMetrics());
    try std.testing.expectEqual(expected, whisper.verticalMetrics());
    try std.testing.expectEqual(balloon.FontInfo{
        .line_height = 223,
        .base_add = 40,
        .top_offset = 0,
    }, metrics.fontInfo());

    const measured = metrics.textMeasurer().measure("COMIC CHAT");
    const expected_width = sourceRound(@as(f64, @floatFromInt(Canvas.textWidth("COMIC CHAT"))) * 240.0 / 20.0);
    try std.testing.expectEqual(expected_width, measured.width);
    try std.testing.expectEqual(@as(i32, 276), measured.height);

    // LOGFONT requests 240 TWIPs (about 32.9 device pixels); CFontInfo advances
    // the compacted line by 223 TWIPs after the -53 source vertical kernel.
    try std.testing.expectApproxEqAbs(@as(f64, 32.87), @as(f64, @floatFromInt(metrics.logical_height)) * Transform.panel315().penScale(), 0.02);
    try std.testing.expectApproxEqAbs(@as(f64, 30.54), @as(f64, @floatFromInt(metrics.fontInfo().line_height)) * Transform.panel315().penScale(), 0.02);
}

test "inline formatting selects Bold Italic metrics palette underline and transparent ink" {
    const allocator = std.testing.allocator;
    const metrics = AtlasMetrics{};
    const italic = [_]formatting.Change{.{
        .offset = 0,
        .format = formatting.effect.italic,
    }};
    const plain_size = metrics.textMeasurer().measure("D0.");
    const italic_size = metrics.textMeasurer().measureFormatted("D0.", &italic, 0);
    try std.testing.expectEqual(
        sourceRound(@as(f64, @floatFromInt(Canvas.textWidthStyled("D0.", .whisper))) * 240.0 / 20.0),
        italic_size.width,
    );
    try std.testing.expect(italic_size.width != plain_size.width);

    const transparent_state = formatting.effect.foreground |
        formatting.effect.background | (@as(u16, 4) << 4) | 4;
    const transparent = [_]formatting.Change{.{ .offset = 0, .format = transparent_state }};
    try std.testing.expectEqual(
        plain_size.width,
        metrics.textMeasurer().measureFormatted("D0.", &transparent, 0).width,
    );
    try std.testing.expectEqual(@as(Color, 0xff0000ff), formatColor(formatting.effect.link));
    try std.testing.expectEqual(
        @as(Color, 0xffff0000),
        formatColor(formatting.effect.foreground | (@as(u16, 4) << 4)),
    );

    var text_bytes = [_]u8{'A'};
    var red_underline = [_]formatting.Change{.{
        .offset = 0,
        .format = formatting.effect.foreground | formatting.effect.underline |
            (@as(u16, 4) << 4),
    }};
    var lines = [_]balloon.TextLine{.{
        .start = 0,
        .len = 1,
        .width = 0,
        .x = 300,
        .y = -300,
    }};
    var geometry = testBoxGeometry(false, @constCast(&[_]balloon.ThoughtBubble{}));
    geometry.text = &text_bytes;
    geometry.formatting = &red_underline;
    geometry.lines = &lines;
    var canvas = try Canvas.init(allocator, 315, 315);
    defer canvas.deinit(allocator);
    canvas.clear(canvas_mod.white);
    drawText(&canvas, &geometry, Transform.panel315(), clipFor(&canvas, Transform.panel315()), metrics);
    const underline_y = Transform.panel315().map(.{ .x = 300, .y = -516 }).y;
    try std.testing.expectEqual(@as(Color, 0xffff0000), canvas.px[@as(usize, @intCast(underline_y)) * canvas.width + 45]);

    geometry.formatting = @constCast(&transparent);
    canvas.clear(canvas_mod.white);
    drawText(&canvas, &geometry, Transform.panel315(), clipFor(&canvas, Transform.panel315()), metrics);
    for (canvas.px) |pixel| try std.testing.expectEqual(canvas_mod.white, pixel);
}

test "solid action path fills white and strokes the 28 TWIP black boundary" {
    const allocator = std.testing.allocator;
    var canvas = try Canvas.init(allocator, 315, 315);
    defer canvas.deinit(allocator);
    canvas.clear(0xff336699);
    const geometry = testBoxGeometry(false, @constCast(&[_]balloon.ThoughtBubble{}));
    try drawGeometry(allocator, &canvas, &geometry, Transform.panel315());
    try std.testing.expectEqual(canvas_mod.white, canvas.px[100 * canvas.width + 100]);
    try std.testing.expectEqual(canvas_mod.black, canvas.px[41 * canvas.width + 100]);
    try std.testing.expectEqual(@as(Color, 0xff336699), canvas.px[250 * canvas.width + 100]);
}

test "whisper trajectory has source black dash runs separated by white nimbus" {
    const allocator = std.testing.allocator;
    var canvas = try Canvas.init(allocator, 315, 315);
    defer canvas.deinit(allocator);
    canvas.clear(0xff336699);
    const geometry = testBoxGeometry(true, @constCast(&[_]balloon.ThoughtBubble{}));
    try drawGeometry(allocator, &canvas, &geometry, Transform.panel315());
    var black_pixels: usize = 0;
    var white_pixels: usize = 0;
    const y: usize = 41;
    for (45..270) |x| {
        const pixel = canvas.px[y * canvas.width + x];
        if (pixel == canvas_mod.black) black_pixels += 1;
        if (pixel == canvas_mod.white) white_pixels += 1;
    }
    try std.testing.expect(black_pixels > 20);
    try std.testing.expect(white_pixels > 20);
}

test "thought ellipses are filled after the cloud" {
    const allocator = std.testing.allocator;
    var canvas = try Canvas.init(allocator, 315, 315);
    defer canvas.deinit(allocator);
    canvas.clear(0xff336699);
    var bubbles = [_]balloon.ThoughtBubble{.{ .left = 1000, .top = -1450, .right = 1300, .bottom = -1750 }};
    const geometry = testBoxGeometry(false, &bubbles);
    try drawGeometry(allocator, &canvas, &geometry, Transform.panel315());
    const center = Transform.panel315().map(.{ .x = 1150, .y = -1600 });
    try std.testing.expectEqual(canvas_mod.white, canvas.px[@as(usize, @intCast(center.y)) * canvas.width + @as(usize, @intCast(center.x))]);
}

test "halftone image scaling is alpha-correct, mirrored, and viewport clipped" {
    const allocator = std.testing.allocator;
    var canvas = try Canvas.init(allocator, 8, 4);
    defer canvas.deinit(allocator);
    canvas.clear(canvas_mod.white);
    const pixels = [_]Color{ 0xffff0000, 0x000000ff };
    const transform = try Transform.init(
        .{ .left = 0, .top = 0, .right = 4, .bottom = -2 },
        .{ .x = 2, .y = 0, .width = 4, .height = 2 },
    );
    blitImage(&canvas, &pixels, 2, 1, .{ .left = 0, .top = 0, .right = 4, .bottom = -2 }, transform, false);
    try std.testing.expectEqual(canvas_mod.white, canvas.px[0]);
    try std.testing.expect((canvas.px[2] & 0x0000ff00) < (canvas.px[5] & 0x0000ff00));

    canvas.clear(canvas_mod.white);
    blitImage(&canvas, &pixels, 2, 1, .{ .left = 0, .top = 0, .right = 4, .bottom = -2 }, transform, true);
    try std.testing.expect((canvas.px[5] & 0x0000ff00) < (canvas.px[2] & 0x0000ff00));
}

test "halftone image region samples a source pixel crop" {
    const allocator = std.testing.allocator;
    var canvas = try Canvas.init(allocator, 4, 2);
    defer canvas.deinit(allocator);
    canvas.clear(canvas_mod.black);
    const pixels = [_]Color{ 0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff };
    const transform = try Transform.init(
        .{ .left = 0, .top = 0, .right = 4, .bottom = -2 },
        .{ .x = 0, .y = 0, .width = 4, .height = 2 },
    );
    blitImageRegion(
        &canvas,
        &pixels,
        4,
        1,
        .{ .left = 1, .top = 0, .right = 2, .bottom = 1 },
        .{ .left = 0, .top = 0, .right = 4, .bottom = -2 },
        transform,
        false,
    );
    for (canvas.px) |pixel| try std.testing.expectEqual(@as(Color, 0xff00ff00), pixel);
}
