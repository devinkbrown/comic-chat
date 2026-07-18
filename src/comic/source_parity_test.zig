//! Cross-module regressions pinned to Microsoft's MIT-licensed Comic Chat 2.5
//! implementation at commit c7df00f60bc8e9fdef413f139e61f7c37e024684.
//!
//! These are deliberately fixed-value tests rather than assertions derived from
//! the Zig implementation.  The reference locations are `panel.h:152-153`,
//! `panel.cpp:259-447,726-819,1058-1138`, `avatar.cpp:55-114,298-411`,
//! `avatario.cpp:45-96`, `avbfile.cpp:1183-1227`, and
//! `balloon.cpp:51-74,1533-1585` in Microsoft's modernized source snapshot.

const std = @import("std");
const avb = @import("../assets/avb.zig");
const bgb = @import("../assets/bgb.zig");
const figure = @import("figure.zig");
const original_layout = @import("original_layout.zig");
const original_balloon = @import("original_balloon.zig");

test "Microsoft panel and balloon constants remain source exact" {
    // panel.h MINUNITPANELWIDTH/MINUNITPANELHEIGHT and AddTalkTos's hard cap.
    try std.testing.expectEqual(@as(i32, 2300), original_layout.default_unit_width);
    try std.testing.expectEqual(@as(i32, 2300), original_layout.default_unit_height);
    try std.testing.expectEqual(@as(usize, 5), original_layout.max_bodies_per_panel);

    // balloon.cpp and panel.cpp layout constants. These values are TWIPs.
    try std.testing.expectEqual(@as(i32, 90), original_balloon.xbox_delta);
    try std.testing.expectEqual(@as(i32, 50), original_balloon.ybox_delta);
    try std.testing.expectEqual(@as(i32, 300), original_balloon.min_route_width);
    try std.testing.expectEqual(@as(i32, 100), original_balloon.x_border);
    try std.testing.expectEqual(@as(i32, 40), original_balloon.y_border);
    try std.testing.expectEqual(@as(i32, -20), original_balloon.top_border);
    try std.testing.expectEqual(@as(i32, 400), original_balloon.border_fudge);
    try std.testing.expectEqual(@as(i32, 500), original_balloon.one_line_threshold);
    try std.testing.expectEqual(@as(i32, 100), original_balloon.min_hook_height);
    try std.testing.expectEqual(@as(usize, 10), original_balloon.max_lines);
    try std.testing.expectEqualStrings("...", original_balloon.continuation);
}

test "LayoutAvatars single-body establishing geometry matches panel.cpp" {
    const bodies = [_]original_layout.Body{.{
        .id = 11,
        .width = 100,
        .height = 200,
        .norm_height = 100,
        .head_height = 90,
        .face_x = 25,
    }};
    const placed = try original_layout.layoutAvatars(
        std.testing.allocator,
        &bodies,
        2300,
        2300,
        true,
    );
    defer std.testing.allocator.free(placed);

    // maxBodyHeight=(int)(2300/1.9)=1210; width=ROUND(100*1210/200)=605;
    // margin=(2300-605)/2=847. The Zig Rect is the y-down transform of the
    // original SetBBox(847,-2300,1452,-1090).
    try std.testing.expectEqual(@as(usize, 1), placed.len);
    try std.testing.expectEqual(@as(i32, 847), placed[0].rect.x);
    try std.testing.expectEqual(@as(i32, 1090), placed[0].rect.y);
    try std.testing.expectEqual(@as(i32, 605), placed[0].rect.w);
    try std.testing.expectEqual(@as(i32, 1210), placed[0].rect.h);
    try std.testing.expectEqual(@as(i32, 998), placed[0].arrow_x);
    try std.testing.expect(!placed[0].flipped);
}

test "LayoutAvatars zoom keeps the original pre-zoom top anchor" {
    const bodies = [_]original_layout.Body{.{
        .id = 11,
        .width = 100,
        .height = 200,
        .norm_height = 100,
        .head_height = 90,
        .face_x = 25,
    }};
    const placed = try original_layout.layoutAvatars(
        std.testing.allocator,
        &bodies,
        2300,
        2300,
        false,
    );
    defer std.testing.allocator.free(placed);

    // panel.cpp:788-803 scales width/height but intentionally does not rewrite
    // top[i]. This seemingly odd anchor is observable and must not be tidied up.
    try std.testing.expectEqual(@as(i32, 590), placed[0].rect.x);
    try std.testing.expectEqual(@as(i32, 1090), placed[0].rect.y);
    try std.testing.expectEqual(@as(i32, 1119), placed[0].rect.w);
    try std.testing.expectEqual(@as(i32, 2239), placed[0].rect.h);
    try std.testing.expectEqual(@as(i32, 870), placed[0].arrow_x);
}

test "greedy avatar ordering preserves insertion and facing source behavior" {
    const talks_to_two = [_]u32{2};
    const bodies = [_]original_layout.Body{
        .{
            .id = 1,
            .width = 100,
            .height = 200,
            .head_height = 90,
            .face_x = 50,
            .talk_to_ids = &talks_to_two,
        },
        .{ .id = 2, .width = 100, .height = 200, .head_height = 90, .face_x = 50 },
    };
    const placed = try original_layout.layoutAvatars(
        std.testing.allocator,
        &bodies,
        2300,
        2300,
        true,
    );
    defer std.testing.allocator.free(placed);

    try std.testing.expectEqual(@as(usize, 0), placed[0].body_index);
    try std.testing.expectEqual(@as(usize, 1), placed[1].body_index);
    try std.testing.expect(!placed[0].flipped);
    try std.testing.expect(placed[1].flipped);
    try std.testing.expectEqual(@as(u32, 2), placed[0].history.last_left);
    try std.testing.expectEqual(@as(u32, 1), placed[1].history.last_right);
}

test "AddTalkTos expands only addressed roster avatars and preserves edge hysteresis" {
    const talks_to = [_]u32{ 2, 2, 99 };
    const speakers = [_]original_layout.Body{.{
        .id = 1,
        .width = 100,
        .height = 200,
        .head_height = 90,
        .face_x = 50,
        .talk_to_ids = &talks_to,
        .history = .{ .last_dir = true, .last_right = 77, .last_left = 88 },
    }};
    const roster = [_]original_layout.Body{
        .{ .id = 2, .width = 80, .height = 180, .head_height = 80, .face_x = 40 },
        .{ .id = 3, .width = 90, .height = 190, .head_height = 85, .face_x = 45 },
    };
    const expanded = try original_layout.addTalkTos(std.testing.allocator, &speakers, &roster);
    defer std.testing.allocator.free(expanded);
    try std.testing.expectEqual(@as(usize, 2), expanded.len);
    try std.testing.expectEqual(@as(u32, 1), expanded[0].id);
    try std.testing.expectEqual(@as(u32, 2), expanded[1].id);

    // UpdateHistoresis only overwrites a neighbor field when that neighbor
    // exists. A one-body panel therefore retains both historical edge values.
    const single = try original_layout.layoutAvatars(
        std.testing.allocator,
        &speakers,
        2300,
        2300,
        true,
    );
    defer std.testing.allocator.free(single);
    try std.testing.expectEqual(@as(u32, 77), single[0].history.last_right);
    try std.testing.expectEqual(@as(u32, 88), single[0].history.last_left);
    try std.testing.expect(single[0].history.last_dir);
}

test "AddLine panel preflight matches title repeat cap and forced breaks" {
    try std.testing.expect(original_layout.mustStartNewPanel(false, 1, 0, false));
    try std.testing.expect(!original_layout.mustStartNewPanel(false, 2, 4, false));
    try std.testing.expect(original_layout.mustStartNewPanel(false, 2, 5, false));
    try std.testing.expect(original_layout.mustStartNewPanel(false, 2, 1, true));
    try std.testing.expect(original_layout.mustStartNewPanel(true, 9, 0, false));
}

fn pose(layer: avb.PoseLayer, id: u32, emotion_index: u16, intensity: u8) avb.PoseRecord {
    return .{
        .layer = layer,
        .pose_id = id,
        .images = .{ .{}, .{}, .{} },
        .emotion_index = emotion_index,
        .intensity = intensity,
        .center = .{},
        .delta = .{},
        .face = .{},
    };
}

test "authored pose selection uses angle then intensity and exact gestures" {
    const records = [_]avb.PoseRecord{
        pose(.face, 1, 9, 0),
        pose(.face, 2, 1, 80),
        pose(.face, 3, 1, 140),
        pose(.face, 4, 2, 100),
        pose(.torso, 5, 9, 0),
        pose(.torso, 6, 10, 255),
        pose(.torso, 7, 14, 255),
    };

    // EM_NEUTRAL and EM_HAPPY are both angle zero in avatar.h. Intensity is
    // therefore the tie breaker, exactly as in GetHeadAndBodyFromEmotion.
    try std.testing.expectEqual(@as(u32, 2), bgb.selectPose(&records, .face, 9, 90).?.pose_id);
    // Gesture values (1001+) do not enter the emotion wheel; they require the
    // first authored exact index.
    try std.testing.expectEqual(@as(u32, 7), bgb.selectPose(&records, .torso, 14, 1).?.pose_id);
    try std.testing.expect(bgb.selectPose(&records, .torso, 12, 255) == null);
}

test "released AVB metadata and selected neck anchors stay byte exact" {
    const data = @embedFile("../assets/testdata/anna.avb");
    var table = try avb.parsePoseTable(std.testing.allocator, data);
    defer table.deinit(std.testing.allocator);

    try std.testing.expectEqual(avb.Kind.avatar, table.kind);
    try std.testing.expectEqual(@as(usize, 34), table.records.len);
    const first = table.records[0];
    try std.testing.expectEqual(avb.PoseLayer.face, first.layer);
    try std.testing.expectEqual(@as(u16, 9), first.emotion_index);
    try std.testing.expectEqual(@as(u8, 0), first.intensity);
    try std.testing.expectEqual(@as(i16, 92), first.center.x);
    try std.testing.expectEqual(@as(i16, 111), first.center.y);
    try std.testing.expectEqual(@as(i16, -4), first.delta.x);
    try std.testing.expectEqual(@as(i16, 3), first.delta.y);
    try std.testing.expectEqual(@as(i16, 106), first.face.x);
    try std.testing.expectEqual(@as(i16, 80), first.face.y);

    const anchors = bgb.neckAnchorsForEmotion(data, 9, 0, 9, 0).?;
    try std.testing.expectEqual(@as(i32, -3), anchors.body.x - anchors.head.x);
    try std.testing.expectEqual(@as(i32, -82), anchors.body.y - anchors.head.y);
}

test "figure dimensions use the selected authored head and torso anchors" {
    const allocator = std.testing.allocator;
    const data = @embedFile("../assets/testdata/anna.avb");
    var head = try bgb.decodePoseForEmotion(allocator, data, .face, 9, 0);
    defer head.deinit(allocator);
    var torso = try bgb.decodePoseForEmotion(allocator, data, .torso, 9, 0);
    defer torso.deinit(allocator);
    const anchors = bgb.neckAnchorsForEmotion(data, 9, 0, 9, 0).?;
    const dx = anchors.body.x - anchors.head.x;
    const dy = anchors.body.y - anchors.head.y;
    const torso_x = @max(@as(i32, 0), -dx);
    const torso_y = @max(@as(i32, 0), -dy);
    const head_x = torso_x + dx;
    const head_y = torso_y + dy;
    const expected_width: u32 = @intCast(@max(
        torso_x + @as(i32, @intCast(torso.width)),
        head_x + @as(i32, @intCast(head.width)),
    ));
    const expected_height: u32 = @intCast(@max(
        torso_y + @as(i32, @intCast(torso.height)),
        head_y + @as(i32, @intCast(head.height)),
    ));

    var assembled = try figure.assembleForText(allocator, data, "ordinary text");
    defer assembled.deinit(allocator);
    try std.testing.expectEqual(expected_width, assembled.width);
    try std.testing.expectEqual(expected_height, assembled.height);
}

const TestMetrics = struct {
    fn measure(_: *const anyopaque, text: []const u8) original_balloon.Size {
        return .{ .width = @as(i32, @intCast(text.len)) * 25, .height = 100 };
    }

    fn adapter(self: *const TestMetrics) original_balloon.TextMeasurer {
        return .{ .context = self, .measure_fn = measure };
    }
};

test "normal balloon construction applies original capitalization and seeded geometry" {
    const metrics = TestMetrics{};
    const inputs = [_]original_balloon.BalloonInput{.{
        .text = "Hi from source",
        .kind = .say,
        .arrow_x = 1100,
        .speaker_box = .{ .left = 800, .top = -700, .right = 1400, .bottom = -1500 },
    }};
    var first = try original_balloon.layoutPanel(
        std.testing.allocator,
        &inputs,
        .{ .left = 60, .top = -60, .right = 2240, .bottom = -1150 },
        1234,
        .{ .line_height = 100 },
        metrics.adapter(),
    );
    defer first.deinit(std.testing.allocator);
    var second = try original_balloon.layoutPanel(
        std.testing.allocator,
        &inputs,
        .{ .left = 60, .top = -60, .right = 2240, .bottom = -1150 },
        1234,
        .{ .line_height = 100 },
        metrics.adapter(),
    );
    defer second.deinit(std.testing.allocator);

    try std.testing.expectEqualStrings("HI FROM SOURCE", first.balloons[0].text);
    try std.testing.expectEqual(first.balloons[0].origin, second.balloons[0].origin);
    try std.testing.expectEqualSlices(
        original_balloon.Point,
        first.balloons[0].outline_points,
        second.balloons[0].outline_points,
    );
}
