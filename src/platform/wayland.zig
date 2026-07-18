//! Minimal native Wayland window backend, implemented directly on the wire.
//!
//! This module has no libwayland, libxkbcommon, C import, or XWayland
//! dependency. It binds the core globals and stable xdg-shell v1, presents the
//! shared software framebuffer through mmap-backed ARGB8888 `wl_buffer`s, and
//! translates keyboard events into the same public event shape as the X11
//! backend.
//!
//! Keyboard: the compositor-provided XKB keymap fd is received (see
//! `xkb.zig`'s bounded text-format parser) and drives translation for the
//! configured layout's base and Shift levels — non-US layouts now produce
//! their real characters, not a hardcoded US table. Client-side key repeat
//! (Wayland deliberately leaves this to the client, unlike X11's native
//! auto-repeat) is implemented via `repeat_info` + `Window.checkRepeat`.
//!
//! Remaining keyboard limitation: AltGr/ISO Level3 and other multi-level
//! layouts, compose/dead-key sequences, and IME input are not represented —
//! see `xkb.zig`'s module doc for the precise parsing scope and why. A key
//! whose keymap entry falls outside that scope, or any key before the first
//! keymap event arrives, falls back to the hardcoded US evdev table below.

const std = @import("std");
const linux = std.os.linux;
const posix = std.posix;
const net = std.Io.net;
const xkb = @import("xkb.zig");

const wl_display: u32 = 1;
const max_message_size: usize = 1024 * 1024;
const shm_argb8888: u32 = 0;
const seat_keyboard: u32 = 1 << 1;

// Request opcodes from wayland.xml and stable/xdg-shell/xdg-shell.xml.
const display_sync: u16 = 0;
const display_get_registry: u16 = 1;
const registry_bind: u16 = 0;
const compositor_create_surface: u16 = 0;
const shm_create_pool: u16 = 0;
const shm_pool_create_buffer: u16 = 0;
const shm_pool_destroy: u16 = 1;
const buffer_destroy: u16 = 0;
const surface_destroy: u16 = 0;
const surface_attach: u16 = 1;
const surface_damage: u16 = 2;
const surface_commit: u16 = 6;
const surface_damage_buffer: u16 = 9;
const seat_get_keyboard: u16 = 1;
const seat_release: u16 = 3;
const keyboard_release: u16 = 0;
const xdg_wm_base_destroy: u16 = 0;
const xdg_wm_base_get_xdg_surface: u16 = 2;
const xdg_wm_base_pong: u16 = 3;
const xdg_surface_destroy: u16 = 0;
const xdg_surface_get_toplevel: u16 = 1;
const xdg_surface_ack_configure: u16 = 4;
const xdg_toplevel_destroy: u16 = 0;
const xdg_toplevel_set_title: u16 = 2;
const xdg_toplevel_set_app_id: u16 = 3;

pub const Key = union(enum) {
    char: u8,
    backspace,
    enter,
    escape,
    tab,
    left,
    right,
    up,
    down,
    home,
    end,
    page_up,
    page_down,
    delete,
    other,
};

pub const Event = union(enum) {
    key: Key,
    resize: struct { w: u32, h: u32 },
    expose,
    close,
    other,
};

const Global = struct {
    name: u32 = 0,
    version: u32 = 0,
};

const Globals = struct {
    compositor: Global = .{},
    shm: Global = .{},
    seat: Global = .{},
    xdg_wm_base: Global = .{},

    fn record(self: *Globals, name: u32, interface: []const u8, version: u32) void {
        const value = Global{ .name = name, .version = version };
        if (std.mem.eql(u8, interface, "wl_compositor") and self.compositor.name == 0) {
            self.compositor = value;
        } else if (std.mem.eql(u8, interface, "wl_shm") and self.shm.name == 0) {
            self.shm = value;
        } else if (std.mem.eql(u8, interface, "wl_seat") and self.seat.name == 0) {
            self.seat = value;
        } else if (std.mem.eql(u8, interface, "xdg_wm_base") and self.xdg_wm_base.name == 0) {
            self.xdg_wm_base = value;
        }
    }
};

const Message = struct {
    object: u32,
    opcode: u16,
    body: []u8,

    fn deinit(self: Message, gpa: std.mem.Allocator) void {
        gpa.free(self.body);
    }
};

const Connection = struct {
    io: std.Io,
    stream: net.Stream,
    next_id: u32 = 2,
    /// A file descriptor delivered by SCM_RIGHTS on the most recent read that
    /// has not yet been claimed by a specific event handler (e.g.
    /// wl_keyboard.keymap). Wayland attaches at most one fd per message this
    /// client parses, and the handler that expects one claims it via
    /// `takePendingFd` in the same dispatch turn that read it.
    pending_fd: ?posix.fd_t = null,

    fn allocId(self: *Connection) !u32 {
        if (self.next_id >= 0xff000000) return error.ObjectIdsExhausted;
        const id = self.next_id;
        self.next_id += 1;
        return id;
    }

    /// Returns and clears the fd captured by the most recent `readExact`, if
    /// any. Closes and discards a stale unclaimed fd from an earlier message
    /// before returning the new one, since this client only ever expects one
    /// fd in flight at a time.
    fn takePendingFd(self: *Connection) ?posix.fd_t {
        const fd_value = self.pending_fd;
        self.pending_fd = null;
        return fd_value;
    }

    fn writeAll(self: *Connection, bytes: []const u8) !void {
        var off: usize = 0;
        while (off < bytes.len) {
            const n = try self.io.vtable.netWrite(
                self.io.userdata,
                self.stream.socket.handle,
                "",
                &[_][]const u8{bytes[off..]},
                1,
            );
            if (n == 0) return error.WriteZero;
            off += n;
        }
    }

    /// Every message read goes through raw `recvmsg`, not the `Io` vtable, so
    /// an SCM_RIGHTS fd attached to any byte in this read (the compositor
    /// sends wl_keyboard.keymap's fd alongside its 16-byte wire message) is
    /// captured rather than silently dropped by a plain `read`/`recv`. This
    /// mirrors `writeWithFd`'s raw-syscall approach on the send side.
    fn readExact(self: *Connection, dst: []u8) !void {
        const header_space = comptime alignForward(@sizeOf(linux.cmsghdr), @alignOf(linux.cmsghdr));
        const control_space = comptime header_space + alignForward(@sizeOf(i32), @alignOf(linux.cmsghdr));

        var off: usize = 0;
        while (off < dst.len) {
            var control: [control_space]u8 align(@alignOf(linux.cmsghdr)) = @splat(0);
            var iov: posix.iovec = .{ .base = dst[off..].ptr, .len = dst.len - off };
            var msg: linux.msghdr = .{
                .name = null,
                .namelen = 0,
                .iov = (&iov)[0..1],
                .iovlen = 1,
                .control = &control,
                .controllen = control.len,
                .flags = 0,
            };

            const n: usize = while (true) {
                const rc = linux.recvmsg(self.stream.socket.handle, &msg, linux.MSG.CMSG_CLOEXEC);
                switch (linux.errno(rc)) {
                    .SUCCESS => break @intCast(rc),
                    .INTR => continue,
                    .AGAIN => return error.WouldBlock,
                    .CONNRESET, .NOTCONN => return error.ConnectionResetByPeer,
                    else => return error.ReadFailed,
                }
            };
            if (n == 0) return error.EndOfStream;
            off += n;

            if (msg.controllen >= header_space + @sizeOf(i32)) {
                const cmsg: *const linux.cmsghdr = @ptrCast(&control);
                if (cmsg.level == linux.SOL.SOCKET and cmsg.type == linux.SCM.RIGHTS) {
                    const received: i32 = @as(*const i32, @ptrCast(@alignCast(control[header_space..].ptr))).*;
                    if (self.pending_fd) |stale| _ = linux.close(stale);
                    self.pending_fd = received;
                }
            }
        }
    }

    fn readMessage(self: *Connection, gpa: std.mem.Allocator) !Message {
        var wire_header: [8]u8 = undefined;
        try self.readExact(&wire_header);
        const object = get32(wire_header[0..4]);
        const word = get32(wire_header[4..8]);
        const opcode: u16 = @intCast(word & 0xffff);
        const size: usize = @intCast(word >> 16);
        if (object == 0 or size < 8 or (size & 3) != 0 or size > max_message_size) {
            return error.InvalidWaylandMessage;
        }
        const body = try gpa.alloc(u8, size - 8);
        errdefer gpa.free(body);
        try self.readExact(body);
        return .{ .object = object, .opcode = opcode, .body = body };
    }

    /// Send a Wayland request whose signature contains one `fd` argument.
    /// File descriptors consume no bytes in the Wayland wire payload and are
    /// attached to the first byte with SCM_RIGHTS.
    fn writeWithFd(self: *Connection, bytes: []const u8, fd_value: i32) !void {
        const header_space = comptime alignForward(@sizeOf(linux.cmsghdr), @alignOf(linux.cmsghdr));
        const control_space = comptime header_space + alignForward(@sizeOf(i32), @alignOf(linux.cmsghdr));
        var control: [control_space]u8 align(@alignOf(linux.cmsghdr)) = @splat(0);

        const cmsg: *linux.cmsghdr = @ptrCast(&control);
        cmsg.* = .{
            .len = header_space + @sizeOf(i32),
            .level = linux.SOL.SOCKET,
            .type = linux.SCM.RIGHTS,
        };
        std.mem.writeInt(i32, control[header_space .. header_space + @sizeOf(i32)], fd_value, .native);

        var iov: posix.iovec_const = .{ .base = bytes.ptr, .len = bytes.len };
        const msg: linux.msghdr_const = .{
            .name = null,
            .namelen = 0,
            .iov = (&iov)[0..1],
            .iovlen = 1,
            .control = &control,
            .controllen = control.len,
            .flags = 0,
        };

        var sent: usize = 0;
        while (true) {
            const rc = linux.sendmsg(self.stream.socket.handle, &msg, linux.MSG.NOSIGNAL);
            switch (linux.errno(rc)) {
                .SUCCESS => {
                    sent = @intCast(rc);
                    break;
                },
                .INTR => continue,
                .AGAIN => return error.WouldBlock,
                .PIPE, .NOTCONN => return error.ConnectionResetByPeer,
                .NOBUFS, .NOMEM => return error.SystemResources,
                else => return error.SendFdFailed,
            }
        }
        if (sent == 0) return error.WriteZero;
        if (sent < bytes.len) try self.writeAll(bytes[sent..]);
    }
};

const Buffer = struct {
    id: u32,
    width: u32,
    height: u32,
    memory: []align(std.heap.page_size_min) u8,
    busy: bool = false,

    fn deinit(self: *Buffer) void {
        posix.munmap(self.memory);
        self.* = undefined;
    }
};

/// Open a native Wayland window, display an image, and wait for a key or close.
pub fn show(gpa: std.mem.Allocator, pixels: []const u32, w: u32, h: u32) !void {
    const window = try Window.open(gpa, w, h, "comicchat");
    defer window.deinit();
    try window.present(pixels, w, h);
    while (true) {
        switch (try window.nextEvent()) {
            .key, .close => return,
            .expose => try window.present(pixels, w, h),
            else => {},
        }
    }
}

/// A heap-pinned native Wayland xdg-toplevel.
pub const Window = struct {
    gpa: std.mem.Allocator,
    threaded: std.Io.Threaded,
    conn: Connection,

    registry_id: u32 = 0,
    compositor_id: u32 = 0,
    compositor_version: u32 = 0,
    shm_id: u32 = 0,
    seat_id: u32 = 0,
    seat_version: u32 = 0,
    keyboard_id: u32 = 0,
    surface_id: u32 = 0,
    xdg_wm_base_id: u32 = 0,
    xdg_surface_id: u32 = 0,
    xdg_toplevel_id: u32 = 0,

    width: u32,
    height: u32,
    pending_width: i32 = 0,
    pending_height: i32 = 0,
    configured: bool = false,
    argb_supported: bool = false,
    shift_left: bool = false,
    shift_right: bool = false,
    caps_lock: bool = false,
    /// The compositor's layout, once a keymap event with a supported format
    /// has been received and successfully parsed. Null before that (falls
    /// back to evdevToKey's hardcoded US table) and if the compositor sent
    /// an unsupported format or a keymap this bounded parser could not read.
    xkb_keymap: ?xkb.Keymap = null,
    /// Repeats per second and initial hold delay from the compositor's
    /// repeat_info (wl_keyboard v4+); non-positive rate means repeat is
    /// disabled entirely, matching the Wayland protocol's own convention.
    repeat_rate_per_sec: i32 = 0,
    repeat_delay_ms: i32 = 0,
    /// The evdev code and effective shift state of the one non-modifier key
    /// currently held, so `checkRepeat` can keep synthesizing key events
    /// without a matching wire message for each one — Wayland deliberately
    /// leaves repeat entirely to the client (see the module doc).
    held_key_code: ?u32 = null,
    held_key_shift: bool = false,
    next_repeat_at_ms: u64 = 0,
    buffers: std.ArrayList(Buffer) = .empty,

    pub fn open(gpa: std.mem.Allocator, w: u32, h: u32, title: []const u8) !*Window {
        if (w == 0 or h == 0 or w > std.math.maxInt(i32) or h > std.math.maxInt(i32)) {
            return error.InvalidWindowSize;
        }
        const socket_path = try waylandSocketPath(gpa);
        defer gpa.free(socket_path);

        const self = try gpa.create(Window);
        errdefer gpa.destroy(self);
        self.gpa = gpa;
        self.threaded = std.Io.Threaded.init(gpa, .{});
        errdefer self.threaded.deinit();

        const io = self.threaded.io();
        const stream = try openUnixSocket(io, socket_path);
        errdefer stream.close(io);
        self.conn = .{ .io = io, .stream = stream };
        self.registry_id = 0;
        self.compositor_id = 0;
        self.compositor_version = 0;
        self.shm_id = 0;
        self.seat_id = 0;
        self.seat_version = 0;
        self.keyboard_id = 0;
        self.surface_id = 0;
        self.xdg_wm_base_id = 0;
        self.xdg_surface_id = 0;
        self.xdg_toplevel_id = 0;
        self.width = w;
        self.height = h;
        self.pending_width = 0;
        self.pending_height = 0;
        self.configured = false;
        self.argb_supported = false;
        self.shift_left = false;
        self.shift_right = false;
        self.caps_lock = false;
        self.buffers = .empty;

        var globals: Globals = .{};
        try self.discoverGlobals(&globals);
        if (globals.compositor.name == 0) return error.MissingWaylandCompositor;
        if (globals.shm.name == 0) return error.MissingWaylandShm;
        if (globals.seat.name == 0) return error.MissingWaylandSeat;
        if (globals.xdg_wm_base.name == 0) return error.MissingXdgWmBase;

        self.compositor_id = try self.conn.allocId();
        self.compositor_version = @min(globals.compositor.version, 4);
        try sendBind(&self.conn, self.gpa, self.registry_id, globals.compositor, "wl_compositor", self.compositor_version, self.compositor_id);

        self.shm_id = try self.conn.allocId();
        try sendBind(&self.conn, self.gpa, self.registry_id, globals.shm, "wl_shm", 1, self.shm_id);

        self.seat_id = try self.conn.allocId();
        self.seat_version = @min(globals.seat.version, 5);
        try sendBind(&self.conn, self.gpa, self.registry_id, globals.seat, "wl_seat", self.seat_version, self.seat_id);

        self.xdg_wm_base_id = try self.conn.allocId();
        try sendBind(&self.conn, self.gpa, self.registry_id, globals.xdg_wm_base, "xdg_wm_base", 1, self.xdg_wm_base_id);

        self.surface_id = try self.conn.allocId();
        try sendOneU32(&self.conn, self.compositor_id, compositor_create_surface, self.surface_id);
        self.xdg_surface_id = try self.conn.allocId();
        try sendTwoU32(&self.conn, self.xdg_wm_base_id, xdg_wm_base_get_xdg_surface, self.xdg_surface_id, self.surface_id);
        self.xdg_toplevel_id = try self.conn.allocId();
        try sendOneU32(&self.conn, self.xdg_surface_id, xdg_surface_get_toplevel, self.xdg_toplevel_id);
        try sendString(&self.conn, self.gpa, self.xdg_toplevel_id, xdg_toplevel_set_title, title);
        try sendString(&self.conn, self.gpa, self.xdg_toplevel_id, xdg_toplevel_set_app_id, "comicchat");

        // xdg-shell forbids attaching a buffer before this initial, empty
        // commit has elicited a configure which the client acknowledges.
        try sendEmpty(&self.conn, self.surface_id, surface_commit);
        while (!self.configured) {
            const msg = try self.conn.readMessage(self.gpa);
            defer msg.deinit(self.gpa);
            _ = try self.dispatch(msg);
        }
        if (!self.argb_supported) return error.Argb8888Unsupported;
        return self;
    }

    pub fn deinit(self: *Window) void {
        self.destroyProtocolObjects() catch {};
        self.conn.stream.close(self.conn.io);
        if (self.xkb_keymap) |*keymap| keymap.deinit();
        for (self.buffers.items) |*buffer| buffer.deinit();
        self.buffers.deinit(self.gpa);
        self.threaded.deinit();
        self.gpa.destroy(self);
    }

    /// Pollable Wayland connection socket.
    pub fn fd(self: *const Window) i32 {
        return self.conn.stream.socket.handle;
    }

    /// Commit a full 0xAARRGGBB frame through a reusable ARGB8888 wl_buffer.
    pub fn present(self: *Window, pixels: []const u32, w: u32, h: u32) !void {
        if (!self.configured) return error.SurfaceNotConfigured;
        if (w == 0 or h == 0 or w > std.math.maxInt(i32) or h > std.math.maxInt(i32)) {
            return error.InvalidWindowSize;
        }
        const count = try std.math.mul(usize, @as(usize, w), @as(usize, h));
        if (pixels.len != count) return error.BadFramebufferSize;

        self.discardIdleBuffersExcept(w, h);
        var index: ?usize = null;
        for (self.buffers.items, 0..) |buffer, i| {
            if (!buffer.busy and buffer.width == w and buffer.height == h) {
                index = i;
                break;
            }
        }
        if (index == null) {
            try self.buffers.append(self.gpa, try self.createBuffer(w, h));
            index = self.buffers.items.len - 1;
        }
        const buffer = &self.buffers.items[index.?];
        const bytes = std.mem.sliceAsBytes(pixels);
        @memcpy(buffer.memory[0..bytes.len], bytes);
        buffer.busy = true;

        try sendAttach(&self.conn, self.surface_id, buffer.id);
        if (self.compositor_version >= 4) {
            try sendDamage(&self.conn, self.surface_id, surface_damage_buffer, w, h);
        } else {
            try sendDamage(&self.conn, self.surface_id, surface_damage, w, h);
        }
        try sendEmpty(&self.conn, self.surface_id, surface_commit);
    }

    /// Read and dispatch exactly one wire event. Protocol-only events (buffer
    /// release, ping, keymap, seat name) are handled internally and reported
    /// as `.other`. Keeping this one-message boundary is required by poll-based
    /// callers: a release can be the only readable message, and waiting here
    /// for a later visible event would starve the IRC socket indefinitely.
    pub fn nextEvent(self: *Window) !Event {
        const msg = try self.conn.readMessage(self.gpa);
        defer msg.deinit(self.gpa);
        return try self.dispatch(msg) orelse .other;
    }

    /// Synthesizes the next key-repeat event, if a key is held and its
    /// repeat interval has elapsed. Unlike X11 (which gets repeat for free
    /// from the X server's own auto-repeat), Wayland deliberately leaves
    /// this entirely to the client (see the module doc) — callers must poll
    /// this on every loop tick, not only when the compositor socket has
    /// data ready, since a repeat fires with no new wire message at all.
    /// Always recomputes the next deadline from the current time rather
    /// than accumulating fixed steps, so a delayed poll loop does not fire
    /// a burst of catch-up repeats once it resumes.
    pub fn checkRepeat(self: *Window) ?Event {
        const code = self.held_key_code orelse return null;
        if (self.repeat_rate_per_sec <= 0) return null;
        const now = nowMs(self.conn.io);
        if (now < self.next_repeat_at_ms) return null;
        const interval_ms: u64 = @intCast(@max(1, @divTrunc(1000, self.repeat_rate_per_sec)));
        self.next_repeat_at_ms = now +| interval_ms;
        return .{ .key = self.translateKey(code, self.held_key_shift) };
    }

    fn discoverGlobals(self: *Window, globals: *Globals) !void {
        self.registry_id = try self.conn.allocId();
        try sendOneU32(&self.conn, wl_display, display_get_registry, self.registry_id);
        const callback = try self.conn.allocId();
        try sendOneU32(&self.conn, wl_display, display_sync, callback);

        while (true) {
            const msg = try self.conn.readMessage(self.gpa);
            defer msg.deinit(self.gpa);
            if (msg.object == wl_display and msg.opcode == 0) return error.WaylandProtocolError;
            if (msg.object == self.registry_id and msg.opcode == 0) {
                const global = try parseRegistryGlobal(msg.body);
                globals.record(global.name, global.interface, global.version);
            } else if (msg.object == callback and msg.opcode == 0) {
                if (msg.body.len != 4) return error.InvalidWaylandMessage;
                return;
            }
        }
    }

    fn dispatch(self: *Window, msg: Message) !?Event {
        if (msg.object == wl_display) {
            if (msg.opcode == 0) return error.WaylandProtocolError;
            return null; // delete_id
        }
        if (msg.object == self.xdg_wm_base_id) {
            if (msg.opcode == 0) {
                if (msg.body.len != 4) return error.InvalidWaylandMessage;
                try sendOneU32(&self.conn, self.xdg_wm_base_id, xdg_wm_base_pong, get32(msg.body));
            }
            return null;
        }
        if (msg.object == self.shm_id) {
            if (msg.opcode == 0) {
                if (msg.body.len != 4) return error.InvalidWaylandMessage;
                if (get32(msg.body) == shm_argb8888) self.argb_supported = true;
            }
            return null;
        }
        if (msg.object == self.seat_id) {
            if (msg.opcode == 0) {
                if (msg.body.len != 4) return error.InvalidWaylandMessage;
                try self.updateSeatCapabilities(get32(msg.body));
            }
            return null;
        }
        if (self.keyboard_id != 0 and msg.object == self.keyboard_id) {
            return try self.keyboardEvent(msg.opcode, msg.body);
        }
        if (msg.object == self.xdg_toplevel_id) {
            switch (msg.opcode) {
                0 => {
                    if (msg.body.len < 12) return error.InvalidWaylandMessage;
                    self.pending_width = getI32(msg.body[0..4]);
                    self.pending_height = getI32(msg.body[4..8]);
                    const array_len: usize = @intCast(get32(msg.body[8..12]));
                    if (array_len > msg.body.len - 12) return error.InvalidWaylandMessage;
                    if (12 + pad4(array_len) != msg.body.len) return error.InvalidWaylandMessage;
                    return null;
                },
                1 => return Event.close,
                else => return null,
            }
        }
        if (msg.object == self.xdg_surface_id) {
            if (msg.opcode != 0) return null;
            if (msg.body.len != 4) return error.InvalidWaylandMessage;
            try sendOneU32(&self.conn, self.xdg_surface_id, xdg_surface_ack_configure, get32(msg.body));
            const old_w = self.width;
            const old_h = self.height;
            if (self.pending_width > 0) self.width = @intCast(self.pending_width);
            if (self.pending_height > 0) self.height = @intCast(self.pending_height);
            self.pending_width = 0;
            self.pending_height = 0;
            self.configured = true;
            if (self.width != old_w or self.height != old_h) {
                return .{ .resize = .{ .w = self.width, .h = self.height } };
            }
            return Event.expose;
        }
        if (msg.object == self.surface_id) return null;
        for (self.buffers.items) |*buffer| {
            if (msg.object == buffer.id) {
                if (msg.opcode == 0) buffer.busy = false;
                return null;
            }
        }
        return null;
    }

    fn updateSeatCapabilities(self: *Window, capabilities: u32) !void {
        if ((capabilities & seat_keyboard) != 0) {
            if (self.keyboard_id == 0) {
                self.keyboard_id = try self.conn.allocId();
                try sendOneU32(&self.conn, self.seat_id, seat_get_keyboard, self.keyboard_id);
            }
        } else if (self.keyboard_id != 0) {
            if (self.seat_version >= 3) try sendEmpty(&self.conn, self.keyboard_id, keyboard_release);
            self.keyboard_id = 0;
            self.shift_left = false;
            self.shift_right = false;
        }
    }

    fn keyboardEvent(self: *Window, opcode: u16, body: []const u8) !?Event {
        switch (opcode) {
            0 => { // keymap(format, fd, size); fd is ancillary, not in body
                if (body.len != 8) return error.InvalidWaylandMessage;
                const format = get32(body[0..4]);
                const size = get32(body[4..8]);
                if (self.conn.takePendingFd()) |fd_value| {
                    defer _ = linux.close(fd_value);
                    if (self.loadKeymap(fd_value, format, size)) |parsed| {
                        if (self.xkb_keymap) |*old| old.deinit();
                        self.xkb_keymap = parsed;
                    } else |_| {
                        // Unsupported format or a malformed keymap this bounded
                        // parser cannot read: keep whatever keymap (or none) we
                        // already had rather than failing the connection over a
                        // layout we cannot represent. evdevToKey remains the
                        // fallback either way.
                    }
                }
                return null;
            },
            1 => { // enter(serial, surface, keys array)
                if (body.len < 12) return error.InvalidWaylandMessage;
                const keys_len: usize = @intCast(get32(body[8..12]));
                if (12 + pad4(keys_len) != body.len) return error.InvalidWaylandMessage;
                return null;
            },
            2 => { // leave
                if (body.len != 8) return error.InvalidWaylandMessage;
                self.shift_left = false;
                self.shift_right = false;
                self.held_key_code = null;
                return null;
            },
            3 => { // key(serial, time, key, state)
                if (body.len != 16) return error.InvalidWaylandMessage;
                const code = get32(body[8..12]);
                const state = get32(body[12..16]);
                const down = state != 0;
                switch (code) {
                    42 => self.shift_left = down,
                    54 => self.shift_right = down,
                    58 => if (state == 1) {
                        self.caps_lock = !self.caps_lock;
                    },
                    else => {},
                }
                if (code != 42 and code != 54 and code != 58) {
                    if (down) {
                        self.held_key_code = code;
                        self.held_key_shift = self.shift_left or self.shift_right;
                        self.next_repeat_at_ms = nowMs(self.conn.io) +| @as(u64, @intCast(@max(0, self.repeat_delay_ms)));
                    } else if (self.held_key_code == code) {
                        self.held_key_code = null;
                    }
                }
                if (!down or code == 42 or code == 54 or code == 58) return null;
                return .{ .key = self.translateKey(code, self.shift_left or self.shift_right) };
            },
            4 => { // modifiers(serial, depressed, latched, locked, group)
                if (body.len != 20) return error.InvalidWaylandMessage;
                // Modifier bit positions are defined by the compositor's XKB
                // keymap. This dependency-free backend intentionally ignores
                // that keymap, so retain the physical evdev key state above
                // instead of guessing Shift/Caps bit indices.
                return null;
            },
            5 => { // repeat_info(rate, delay), available because the seat is bound at v4+
                if (body.len != 8) return error.InvalidWaylandMessage;
                self.repeat_rate_per_sec = getI32(body[0..4]);
                self.repeat_delay_ms = getI32(body[4..8]);
                return null;
            },
            else => return null,
        }
    }

    /// mmaps and parses a compositor-supplied keymap fd. The mapping is
    /// unmapped before returning either way: `xkb.parse` copies everything
    /// it needs into the returned `Keymap`'s own arena, so the raw text is
    /// not needed past this call.
    fn loadKeymap(self: *Window, fd_value: posix.fd_t, format: u32, size: u32) !xkb.Keymap {
        if (size == 0) return error.EmptyKeymap;
        const mapped = try posix.mmap(null, size, .{ .READ = true }, .{ .TYPE = .PRIVATE }, fd_value, 0);
        defer posix.munmap(mapped);
        const text_len = std.mem.indexOfScalar(u8, mapped, 0) orelse mapped.len;
        return xkb.parse(self.gpa, format, mapped[0..text_len]);
    }

    /// Translates one physical key press using the compositor's real layout
    /// when available, falling back to evdevToKey's US table otherwise (no
    /// keymap yet, an unsupported/unparseable one, or a keysym this bounded
    /// translator does not represent).
    ///
    /// Caps Lock only affects alphabetic keys (matching evdevToKey's
    /// existing shift-XOR-caps behavior for letters): the key's own
    /// unshifted keysym decides whether it is one, then the *effective*
    /// shift state — physical shift XOR caps lock for a letter, physical
    /// shift alone otherwise — selects which of the keymap's two levels to
    /// read, rather than hand-flipping ASCII case after the fact (which
    /// would silently be wrong for a layout where the shifted symbol is not
    /// simply the base character's uppercase form).
    fn translateKey(self: *Window, code: u32, shift: bool) Key {
        if (self.xkb_keymap) |*keymap| {
            if (keymap.keysymFor(code, false)) |base_keysym| {
                const is_letter = base_keysym.len == 1 and std.ascii.isAlphabetic(base_keysym[0]);
                const effective_shift = if (is_letter) (shift != self.caps_lock) else shift;
                if (keymap.keysymFor(code, effective_shift)) |keysym| {
                    if (xkb.charForKeysym(keysym)) |ch| return .{ .char = ch };
                    if (xkb.namedKeyForKeysym(keysym)) |named| return namedKeyToKey(named);
                }
            }
        }
        return evdevToKey(code, shift, self.caps_lock);
    }

    fn createBuffer(self: *Window, w: u32, h: u32) !Buffer {
        const stride = try std.math.mul(usize, @as(usize, w), 4);
        const byte_len = try std.math.mul(usize, stride, @as(usize, h));
        if (stride > std.math.maxInt(i32) or byte_len > std.math.maxInt(i32)) return error.FramebufferTooLarge;

        const fd_value = try posix.memfd_create("comicchat-wayland", linux.MFD.CLOEXEC);
        defer _ = linux.close(fd_value);
        try truncateFd(fd_value, @intCast(byte_len));
        const memory = try posix.mmap(
            null,
            byte_len,
            .{ .READ = true, .WRITE = true },
            .{ .TYPE = .SHARED },
            fd_value,
            0,
        );
        errdefer posix.munmap(memory);

        const pool_id = try self.conn.allocId();
        var pool_req: [16]u8 = @splat(0);
        header(&pool_req, self.shm_id, shm_create_pool);
        put32(pool_req[8..12], pool_id);
        putI32(pool_req[12..16], @intCast(byte_len));
        try self.conn.writeWithFd(&pool_req, fd_value);

        const buffer_id = try self.conn.allocId();
        var buffer_req: [32]u8 = @splat(0);
        header(&buffer_req, pool_id, shm_pool_create_buffer);
        put32(buffer_req[8..12], buffer_id);
        putI32(buffer_req[12..16], 0);
        putI32(buffer_req[16..20], @intCast(w));
        putI32(buffer_req[20..24], @intCast(h));
        putI32(buffer_req[24..28], @intCast(stride));
        put32(buffer_req[28..32], shm_argb8888);
        try self.conn.writeAll(&buffer_req);
        try sendEmpty(&self.conn, pool_id, shm_pool_destroy);

        return .{ .id = buffer_id, .width = w, .height = h, .memory = memory };
    }

    fn discardIdleBuffersExcept(self: *Window, w: u32, h: u32) void {
        var i: usize = 0;
        while (i < self.buffers.items.len) {
            const buffer = self.buffers.items[i];
            if (!buffer.busy and (buffer.width != w or buffer.height != h)) {
                sendEmpty(&self.conn, buffer.id, buffer_destroy) catch {};
                var removed = self.buffers.swapRemove(i);
                removed.deinit();
            } else {
                i += 1;
            }
        }
    }

    fn destroyProtocolObjects(self: *Window) !void {
        for (self.buffers.items) |buffer| try sendEmpty(&self.conn, buffer.id, buffer_destroy);
        if (self.keyboard_id != 0 and self.seat_version >= 3) try sendEmpty(&self.conn, self.keyboard_id, keyboard_release);
        if (self.xdg_toplevel_id != 0) try sendEmpty(&self.conn, self.xdg_toplevel_id, xdg_toplevel_destroy);
        if (self.xdg_surface_id != 0) try sendEmpty(&self.conn, self.xdg_surface_id, xdg_surface_destroy);
        if (self.surface_id != 0) try sendEmpty(&self.conn, self.surface_id, surface_destroy);
        if (self.xdg_wm_base_id != 0) try sendEmpty(&self.conn, self.xdg_wm_base_id, xdg_wm_base_destroy);
        if (self.seat_id != 0 and self.seat_version >= 5) try sendEmpty(&self.conn, self.seat_id, seat_release);
    }
};

const RegistryGlobal = struct {
    name: u32,
    interface: []const u8,
    version: u32,
};

fn parseRegistryGlobal(body: []const u8) !RegistryGlobal {
    if (body.len < 12) return error.InvalidWaylandMessage;
    const name = get32(body[0..4]);
    const string_len: usize = @intCast(get32(body[4..8]));
    if (string_len == 0) return error.InvalidWaylandString;
    if (string_len > body.len - 8) return error.InvalidWaylandMessage;
    const string_end = 8 + string_len;
    const version_off = 8 + pad4(string_len);
    if (string_end > body.len or version_off + 4 != body.len or body[string_end - 1] != 0) {
        return error.InvalidWaylandMessage;
    }
    return .{
        .name = name,
        .interface = body[8 .. string_end - 1],
        .version = get32(body[version_off .. version_off + 4]),
    };
}

fn sendBind(
    conn: *Connection,
    gpa: std.mem.Allocator,
    registry: u32,
    global: Global,
    interface: []const u8,
    version: u32,
    id: u32,
) !void {
    const string_size = try encodedStringSize(interface);
    const total = try std.math.add(usize, 20, string_size);
    if (total > std.math.maxInt(u16)) return error.WaylandMessageTooLarge;
    const req = try gpa.alloc(u8, total);
    defer gpa.free(req);
    @memset(req, 0);
    header(req, registry, registry_bind);
    put32(req[8..12], global.name);
    encodeString(req[12 .. 12 + string_size], interface);
    put32(req[12 + string_size .. 16 + string_size], version);
    put32(req[16 + string_size .. 20 + string_size], id);
    try conn.writeAll(req);
}

fn sendString(conn: *Connection, gpa: std.mem.Allocator, object: u32, opcode: u16, value: []const u8) !void {
    const string_size = try encodedStringSize(value);
    const total = try std.math.add(usize, 8, string_size);
    if (total > std.math.maxInt(u16)) return error.WaylandMessageTooLarge;
    const req = try gpa.alloc(u8, total);
    defer gpa.free(req);
    @memset(req, 0);
    header(req, object, opcode);
    encodeString(req[8..], value);
    try conn.writeAll(req);
}

fn sendEmpty(conn: *Connection, object: u32, opcode: u16) !void {
    var req: [8]u8 = @splat(0);
    header(&req, object, opcode);
    try conn.writeAll(&req);
}

fn sendOneU32(conn: *Connection, object: u32, opcode: u16, a: u32) !void {
    var req: [12]u8 = @splat(0);
    header(&req, object, opcode);
    put32(req[8..12], a);
    try conn.writeAll(&req);
}

fn sendTwoU32(conn: *Connection, object: u32, opcode: u16, a: u32, b: u32) !void {
    var req: [16]u8 = @splat(0);
    header(&req, object, opcode);
    put32(req[8..12], a);
    put32(req[12..16], b);
    try conn.writeAll(&req);
}

fn sendAttach(conn: *Connection, surface: u32, buffer: u32) !void {
    var req: [20]u8 = @splat(0);
    header(&req, surface, surface_attach);
    put32(req[8..12], buffer);
    // x and y are both zero
    try conn.writeAll(&req);
}

fn sendDamage(conn: *Connection, surface: u32, opcode: u16, w: u32, h: u32) !void {
    var req: [24]u8 = @splat(0);
    header(&req, surface, opcode);
    putI32(req[16..20], @intCast(w));
    putI32(req[20..24], @intCast(h));
    try conn.writeAll(&req);
}

fn encodedStringSize(value: []const u8) !usize {
    if (std.mem.indexOfScalar(u8, value, 0) != null) return error.InvalidWaylandString;
    if (value.len > std.math.maxInt(u16) - 8) return error.WaylandMessageTooLarge;
    const with_nul = try std.math.add(usize, value.len, 1);
    return pad4(with_nul) + 4;
}

fn encodeString(dst: []u8, value: []const u8) void {
    const with_nul = value.len + 1;
    put32(dst[0..4], @intCast(with_nul));
    @memcpy(dst[4 .. 4 + value.len], value);
    dst[4 + value.len] = 0;
}

fn header(bytes: []u8, object: u32, opcode: u16) void {
    std.debug.assert(bytes.len >= 8 and bytes.len <= std.math.maxInt(u16) and (bytes.len & 3) == 0);
    put32(bytes[0..4], object);
    put32(bytes[4..8], (@as(u32, @intCast(bytes.len)) << 16) | opcode);
}

fn namedKeyToKey(named: xkb.NamedKey) Key {
    return switch (named) {
        .backspace => .backspace,
        .enter => .enter,
        .escape => .escape,
        .tab => .tab,
        .left => .left,
        .right => .right,
        .up => .up,
        .down => .down,
        .home => .home,
        .end => .end,
        .page_up => .page_up,
        .page_down => .page_down,
        .delete => .delete,
    };
}

fn evdevToKey(code: u32, shift: bool, caps_lock: bool) Key {
    return switch (code) {
        1 => .escape,
        14 => .backspace,
        15 => .tab,
        28, 96 => .enter,
        102 => .home,
        103 => .up,
        104 => .page_up,
        105 => .left,
        106 => .right,
        107 => .end,
        108 => .down,
        109 => .page_down,
        111 => .delete,
        2 => asciiPair('1', '!', shift),
        3 => asciiPair('2', '@', shift),
        4 => asciiPair('3', '#', shift),
        5 => asciiPair('4', '$', shift),
        6 => asciiPair('5', '%', shift),
        7 => asciiPair('6', '^', shift),
        8 => asciiPair('7', '&', shift),
        9 => asciiPair('8', '*', shift),
        10 => asciiPair('9', '(', shift),
        11 => asciiPair('0', ')', shift),
        12 => asciiPair('-', '_', shift),
        13 => asciiPair('=', '+', shift),
        16 => asciiLetter('q', shift, caps_lock),
        17 => asciiLetter('w', shift, caps_lock),
        18 => asciiLetter('e', shift, caps_lock),
        19 => asciiLetter('r', shift, caps_lock),
        20 => asciiLetter('t', shift, caps_lock),
        21 => asciiLetter('y', shift, caps_lock),
        22 => asciiLetter('u', shift, caps_lock),
        23 => asciiLetter('i', shift, caps_lock),
        24 => asciiLetter('o', shift, caps_lock),
        25 => asciiLetter('p', shift, caps_lock),
        26 => asciiPair('[', '{', shift),
        27 => asciiPair(']', '}', shift),
        30 => asciiLetter('a', shift, caps_lock),
        31 => asciiLetter('s', shift, caps_lock),
        32 => asciiLetter('d', shift, caps_lock),
        33 => asciiLetter('f', shift, caps_lock),
        34 => asciiLetter('g', shift, caps_lock),
        35 => asciiLetter('h', shift, caps_lock),
        36 => asciiLetter('j', shift, caps_lock),
        37 => asciiLetter('k', shift, caps_lock),
        38 => asciiLetter('l', shift, caps_lock),
        39 => asciiPair(';', ':', shift),
        40 => asciiPair('\'', '"', shift),
        41 => asciiPair('`', '~', shift),
        43 => asciiPair('\\', '|', shift),
        44 => asciiLetter('z', shift, caps_lock),
        45 => asciiLetter('x', shift, caps_lock),
        46 => asciiLetter('c', shift, caps_lock),
        47 => asciiLetter('v', shift, caps_lock),
        48 => asciiLetter('b', shift, caps_lock),
        49 => asciiLetter('n', shift, caps_lock),
        50 => asciiLetter('m', shift, caps_lock),
        51 => asciiPair(',', '<', shift),
        52 => asciiPair('.', '>', shift),
        53 => asciiPair('/', '?', shift),
        57 => .{ .char = ' ' },
        else => .other,
    };
}

fn asciiPair(normal: u8, shifted: u8, shift: bool) Key {
    return .{ .char = if (shift) shifted else normal };
}

fn asciiLetter(lower: u8, shift: bool, caps_lock: bool) Key {
    return .{ .char = if (shift != caps_lock) lower - ('a' - 'A') else lower };
}

fn waylandSocketPath(gpa: std.mem.Allocator) ![]u8 {
    const fd_value = try openReadOnly("/proc/self/environ");
    defer _ = linux.close(fd_value);
    var env: std.ArrayList(u8) = .empty;
    defer env.deinit(gpa);
    var scratch: [4096]u8 = undefined;
    while (true) {
        const n = try readSomeFd(fd_value, &scratch);
        if (n == 0) break;
        try env.appendSlice(gpa, scratch[0..n]);
    }
    return socketPathFromEnvironment(gpa, env.items);
}

fn socketPathFromEnvironment(gpa: std.mem.Allocator, env: []const u8) ![]u8 {
    const display = environmentValue(env, "WAYLAND_DISPLAY") orelse "wayland-0";
    if (display.len == 0) return error.WaylandDisplayUnset;
    if (display[0] == '/') return gpa.dupe(u8, display);
    const runtime = environmentValue(env, "XDG_RUNTIME_DIR") orelse return error.XdgRuntimeDirUnset;
    if (runtime.len == 0) return error.XdgRuntimeDirUnset;
    return std.fmt.allocPrint(gpa, "{s}/{s}", .{ std.mem.trimEnd(u8, runtime, "/"), display });
}

fn environmentValue(env: []const u8, name: []const u8) ?[]const u8 {
    var start: usize = 0;
    while (start < env.len) {
        const rest = env[start..];
        const item_len = std.mem.indexOfScalar(u8, rest, 0) orelse rest.len;
        const item = rest[0..item_len];
        if (item.len > name.len and item[name.len] == '=' and std.mem.eql(u8, item[0..name.len], name)) {
            return item[name.len + 1 ..];
        }
        start += item_len + 1;
    }
    return null;
}

fn openUnixSocket(io: std.Io, path: []const u8) !net.Stream {
    const address = try net.UnixAddress.init(path);
    return net.UnixAddress.connect(&address, io) catch |err| switch (err) {
        error.FileNotFound => error.WaylandUnavailable,
        error.AccessDenied, error.PermissionDenied => error.AccessDenied,
        else => err,
    };
}

fn truncateFd(fd_value: i32, length: i64) !void {
    const rc = linux.ftruncate(fd_value, length);
    switch (linux.errno(rc)) {
        .SUCCESS => {},
        .INTR => return truncateFd(fd_value, length),
        .FBIG => return error.FileTooBig,
        .NOSPC => return error.NoSpaceLeft,
        else => return error.TruncateFailed,
    }
}

fn openReadOnly(path: [*:0]const u8) !i32 {
    const rc = linux.open(path, .{ .ACCMODE = .RDONLY, .CLOEXEC = true }, 0);
    switch (linux.errno(rc)) {
        .SUCCESS => return @intCast(rc),
        .NOENT => return error.FileNotFound,
        .ACCES, .PERM => return error.AccessDenied,
        else => return error.OpenFailed,
    }
}

fn readSomeFd(fd_value: i32, dst: []u8) !usize {
    while (true) {
        const rc = linux.read(fd_value, dst.ptr, dst.len);
        switch (linux.errno(rc)) {
            .SUCCESS => return @intCast(rc),
            .INTR => continue,
            else => return error.ReadFailed,
        }
    }
}

pub fn pad4(n: usize) usize {
    return (n + 3) & ~@as(usize, 3);
}

fn alignForward(n: usize, alignment: usize) usize {
    return (n + alignment - 1) & ~(alignment - 1);
}

/// Mirrors main.zig's own `monotonicMilliseconds` exactly (same clock, same
/// non-negative clamp) so key-repeat timing recorded here and the poll
/// loop's `checkRepeat` calls agree, whichever `Io` handle each reads it
/// through.
fn nowMs(io: std.Io) u64 {
    const milliseconds = std.Io.Clock.awake.now(io).toMilliseconds();
    return if (milliseconds > 0) @intCast(milliseconds) else 0;
}

fn get32(bytes: []const u8) u32 {
    return std.mem.readInt(u32, bytes[0..4], .native);
}

fn getI32(bytes: []const u8) i32 {
    return @bitCast(get32(bytes));
}

fn put32(bytes: []u8, value: u32) void {
    std.mem.writeInt(u32, bytes[0..4], value, .native);
}

fn putI32(bytes: []u8, value: i32) void {
    put32(bytes, @bitCast(value));
}

// --- Pure protocol and translation tests ------------------------------------

test "Wayland header packs object size and opcode" {
    var req: [12]u8 = @splat(0);
    header(&req, 0x10203040, 0x55aa);
    try std.testing.expectEqual(@as(u32, 0x10203040), get32(req[0..4]));
    try std.testing.expectEqual(@as(u32, 0x000c55aa), get32(req[4..8]));
}

test "Wayland strings include nul and four-byte padding" {
    try std.testing.expectEqual(@as(usize, 8), try encodedStringSize("abc"));
    try std.testing.expectEqual(@as(usize, 12), try encodedStringSize("abcd"));
    var bytes: [12]u8 = @splat(0xcc);
    encodeString(&bytes, "hello");
    try std.testing.expectEqual(@as(u32, 6), get32(bytes[0..4]));
    try std.testing.expectEqualSlices(u8, "hello\x00", bytes[4..10]);
    try std.testing.expectError(error.InvalidWaylandString, encodedStringSize("a\x00b"));
}

test "registry global parser follows Wayland string alignment" {
    var body: [28]u8 = @splat(0);
    put32(body[0..4], 17);
    put32(body[4..8], 14); // "wl_compositor" plus nul
    @memcpy(body[8..21], "wl_compositor");
    body[21] = 0;
    put32(body[24..28], 6);
    const global = try parseRegistryGlobal(&body);
    try std.testing.expectEqual(@as(u32, 17), global.name);
    try std.testing.expectEqualStrings("wl_compositor", global.interface);
    try std.testing.expectEqual(@as(u32, 6), global.version);
}

test "Wayland socket path honors absolute display and runtime directory" {
    const gpa = std.testing.allocator;
    const relative = try socketPathFromEnvironment(gpa, "XDG_RUNTIME_DIR=/run/user/1000\x00WAYLAND_DISPLAY=wayland-2\x00");
    defer gpa.free(relative);
    try std.testing.expectEqualStrings("/run/user/1000/wayland-2", relative);

    const absolute = try socketPathFromEnvironment(gpa, "WAYLAND_DISPLAY=/tmp/nested/wayland.sock\x00");
    defer gpa.free(absolute);
    try std.testing.expectEqualStrings("/tmp/nested/wayland.sock", absolute);
    try std.testing.expectError(error.XdgRuntimeDirUnset, socketPathFromEnvironment(gpa, "A=B\x00"));
}

test "US evdev fallback maps text modifiers and navigation" {
    try std.testing.expectEqual(Key{ .char = 'a' }, evdevToKey(30, false, false));
    try std.testing.expectEqual(Key{ .char = 'A' }, evdevToKey(30, true, false));
    try std.testing.expectEqual(Key{ .char = 'A' }, evdevToKey(30, false, true));
    try std.testing.expectEqual(Key{ .char = 'a' }, evdevToKey(30, true, true));
    try std.testing.expectEqual(Key{ .char = '!' }, evdevToKey(2, true, false));
    try std.testing.expectEqual(Key.left, evdevToKey(105, false, false));
    try std.testing.expectEqual(Key.delete, evdevToKey(111, false, false));
    try std.testing.expectEqual(Key.other, evdevToKey(59, false, false));
}

test "SCM control alignment is sufficient for one fd" {
    const head = alignForward(@sizeOf(linux.cmsghdr), @alignOf(linux.cmsghdr));
    const space = head + alignForward(@sizeOf(i32), @alignOf(linux.cmsghdr));
    try std.testing.expect(space >= @sizeOf(linux.cmsghdr) + @sizeOf(i32));
    try std.testing.expectEqual(@as(usize, 0), head % @alignOf(i32));
}

test "Wayland fd request transfers SCM_RIGHTS without a wire placeholder" {
    var sockets: [2]i32 = undefined;
    const pair_rc = linux.socketpair(linux.AF.UNIX, linux.SOCK.STREAM | linux.SOCK.CLOEXEC, 0, &sockets);
    try std.testing.expectEqual(linux.E.SUCCESS, linux.errno(pair_rc));
    defer _ = linux.close(sockets[0]);
    defer _ = linux.close(sockets[1]);

    const sent_fd = try posix.memfd_create("comicchat-wayland-test", linux.MFD.CLOEXEC);
    defer _ = linux.close(sent_fd);

    var threaded = std.Io.Threaded.init(std.testing.allocator, .{});
    defer threaded.deinit();
    var conn = Connection{
        .io = threaded.io(),
        .stream = .{ .socket = .{ .handle = sockets[0], .address = undefined } },
    };
    var request: [16]u8 = @splat(0);
    header(&request, 7, shm_create_pool);
    put32(request[8..12], 8);
    putI32(request[12..16], 4096);
    try conn.writeWithFd(&request, sent_fd);

    var received: [16]u8 = undefined;
    var control: [64]u8 align(@alignOf(linux.cmsghdr)) = @splat(0);
    var iov: posix.iovec = .{ .base = &received, .len = received.len };
    var msg: linux.msghdr = .{
        .name = null,
        .namelen = 0,
        .iov = (&iov)[0..1],
        .iovlen = 1,
        .control = &control,
        .controllen = control.len,
        .flags = 0,
    };
    const recv_rc = linux.recvmsg(sockets[1], &msg, linux.MSG.CMSG_CLOEXEC);
    try std.testing.expectEqual(linux.E.SUCCESS, linux.errno(recv_rc));
    try std.testing.expectEqual(request.len, @as(usize, @intCast(recv_rc)));
    try std.testing.expectEqualSlices(u8, &request, &received);

    const cmsg: *const linux.cmsghdr = @ptrCast(&control);
    try std.testing.expectEqual(linux.SOL.SOCKET, cmsg.level);
    try std.testing.expectEqual(@as(i32, linux.SCM.RIGHTS), cmsg.type);
    const fd_off = alignForward(@sizeOf(linux.cmsghdr), @alignOf(linux.cmsghdr));
    const received_fd = @as(*const i32, @ptrCast(@alignCast(control[fd_off..].ptr))).*;
    defer _ = linux.close(received_fd);
    try std.testing.expect(received_fd >= 0);
}

test "protocol-only Wayland message returns other without a second blocking read" {
    var sockets: [2]i32 = undefined;
    const pair_rc = linux.socketpair(linux.AF.UNIX, linux.SOCK.STREAM | linux.SOCK.CLOEXEC, 0, &sockets);
    try std.testing.expectEqual(linux.E.SUCCESS, linux.errno(pair_rc));
    defer _ = linux.close(sockets[0]);
    defer _ = linux.close(sockets[1]);

    var threaded = std.Io.Threaded.init(std.testing.allocator, .{});
    defer threaded.deinit();
    var window = Window{
        .gpa = std.testing.allocator,
        .threaded = undefined,
        .conn = .{
            .io = threaded.io(),
            .stream = .{ .socket = .{ .handle = sockets[0], .address = undefined } },
        },
        .surface_id = 42,
        .width = 1,
        .height = 1,
    };

    var wire: [8]u8 = @splat(0);
    header(&wire, window.surface_id, 0);
    const written = linux.write(sockets[1], &wire, wire.len);
    try std.testing.expectEqual(linux.E.SUCCESS, linux.errno(written));
    try std.testing.expectEqual(wire.len, @as(usize, @intCast(written)));
    // A regression to the old "loop until visible" implementation observes
    // EOF here and fails instead of returning the protocol-only event.
    try std.testing.expectEqual(linux.E.SUCCESS, linux.errno(linux.shutdown(sockets[1], linux.SHUT.WR)));

    try std.testing.expectEqual(Event.other, try window.nextEvent());
}

test "readMessage captures an SCM_RIGHTS fd sent alongside the wire message" {
    var sockets: [2]i32 = undefined;
    const pair_rc = linux.socketpair(linux.AF.UNIX, linux.SOCK.STREAM | linux.SOCK.CLOEXEC, 0, &sockets);
    try std.testing.expectEqual(linux.E.SUCCESS, linux.errno(pair_rc));
    defer _ = linux.close(sockets[0]);
    defer _ = linux.close(sockets[1]);

    var threaded = std.Io.Threaded.init(std.testing.allocator, .{});
    defer threaded.deinit();
    var reader_conn = Connection{
        .io = threaded.io(),
        .stream = .{ .socket = .{ .handle = sockets[0], .address = undefined } },
    };

    // Mirror the compositor: one sendmsg carrying the wl_keyboard.keymap wire
    // bytes (object=99, opcode=0, body = format:u32=1, size:u32=4096) with an
    // SCM_RIGHTS fd attached, exactly as writeWithFd attaches one on the send
    // side this client already exercises.
    const keymap_fd = try posix.memfd_create("comicchat-wayland-keymap-test", linux.MFD.CLOEXEC);
    const marker = "xkb_keymap_marker";
    const write_rc = linux.write(keymap_fd, marker.ptr, marker.len);
    try std.testing.expectEqual(linux.E.SUCCESS, linux.errno(write_rc));
    try std.testing.expectEqual(marker.len, @as(usize, @intCast(write_rc)));

    var wire: [16]u8 = @splat(0);
    header(&wire, 99, 0);
    put32(wire[8..12], 1);
    put32(wire[12..16], 4096);

    var writer_conn = Connection{
        .io = threaded.io(),
        .stream = .{ .socket = .{ .handle = sockets[1], .address = undefined } },
    };
    try writer_conn.writeWithFd(&wire, keymap_fd);
    _ = linux.close(keymap_fd);

    const msg = try reader_conn.readMessage(std.testing.allocator);
    defer msg.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u32, 99), msg.object);
    try std.testing.expectEqual(@as(u16, 0), msg.opcode);

    const received_fd = reader_conn.takePendingFd() orelse return error.TestExpectedFd;
    defer _ = linux.close(received_fd);
    // keymap_fd is already closed above; the kernel is free to recycle its
    // number for received_fd, so equality here is expected, not proof of a
    // bug. The content readback below is the real correctness proof: the
    // received descriptor genuinely refers to the memfd the writer sent.
    try std.testing.expectEqual(@as(u64, 0), linux.lseek(received_fd, 0, linux.SEEK.SET));
    var readback: [marker.len]u8 = undefined;
    const read_rc = linux.read(received_fd, &readback, readback.len);
    try std.testing.expectEqual(linux.E.SUCCESS, linux.errno(read_rc));
    try std.testing.expectEqual(readback.len, @as(usize, @intCast(read_rc)));
    try std.testing.expectEqualSlices(u8, marker, &readback);

    // The fd is claimed exactly once; a second take sees nothing left over.
    try std.testing.expectEqual(@as(?posix.fd_t, null), reader_conn.takePendingFd());
}

test "a real keymap event overrides evdevToKey's US table for the same physical key" {
    var sockets: [2]i32 = undefined;
    const pair_rc = linux.socketpair(linux.AF.UNIX, linux.SOCK.STREAM | linux.SOCK.CLOEXEC, 0, &sockets);
    try std.testing.expectEqual(linux.E.SUCCESS, linux.errno(pair_rc));
    defer _ = linux.close(sockets[1]);

    var threaded = std.Io.Threaded.init(std.testing.allocator, .{});
    defer threaded.deinit();
    var window = Window{
        .gpa = std.testing.allocator,
        .threaded = undefined,
        .conn = .{
            .io = threaded.io(),
            .stream = .{ .socket = .{ .handle = sockets[0], .address = undefined } },
        },
        .keyboard_id = 77,
        .width = 1,
        .height = 1,
    };
    defer _ = linux.close(sockets[0]);
    defer if (window.xkb_keymap) |*keymap| keymap.deinit();

    // A minimal synthetic layout that swaps evdev code 30 (the physical key
    // the US table maps to 'a'/'A') to 'q'/'Q', so a passing test proves the
    // keymap was genuinely consulted rather than coincidentally agreeing
    // with the fallback.
    const synthetic_keymap =
        \\xkb_keymap {
        \\  xkb_keycodes "test" {
        \\      <AC01> = 38;
        \\  };
        \\  xkb_symbols "test" {
        \\      key <AC01> { [ q, Q ] };
        \\  };
        \\};
    ;
    const keymap_fd = try posix.memfd_create("comicchat-wayland-keymap-e2e-test", linux.MFD.CLOEXEC);
    defer _ = linux.close(keymap_fd);
    const write_rc = linux.write(keymap_fd, synthetic_keymap.ptr, synthetic_keymap.len);
    try std.testing.expectEqual(linux.E.SUCCESS, linux.errno(write_rc));
    try std.testing.expectEqual(synthetic_keymap.len, @as(usize, @intCast(write_rc)));

    var writer_conn = Connection{
        .io = threaded.io(),
        .stream = .{ .socket = .{ .handle = sockets[1], .address = undefined } },
    };
    var keymap_wire: [16]u8 = @splat(0);
    header(&keymap_wire, window.keyboard_id, 0);
    put32(keymap_wire[8..12], 1); // format = XKB_V1_TEXT
    put32(keymap_wire[12..16], @intCast(synthetic_keymap.len));
    try writer_conn.writeWithFd(&keymap_wire, keymap_fd);

    try std.testing.expectEqual(Event.other, try window.nextEvent());
    try std.testing.expect(window.xkb_keymap != null);

    var key_wire: [24]u8 = @splat(0);
    header(&key_wire, window.keyboard_id, 3);
    put32(key_wire[8..12], 1); // serial
    put32(key_wire[12..16], 0); // time
    put32(key_wire[16..20], 30); // evdev code (the US table's 'a' key)
    put32(key_wire[20..24], 1); // state = pressed
    const key_written = linux.write(sockets[1], &key_wire, key_wire.len);
    try std.testing.expectEqual(linux.E.SUCCESS, linux.errno(key_written));
    try std.testing.expectEqual(key_wire.len, @as(usize, @intCast(key_written)));

    try std.testing.expectEqual(Event{ .key = .{ .char = 'q' } }, try window.nextEvent());
}

fn checkRepeatTestWindow(threaded: *std.Io.Threaded) Window {
    return Window{
        .gpa = std.testing.allocator,
        .threaded = undefined,
        .conn = .{ .io = threaded.io(), .stream = .{ .socket = .{ .handle = -1, .address = undefined } } },
        .width = 1,
        .height = 1,
    };
}

test "checkRepeat is silent with no key held, a non-positive rate, or before the deadline" {
    var threaded = std.Io.Threaded.init(std.testing.allocator, .{});
    defer threaded.deinit();
    var window = checkRepeatTestWindow(&threaded);

    // Nothing held at all.
    try std.testing.expectEqual(@as(?Event, null), window.checkRepeat());

    // Held, but the compositor either never sent repeat_info or sent a
    // non-positive rate (repeat explicitly disabled per protocol convention).
    window.held_key_code = 30;
    window.repeat_rate_per_sec = 0;
    try std.testing.expectEqual(@as(?Event, null), window.checkRepeat());

    // Held with a valid rate, but the deadline is far in the future.
    window.repeat_rate_per_sec = 25;
    window.next_repeat_at_ms = nowMs(window.conn.io) +| 60_000;
    try std.testing.expectEqual(@as(?Event, null), window.checkRepeat());
}

test "checkRepeat fires once the deadline has passed and reschedules from now, not by accumulation" {
    var threaded = std.Io.Threaded.init(std.testing.allocator, .{});
    defer threaded.deinit();
    var window = checkRepeatTestWindow(&threaded);

    window.held_key_code = 30; // evdevToKey's 'a' key, no keymap loaded
    window.held_key_shift = false;
    window.repeat_rate_per_sec = 25; // interval = 40ms
    window.next_repeat_at_ms = nowMs(window.conn.io); // already due

    const before = window.next_repeat_at_ms;
    const event = window.checkRepeat();
    try std.testing.expectEqual(Event{ .key = .{ .char = 'a' } }, event.?);
    // Rescheduled forward from *now* (>= before, by roughly one interval),
    // not left at the stale deadline that just fired.
    try std.testing.expect(window.next_repeat_at_ms >= before);

    // Immediately checking again is not yet due (the new deadline is ~40ms
    // out), proving this does not fire on every call once due.
    try std.testing.expectEqual(@as(?Event, null), window.checkRepeat());
}

test "releasing the held key stops repeat, and repeat_info updates rate/delay" {
    var sockets: [2]i32 = undefined;
    const pair_rc = linux.socketpair(linux.AF.UNIX, linux.SOCK.STREAM | linux.SOCK.CLOEXEC, 0, &sockets);
    try std.testing.expectEqual(linux.E.SUCCESS, linux.errno(pair_rc));
    defer _ = linux.close(sockets[1]);

    var threaded = std.Io.Threaded.init(std.testing.allocator, .{});
    defer threaded.deinit();
    var window = Window{
        .gpa = std.testing.allocator,
        .threaded = undefined,
        .conn = .{ .io = threaded.io(), .stream = .{ .socket = .{ .handle = sockets[0], .address = undefined } } },
        .keyboard_id = 55,
        .width = 1,
        .height = 1,
    };
    defer _ = linux.close(sockets[0]);

    var repeat_info_wire: [16]u8 = @splat(0);
    header(&repeat_info_wire, window.keyboard_id, 5);
    put32(repeat_info_wire[8..12], 33); // rate
    put32(repeat_info_wire[12..16], 500); // delay
    var wrote = linux.write(sockets[1], &repeat_info_wire, repeat_info_wire.len);
    try std.testing.expectEqual(linux.E.SUCCESS, linux.errno(wrote));
    try std.testing.expectEqual(Event.other, try window.nextEvent());
    try std.testing.expectEqual(@as(i32, 33), window.repeat_rate_per_sec);
    try std.testing.expectEqual(@as(i32, 500), window.repeat_delay_ms);

    var key_down: [24]u8 = @splat(0);
    header(&key_down, window.keyboard_id, 3);
    put32(key_down[16..20], 30);
    put32(key_down[20..24], 1); // pressed
    wrote = linux.write(sockets[1], &key_down, key_down.len);
    try std.testing.expectEqual(linux.E.SUCCESS, linux.errno(wrote));
    try std.testing.expectEqual(Event{ .key = .{ .char = 'a' } }, try window.nextEvent());
    try std.testing.expectEqual(@as(?u32, 30), window.held_key_code);

    var key_up: [24]u8 = @splat(0);
    header(&key_up, window.keyboard_id, 3);
    put32(key_up[16..20], 30);
    put32(key_up[20..24], 0); // released
    wrote = linux.write(sockets[1], &key_up, key_up.len);
    try std.testing.expectEqual(linux.E.SUCCESS, linux.errno(wrote));
    try std.testing.expectEqual(Event.other, try window.nextEvent());
    try std.testing.expectEqual(@as(?u32, null), window.held_key_code);
    try std.testing.expectEqual(@as(?Event, null), window.checkRepeat());
}

test "Wayland Window entry points compile without a compositor" {
    // The invalid size returns before environment/socket access, while making
    // Zig analyze the complete native backend call graph.
    try std.testing.expectError(error.InvalidWindowSize, show(std.testing.allocator, &.{}, 0, 1));
}
