//! Minimal pure-Zig X11 window backend.
//!
//! Talks to the local X server over `/tmp/.X11-unix/X<n>` and uploads an
//! RGBA framebuffer with PutImage. No Xlib, no C imports.
//!
//! Two layers:
//!   * `show(...)` — one-shot: open a window, draw an image, wait for a
//!     keypress / close.
//!   * `Window` — interactive: open/present/nextEvent/fd, with keyboard
//!     translation (GetKeyboardMapping) and resize/close events, suitable for
//!     a poll(2)-driven client event loop.

const std = @import("std");
const linux = std.os.linux;
const net = std.Io.net;
const shared_event = @import("event.zig");

const image_depth = 24;
const z_pixmap = 2;

const input_output = 1;

const event_key_press: u32 = 1 << 0;
const event_button_press: u32 = 1 << 2;
const event_button_release: u32 = 1 << 3;
const event_pointer_motion: u32 = 1 << 6;
const event_exposure: u32 = 1 << 15;
const event_structure: u32 = 1 << 17;

const cw_back_pixel: u32 = 1 << 1;
const cw_border_pixel: u32 = 1 << 3;
const cw_event_mask: u32 = 1 << 11;

const gc_foreground: u32 = 1 << 2;
const gc_background: u32 = 1 << 3;

const atom_atom = 4;
const atom_wm_name = 39;
const atom_string = 31;
const prop_replace = 0;

const request_put_image_header_units = 6;
const min_max_request_units = 64;

const XConn = struct {
    io: std.Io,
    stream: net.Stream,
    next_id: u32,
    resource_mask: u32,
    screen: Screen,
    max_request_units: u16,
    min_keycode: u8,
    max_keycode: u8,
    wm_protocols: u32 = 0,
    wm_delete_window: u32 = 0,

    fn allocId(self: *XConn) !u32 {
        const slot = self.next_id & self.resource_mask;
        if (slot == 0 and self.next_id != 0) return error.ResourceIdsExhausted;
        self.next_id += 1;
        return (self.screen.resource_base & ~self.resource_mask) | slot;
    }
};

const Screen = struct {
    resource_base: u32,
    resource_mask: u32,
    root: u32,
    root_visual: u32,
    root_depth: u8,
    white_pixel: u32,
    black_pixel: u32,
};

const Display = struct {
    number: u16,
};

const Setup = struct {
    screen: Screen,
    max_request_units: u16,
    min_keycode: u8,
    max_keycode: u8,
};

// --- Events -----------------------------------------------------------------

pub const Key = shared_event.Key;
pub const Event = shared_event.Event;

// --- Keyboard mapping --------------------------------------------------------

/// Keycode → keysym table fetched with GetKeyboardMapping. Translation itself
/// is pure and testable without a server.
pub const Keymap = struct {
    syms: []u32,
    per: u8,
    min: u8,

    pub fn deinit(self: *Keymap, gpa: std.mem.Allocator) void {
        gpa.free(self.syms);
        self.* = undefined;
    }

    pub fn keysym(self: *const Keymap, keycode: u8, state: u16) u32 {
        if (keycode < self.min or self.per == 0) return 0;
        const idx = @as(usize, keycode - self.min) * self.per;
        if (idx >= self.syms.len) return 0;
        const s0 = self.syms[idx];
        const s1 = if (self.per > 1 and idx + 1 < self.syms.len) self.syms[idx + 1] else 0;

        const shift = (state & 0x1) != 0;
        const lock = (state & 0x2) != 0;

        var sym = s0;
        if (shift and s1 != 0) sym = s1;
        // Alphabetic case rules (X11 §5): when the shifted column is NoSymbol,
        // the pair is the lower/upper case of column 0; CapsLock upcases too.
        if ((shift and s1 == 0) or (lock and !shift)) {
            if (sym >= 'a' and sym <= 'z') sym -= 32;
        }
        return sym;
    }

    pub fn translate(self: *const Keymap, keycode: u8, state: u16) Key {
        return keysymToKey(self.keysym(keycode, state));
    }
};

pub fn keysymToKey(sym: u32) Key {
    if (sym >= 0x20 and sym <= 0xff) return .{ .char = @intCast(sym) };
    if (sym >= 0x01000000 and sym <= 0x0110ffff) return .{ .char = @intCast(sym - 0x01000000) };
    return switch (sym) {
        0xff08 => .backspace,
        0xff09 => .tab,
        0xff0d, 0xff8d => .enter, // Return, KP_Enter
        0xff1b => .escape,
        0xff51 => .left,
        0xff52 => .up,
        0xff53 => .right,
        0xff54 => .down,
        0xff50 => .home,
        0xff57 => .end,
        0xff55 => .page_up,
        0xff56 => .page_down,
        0xffff => .delete,
        else => .other,
    };
}

// --- One-shot viewer ----------------------------------------------------------

/// Open a local X11 window, draw `pixels` (0xAARRGGBB), and run until keypress
/// or WM close. DISPLAY is read at runtime; tests/builds do not need X.
pub fn show(gpa: std.mem.Allocator, pixels: []const u32, w: u32, h: u32) !void {
    const win = try Window.open(gpa, w, h, "comicchat");
    defer win.deinit();
    try win.present(pixels, w, h);
    while (true) {
        switch (try win.nextEvent()) {
            .key, .close => return,
            .expose => try win.present(pixels, w, h),
            else => {},
        }
    }
}

// --- Interactive window --------------------------------------------------------

/// A live X11 window. Heap-pinned (owns its Io runtime, whose vtable points
/// back into the struct), so `open` returns a pointer that must not move.
pub const Window = struct {
    gpa: std.mem.Allocator,
    threaded: std.Io.Threaded,
    conn: XConn,
    window: u32,
    gc: u32,
    width: u32,
    height: u32,
    keymap: Keymap,

    pub fn open(gpa: std.mem.Allocator, w: u32, h: u32, title: []const u8) !*Window {
        const display = try readDisplay(gpa);
        defer gpa.free(display);
        const parsed = try parseDisplay(display);

        const self = try gpa.create(Window);
        errdefer gpa.destroy(self);
        self.gpa = gpa;
        self.threaded = std.Io.Threaded.init(gpa, .{});
        errdefer self.threaded.deinit();
        const io = self.threaded.io();

        self.conn = try connectDisplay(gpa, io, parsed);
        errdefer self.conn.stream.close(io);
        if (self.conn.screen.root_depth != image_depth) return error.UnsupportedDepth;

        self.width = w;
        self.height = h;
        self.window = try self.conn.allocId();
        self.gc = try self.conn.allocId();
        try createWindow(&self.conn, self.window, @intCast(w), @intCast(h));
        try createGc(&self.conn, self.gc, self.window);
        try installWmClose(&self.conn, self.window);
        try setTitle(&self.conn, self.window, title);
        self.keymap = try fetchKeymap(gpa, &self.conn);
        errdefer self.keymap.deinit(gpa);
        try mapWindow(&self.conn, self.window);
        return self;
    }

    pub fn deinit(self: *Window) void {
        self.keymap.deinit(self.gpa);
        self.conn.stream.close(self.conn.io);
        self.threaded.deinit();
        self.gpa.destroy(self);
    }

    /// Socket handle, for poll(2)-based event loops.
    pub fn fd(self: *const Window) i32 {
        return self.conn.stream.socket.handle;
    }

    /// Upload a full frame. `w`/`h` are the frame's own dimensions (normally
    /// the current window size).
    pub fn present(self: *Window, pixels: []const u32, w: u32, h: u32) !void {
        if (pixels.len != @as(usize, w) * @as(usize, h)) return error.BadFramebufferSize;
        try putImage(self.gpa, &self.conn, self.window, self.gc, pixels, w, h);
    }

    /// Blocking read of the next event. Call only when data is available
    /// (after poll) to avoid stalling, or when blocking is intended.
    pub fn nextEvent(self: *Window) !Event {
        var raw: [32]u8 = undefined;
        try readExact(&self.conn, &raw);
        return self.decode(raw);
    }

    fn decode(self: *Window, event: [32]u8) Event {
        const kind = event[0] & 0x7f;
        switch (kind) {
            0 => return .close, // request error; treat as fatal for the UI
            2 => { // KeyPress
                const keycode = event[1];
                const state = get16(event[28..30]);
                return .{ .key = .{
                    .key = self.keymap.translate(keycode, state),
                    .modifiers = .{
                        .shift = state & 1 != 0,
                        .control = state & 4 != 0,
                        .alt = state & 8 != 0,
                    },
                } };
            },
            4, 5, 6 => {
                const x: i32 = @as(i16, @bitCast(get16(event[24..26])));
                const y: i32 = @as(i16, @bitCast(get16(event[26..28])));
                if (kind == 6) return .{ .pointer = .{ .kind = .move, .x = x, .y = y } };
                const detail = event[1];
                if (kind == 4 and (detail == 4 or detail == 5)) return .{ .pointer = .{
                    .kind = .wheel,
                    .x = x,
                    .y = y,
                    .wheel_y = if (detail == 4) 1 else -1,
                } };
                const button: shared_event.PointerButton = switch (detail) {
                    1 => .primary,
                    2 => .middle,
                    3 => .secondary,
                    else => .none,
                };
                return .{ .pointer = .{
                    .kind = if (kind == 4) .down else .up,
                    .x = x,
                    .y = y,
                    .button = button,
                } };
            },
            12 => return .expose,
            17 => return .close, // DestroyNotify
            22 => { // ConfigureNotify
                const w: u32 = get16(event[20..22]);
                const h: u32 = get16(event[22..24]);
                if (w == 0 or h == 0) return .other;
                if (w == self.width and h == self.height) return .other;
                self.width = w;
                self.height = h;
                return .{ .resize = .{ .w = w, .h = h } };
            },
            33 => { // ClientMessage
                if (get32(event[8..12]) == self.conn.wm_protocols and
                    get32(event[12..16]) == self.conn.wm_delete_window) return .close;
                return .other;
            },
            else => return .other,
        }
    }
};

fn readDisplay(gpa: std.mem.Allocator) ![]u8 {
    const fd = try openReadOnly("/proc/self/environ");
    defer _ = linux.close(fd);

    var env: std.ArrayList(u8) = .empty;
    defer env.deinit(gpa);

    var buf: [4096]u8 = undefined;
    while (true) {
        const n = try readSomeFd(fd, &buf);
        if (n == 0) break;
        try env.appendSlice(gpa, buf[0..n]);
    }

    var start: usize = 0;
    while (start < env.items.len) {
        const rest = env.items[start..];
        const end_rel = std.mem.indexOfScalar(u8, rest, 0) orelse rest.len;
        const item = rest[0..end_rel];
        if (std.mem.startsWith(u8, item, "DISPLAY=")) {
            const value = item["DISPLAY=".len..];
            if (value.len == 0) return error.DisplayUnset;
            return gpa.dupe(u8, value);
        }
        start += end_rel + 1;
    }
    return error.DisplayUnset;
}

fn parseDisplay(display: []const u8) !Display {
    const colon = std.mem.lastIndexOfScalar(u8, display, ':') orelse return error.InvalidDisplay;
    var i = colon + 1;
    const start = i;
    while (i < display.len and display[i] >= '0' and display[i] <= '9') : (i += 1) {}
    if (i == start) return error.InvalidDisplay;
    return .{ .number = try std.fmt.parseInt(u16, display[start..i], 10) };
}

fn connectDisplay(gpa: std.mem.Allocator, io: std.Io, display: Display) !XConn {
    var path_buf: [64]u8 = undefined;
    const path = try std.fmt.bufPrint(&path_buf, "/tmp/.X11-unix/X{d}", .{display.number});

    const stream = try openUnixSocket(io, path);
    errdefer stream.close(io);

    var hello: [12]u8 = @splat(0);
    hello[0] = 'l';
    put16(hello[2..4], 11);
    try writeAllRaw(io, stream, &hello);

    var header: [8]u8 = undefined;
    try readExactRaw(io, stream, &header);
    const extra_len = @as(usize, get16(header[6..8])) * 4;
    if (header[0] != 1) {
        const extra = try gpa.alloc(u8, extra_len);
        defer gpa.free(extra);
        try readExactRaw(io, stream, extra);
        return error.X11SetupFailed;
    }

    const body = try gpa.alloc(u8, extra_len);
    defer gpa.free(body);
    try readExactRaw(io, stream, body);
    const setup = try parseSetup(body);

    return .{
        .io = io,
        .stream = stream,
        .next_id = 1,
        .resource_mask = setup.screen.resource_mask,
        .screen = setup.screen,
        .max_request_units = @max(setup.max_request_units, min_max_request_units),
        .min_keycode = setup.min_keycode,
        .max_keycode = setup.max_keycode,
    };
}

/// Parse the connection-setup "additional data" (everything after the 8-byte
/// success header). Fixed part per the X11 protocol spec:
///   0  release, 4 resource-id-base, 8 resource-id-mask, 12 motion-buffer-size,
///   16 vendor length (u16), 18 maximum-request-length (u16),
///   20 #screens (u8), 21 #formats (u8), 22 image-byte-order, 23 bit-order,
///   24 scanline-unit, 25 scanline-pad, 26 min-keycode, 27 max-keycode,
///   28..32 unused, 32 vendor string (padded), then formats (8B each),
///   then screens.
fn parseSetup(body: []const u8) !Setup {
    if (body.len < 32) return error.ShortSetupReply;
    const resource_base = get32(body[4..8]);
    const resource_mask = get32(body[8..12]);
    const vendor_len = get16(body[16..18]);
    const max_request = get16(body[18..20]);
    const roots_len = body[20];
    const formats_len = body[21];
    const min_keycode = body[26];
    const max_keycode = body[27];
    if (roots_len == 0) return error.NoScreen;

    const off: usize = 32 + pad4(vendor_len) + @as(usize, formats_len) * 8;
    if (off + 40 > body.len) return error.ShortSetupReply;

    const root = get32(body[off + 0 .. off + 4]);
    const white = get32(body[off + 8 .. off + 12]);
    const black = get32(body[off + 12 .. off + 16]);
    const root_visual = get32(body[off + 32 .. off + 36]);
    const root_depth = body[off + 38];

    return .{
        .screen = .{
            .resource_base = resource_base,
            .resource_mask = resource_mask,
            .root = root,
            .root_visual = root_visual,
            .root_depth = root_depth,
            .white_pixel = white,
            .black_pixel = black,
        },
        .max_request_units = max_request,
        .min_keycode = min_keycode,
        .max_keycode = max_keycode,
    };
}

fn createWindow(conn: *XConn, window: u32, w: u16, h: u16) !void {
    const values = [_]u32{
        conn.screen.white_pixel,
        conn.screen.black_pixel,
        event_key_press | event_button_press | event_button_release |
            event_pointer_motion | event_exposure | event_structure,
    };
    const value_mask = cw_back_pixel | cw_border_pixel | cw_event_mask;
    var req: [44]u8 = @splat(0);
    req[0] = 1;
    req[1] = conn.screen.root_depth;
    put16(req[2..4], @intCast(req.len / 4));
    put32(req[4..8], window);
    put32(req[8..12], conn.screen.root);
    put16(req[16..18], w);
    put16(req[18..20], h);
    put16(req[22..24], input_output);
    put32(req[24..28], conn.screen.root_visual);
    put32(req[28..32], value_mask);
    put32(req[32..36], values[0]);
    put32(req[36..40], values[1]);
    put32(req[40..44], values[2]);
    try writeAll(conn, &req);
}

fn createGc(conn: *XConn, gc: u32, drawable: u32) !void {
    var req: [24]u8 = @splat(0);
    req[0] = 55;
    put16(req[2..4], @intCast(req.len / 4));
    put32(req[4..8], gc);
    put32(req[8..12], drawable);
    put32(req[12..16], gc_foreground | gc_background);
    put32(req[16..20], conn.screen.black_pixel);
    put32(req[20..24], conn.screen.white_pixel);
    try writeAll(conn, &req);
}

fn installWmClose(conn: *XConn, window: u32) !void {
    conn.wm_protocols = try internAtom(conn, "WM_PROTOCOLS");
    conn.wm_delete_window = try internAtom(conn, "WM_DELETE_WINDOW");

    var req: [28]u8 = @splat(0);
    req[0] = 18;
    req[1] = prop_replace;
    put16(req[2..4], @intCast(req.len / 4));
    put32(req[4..8], window);
    put32(req[8..12], conn.wm_protocols);
    put32(req[12..16], atom_atom);
    req[16] = 32;
    put32(req[20..24], 1);
    put32(req[24..28], conn.wm_delete_window);
    try writeAll(conn, &req);
}

fn setTitle(conn: *XConn, window: u32, title: []const u8) !void {
    if (title.len == 0 or title.len > 255) return;
    const padded = pad4(title.len);
    var buf: [24 + 256 + 4]u8 = undefined;
    const req = buf[0 .. 24 + padded];
    @memset(req, 0);
    req[0] = 18;
    req[1] = prop_replace;
    put16(req[2..4], @intCast(req.len / 4));
    put32(req[4..8], window);
    put32(req[8..12], atom_wm_name);
    put32(req[12..16], atom_string);
    req[16] = 8;
    put32(req[20..24], @intCast(title.len));
    @memcpy(req[24 .. 24 + title.len], title);
    try writeAll(conn, req);
}

fn internAtom(conn: *XConn, name: []const u8) !u32 {
    if (name.len > std.math.maxInt(u16)) return error.NameTooLong;
    const padded = pad4(name.len);
    var req = try std.heap.page_allocator.alloc(u8, 8 + padded);
    defer std.heap.page_allocator.free(req);
    @memset(req, 0);
    req[0] = 16;
    req[1] = 0;
    put16(req[2..4], @intCast(req.len / 4));
    put16(req[4..6], @intCast(name.len));
    @memcpy(req[8 .. 8 + name.len], name);
    try writeAll(conn, req);

    const reply = try readReply(conn);
    return get32(reply[8..12]);
}

/// GetKeyboardMapping for the full keycode range.
fn fetchKeymap(gpa: std.mem.Allocator, conn: *XConn) !Keymap {
    const count: u8 = conn.max_keycode - conn.min_keycode + 1;
    var req: [8]u8 = @splat(0);
    req[0] = 101;
    put16(req[2..4], 2);
    req[4] = conn.min_keycode;
    req[5] = count;
    try writeAll(conn, &req);

    const reply = try readReply(conn);
    const per = reply[1];
    const total = @as(usize, get32(reply[4..8])); // u32 keysyms following

    const syms = try gpa.alloc(u32, total);
    errdefer gpa.free(syms);
    const raw = try gpa.alloc(u8, total * 4);
    defer gpa.free(raw);
    try readExact(conn, raw);
    for (syms, 0..) |*s, i| s.* = get32(raw[i * 4 .. i * 4 + 4]);

    return .{ .syms = syms, .per = per, .min = conn.min_keycode };
}

/// Wait for the next reply, skipping (discarding) any events that arrive
/// first. Only used during setup, before the event loop starts.
fn readReply(conn: *XConn) ![32]u8 {
    while (true) {
        var head: [32]u8 = undefined;
        try readExact(conn, &head);
        switch (head[0]) {
            0 => return error.X11ServerError,
            1 => return head,
            else => continue, // stray event during setup
        }
    }
}

fn mapWindow(conn: *XConn, window: u32) !void {
    var req: [8]u8 = @splat(0);
    req[0] = 8;
    put16(req[2..4], 2);
    put32(req[4..8], window);
    try writeAll(conn, &req);
}

fn putImage(
    gpa: std.mem.Allocator,
    conn: *XConn,
    drawable: u32,
    gc: u32,
    pixels: []const u32,
    w: u32,
    h: u32,
) !void {
    const row_bytes = try std.math.mul(usize, w, 4);
    const max_units = @max(conn.max_request_units, min_max_request_units);
    if (max_units <= request_put_image_header_units) return error.MaxRequestTooSmall;
    const max_payload = (@as(usize, max_units) - request_put_image_header_units) * 4;
    const rows_per_chunk = @divFloor(max_payload, row_bytes);
    if (rows_per_chunk == 0) return error.ImageTooWide;

    const chunk_rows = @min(rows_per_chunk, h);
    const chunk_bytes = row_bytes * @as(usize, chunk_rows);
    var data = try gpa.alloc(u8, pad4(chunk_bytes));
    defer gpa.free(data);

    var y: u32 = 0;
    while (y < h) {
        const rows: u32 = @intCast(@min(@as(usize, h - y), rows_per_chunk));
        const raw_len = row_bytes * @as(usize, rows);
        encodeBgrx(data[0..raw_len], pixels[@as(usize, y) * w ..], w, rows);
        @memset(data[raw_len..pad4(raw_len)], 0);

        var header: [24]u8 = @splat(0);
        header[0] = 72;
        header[1] = z_pixmap;
        put16(header[2..4], @intCast(request_put_image_header_units + pad4(raw_len) / 4));
        put32(header[4..8], drawable);
        put32(header[8..12], gc);
        put16(header[12..14], @intCast(w));
        put16(header[14..16], @intCast(rows));
        put16(header[16..18], 0);
        put16(header[18..20], @intCast(y));
        header[21] = image_depth;
        try writeAll(conn, &header);
        try writeAll(conn, data[0..pad4(raw_len)]);

        y += rows;
    }
}

fn encodeBgrx(dst: []u8, pixels: []const u32, w: u32, h: u32) void {
    var i: usize = 0;
    const count = @as(usize, w) * @as(usize, h);
    while (i < count) : (i += 1) {
        const px = pixels[i];
        dst[i * 4 + 0] = @intCast(px & 0xff);
        dst[i * 4 + 1] = @intCast((px >> 8) & 0xff);
        dst[i * 4 + 2] = @intCast((px >> 16) & 0xff);
        dst[i * 4 + 3] = 0;
    }
}

fn openUnixSocket(io: std.Io, path: []const u8) !net.Stream {
    const addr = try net.UnixAddress.init(path);
    return net.UnixAddress.connect(&addr, io) catch |err| switch (err) {
        error.FileNotFound => error.XServerUnavailable,
        error.AccessDenied, error.PermissionDenied => error.AccessDenied,
        else => err,
    };
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

fn readSomeFd(fd: i32, dst: []u8) !usize {
    while (true) {
        const rc = linux.read(fd, dst.ptr, dst.len);
        switch (linux.errno(rc)) {
            .SUCCESS => return @intCast(rc),
            .INTR => continue,
            .AGAIN => return error.WouldBlock,
            .CONNRESET => return error.ConnectionResetByPeer,
            else => return error.ReadFailed,
        }
    }
}

fn readSomeRaw(io: std.Io, stream: net.Stream, dst: []u8) !usize {
    var iov = [_][]u8{dst};
    return stream.read(io, iov[0..]);
}

fn readExactRaw(io: std.Io, stream: net.Stream, dst: []u8) !void {
    var off: usize = 0;
    while (off < dst.len) {
        const n = try readSomeRaw(io, stream, dst[off..]);
        if (n == 0) return error.EndOfStream;
        off += n;
    }
}

fn readExact(conn: *XConn, dst: []u8) !void {
    try readExactRaw(conn.io, conn.stream, dst);
}

fn writeAllRaw(io: std.Io, stream: net.Stream, bytes: []const u8) !void {
    var off: usize = 0;
    while (off < bytes.len) {
        const n = try io.vtable.netWrite(
            io.userdata,
            stream.socket.handle,
            "",
            &[_][]const u8{bytes[off..]},
            1,
        );
        if (n == 0) return error.WriteZero;
        off += n;
    }
}

fn writeAll(conn: *XConn, bytes: []const u8) !void {
    try writeAllRaw(conn.io, conn.stream, bytes);
}

pub fn pad4(n: usize) usize {
    return (n + 3) & ~@as(usize, 3);
}

pub fn putImageUnits(byte_len: usize) usize {
    return request_put_image_header_units + pad4(byte_len) / 4;
}

fn get16(bytes: []const u8) u16 {
    return std.mem.readInt(u16, bytes[0..2], .little);
}

fn get32(bytes: []const u8) u32 {
    return std.mem.readInt(u32, bytes[0..4], .little);
}

fn put16(bytes: []u8, value: u16) void {
    std.mem.writeInt(u16, bytes[0..2], value, .little);
}

fn put32(bytes: []u8, value: u32) void {
    std.mem.writeInt(u32, bytes[0..4], value, .little);
}

// --- Tests --------------------------------------------------------------------

test "x11 request padding and PutImage length math" {
    try std.testing.expectEqual(@as(usize, 0), pad4(0));
    try std.testing.expectEqual(@as(usize, 4), pad4(1));
    try std.testing.expectEqual(@as(usize, 4), pad4(4));
    try std.testing.expectEqual(@as(usize, 8), pad4(5));
    try std.testing.expectEqual(@as(usize, 7), putImageUnits(1));
    try std.testing.expectEqual(@as(usize, 8), putImageUnits(8));
}

test "x11 BGRX encoder ignores alpha and uses little-endian XImage order" {
    var out: [8]u8 = undefined;
    encodeBgrx(&out, &[_]u32{ 0xff112233, 0x80445566 }, 2, 1);
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0x33, 0x22, 0x11, 0, 0x66, 0x55, 0x44, 0 }, &out);
}

test "x11 DISPLAY parser accepts screen suffix" {
    try std.testing.expectEqual(@as(u16, 0), (try parseDisplay(":0")).number);
    try std.testing.expectEqual(@as(u16, 12), (try parseDisplay("localhost:12.1")).number);
    try std.testing.expectError(error.InvalidDisplay, parseDisplay("localhost"));
}

test "x11 setup parser reads spec offsets (vendor@16, screens@20, keycodes@26)" {
    // Synthetic setup body: vendor "Test" (4), one format, one screen.
    var body: [32 + 4 + 8 + 40]u8 = @splat(0);
    put32(body[4..8], 0x00400000); // resource base
    put32(body[8..12], 0x000fffff); // resource mask
    put16(body[16..18], 4); // vendor length
    put16(body[18..20], 0xffff); // max request
    body[20] = 1; // screens
    body[21] = 1; // formats
    body[26] = 8; // min keycode
    body[27] = 255; // max keycode
    @memcpy(body[32..36], "Test");
    const s = 32 + 4 + 8; // screen offset
    put32(body[s + 0 .. s + 4], 99); // root window
    put32(body[s + 8 .. s + 12], 0xffffff); // white
    put32(body[s + 12 .. s + 16], 0); // black
    put32(body[s + 32 .. s + 36], 0x21); // root visual
    body[s + 38] = 24; // root depth

    const setup = try parseSetup(&body);
    try std.testing.expectEqual(@as(u32, 99), setup.screen.root);
    try std.testing.expectEqual(@as(u32, 0x21), setup.screen.root_visual);
    try std.testing.expectEqual(@as(u8, 24), setup.screen.root_depth);
    try std.testing.expectEqual(@as(u16, 0xffff), setup.max_request_units);
    try std.testing.expectEqual(@as(u8, 8), setup.min_keycode);
    try std.testing.expectEqual(@as(u8, 255), setup.max_keycode);
    try std.testing.expectEqual(@as(u32, 0xffffff), setup.screen.white_pixel);
}

test "keysymToKey maps printable ASCII and editing keys" {
    try std.testing.expectEqual(Key{ .char = 'a' }, keysymToKey('a'));
    try std.testing.expectEqual(Key{ .char = ' ' }, keysymToKey(' '));
    try std.testing.expectEqual(Key{ .char = '~' }, keysymToKey('~'));
    try std.testing.expectEqual(Key.backspace, keysymToKey(0xff08));
    try std.testing.expectEqual(Key.enter, keysymToKey(0xff0d));
    try std.testing.expectEqual(Key.enter, keysymToKey(0xff8d));
    try std.testing.expectEqual(Key.escape, keysymToKey(0xff1b));
    try std.testing.expectEqual(Key.page_up, keysymToKey(0xff55));
    try std.testing.expectEqual(Key.other, keysymToKey(0xffe1)); // Shift_L
}

test "Keymap.translate: shift columns and alpha case rules" {
    // Two keycodes starting at min=8, 2 keysyms per keycode:
    //   keycode 8: 'a', NoSymbol  (alpha pair by case rule)
    //   keycode 9: '1', '!'       (explicit shifted column)
    var syms = [_]u32{ 'a', 0, '1', '!' };
    const km = Keymap{ .syms = &syms, .per = 2, .min = 8 };

    try std.testing.expectEqual(Key{ .char = 'a' }, km.translate(8, 0));
    try std.testing.expectEqual(Key{ .char = 'A' }, km.translate(8, 1)); // shift
    try std.testing.expectEqual(Key{ .char = 'A' }, km.translate(8, 2)); // capslock
    try std.testing.expectEqual(Key{ .char = '1' }, km.translate(9, 0));
    try std.testing.expectEqual(Key{ .char = '!' }, km.translate(9, 1));
    try std.testing.expectEqual(Key{ .char = '1' }, km.translate(9, 2)); // lock ≠ shift for digits
    try std.testing.expectEqual(Key.other, km.translate(7, 0)); // below min
}
