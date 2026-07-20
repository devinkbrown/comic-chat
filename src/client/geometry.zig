//! Source-derived geometry for the portable chat buffer. Values come from
//! the historical `chatview.cpp`, `spltchat.cpp`, `saywnd.cpp`, and `tabbar.cpp`.
//! The legacy client used device pixels at 96 DPI; callers scale the complete
//! result for other output scales rather than changing its proportions.

const std = @import("std");

pub const Rect = struct {
    x: i32,
    y: i32,
    w: i32,
    h: i32,

    pub fn right(self: Rect) i32 {
        return self.x + self.w;
    }

    pub fn bottom(self: Rect) i32 {
        return self.y + self.h;
    }
};

pub const menu_height: i32 = 34;
pub const toolbar_height: i32 = 46;
pub const tab_bar_height: i32 = 38;
pub const status_height: i32 = 26;
pub const splitter: i32 = 8;
pub const say_min_height: i32 = 54;
pub const say_button_size: i32 = 46;
pub const say_button_count: i32 = 5; // say, think, whisper, action, sound.
pub const client_percent: i32 = 80; // CSplitChatV::m_nPctLeft.
pub const member_percent: i32 = 30; // inverse of CSplitChat::m_nPctBottom=70.

pub const Layout = struct {
    menu: Rect,
    toolbar: Rect,
    tabs: Rect,
    buffer: Rect,
    transcript: Rect,
    say: Rect,
    say_editor: Rect,
    say_actions: Rect,
    say_action_size: i32,
    right: Rect,
    members: Rect,
    body_camera: Rect,
    status: Rect,

    pub fn compute(width: u32, height: u32, comic_mode: bool, show_members: bool) Layout {
        const w: i32 = @intCast(width);
        const h: i32 = @intCast(height);
        const status_y = @max(menu_height + toolbar_height + tab_bar_height, h - status_height);
        const buffer_y = menu_height + toolbar_height + tab_bar_height;
        const buffer_h = @max(0, status_y - buffer_y);
        const effective_members = show_members and w >= 760;
        const left_w = if (effective_members) @divTrunc(w * client_percent, 100) else w;
        const right_w = if (effective_members) @max(0, w - left_w - splitter) else 0;
        const say_h = @min(say_min_height, buffer_h);
        const transcript_h = @max(0, buffer_h - say_h - splitter);
        const action_size: i32 = if (left_w < 560) 38 else say_button_size;
        const action_w = @min(left_w, action_size * say_button_count);
        const right_x = if (effective_members) @min(w, left_w + splitter) else w;
        const member_h = if (!effective_members) 0 else if (comic_mode) @divTrunc(buffer_h * member_percent, 100) else buffer_h;
        const body_y = buffer_y + member_h + if (comic_mode) splitter else 0;
        const body_h = if (effective_members and comic_mode) @max(0, buffer_h - member_h - splitter) else 0;

        return .{
            .menu = .{ .x = 0, .y = 0, .w = w, .h = menu_height },
            .toolbar = .{ .x = 0, .y = menu_height, .w = w, .h = toolbar_height },
            .tabs = .{ .x = 0, .y = menu_height + toolbar_height, .w = w, .h = tab_bar_height },
            .buffer = .{ .x = 0, .y = buffer_y, .w = w, .h = buffer_h },
            .transcript = .{ .x = 0, .y = buffer_y, .w = left_w, .h = transcript_h },
            .say = .{ .x = 0, .y = buffer_y + transcript_h + splitter, .w = left_w, .h = say_h },
            .say_editor = .{ .x = 0, .y = buffer_y + transcript_h + splitter, .w = @max(0, left_w - action_w), .h = say_h },
            .say_actions = .{ .x = @max(0, left_w - action_w), .y = buffer_y + transcript_h + splitter, .w = action_w, .h = say_h },
            .say_action_size = action_size,
            .right = .{ .x = right_x, .y = buffer_y, .w = right_w, .h = buffer_h },
            .members = .{ .x = right_x, .y = buffer_y, .w = right_w, .h = member_h },
            .body_camera = .{ .x = right_x, .y = body_y, .w = right_w, .h = body_h },
            .status = .{ .x = 0, .y = status_y, .w = w, .h = @max(0, h - status_y) },
        };
    }
};

pub fn comicColumnControl(layout: Layout) Rect {
    return .{ .x = @max(layout.tabs.x + 300, layout.transcript.right() - 140), .y = layout.tabs.y + 6, .w = 128, .h = 26 };
}

pub fn comicColumnDecrease(layout: Layout) Rect {
    const control = comicColumnControl(layout);
    return .{ .x = control.x, .y = control.y, .w = 30, .h = control.h };
}

pub fn comicColumnIncrease(layout: Layout) Rect {
    const control = comicColumnControl(layout);
    return .{ .x = control.right() - 30, .y = control.y, .w = 30, .h = control.h };
}

test "comic buffer preserves source 80/20 and 30/70 split geometry" {
    const layout = Layout.compute(1000, 700, true, true);
    try std.testing.expectEqual(@as(i32, 800), layout.transcript.w);
    try std.testing.expectEqual(@as(i32, 192), layout.right.w);
    try std.testing.expectEqual(@as(i32, 54), layout.say.h);
    try std.testing.expectEqual(@as(i32, 230), layout.say_actions.w);
    try std.testing.expectEqual(@divTrunc(layout.buffer.h * 30, 100), layout.members.h);
    try std.testing.expectEqual(layout.buffer.h - layout.members.h - splitter, layout.body_camera.h);
}

test "text buffer gives the entire right pane to the member list" {
    const layout = Layout.compute(960, 720, false, true);
    try std.testing.expectEqual(layout.buffer.h, layout.members.h);
    try std.testing.expectEqual(@as(i32, 0), layout.body_camera.h);
}

test "hidden member pane returns the workspace to the chat buffer" {
    const layout = Layout.compute(960, 720, true, false);
    try std.testing.expectEqual(@as(i32, 960), layout.transcript.w);
    try std.testing.expectEqual(@as(i32, 0), layout.right.w);
    try std.testing.expectEqual(@as(i32, 0), layout.members.h);
    try std.testing.expectEqual(@as(i32, 0), layout.body_camera.h);
}

test "compact windows collapse the inspector and preserve a usable composer" {
    const layout = Layout.compute(640, 480, true, true);
    try std.testing.expectEqual(@as(i32, 0), layout.right.w);
    try std.testing.expect(layout.say_editor.w >= 400);
    try std.testing.expectEqual(say_button_size, layout.say_action_size);
}
