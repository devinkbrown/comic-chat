//! Source-faithful Comic Chat 2.5 title-panel planning.
//!
//! Directly ported from Microsoft's MIT-licensed source at commit
//! `c7df00f60bc8e9fdef413f139e61f7c37e024684`:
//! - `panel.cpp:31-45,56-62`: icon/row/page constants and two-column default.
//! - `panel.cpp:477-512`: `AddStarsAux` participant ordering.
//! - `panel.cpp:1266-1297`: page bounds and `AddTitle` element order.
//! - `panel.cpp:1391-1445`: `AddStars` capacity and geometry.
//! - `fonts.cpp:98-140`: title/shout font construction and metrics.
//! - `balloon.cpp:347-425,668-702,876-905,1112-1129`: label wrapping,
//!   measurement, multi-line drawing, and star-label ellipsis.
//!
//! The original measures text through the active Windows GDI `LOGFONT`, whose
//! face, size, charset, DPI substitution, and localized resources are runtime
//! data. Consequently this module does not invent portable text widths. The
//! backend supplies a measurement callback and the resulting source geometry
//! is otherwise deterministic on Windows and Wayland.

const std = @import("std");

pub const unit_width: i32 = 2300;
pub const unit_height: i32 = 2300;
pub const panels_per_row: usize = 2;
pub const horizontal_interstice: i32 = 144;
pub const vertical_interstice: i32 = 144;
pub const border_width: i32 = 60;

pub const icon_size: i32 = 500;
pub const icon_space: i32 = 100;
pub const below_starring: i32 = 300;
pub const source_design_height: i32 = 4860;

pub const FontConstruction = struct {
    title_request_height: i32,
    shout_request_height: i32,
    title_leading: i32,
    title_base_add: i32,
};

/// Literal negative-`LOGFONT` clamp from `UpdateTitleFonts`. Because Win32
/// character heights are negative, `min` selects the *larger magnitude*
/// request. At the shipped 12pt/-240 balloon setting this produces -288 for
/// the title and clamps Starring/name labels to -240.
pub fn fontConstruction(balloon_request_height: i32) FontConstruction {
    const reduction: f32 = @as(f32, @floatFromInt(unit_width)) / @as(f32, source_design_height);
    const scaled_title: i32 = @intFromFloat(@as(f32, -576.0) * reduction);
    const scaled_shout: i32 = @intFromFloat(@as(f32, -252.0) * reduction);
    const title_cap: i32 = @intFromFloat(1.2 * @as(f64, @floatFromInt(balloon_request_height)));
    return .{
        .title_request_height = @min(scaled_title, title_cap),
        .shout_request_height = @min(scaled_shout, balloon_request_height),
        .title_leading = @intFromFloat(@as(f32, -220.0) * reduction),
        .title_base_add = @intFromFloat(@as(f32, 120.0) * reduction),
    };
}

/// English `IDS_TITLE1..IDS_TITLE16` resources from `chat.rc`. Other locale
/// front ends can keep supplying an explicit localized title.
pub const english_random_titles = [_][]const u8{
    "EVERYONE'S A COMIC",
    "DOGGY DOGGY WAH WAH",
    "YOU SHOULDA BEEN THERE",
    "NO EXIT",
    "WISH YOU WERE HERE",
    "DEEPEST DARKEST DESIRES",
    "JUST US CHUMPS",
    "SIGHTED IN CYBERSPACE",
    "THE GANG'S ALL HERE",
    "BORN TO CHAT",
    "NETWORKED NERDS",
    "VIRTUALLY VACUOUS",
    "IF I ONLY HAD A BRAIN",
    "SLUMBER PARTY",
    "MEET MARKET",
    "MICROSOFT CHAT",
};

/// `GetRandomTitle` receives one `randfloat()` result, truncates its scaled
/// index, and defensively clamps the (otherwise unreachable) upper endpoint.
pub fn englishRandomTitle(random_fraction: f64) []const u8 {
    var chosen: usize = if (random_fraction <= 0)
        0
    else
        @intFromFloat(random_fraction * @as(f64, english_random_titles.len));
    chosen = @min(chosen, english_random_titles.len - 1);
    return english_random_titles[chosen];
}

pub const Rect = struct {
    left: i32,
    bottom: i32,
    right: i32,
    top: i32,

    pub fn width(self: Rect) i32 {
        return self.right - self.left;
    }

    pub fn height(self: Rect) i32 {
        return self.top - self.bottom;
    }
};

pub const FontRole = enum { title, shout };

pub const Measurement = struct {
    /// Raw single-line GDI extent. It may exceed `maximum_width`; the caller
    /// owns the source-compatible wrapping decision.
    width: i32,
    line_count: usize = 1,
};

pub const MeasureTextFn = *const fn (
    context: ?*const anyopaque,
    role: FontRole,
    text: []const u8,
    maximum_width: i32,
) Measurement;

pub const FontMetrics = struct {
    title_line_height: i32,
    title_base_add: i32,
    shout_line_height: i32,
    shout_base_add: i32,
};

pub const TextMeasurer = struct {
    context: ?*const anyopaque = null,
    measure_text: MeasureTextFn,
    fonts: FontMetrics,

    fn measure(self: TextMeasurer, role: FontRole, text: []const u8, maximum_width: i32) Error!Measurement {
        const measured = self.measure_text(self.context, role, text, maximum_width);
        if (measured.width < 0 or measured.line_count != 1)
            return error.InvalidMeasurement;
        return measured;
    }
};

/// Input order is the original nickname-map enumeration order. `AddStarsAux`
/// then keeps self first, non-departed users before departed users, and larger
/// send counts first within a departed/non-departed group. `has_icon == false`
/// is the source's `!newAv->m_icon` filter.
pub const Participant = struct {
    name: []const u8,
    is_self: bool = false,
    departed: bool = false,
    sends: u32 = 0,
    has_icon: bool = true,
};

pub const Label = struct {
    text: []const u8,
    role: FontRole,
    /// The rectangle assigned by `SetBBox`; ordinary `TextOut` is not clipped
    /// to it, while `CStarLabel::Draw` passes it to `DrawTextEx` as its clip.
    bbox: Rect,
    /// Result returned by `CLabel::GetBBox` after source line breaking.
    measured_bbox: Rect,
    left_justified: bool,
    lines: []Line,
    end_ellipsis: bool = false,

    pub fn deinit(self: *Label, gpa: std.mem.Allocator) void {
        gpa.free(self.lines);
        self.* = undefined;
    }
};

pub const Line = struct {
    start: usize,
    len: usize,
    width: i32,

    pub fn bytes(self: Line, text: []const u8) []const u8 {
        return text[self.start .. self.start + self.len];
    }
};

pub const Star = struct {
    participant_index: usize,
    name: []const u8,
    icon_bbox: Rect,
    label: Label,
};

pub const Options = struct {
    /// `IDS_STARRING` is localized in the Windows resource DLL. A portable
    /// caller should pass its localized equivalent here.
    starring_text: []const u8 = "Starring",
    /// Direct equivalent of `MyAvatarID() != 0`; no stars are added before
    /// registration, though the two heading labels remain.
    registered: bool = true,
};

pub const Plan = struct {
    title: Label,
    starring: Label,
    stars: []Star,
    has_border: bool = false,
    backdrop_id: u32 = 0,
    max_stars: usize,
    row_height: i32,

    pub fn deinit(self: *Plan, gpa: std.mem.Allocator) void {
        self.title.deinit(gpa);
        self.starring.deinit(gpa);
        for (self.stars) |*star| star.label.deinit(gpa);
        gpa.free(self.stars);
        self.* = undefined;
    }

    /// `AddTitle` appends title, Starring, then each icon/label pair.
    pub fn elementCount(self: Plan) usize {
        return 2 + self.stars.len * 2;
    }
};

pub const Error = error{
    InvalidFontMetrics,
    InvalidMeasurement,
    NoCharacterFits,
    InvalidPageIndex,
} || std.mem.Allocator.Error;

const max_lines: usize = 10;

fn cSpace(byte: u8) bool {
    return switch (byte) {
        ' ', '\t', '\n', '\r', 0x0b, 0x0c => true,
        else => false,
    };
}

fn nextStart(text: []const u8, start: usize) usize {
    var at = start;
    while (at < text.len and cSpace(text[at])) at += 1;
    return at;
}

fn nextEnd(text: []const u8, start: usize) usize {
    var at = start;
    while (at < text.len and cSpace(text[at])) at += 1;
    while (at < text.len and !cSpace(text[at])) at += 1;
    return at;
}

fn upcomingReturn(text: []const u8, start: usize) bool {
    var at = start;
    while (at < text.len and cSpace(text[at])) : (at += 1)
        if (text[at] == '\n') return true;
    return false;
}

fn forceLineBreak(
    text: []const u8,
    maximum_width: i32,
    measurer: TextMeasurer,
    role: FontRole,
) Error!Line {
    var len: usize = 0;
    var width: i32 = 0;
    while (len < text.len) {
        const previous = len;
        len += 1;
        const extent = try measurer.measure(role, text[0..len], maximum_width);
        // `ForceLineBreak` tests the byte after the measured candidate. The
        // last character is therefore left for the next line by this path.
        if (len < text.len and extent.width <= maximum_width) {
            width = extent.width;
        } else {
            len = previous;
            if (len == 0) return error.NoCharacterFits;
            return .{ .start = 0, .len = len, .width = width };
        }
    }
    unreachable;
}

/// Port of the active 2.5 `BreakIntoLines`, including hard returns, forced
/// long-word splits, whitespace trimming between lines, and `MAXLINES == 10`.
fn breakIntoLines(
    gpa: std.mem.Allocator,
    text: []const u8,
    maximum_width: i32,
    measurer: TextMeasurer,
    role: FontRole,
) Error![]Line {
    if (maximum_width <= 0) return error.NoCharacterFits;
    if (text.len == 0) {
        const empty = try gpa.alloc(Line, 1);
        empty[0] = .{ .start = 0, .len = 0, .width = 0 };
        return empty;
    }

    var result: std.ArrayList(Line) = .empty;
    errdefer result.deinit(gpa);
    var string_start: usize = 0;
    var line_end: usize = 0;
    var this_length: usize = 0;
    var last_width: i32 = 0;

    while (true) {
        line_end = nextEnd(text, line_end);
        var last_length = this_length;
        this_length = line_end - string_start;
        const extent = try measurer.measure(role, text[string_start..line_end], maximum_width);
        const found_return = upcomingReturn(text, line_end);

        if (extent.width <= maximum_width and !found_return) {
            if (line_end == text.len) {
                try result.append(gpa, .{ .start = string_start, .len = this_length, .width = extent.width });
                break;
            }
            last_width = extent.width;
        } else {
            if (last_length == 0 and extent.width > maximum_width) {
                const forced = try forceLineBreak(text[string_start..], maximum_width, measurer, role);
                last_length = forced.len;
                last_width = forced.width;
            } else if (found_return and extent.width <= maximum_width) {
                last_length = this_length;
                last_width = extent.width;
            }
            try result.append(gpa, .{ .start = string_start, .len = last_length, .width = last_width });
            string_start = nextStart(text, string_start + last_length);
            line_end = string_start;
            if (string_start == text.len or result.items.len >= max_lines) break;
            this_length = 0;
        }
    }
    return result.toOwnedSlice(gpa);
}

fn makeLabel(
    gpa: std.mem.Allocator,
    text: []const u8,
    role: FontRole,
    bbox: Rect,
    left_justified: bool,
    end_ellipsis: bool,
    measurer: TextMeasurer,
) Error!Label {
    const lines = try breakIntoLines(gpa, text, bbox.width(), measurer, role);
    errdefer gpa.free(lines);
    var widest: i32 = 0;
    for (lines) |line| widest = @max(widest, line.width);
    const fonts = measurer.fonts;
    const line_height = if (role == .title) fonts.title_line_height else fonts.shout_line_height;
    const base_add = if (role == .title) fonts.title_base_add else fonts.shout_base_add;
    const measured_left = if (left_justified) bbox.left else bbox.left + @divTrunc(bbox.width() - widest, 2);
    return .{
        .text = text,
        .role = role,
        .bbox = bbox,
        .measured_bbox = .{
            .left = measured_left,
            .bottom = bbox.top - @as(i32, @intCast(lines.len)) * line_height - base_add,
            .right = measured_left + widest,
            .top = bbox.top,
        },
        .left_justified = left_justified,
        .lines = lines,
        .end_ellipsis = end_ellipsis,
    };
}

/// Plan the original borderless/no-backdrop title panel in its y-up logical
/// coordinate system (`top == 0`, `bottom == -2300`).
pub fn build(
    gpa: std.mem.Allocator,
    title_text: []const u8,
    participants: []const Participant,
    measurer: TextMeasurer,
    options: Options,
) Error!Plan {
    const fonts = measurer.fonts;
    if (fonts.title_line_height <= 0 or fonts.shout_line_height <= 0 or
        fonts.title_base_add < 0 or fonts.shout_base_add < 0)
        return error.InvalidFontMetrics;

    // AddTitle assigns a half-panel constraint, although BreakIntoLines only
    // consumes its width and may measure below its nominal bottom.
    var title = try makeLabel(gpa, title_text, .title, .{
        .left = 0,
        .bottom = -@divTrunc(unit_height, 2),
        .right = unit_width,
        .top = -100,
    }, false, false, measurer);
    errdefer title.deinit(gpa);

    // The Starring label starts exactly at the measured title bottom.
    var starring = try makeLabel(gpa, options.starring_text, .shout, .{
        .left = 0,
        .bottom = -unit_height,
        .right = unit_width,
        .top = title.measured_bbox.bottom,
    }, false, false, measurer);
    errdefer starring.deinit(gpa);

    const row_height = @max(icon_size, fonts.shout_line_height);
    var row_bottom = starring.measured_bbox.bottom - @divTrunc(below_starring * unit_height, source_design_height);
    const signed_max_stars = @divTrunc(unit_height + row_bottom, row_height);
    const max_stars: usize = if (signed_max_stars > 0) @intCast(signed_max_stars) else 0;
    row_bottom -= row_height;

    var ordered: std.ArrayList(usize) = .empty;
    defer ordered.deinit(gpa);
    if (options.registered)
        try orderParticipants(gpa, &ordered, participants, max_stars);
    const star_count = @min(max_stars, ordered.items.len);

    // AddStars measures every retained name first, then centers the widest
    // icon+gap+label composite for all rows.
    var measured_widths: std.ArrayList(i32) = .empty;
    defer measured_widths.deinit(gpa);
    try measured_widths.ensureTotalCapacity(gpa, star_count);
    var widest_name: i32 = 0;
    for (ordered.items[0..star_count]) |participant_index| {
        var measured = try makeLabel(gpa, participants[participant_index].name, .shout, .{
            .left = 0,
            .bottom = -unit_height,
            .right = unit_width,
            .top = 0,
        }, false, false, measurer);
        defer measured.deinit(gpa);
        try measured_widths.append(gpa, measured.measured_bbox.width());
        widest_name = @max(widest_name, measured.measured_bbox.width());
    }

    const maximum_width = widest_name + icon_size + icon_space;
    const icon_offset = @max(@divTrunc(unit_width - maximum_width, 2), 0);
    const text_offset = icon_offset + icon_size + icon_space;
    const icon_vertical_displacement = @divTrunc(row_height - icon_size, 2);
    const text_vertical_displacement = @divTrunc(row_height - fonts.shout_line_height, 2);

    const stars = try gpa.alloc(Star, star_count);
    errdefer gpa.free(stars);
    var initialized_stars: usize = 0;
    errdefer for (stars[0..initialized_stars]) |*star| star.label.deinit(gpa);
    for (stars, ordered.items[0..star_count]) |*star, participant_index| {
        const participant = participants[participant_index];
        const label_bbox = Rect{
            .left = text_offset,
            .bottom = row_bottom + text_vertical_displacement,
            .right = unit_width,
            .top = row_bottom + fonts.shout_line_height + text_vertical_displacement,
        };
        var label = try makeLabel(gpa, participant.name, .shout, label_bbox, true, true, measurer);
        errdefer label.deinit(gpa);
        star.* = .{
            .participant_index = participant_index,
            .name = participant.name,
            .icon_bbox = .{
                .left = icon_offset,
                .bottom = row_bottom + icon_vertical_displacement,
                .right = icon_offset + icon_size,
                .top = row_bottom + icon_size + icon_vertical_displacement,
            },
            .label = label,
        };
        initialized_stars += 1;
        row_bottom -= row_height;
    }

    return .{
        .title = title,
        .starring = starring,
        .stars = stars,
        .max_stars = max_stars,
        .row_height = row_height,
    };
}

fn orderParticipants(
    gpa: std.mem.Allocator,
    ordered: *std.ArrayList(usize),
    participants: []const Participant,
    max_stars: usize,
) std.mem.Allocator.Error!void {
    for (participants, 0..) |candidate, candidate_index| {
        if (!candidate.has_icon) continue;
        if (candidate.is_self) {
            try ordered.insert(gpa, 0, candidate_index);
            continue;
        }

        var inserted = false;
        // Index zero is skipped literally because the source reserves it for
        // the local user. Preserve that behavior even if input order is odd.
        var i: usize = 1;
        while (i < ordered.items.len) : (i += 1) {
            const existing = participants[ordered.items[i]];
            if ((!candidate.departed and existing.departed) or
                (candidate.departed == existing.departed and candidate.sends > existing.sends))
            {
                try ordered.insert(gpa, i, candidate_index);
                inserted = true;
                break;
            }
        }

        // Port of `upper <= maxStars - 1`; expressed without unsigned
        // underflow. AddStars later truncates this list to maxStars.
        if (!inserted and (max_stars > 0 and ordered.items.len <= max_stars))
            try ordered.append(gpa, candidate_index);
    }
}

/// Exact two-column unit-panel placement used by `RefreshPanelN`.
pub fn panelRect(panel_index: usize, left_x: i32, top_y: i32) Error!Rect {
    const column: i32 = @intCast(panel_index % panels_per_row);
    const row: i32 = @intCast(panel_index / panels_per_row);
    const left = left_x + column * (unit_width + vertical_interstice);
    const top = top_y - row * (unit_height + horizontal_interstice);
    return .{ .left = left, .bottom = top - unit_height, .right = left + unit_width, .top = top };
}

/// Exact `GetBBox` page extent for the default two-column layout.
pub fn pageBounds(panel_count: usize, left_x: i32, top_y: i32) Error!Rect {
    if (panel_count == 0) return error.InvalidPageIndex;
    const rows: i32 = @intCast((panel_count - 1) / panels_per_row + 1);
    const columns: i32 = @intCast(@min(panel_count, panels_per_row));
    return .{
        .left = left_x,
        .bottom = top_y - (rows * unit_height + (rows - 1) * horizontal_interstice),
        .right = left_x + columns * unit_width + (columns - 1) * vertical_interstice,
        .top = top_y,
    };
}

fn fixedMeasure(_: ?*const anyopaque, _: FontRole, text: []const u8, maximum_width: i32) Measurement {
    _ = maximum_width;
    return .{ .width = @intCast(text.len * 20) };
}

const test_measurer = TextMeasurer{
    .measure_text = fixedMeasure,
    .fonts = .{
        .title_line_height = 200,
        .title_base_add = 20,
        .shout_line_height = 100,
        .shout_base_add = 0,
    },
};

test "AddTitle headings use measured centered bounds and source element order" {
    const gpa = std.testing.allocator;
    var plan = try build(gpa, "A Title", &.{}, test_measurer, .{});
    defer plan.deinit(gpa);

    try std.testing.expectEqual(Rect{ .left = 0, .bottom = -1150, .right = 2300, .top = -100 }, plan.title.bbox);
    try std.testing.expectEqual(Rect{ .left = 1080, .bottom = -320, .right = 1220, .top = -100 }, plan.title.measured_bbox);
    try std.testing.expectEqual(Rect{ .left = 0, .bottom = -2300, .right = 2300, .top = -320 }, plan.starring.bbox);
    try std.testing.expectEqual(Rect{ .left = 1070, .bottom = -420, .right = 1230, .top = -320 }, plan.starring.measured_bbox);
    try std.testing.expect(!plan.has_border);
    try std.testing.expectEqual(@as(u32, 0), plan.backdrop_id);
    try std.testing.expectEqual(@as(usize, 2), plan.elementCount());
}

test "AddStars ordering capacity and geometry match panel.cpp" {
    const gpa = std.testing.allocator;
    const participants = [_]Participant{
        .{ .name = "Me", .is_self = true, .sends = 1 },
        .{ .name = "Gone", .departed = true, .sends = 99 },
        .{ .name = "Amy", .sends = 2 },
        .{ .name = "Chatter", .sends = 10 },
        .{ .name = "NoIcon", .sends = 100, .has_icon = false },
    };
    var plan = try build(gpa, "A Title", &participants, test_measurer, .{});
    defer plan.deinit(gpa);

    // title bottom -320, Starring bottom -420, gap truncates to 141; this
    // leaves three 500-unit rows in a 2300-unit panel.
    try std.testing.expectEqual(@as(usize, 3), plan.max_stars);
    try std.testing.expectEqual(@as(usize, 3), plan.stars.len);
    try std.testing.expectEqualStrings("Me", plan.stars[0].name);
    try std.testing.expectEqualStrings("Chatter", plan.stars[1].name);
    try std.testing.expectEqualStrings("Amy", plan.stars[2].name);
    try std.testing.expectEqual(@as(usize, 8), plan.elementCount());

    // Widest retained name is 140 units, so the 740-unit composite centers at
    // x=780 and labels start after the literal 500+100 icon/gap.
    try std.testing.expectEqual(Rect{ .left = 780, .bottom = -1061, .right = 1280, .top = -561 }, plan.stars[0].icon_bbox);
    try std.testing.expectEqual(Rect{ .left = 1380, .bottom = -861, .right = 2300, .top = -761 }, plan.stars[0].label.bbox);
    try std.testing.expectEqual(@as(i32, -1561), plan.stars[1].icon_bbox.bottom);
    try std.testing.expect(plan.stars[0].label.left_justified);
}

test "unregistered title omits stars exactly like MyAvatarID zero" {
    const gpa = std.testing.allocator;
    const participants = [_]Participant{.{ .name = "Me", .is_self = true }};
    var plan = try build(gpa, "A Title", &participants, test_measurer, .{ .registered = false });
    defer plan.deinit(gpa);
    try std.testing.expectEqual(@as(usize, 0), plan.stars.len);
    try std.testing.expectEqual(@as(usize, 2), plan.elementCount());
}

test "two-column panel placement and page bounds use 144-unit interstices" {
    try std.testing.expectEqual(
        Rect{ .left = 0, .bottom = -2300, .right = 2300, .top = 0 },
        try panelRect(0, 0, 0),
    );
    try std.testing.expectEqual(
        Rect{ .left = 2444, .bottom = -2300, .right = 4744, .top = 0 },
        try panelRect(1, 0, 0),
    );
    try std.testing.expectEqual(
        Rect{ .left = 0, .bottom = -4744, .right = 2300, .top = -2444 },
        try panelRect(2, 0, 0),
    );
    try std.testing.expectEqual(
        Rect{ .left = 10, .bottom = -4724, .right = 4754, .top = 20 },
        try pageBounds(3, 10, 20),
    );
    try std.testing.expectError(error.InvalidPageIndex, pageBounds(0, 0, 0));
}

test "font and GDI measurement constraints are rejected instead of guessed" {
    const gpa = std.testing.allocator;
    var bad = test_measurer;
    bad.fonts.shout_line_height = 0;
    try std.testing.expectError(error.InvalidFontMetrics, build(gpa, "Title", &.{}, bad, .{}));

    const BadMeasure = struct {
        fn call(_: ?*const anyopaque, _: FontRole, _: []const u8, maximum_width: i32) Measurement {
            _ = maximum_width;
            return .{ .width = 10, .line_count = 2 };
        }
    };
    bad = test_measurer;
    bad.measure_text = BadMeasure.call;
    try std.testing.expectError(error.InvalidMeasurement, build(gpa, "Title", &.{}, bad, .{}));
}

test "default title LOGFONT requests preserve negative-height clamps" {
    const construction = fontConstruction(-240);
    try std.testing.expectEqual(@as(i32, -288), construction.title_request_height);
    try std.testing.expectEqual(@as(i32, -240), construction.shout_request_height);
    try std.testing.expectEqual(@as(i32, -104), construction.title_leading);
    try std.testing.expectEqual(@as(i32, 56), construction.title_base_add);
}

test "CLabel wrapping preserves hard returns and ten-line source cap" {
    const gpa = std.testing.allocator;
    const many_lines = "A\nB\nC\nD\nE\nF\nG\nH\nI\nJ\nK";
    var plan = try build(gpa, many_lines, &.{}, test_measurer, .{});
    defer plan.deinit(gpa);

    try std.testing.expectEqual(@as(usize, 10), plan.title.lines.len);
    try std.testing.expectEqualStrings("A", plan.title.lines[0].bytes(many_lines));
    try std.testing.expectEqualStrings("J", plan.title.lines[9].bytes(many_lines));
    try std.testing.expectEqual(@as(i32, -100 - 10 * 200 - 20), plan.title.measured_bbox.bottom);
    // AddTitle places Starring from GetBBox even when the title ran below its
    // nominal half-panel SetBBox rectangle.
    try std.testing.expectEqual(plan.title.measured_bbox.bottom, plan.starring.bbox.top);
}

test "random English title table and endpoint clamp match resources" {
    try std.testing.expectEqual(@as(usize, 16), english_random_titles.len);
    try std.testing.expectEqualStrings("EVERYONE'S A COMIC", englishRandomTitle(0.0));
    try std.testing.expectEqualStrings("THE GANG'S ALL HERE", englishRandomTitle(8.0 / 16.0));
    try std.testing.expectEqualStrings("MICROSOFT CHAT", englishRandomTitle(1.0));
}

test "star labels retain DrawTextEx single-line ellipsis mode after measurement" {
    const gpa = std.testing.allocator;
    const participants = [_]Participant{.{
        .name = "THIS PARTICIPANT NAME IS DELIBERATELY LONG ENOUGH TO CROSS THE FINAL STAR LABEL RECTANGLE AND REQUIRE MORE THAN ONE SOURCE MEASUREMENT LINE",
        .is_self = true,
    }};
    var plan = try build(gpa, "Title", &participants, test_measurer, .{});
    defer plan.deinit(gpa);
    try std.testing.expectEqual(@as(usize, 1), plan.stars.len);
    try std.testing.expect(plan.stars[0].label.end_ellipsis);
    try std.testing.expect(plan.stars[0].label.lines.len > 1);
    try std.testing.expect(plan.stars[0].label.bbox.width() <
        @as(i32, @intCast(participants[0].name.len * 20)));
}
