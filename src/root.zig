//! comicchat library root — platform-independent core.
//!
//! Everything reachable from here is pure Zig with zero dependencies and no
//! C interop. The windowing/rendering/input backends (added later) build on
//! top of this and are the only OS-specific code in the project.

const std = @import("std");

pub const proto = struct {
    pub const record = @import("proto/record.zig");
};

pub const assets = struct {
    pub const avb = @import("assets/avb.zig");
    pub const bgb = @import("assets/bgb.zig");
};

pub const comic = struct {
    pub const figure = @import("comic/figure.zig");
    pub const strip = @import("comic/strip.zig");
    pub const layout = @import("comic/layout.zig");
};

pub const render = struct {
    pub const canvas = @import("render/canvas.zig");
    pub const font = @import("render/font.zig");
    pub const png = @import("render/png.zig");
};

pub const platform = struct {
    pub const x11 = @import("platform/x11.zig");
};

pub const net = struct {
    pub const message = @import("net/message.zig");
    pub const irc = @import("net/irc.zig");
    pub const transport = @import("net/transport.zig");
    pub const client = @import("net/client.zig");
};

test {
    // 0.16 dropped refAllDeclsRecursive; reference each module so its tests run.
    std.testing.refAllDecls(@This());
    _ = @import("proto/record.zig");
    _ = @import("assets/avb.zig");
    _ = @import("assets/bgb.zig");
    _ = @import("render/canvas.zig");
    _ = @import("comic/figure.zig");
    _ = @import("comic/strip.zig");
    _ = @import("comic/layout.zig");
    _ = @import("render/png.zig");
    _ = @import("platform/x11.zig");
    _ = @import("net/message.zig");
    _ = @import("net/irc.zig");
    _ = @import("net/transport.zig"); // compile-checked (no live socket test)
    _ = @import("net/client.zig");
}
