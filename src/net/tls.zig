//! Verified TLS 1.2/1.3 client transport backed by the pinned official
//! mbedTLS 3.6.6 sources. Certificate verification and SNI are mandatory.

const std = @import("std");
const builtin = @import("builtin");

const Context = opaque {};

extern fn cc_tls_connect_fd(
    host: [*:0]const u8,
    socket_fd: isize,
    ca_file: ?[*:0]const u8,
    handshake_timeout_ms: u32,
    out_context: *?*Context,
    native_error: *c_int,
) c_int;
extern fn cc_tls_free(context: ?*Context) void;
extern fn cc_tls_fd(context: *const Context) c_int;
extern fn cc_tls_write(context: *Context, bytes: [*]const u8, length: usize) c_int;
extern fn cc_tls_read(context: *Context, bytes: [*]u8, length: usize) c_int;
extern fn cc_tls_read_timeout(context: *Context, bytes: [*]u8, length: usize, milliseconds: u32) c_int;
extern fn cc_tls_is_timeout(result: c_int) c_int;
extern fn cc_tls_version_number() u32;

pub const expected_version: u32 = 0x03060600;

pub const ConnectOptions = struct {
    /// PEM bundle override. Null loads the operating-system/default CA roots.
    ca_file: ?[]const u8 = null,
    handshake_timeout_ms: u32 = 15_000,
};

pub const TlsTransport = struct {
    gpa: std.mem.Allocator,
    context: *Context,

    /// Start verified TLS over an already connected stream socket. Ownership
    /// of `stream` transfers on entry, including every error path.
    pub fn connectSocket(
        gpa: std.mem.Allocator,
        io: std.Io,
        stream: std.Io.net.Stream,
        host: []const u8,
        options: ConnectOptions,
    ) !*TlsTransport {
        var native_owns_socket = false;
        defer if (!native_owns_socket) stream.close(io);
        const self = try gpa.create(TlsTransport);
        errdefer gpa.destroy(self);

        const host_z = try gpa.dupeSentinel(u8, host, 0);
        defer {
            std.crypto.secureZero(u8, host_z);
            gpa.free(host_z);
        }
        const ca_z = if (options.ca_file) |path| try gpa.dupeSentinel(u8, path, 0) else null;
        defer if (ca_z) |path| {
            std.crypto.secureZero(u8, path);
            gpa.free(path);
        };

        var context: ?*Context = null;
        var native_error: c_int = 0;
        const socket_fd: isize = if (comptime builtin.os.tag == .windows)
            @intCast(@intFromPtr(stream.socket.handle))
        else
            @intCast(stream.socket.handle);
        native_owns_socket = true;
        const stage = cc_tls_connect_fd(
            host_z.ptr,
            socket_fd,
            if (ca_z) |path| path.ptr else null,
            options.handshake_timeout_ms,
            &context,
            &native_error,
        );
        if (stage != 0) return mapConnectStage(stage);
        self.* = .{ .gpa = gpa, .context = context orelse return error.TlsConfigurationFailed };
        return self;
    }

    pub fn deinit(self: *TlsTransport) void {
        cc_tls_free(self.context);
        const gpa = self.gpa;
        std.crypto.secureZero(u8, std.mem.asBytes(self));
        gpa.destroy(self);
    }

    pub fn fd(self: *const TlsTransport) i32 {
        return @intCast(cc_tls_fd(self.context));
    }

    pub fn send(self: *TlsTransport, bytes: []const u8) !void {
        var offset: usize = 0;
        while (offset < bytes.len) {
            const result = cc_tls_write(self.context, bytes[offset..].ptr, bytes.len - offset);
            if (result < 0) return error.TlsWriteFailed;
            if (result == 0) return error.WriteZero;
            offset += @intCast(result);
        }
    }

    pub fn recv(self: *TlsTransport, dst: []u8) !usize {
        if (dst.len == 0) return 0;
        const result = cc_tls_read(self.context, dst.ptr, dst.len);
        if (result < 0) return error.TlsReadFailed;
        return @intCast(result);
    }

    pub fn recvTimeout(self: *TlsTransport, dst: []u8, milliseconds: i64) !?usize {
        if (dst.len == 0) return 0;
        const bounded: u32 = @intCast(@max(0, @min(milliseconds, std.math.maxInt(u32))));
        const result = cc_tls_read_timeout(self.context, dst.ptr, dst.len, bounded);
        if (cc_tls_is_timeout(result) != 0) return null;
        if (result < 0) return error.TlsReadFailed;
        return @intCast(result);
    }
};

fn mapConnectStage(stage: c_int) anyerror {
    return switch (stage) {
        1 => error.OutOfMemory,
        2 => error.PsaInitializationFailed,
        3 => error.EntropyInitializationFailed,
        4 => error.CertificateAuthorityLoadFailed,
        5 => error.TlsConfigurationFailed,
        6 => error.InvalidTlsHostname,
        7 => error.ConnectionFailed,
        8 => error.TlsHandshakeFailed,
        9 => error.CertificateVerificationFailed,
        else => error.TlsConfigurationFailed,
    };
}

test "linked mbedTLS release is exactly 3.6.6" {
    try std.testing.expectEqual(expected_version, cc_tls_version_number());
}

test "TLS setup stages have deterministic public errors" {
    try std.testing.expect(mapConnectStage(4) == error.CertificateAuthorityLoadFailed);
    try std.testing.expect(mapConnectStage(8) == error.TlsHandshakeFailed);
    try std.testing.expect(mapConnectStage(9) == error.CertificateVerificationFailed);
}
