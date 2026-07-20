//! Portable connected socket boundary.
//!
//! Zig 0.17's Windows `std.Io.Threaded` AFD path is not implemented by Wine.
//! The Windows arm therefore uses Winsock directly; every other target keeps
//! the standard Zig stream. Both arms expose the same small transport API.

const std = @import("std");
const builtin = @import("builtin");
const net = std.Io.net;

pub const ConnectedSocket = union(enum) {
    standard: Standard,
    windows: Windows,

    const Standard = struct {
        io: std.Io,
        stream: net.Stream,
    };

    pub fn connect(io: std.Io, address: net.IpAddress, mode: net.Socket.Mode, timeout_ms: u32) !ConnectedSocket {
        if (comptime builtin.os.tag == .windows) {
            return .{ .windows = try Windows.connect(address, mode, timeout_ms) };
        }
        return .{ .standard = .{
            .io = io,
            .stream = try address.connect(io, .{ .mode = mode }),
        } };
    }

    pub fn close(self: *const ConnectedSocket) void {
        switch (self.*) {
            .standard => |standard| standard.stream.close(standard.io),
            .windows => |windows| if (comptime builtin.os.tag == .windows) windows.close() else unreachable,
        }
    }

    pub fn fd(self: *const ConnectedSocket) i32 {
        return switch (self.*) {
            .standard => |standard| standard.stream.socket.handle,
            // The Windows UI never polls this value; socket readiness is
            // handled inside recvTimeout. Avoid narrowing a pointer-sized
            // SOCKET into the POSIX-only public fd surface.
            .windows => -1,
        };
    }

    pub fn sendAll(self: *const ConnectedSocket, bytes: []const u8) !void {
        switch (self.*) {
            .standard => |standard| {
                var offset: usize = 0;
                while (offset < bytes.len) {
                    const n = try standard.io.vtable.netWrite(
                        standard.io.userdata,
                        standard.stream.socket.handle,
                        "",
                        &.{bytes[offset..]},
                        1,
                    );
                    if (n == 0) return error.WriteZero;
                    offset += n;
                }
            },
            .windows => |windows| if (comptime builtin.os.tag == .windows) try windows.sendAll(bytes) else unreachable,
        }
    }

    pub fn recv(self: *const ConnectedSocket, destination: []u8) !usize {
        return switch (self.*) {
            .standard => |standard| blk: {
                var buffers = [_][]u8{destination};
                break :blk try standard.stream.read(standard.io, &buffers);
            },
            .windows => |windows| if (comptime builtin.os.tag == .windows) windows.recv(destination) else unreachable,
        };
    }

    pub fn recvTimeout(self: *const ConnectedSocket, destination: []u8, timeout_ms: u32) !?usize {
        return switch (self.*) {
            .standard => |standard| blk: {
                var buffers = [_][]u8{destination};
                const result = standard.io.operateTimeout(.{ .net_read = .{
                    .socket_handle = standard.stream.socket.handle,
                    .data = &buffers,
                } }, .{ .duration = .{
                    .raw = std.Io.Duration.fromMilliseconds(timeout_ms),
                    .clock = .awake,
                } }) catch |err| switch (err) {
                    error.Timeout => break :blk null,
                    else => return err,
                };
                break :blk try result.net_read;
            },
            .windows => |windows| if (comptime builtin.os.tag == .windows) windows.recvTimeout(destination, timeout_ms) else unreachable,
        };
    }
};

const Windows = struct {
    handle: usize,

    const invalid_socket = std.math.maxInt(usize);
    const socket_error = -1;
    const fionbio: i32 = @bitCast(@as(u32, 0x8004667e));
    const wsa_would_block = 10035;
    const wsa_in_progress = 10036;

    const SocketData = extern struct {
        version: u16,
        high_version: u16,
        max_sockets: u16,
        max_udp_datagram: u16,
        vendor_info: ?[*:0]u8,
        description: [257]u8,
        system_status: [129]u8,
    };

    const FdSet = extern struct {
        count: u32 = 0,
        sockets: [64]usize = @splat(0),
    };

    const Api = struct {
        extern "ws2_32" fn WSAStartup(version: u16, data: *SocketData) callconv(.winapi) c_int;
        extern "ws2_32" fn WSACleanup() callconv(.winapi) c_int;
        extern "ws2_32" fn WSAGetLastError() callconv(.winapi) c_int;
        extern "ws2_32" fn socket(family: c_int, socket_type: c_int, protocol: c_int) callconv(.winapi) usize;
        extern "ws2_32" fn connect(handle: usize, address: *const std.os.windows.ws2_32.sockaddr, address_len: c_int) callconv(.winapi) c_int;
        extern "ws2_32" fn closesocket(handle: usize) callconv(.winapi) c_int;
        extern "ws2_32" fn send(handle: usize, bytes: [*]const u8, len: c_int, flags: c_int) callconv(.winapi) c_int;
        extern "ws2_32" fn recv(handle: usize, bytes: [*]u8, len: c_int, flags: c_int) callconv(.winapi) c_int;
        extern "ws2_32" fn ioctlsocket(handle: usize, command: i32, value: *u32) callconv(.winapi) c_int;
        extern "ws2_32" fn select(
            ignored: c_int,
            read_set: ?*FdSet,
            write_set: ?*FdSet,
            error_set: ?*FdSet,
            timeout: ?*std.os.windows.ws2_32.timeval,
        ) callconv(.winapi) c_int;
        extern "ws2_32" fn getsockopt(handle: usize, level: c_int, option: c_int, value: [*]u8, value_len: *c_int) callconv(.winapi) c_int;
    };

    fn connect(address: net.IpAddress, mode: net.Socket.Mode, timeout_ms: u32) !Windows {
        var socket_data: SocketData = undefined;
        if (Api.WSAStartup(0x0202, &socket_data) != 0) return error.SocketUnavailable;
        errdefer _ = Api.WSACleanup();

        const ws2 = std.os.windows.ws2_32;
        const family: c_int = switch (address) {
            .ip4 => ws2.AF.INET,
            .ip6 => ws2.AF.INET6,
        };
        const socket_type: c_int = switch (mode) {
            .stream => ws2.SOCK.STREAM,
            .dgram => ws2.SOCK.DGRAM,
            else => return error.SocketModeUnsupported,
        };
        const protocol: c_int = switch (mode) {
            .stream => ws2.IPPROTO.TCP,
            .dgram => ws2.IPPROTO.UDP,
            else => unreachable,
        };
        const handle = Api.socket(family, socket_type, protocol);
        if (handle == invalid_socket) return error.SocketUnavailable;
        errdefer _ = Api.closesocket(handle);

        var nonblocking: u32 = 1;
        if (Api.ioctlsocket(handle, fionbio, &nonblocking) == socket_error) return error.SocketUnavailable;
        const connected = switch (address) {
            .ip4 => |ipv4| blk: {
                var socket_address = ws2.sockaddr.in{
                    .port = std.mem.nativeToBig(u16, ipv4.port),
                    .addr = @bitCast(ipv4.bytes),
                };
                break :blk Api.connect(handle, @ptrCast(&socket_address), @sizeOf(ws2.sockaddr.in));
            },
            .ip6 => |ipv6| blk: {
                var socket_address = ws2.sockaddr.in6{
                    .port = std.mem.nativeToBig(u16, ipv6.port),
                    .flowinfo = ipv6.flow,
                    .addr = ipv6.bytes,
                    .scope_id = ipv6.interface.index,
                };
                break :blk Api.connect(handle, @ptrCast(&socket_address), @sizeOf(ws2.sockaddr.in6));
            },
        };
        if (connected == socket_error) {
            const connect_error = Api.WSAGetLastError();
            if (connect_error != wsa_would_block and connect_error != wsa_in_progress) return error.ConnectFailed;
            var writable = oneSocketSet(handle);
            var failed = oneSocketSet(handle);
            var timeout = timeoutValue(timeout_ms);
            const ready = Api.select(0, null, &writable, &failed, &timeout);
            if (ready == 0) return error.Timeout;
            if (ready == socket_error) return error.ConnectFailed;
            var result: c_int = 0;
            var result_len: c_int = @sizeOf(c_int);
            if (Api.getsockopt(handle, ws2.SOL.SOCKET, ws2.SO.ERROR, @ptrCast(&result), &result_len) == socket_error or result != 0)
                return error.ConnectFailed;
        }
        var blocking: u32 = 0;
        if (Api.ioctlsocket(handle, fionbio, &blocking) == socket_error) return error.SocketUnavailable;
        return .{ .handle = handle };
    }

    fn close(self: Windows) void {
        _ = Api.closesocket(self.handle);
        _ = Api.WSACleanup();
    }

    fn sendAll(self: Windows, bytes: []const u8) !void {
        var offset: usize = 0;
        while (offset < bytes.len) {
            const amount: c_int = @intCast(@min(bytes.len - offset, @as(usize, std.math.maxInt(c_int))));
            const written = Api.send(self.handle, bytes.ptr + offset, amount, 0);
            if (written == socket_error) return error.WriteFailed;
            if (written == 0) return error.WriteZero;
            offset += @intCast(written);
        }
    }

    fn recv(self: Windows, destination: []u8) !usize {
        if (destination.len == 0) return 0;
        const amount: c_int = @intCast(@min(destination.len, @as(usize, std.math.maxInt(c_int))));
        const received = Api.recv(self.handle, destination.ptr, amount, 0);
        if (received == socket_error) return error.ReadFailed;
        return @intCast(received);
    }

    fn recvTimeout(self: Windows, destination: []u8, timeout_ms: u32) !?usize {
        var readable = oneSocketSet(self.handle);
        var failed = oneSocketSet(self.handle);
        var timeout = timeoutValue(timeout_ms);
        const ready = Api.select(0, &readable, null, &failed, &timeout);
        if (ready == 0) return null;
        if (ready == socket_error or failed.count != 0) return error.ReadFailed;
        return try self.recv(destination);
    }

    fn oneSocketSet(handle: usize) FdSet {
        var set = FdSet{};
        set.count = 1;
        set.sockets[0] = handle;
        return set;
    }

    fn timeoutValue(timeout_ms: u32) std.os.windows.ws2_32.timeval {
        return .{
            .sec = @intCast(timeout_ms / 1000),
            .usec = @intCast((timeout_ms % 1000) * 1000),
        };
    }
};

test "connected socket API remains a compile-time platform boundary" {
    try std.testing.expect(@sizeOf(ConnectedSocket) > 0);
}
