//! Minimal pure-Zig X11 window backend.
//!
//! Talks to the local X server over `/tmp/.X11-unix/X<n>` and uploads an
//! RGBA framebuffer with PutImage. No Xlib, no C imports.

const std = @import("std");
const linux = std.os.linux;
const net = std.Io.net;

const image_depth = 24;
const z_pixmap = 2;

const input_output = 1;

const event_key_press: u32 = 1 << 0;
const event_exposure: u32 = 1 << 15;
const event_structure: u32 = 1 << 17;

const cw_back_pixel: u32 = 1 << 1;
const cw_border_pixel: u32 = 1 << 3;
const cw_event_mask: u32 = 1 << 11;

const gc_foreground: u32 = 1 << 2;
const gc_background: u32 = 1 << 3;

const atom_atom = 4;
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

/// Open a local X11 window, draw `pixels` (0xAARRGGBB), and run until keypress
/// or WM close. DISPLAY is read at runtime; tests/builds do not need X.
pub fn show(gpa: std.mem.Allocator, pixels: []const u32, w: u32, h: u32) !void {
    if (pixels.len != @as(usize, w) * @as(usize, h)) return error.BadFramebufferSize;

    const display = try readDisplay(gpa);
    defer gpa.free(display);
    const parsed = try parseDisplay(display);

    var threaded = std.Io.Threaded.init(gpa, .{});
    defer threaded.deinit();
    const io = threaded.io();

    var conn = try connectDisplay(gpa, io, parsed);
    defer conn.stream.close(io);
    if (conn.screen.root_depth != image_depth) return error.UnsupportedDepth;

    const window = try conn.allocId();
    const gc = try conn.allocId();
    try createWindow(&conn, window, @intCast(w), @intCast(h));
    try createGc(&conn, gc, window);
    try installWmClose(&conn, window);
    try mapWindow(&conn, window);
    try putImage(gpa, &conn, window, gc, pixels, w, h);
    try eventLoop(gpa, &conn, window, gc, pixels, w, h);
}

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

    var hello: [12]u8 = [_]u8{0} ** 12;
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
        .resource_mask = setup.resource_mask,
        .screen = setup,
        .max_request_units = if (setupMaxRequest(body) >= min_max_request_units) setupMaxRequest(body) else min_max_request_units,
    };
}

fn parseSetup(body: []const u8) !Screen {
    if (body.len < 32) return error.ShortSetupReply;
    const resource_base = get32(body[4..8]);
    const resource_mask = get32(body[8..12]);
    const vendor_len = get16(body[20..22]);
    const roots_len = body[24];
    const formats_len = body[25];
    if (roots_len == 0) return error.NoScreen;

    const off: usize = 32 + pad4(vendor_len) + @as(usize, formats_len) * 8;
    if (off + 40 > body.len) return error.ShortSetupReply;

    const root = get32(body[off + 0 .. off + 4]);
    const white = get32(body[off + 8 .. off + 12]);
    const black = get32(body[off + 12 .. off + 16]);
    const root_visual = get32(body[off + 32 .. off + 36]);
    const root_depth = body[off + 38];

    return .{
        .resource_base = resource_base,
        .resource_mask = resource_mask,
        .root = root,
        .root_visual = root_visual,
        .root_depth = root_depth,
        .white_pixel = white,
        .black_pixel = black,
    };
}

fn setupMaxRequest(body: []const u8) u16 {
    if (body.len < 22) return min_max_request_units;
    return get16(body[18..20]);
}

fn createWindow(conn: *XConn, window: u32, w: u16, h: u16) !void {
    const values = [_]u32{
        conn.screen.white_pixel,
        conn.screen.black_pixel,
        event_key_press | event_exposure | event_structure,
    };
    const value_mask = cw_back_pixel | cw_border_pixel | cw_event_mask;
    var req: [44]u8 = [_]u8{0} ** 44;
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
    var req: [24]u8 = [_]u8{0} ** 24;
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

    var req: [28]u8 = [_]u8{0} ** 28;
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

    var reply: [32]u8 = undefined;
    try readExact(conn, &reply);
    if (reply[0] == 0) return error.X11ServerError;
    if (reply[0] != 1) return error.UnexpectedEventWhileWaitingForReply;
    const extra = @as(usize, get32(reply[4..8])) * 4;
    if (extra != 0) try discardExact(conn, extra);
    return get32(reply[8..12]);
}

fn mapWindow(conn: *XConn, window: u32) !void {
    var req: [8]u8 = [_]u8{0} ** 8;
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

        var header: [24]u8 = [_]u8{0} ** 24;
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

fn eventLoop(
    gpa: std.mem.Allocator,
    conn: *XConn,
    window: u32,
    gc: u32,
    pixels: []const u32,
    w: u32,
    h: u32,
) !void {
    while (true) {
        var event: [32]u8 = undefined;
        try readExact(conn, &event);
        const kind = event[0] & 0x7f;
        switch (kind) {
            0 => return error.X11ServerError,
            2 => return,
            12 => try putImage(gpa, conn, window, gc, pixels, w, h),
            17 => return,
            33 => {
                if (get32(event[8..12]) == conn.wm_protocols and get32(event[12..16]) == conn.wm_delete_window) return;
            },
            else => {},
        }
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
    return io.vtable.netRead(io.userdata, stream.socket.handle, iov[0..]);
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

fn discardExact(conn: *XConn, len: usize) !void {
    var remaining = len;
    var buf: [256]u8 = undefined;
    while (remaining > 0) {
        const n = @min(remaining, buf.len);
        try readExact(conn, buf[0..n]);
        remaining -= n;
    }
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
