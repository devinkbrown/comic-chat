//! Verified TLS 1.3 transport backed by Onyx's portable record engine.

const std = @import("std");
const onyx_root = @import("onyx_tls");
const onyx = onyx_root.crypto.tls_client;
const certs = onyx_root.daemon.tls_certs;
const onyx_sign = onyx_root.crypto.sign;

pub const ConnectOptions = struct {
    ca_file: ?[]const u8 = null,
    client_cert_file: ?[]const u8 = null,
    handshake_timeout_ms: u32 = 15_000,
};

pub const TlsTransport = struct {
    gpa: std.mem.Allocator,
    threaded: std.Io.Threaded,
    io: std.Io,
    stream: std.Io.net.Stream,
    client: onyx.Client,
    roots: std.crypto.Certificate.Bundle,
    client_material: ?certs.Loaded = null,
    pending: std.ArrayList(u8) = .empty,

    pub fn connectSocket(gpa: std.mem.Allocator, io_unused: std.Io, stream: std.Io.net.Stream, host: []const u8, options: ConnectOptions) !*TlsTransport {
        _ = io_unused;
        const self = try gpa.create(TlsTransport);
        errdefer gpa.destroy(self);
        self.gpa = gpa;
        self.threaded = std.Io.Threaded.init(gpa, .{});
        errdefer self.threaded.deinit();
        self.io = self.threaded.io();
        self.stream = stream;
        errdefer self.stream.close(self.io);
        self.roots = .empty;
        errdefer self.roots.deinit(gpa);
        const now = std.Io.Clock.real.now(self.io);
        if (options.ca_file) |path| {
            try self.roots.addCertsFromFilePathAbsolute(gpa, self.io, now, path);
        } else try self.roots.rescan(gpa, self.io, now);

        var anchors: std.ArrayList([]const u8) = .empty;
        defer anchors.deinit(gpa);
        var it = self.roots.map.iterator();
        while (it.next()) |entry| {
            const start = entry.value_ptr.*;
            const element = try std.crypto.Certificate.der.Element.parse(self.roots.bytes.items, start);
            try anchors.append(gpa, self.roots.bytes.items[start..element.slice.end]);
        }
        if (anchors.items.len == 0) return error.CertificateAuthorityLoadFailed;
        self.client = try onyx.Client.init(gpa, .{ .server_name = host, .trust_anchors = anchors.items, .now_unix_seconds = now.toSeconds() });
        errdefer self.client.deinit();
        if (options.client_cert_file) |path| {
            var material = try certs.loadOrBootstrap(gpa, self.io, .{ .cert_path = path, .key_path = path });
            errdefer material.deinit(gpa);
            switch (material.key_kind) {
                .ed25519 => self.client.setClientCertForTest(material.cert_chain[0], .{
                    .public_key = material.signing_key.?.public_key.toBytes(),
                    .secret_key = onyx_sign.SecretKey.init(material.signing_key.?.secret_key.toBytes()),
                }),
                .ecdsa_p256 => self.client.setClientCertEcdsaP256ForTest(material.cert_chain[0], material.ecdsa_p256_signing_key.?),
                .rsa => self.client.setClientCertRsaForTest(material.cert_chain[0], material.rsa_signing_key.?),
            }
            self.client_material = material;
        }
        const hello = try self.client.start();
        defer gpa.free(hello);
        try self.writeAll(hello);
        try self.finishHandshake(options.handshake_timeout_ms);
        return self;
    }

    pub fn deinit(self: *TlsTransport) void {
        self.client.deinit();
        if (self.client_material) |*material| material.deinit(self.gpa);
        self.pending.deinit(self.gpa);
        self.roots.deinit(self.gpa);
        self.stream.close(self.io);
        self.threaded.deinit();
        self.gpa.destroy(self);
    }

    pub fn fd(self: *const TlsTransport) i32 {
        return self.stream.socket.handle;
    }

    pub fn send(self: *TlsTransport, bytes: []const u8) !void {
        const record = try self.client.encrypt(bytes);
        defer self.gpa.free(record);
        try self.writeAll(record);
    }

    pub fn recv(self: *TlsTransport, dst: []u8) !usize {
        return (try self.recvInner(dst, null)) orelse 0;
    }
    pub fn recvTimeout(self: *TlsTransport, dst: []u8, milliseconds: i64) !?usize {
        return self.recvInner(dst, @intCast(@max(0, milliseconds)));
    }

    fn finishHandshake(self: *TlsTransport, timeout_ms: u32) !void {
        while (!self.client.handshakeDone()) {
            const record = try self.readRecord(timeout_ms);
            defer self.gpa.free(record);
            switch (try self.client.feed(record)) {
                .need_more => {},
                .bytes_to_send => |out| {
                    defer self.gpa.free(out);
                    try self.writeAll(out);
                },
            }
        }
    }

    fn recvInner(self: *TlsTransport, dst: []u8, timeout_ms: ?u32) !?usize {
        if (self.pending.items.len != 0) return self.takePending(dst);
        while (true) {
            const record = self.readRecord(timeout_ms orelse std.math.maxInt(u32)) catch |err| switch (err) {
                error.Timeout => return null,
                else => return err,
            };
            defer self.gpa.free(record);
            const read = try self.client.decryptApp(record);
            switch (read) {
                .application_data => |plain| {
                    defer self.gpa.free(plain);
                    try self.pending.appendSlice(self.gpa, plain);
                    return self.takePending(dst);
                },
                .control => if (try self.client.takePendingSend()) |out| {
                    defer self.gpa.free(out);
                    try self.writeAll(out);
                },
            }
        }
    }

    fn takePending(self: *TlsTransport, dst: []u8) usize {
        const n = @min(dst.len, self.pending.items.len);
        @memcpy(dst[0..n], self.pending.items[0..n]);
        const rest = self.pending.items.len - n;
        std.mem.copyForwards(u8, self.pending.items[0..rest], self.pending.items[n..]);
        self.pending.items.len = rest;
        return n;
    }

    fn writeAll(self: *TlsTransport, bytes: []const u8) !void {
        var off: usize = 0;
        while (off < bytes.len) {
            const n = try self.io.vtable.netWrite(self.io.userdata, self.stream.socket.handle, "", &.{bytes[off..]}, 1);
            if (n == 0) return error.WriteZero;
            off += n;
        }
    }

    fn readRecord(self: *TlsTransport, timeout_ms: u32) ![]u8 {
        var header: [5]u8 = undefined;
        try self.readExact(&header, timeout_ms);
        const len = std.mem.readInt(u16, header[3..5], .big);
        if (len > 18432) return error.TlsReadFailed;
        const record = try self.gpa.alloc(u8, 5 + len);
        errdefer self.gpa.free(record);
        @memcpy(record[0..5], &header);
        try self.readExact(record[5..], timeout_ms);
        return record;
    }

    fn readExact(self: *TlsTransport, dst: []u8, timeout_ms: u32) !void {
        var off: usize = 0;
        while (off < dst.len) {
            var iov = [_][]u8{dst[off..]};
            const result = self.io.operateTimeout(.{ .net_read = .{ .socket_handle = self.stream.socket.handle, .data = iov[0..] } }, .{ .duration = .{ .raw = std.Io.Duration.fromMilliseconds(timeout_ms), .clock = .awake } }) catch |err| switch (err) {
                error.Timeout => return error.Timeout,
                else => return err,
            };
            const n = try result.net_read;
            if (n == 0) return error.TlsReadFailed;
            off += n;
        }
    }
};
