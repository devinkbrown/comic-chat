//! Cross-module regressions for the released Comic Chat page/balloon stream.
//!
//! Source anchors at upstream commit
//! `c7df00f60bc8e9fdef413f139e61f7c37e024684`:
//! - `panel.cpp:552-558`: every new CPanel stores process-global `rand()`.
//! - `panel.cpp:563-582`: a cloned panel preserves that seed.
//! - `panel.cpp:855-925`: balloon layout consumes the same CRT stream.
//! - `panel.cpp:867`: every layout starts with `srand(m_seed)`.
//! - `panel.cpp:1079-1135`: clone/fresh selection, failed-fit retry, and
//!   continuation recursion.
//! - `panel.cpp:1266-1275`: two-column page bounds with 144-unit interstices.

const std = @import("std");
const page = @import("original_page.zig");
const balloon = @import("original_balloon.zig");
const title = @import("original_title.zig");

const free_rect = balloon.Rect{ .left = 60, .top = -60, .right = 2240, .bottom = -1150 };

const Metrics = struct {
    char_width: i32,
    line_height: i32 = 100,

    fn measure(raw: *const anyopaque, text: []const u8) balloon.Size {
        const self: *const Metrics = @ptrCast(@alignCast(raw));
        var width: i32 = 0;
        var widest: i32 = 0;
        var lines: i32 = 1;
        for (text) |byte| {
            if (byte == '\n' or byte == '\r') {
                widest = @max(widest, width);
                width = 0;
                if (byte == '\n') lines += 1;
            } else if (byte != 0 and byte != '\t' and byte != 0x0b and byte != 0x0c) {
                width += self.char_width;
            }
        }
        return .{ .width = @max(widest, width), .height = lines * self.line_height };
    }

    fn adapter(self: *const Metrics) balloon.TextMeasurer {
        return .{ .context = self, .measure_fn = measure };
    }
};

/// Both modules deliberately expose the source recurrence, but keep distinct
/// nominal types. This bridge makes the single process-global CRT state
/// explicit and copies it back even when LayoutBalloons fails.
fn layoutWithPageRandom(
    gpa: std.mem.Allocator,
    random: *page.MsvcrtRand,
    inputs: []const balloon.BalloonInput,
    metrics: balloon.TextMeasurer,
) balloon.Error!balloon.PanelLayout {
    var shared = balloon.MsvcrtRand.init(random.state);
    defer random.state = shared.state;
    // The callback context is the Metrics value used by every test here.
    const raw_metrics: *const Metrics = @ptrCast(@alignCast(metrics.context));
    return balloon.layoutPanelWithRandom(
        gpa,
        inputs,
        free_rect,
        .{ .line_height = raw_metrics.line_height },
        metrics,
        &shared,
    );
}

fn attempt(result: page.Begin) !page.Attempt {
    return switch (result) {
        .attempt => |value| value,
        .forced_break => error.TestExpectedEqual,
    };
}

test "shared CRT stream determines the next fresh panel seed after balloon layout" {
    const gpa = std.testing.allocator;
    const metrics = Metrics{ .char_width = 40 };
    var random = page.MsvcrtRand{};
    var planner = try page.Planner.init(gpa, &random);
    defer planner.deinit(gpa);

    try std.testing.expectEqual(@as(u32, 41), planner.panels.items[0].seed);
    const first = try attempt(try planner.begin(&random, .{ .speaker_id = 7, .words = "hello from source" }));
    try std.testing.expectEqual(@as(u32, 18467), first.panel.seed);

    const input = [_]balloon.BalloonInput{.{
        .text = first.request.words,
        .arrow_x = 1100,
        .speaker_box = .{ .left = 850, .top = -650, .right = 1350, .bottom = -1500 },
    }};
    var laid_out = try layoutWithPageRandom(gpa, &random, &input, metrics.adapter());
    defer laid_out.deinit(gpa);
    const state_after_layout = random.state;
    _ = try planner.finish(gpa, first, .{ .fit = true });

    // The repeated speaker invokes AddLine's AvatarInPanel fresh-panel rule.
    const next = try attempt(try planner.begin(&random, .{ .speaker_id = 7, .words = "again" }));
    // Fixed draw sequence for panel.cpp:899,903,916 plus balloon ShiftLines.
    try std.testing.expectEqual(@as(u32, 2_846_671_780), state_after_layout);
    try std.testing.expectEqual(@as(u32, 12_803), next.panel.seed);
    try std.testing.expect(!next.replace_last);
    _ = try planner.finish(gpa, next, .{ .fit = true });
    try std.testing.expectEqual(@as(usize, 3), planner.panelCount());
}

test "failed clone leaves consumed CRT state for its fresh retry seed" {
    const gpa = std.testing.allocator;
    const metrics = Metrics{ .char_width = 80, .line_height = 300 };
    var random = page.MsvcrtRand{};
    var planner = try page.Planner.init(gpa, &random);
    defer planner.deinit(gpa);

    const tall_text = "one two three\nfour five six\nseven eight nine\nten eleven twelve\nthirteen fourteen\nfifteen sixteen\nseventeen eighteen\nnineteen twenty";
    const first = try attempt(try planner.begin(&random, .{ .speaker_id = 1, .words = tall_text }));
    const first_input = [_]balloon.BalloonInput{.{
        .text = first.request.words,
        .arrow_x = 500,
        .speaker_box = .{ .left = 250, .top = -650, .right = 750, .bottom = -1500 },
    }};
    // A previously committed panel is sufficient to exercise AddLine's clone
    // path. Its clone immediately re-seeds from this preserved panel seed.
    _ = try planner.finish(gpa, first, .{ .fit = true });

    const clone = try attempt(try planner.begin(&random, .{ .speaker_id = 2, .words = tall_text }));
    try std.testing.expect(clone.replace_last);
    const clone_inputs = [_]balloon.BalloonInput{
        first_input[0],
        .{
            .text = clone.request.words,
            .arrow_x = 1800,
            .speaker_box = .{ .left = 1550, .top = -650, .right = 2050, .bottom = -1500 },
        },
    };
    if (layoutWithPageRandom(gpa, &random, &clone_inputs, metrics.adapter())) |successful| {
        var unexpected = successful;
        unexpected.deinit(gpa);
        return error.TestUnexpectedResult;
    } else |err| try std.testing.expectEqual(error.BalloonsDoNotFit, err);
    const state_after_failure = random.state;

    const retry_step = try planner.finish(gpa, clone, .{ .fit = false });
    const retry_request = switch (retry_step) {
        .retry => |value| value,
        else => return error.TestExpectedEqual,
    };
    const fresh = try attempt(try planner.begin(&random, retry_request));
    // The failed clone is not rolled back: C++ LayoutBalloons consumed these
    // global draws before StartNewPanel recursively constructs its retry.
    try std.testing.expectEqual(@as(u32, 2_317_724_829), state_after_failure);
    try std.testing.expectEqual(@as(u32, 26_982), fresh.panel.seed);
    try std.testing.expect(!fresh.replace_last);
    _ = try planner.finish(gpa, fresh, .{ .fit = true });
    try std.testing.expectEqual(@as(usize, 3), planner.panelCount());
}

test "force-fit continuation commits then starts a fresh same-speaker panel" {
    const gpa = std.testing.allocator;
    const metrics = Metrics{ .char_width = 90, .line_height = 300 };
    var random = page.MsvcrtRand{};
    var planner = try page.Planner.init(gpa, &random);
    defer planner.deinit(gpa);

    const words = "one two three\nfour five six\nseven eight nine\nten eleven twelve\nthirteen fourteen\nfifteen sixteen\nseventeen eighteen\nnineteen twenty";
    const first = try attempt(try planner.begin(&random, .{ .speaker_id = 44, .words = words }));
    const inputs = [_]balloon.BalloonInput{.{
        .text = first.request.words,
        .arrow_x = 1100,
        .speaker_box = .{ .left = 850, .top = -650, .right = 1350, .bottom = -1500 },
    }};
    var layout = try layoutWithPageRandom(gpa, &random, &inputs, metrics.adapter());
    defer layout.deinit(gpa);
    try std.testing.expect(layout.continuation_text != null);
    const state_after_layout = random.state;

    const next_step = try planner.finish(gpa, first, .{
        .fit = true,
        .continuation = .{ .words = layout.continuation_text.? },
    });
    const continuation_line = switch (next_step) {
        .continuation => |value| value,
        else => return error.TestExpectedEqual,
    };
    try std.testing.expect(std.mem.startsWith(u8, continuation_line.words, "..."));
    const continuation_attempt = try attempt(try planner.begin(&random, continuation_line));
    try std.testing.expectEqual(@as(u32, 3_864_011_462), state_after_layout);
    try std.testing.expectEqual(@as(u32, 32_452), continuation_attempt.panel.seed);
    try std.testing.expect(!continuation_attempt.replace_last);
    try std.testing.expectEqual(@as(u32, 44), continuation_attempt.request.speaker_id);
    _ = try planner.finish(gpa, continuation_attempt, .{ .fit = true });
    try std.testing.expectEqual(@as(usize, 3), planner.panelCount());
}

test "source page bounds retain two columns and 144-unit interstices" {
    try std.testing.expectEqual(@as(usize, 2), title.panels_per_row);
    try std.testing.expectEqual(@as(i32, 144), title.horizontal_interstice);
    try std.testing.expectEqual(@as(i32, 144), title.vertical_interstice);
    try std.testing.expectEqual(
        title.Rect{ .left = 0, .bottom = -4744, .right = 4744, .top = 0 },
        try title.pageBounds(4, 0, 0),
    );
    try std.testing.expectEqual(
        title.Rect{ .left = 0, .bottom = -4744, .right = 2300, .top = -2444 },
        try title.panelRect(2, 0, 0),
    );
}
