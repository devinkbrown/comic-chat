//! Platform-neutral semantic snapshot for the software-rendered UI.
//!
//! Native adapters can translate this stable tree to UIA, AT-SPI, or a test
//! harness without inspecting framebuffer pixels.

const std = @import("std");
const geometry = @import("geometry.zig");

pub const Role = enum {
    window,
    menu_bar,
    toolbar,
    tab_list,
    tab,
    transcript,
    member_list,
    composer,
    say_action,
    status,
    dialog,
    button,
    menu,
    menu_item,
    input,
    combo_box,
    list_item,
};

pub const Node = struct {
    id: []const u8,
    role: Role,
    bounds: geometry.Rect,
    label: []const u8,
    selected: bool = false,
    focused: bool = false,
    enabled: bool = true,
};

pub const Snapshot = struct {
    pub const max_nodes = 256;
    nodes: [max_nodes]Node = undefined,
    len: usize = 0,
    status: []const u8 = "",
    truncated: bool = false,

    pub fn append(self: *Snapshot, node: Node) void {
        if (self.len >= self.nodes.len) {
            self.truncated = true;
            return;
        }
        self.nodes[self.len] = node;
        self.len += 1;
    }

    pub fn items(self: *const Snapshot) []const Node {
        return self.nodes[0..self.len];
    }
};

test "semantic snapshots retain stable roles and bounds" {
    var snapshot: Snapshot = .{};
    snapshot.append(.{ .id = "composer", .role = .composer, .bounds = .{ .x = 1, .y = 2, .w = 3, .h = 4 }, .label = "Message" });
    try std.testing.expectEqual(@as(usize, 1), snapshot.items().len);
    try std.testing.expectEqual(Role.composer, snapshot.items()[0].role);
}
