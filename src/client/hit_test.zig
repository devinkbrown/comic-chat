//! Stable hit targets for the modern desktop shell.

const std = @import("std");
const geometry = @import("geometry.zig");
const Canvas = @import("../render/canvas.zig").Canvas;

const Rect = geometry.Rect;

pub const Target = union(enum) {
    none,
    menu: u8,
    toolbar: u8,
    room_tab,
    transcript,
    composer,
    say_action: u8,
    member: usize,
    emotion,
};

pub fn contains(rect: Rect, x: i32, y: i32) bool {
    return x >= rect.x and y >= rect.y and x < rect.right() and y < rect.bottom();
}

pub fn shell(layout: geometry.Layout, comic_mode: bool, x: i32, y: i32, member_count: usize) Target {
    if (contains(layout.menu, x, y)) return .{ .menu = menuIndex(x) orelse return .none };
    if (contains(layout.toolbar, x, y)) return .{ .toolbar = toolbarIndex(layout.toolbar, x) orelse return .none };
    if (contains(layout.tabs, x, y)) return .room_tab;
    if (contains(layout.say_editor, x, y)) return .composer;
    if (contains(layout.say_actions, x, y)) {
        const index = @divTrunc(x - layout.say_actions.x, geometry.say_button_size);
        if (index >= 0 and index < geometry.say_button_count) return .{ .say_action = @intCast(index) };
    }
    if (contains(layout.transcript, x, y)) return .transcript;
    if (contains(layout.members, x, y)) {
        const index: usize = if (comic_mode) iconIndex(layout.members, x, y) else @intCast(@max(0, @divTrunc(y - layout.members.y - 7, 24)));
        if (index < member_count) return .{ .member = index };
        return .none;
    }
    if (comic_mode and contains(layout.body_camera, x, y)) return .emotion;
    return .none;
}

fn menuIndex(pointer_x: i32) ?u8 {
    const items = [_][]const u8{ "File", "Edit", "View", "Format", "Room", "Member", "More" };
    var x: i32 = 12;
    for (items, 0..) |item, index| {
        const right = x + Canvas.textWidth(item) + 28;
        if (pointer_x >= x and pointer_x < right) return @intCast(index);
        x = right;
    }
    return null;
}

fn toolbarIndex(rect: Rect, x: i32) ?u8 {
    if (rect.w < 760) {
        const compact = [_]u8{ 0, 2, 4, 5, 6, 7, 8, 10, 11, 13, 17, 18 };
        const raw = @divTrunc(x - 8, 28);
        if (raw < 0 or raw >= compact.len) return null;
        return compact[@intCast(raw)];
    }
    const starts = [_]i32{
        5,   29,  53,  77,  101, 133, 157, 189, 213, 245, 277, 301,
        325, 349, 381, 405, 429, 461, 485, 509, 533, 557, 581, 605,
    };
    for (starts, 0..) |start, index| if (x >= start and x < start + 24) return @intCast(index);
    return null;
}

fn iconIndex(rect: Rect, x: i32, y: i32) usize {
    const columns: i32 = @max(1, @divTrunc(rect.w, 72));
    const column = @max(0, @divTrunc(x - rect.x, 72));
    const row = @max(0, @divTrunc(y - rect.y, 68));
    return @intCast(row * columns + column);
}

test "source shell hit targets distinguish controls and content" {
    const layout = geometry.Layout.compute(960, 720, true, true);
    try std.testing.expectEqual(Target{ .toolbar = 5 }, shell(layout, true, 140, 30, 3));
    try std.testing.expectEqual(Target{ .menu = 1 }, shell(layout, true, 80, 10, 3));
    try std.testing.expectEqual(Target{ .say_action = 0 }, shell(layout, true, layout.say_actions.x + 2, layout.say.y + 2, 3));
    try std.testing.expectEqual(Target{ .member = 0 }, shell(layout, true, layout.members.x + 3, layout.members.y + 3, 3));
    try std.testing.expectEqual(Target.emotion, shell(layout, true, layout.body_camera.x + 3, layout.body_camera.y + 3, 3));
}
