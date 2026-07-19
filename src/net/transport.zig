//! IRC stream transport. Verified mbedTLS is the secure default; the original
//! std.Io plaintext socket remains available only through an explicit option.

const std = @import("std");
const net = std.Io.net;
const TlsTransport = @import("tls.zig").TlsTransport;
const policy = @import("connection_policy.zig");

pub const Security = enum {
    tls,
    plaintext,
};

pub const ProxyEndpoint = struct {
    host: []const u8,
    port: u16,
};

pub const Proxy = union(enum) {
    direct,
    socks5: ProxyEndpoint,
    http_connect: ProxyEndpoint,
};

pub const ConnectOptions = struct {
    security: Security = .tls,
    ca_file: ?[]const u8 = null,
    client_cert_file: ?[]const u8 = null,
    proxy: Proxy = .direct,
    /// Applied to each concurrently raced address and each proxy read.
    connect_timeout_ms: u32 = 15_000,
};

pub const Transport = struct {
    gpa: std.mem.Allocator,
    backend: Backend,

    const Backend = union(enum) {
        tls: *TlsTransport,
        plaintext: *PlainTransport,
    };

    /// Open a verified TLS connection using system/default CA roots.
    pub fn connect(gpa: std.mem.Allocator, host: []const u8, port: u16) !*Transport {
        return connectWithOptions(gpa, host, port, .{});
    }

    pub fn connectWithOptions(
        gpa: std.mem.Allocator,
        host: []const u8,
        port: u16,
        options: ConnectOptions,
    ) !*Transport {
        try validateConnectInputs(host, port, options);
        const self = try gpa.create(Transport);
        errdefer gpa.destroy(self);
        self.gpa = gpa;
        self.backend = switch (options.security) {
            .tls => tls: {
                var threaded = std.Io.Threaded.init(gpa, .{});
                defer threaded.deinit();
                const io = threaded.io();
                const stream = try connectStream(gpa, io, host, port, options);
                break :tls .{ .tls = try TlsTransport.connectSocket(
                    gpa,
                    io,
                    stream,
                    host,
                    .{
                        .ca_file = options.ca_file,
                        .client_cert_file = options.client_cert_file,
                        .handshake_timeout_ms = options.connect_timeout_ms,
                    },
                ) };
            },
            .plaintext => .{ .plaintext = try PlainTransport.connectWithOptions(gpa, host, port, options) },
        };
        return self;
    }

    pub fn deinit(self: *Transport) void {
        switch (self.backend) {
            .tls => |transport| transport.deinit(),
            .plaintext => |transport| transport.deinit(),
        }
        const gpa = self.gpa;
        self.* = undefined;
        gpa.destroy(self);
    }

    pub fn fd(self: *const Transport) i32 {
        return switch (self.backend) {
            .tls => |transport| transport.fd(),
            .plaintext => |transport| transport.fd(),
        };
    }

    pub fn send(self: *Transport, bytes: []const u8) !void {
        return switch (self.backend) {
            .tls => |transport| transport.send(bytes),
            .plaintext => |transport| transport.send(bytes),
        };
    }

    pub fn recv(self: *Transport, dst: []u8) !usize {
        return switch (self.backend) {
            .tls => |transport| transport.recv(dst),
            .plaintext => |transport| transport.recv(dst),
        };
    }

    pub fn recvTimeout(self: *Transport, dst: []u8, milliseconds: i64) !?usize {
        return switch (self.backend) {
            .tls => |transport| transport.recvTimeout(dst, milliseconds),
            .plaintext => |transport| transport.recvTimeout(dst, milliseconds),
        };
    }
};

/// Compatibility backend for IRC servers which explicitly do not offer TLS.
/// The struct pins itself because its std.Io vtable references `threaded`.
const PlainTransport = struct {
    gpa: std.mem.Allocator,
    threaded: std.Io.Threaded,
    io: std.Io,
    stream: net.Stream,

    /// Resolve `host` and open a TCP stream to `host:port`. Returns a heap
    /// pointer: the `Io` vtable references `self.threaded`, so it must not move.
    pub fn connect(
        gpa: std.mem.Allocator,
        host: []const u8,
        port: u16,
    ) !*PlainTransport {
        return connectWithOptions(gpa, host, port, .{ .security = .plaintext });
    }

    pub fn connectWithOptions(
        gpa: std.mem.Allocator,
        host: []const u8,
        port: u16,
        options: ConnectOptions,
    ) !*PlainTransport {
        const self = try gpa.create(PlainTransport);
        errdefer gpa.destroy(self);

        self.gpa = gpa;
        self.threaded = std.Io.Threaded.init(gpa, .{});
        errdefer self.threaded.deinit();
        self.io = self.threaded.io();

        self.stream = try connectStream(gpa, self.io, host, port, options);
        return self;
    }

    pub fn deinit(self: *PlainTransport) void {
        self.stream.close(self.io);
        self.threaded.deinit();
        self.gpa.destroy(self);
    }

    /// Native socket handle for integration with a platform event loop.
    /// The caller does not own the handle and must not close it.
    pub fn fd(self: *const PlainTransport) i32 {
        return self.stream.socket.handle;
    }

    /// Send all bytes (e.g. a CRLF-terminated command). Loops until every byte
    /// is written. Uses the Io vtable directly to avoid buffered-writer flush
    /// semantics.
    pub fn send(self: *PlainTransport, bytes: []const u8) !void {
        const handle = self.stream.socket.handle;
        var off: usize = 0;
        while (off < bytes.len) {
            // header empty; the payload is the sole `data` buffer, written once
            // (splat = 1). netWrite requires a non-empty `data` slice.
            const n = try self.io.vtable.netWrite(
                self.io.userdata,
                handle,
                "",
                &[_][]const u8{bytes[off..]},
                1,
            );
            if (n == 0) return error.WriteZero;
            off += n;
        }
    }

    /// One read into `dst`; returns bytes read, or 0 at end of stream. Does NOT
    /// block waiting to fill `dst` (unlike Reader.readSliceShort).
    pub fn recv(self: *PlainTransport, dst: []u8) !usize {
        var iov = [_][]u8{dst};
        return self.stream.read(self.io, iov[0..]);
    }

    /// Read once, returning null when `milliseconds` elapse without data.
    /// Win32 uses this to interleave its non-pollable message queue with IRC.
    pub fn recvTimeout(self: *PlainTransport, dst: []u8, milliseconds: i64) !?usize {
        var iov = [_][]u8{dst};
        const result = self.io.operateTimeout(.{ .net_read = .{
            .socket_handle = self.stream.socket.handle,
            .data = iov[0..],
        } }, .{ .duration = .{
            .raw = std.Io.Duration.fromMilliseconds(milliseconds),
            .clock = .awake,
        } }) catch |err| switch (err) {
            error.Timeout => return null,
            else => return err,
        };
        return try result.net_read;
    }
};

fn connectStream(
    gpa: std.mem.Allocator,
    io: std.Io,
    target_host: []const u8,
    target_port: u16,
    options: ConnectOptions,
) !net.Stream {
    const endpoint = switch (options.proxy) {
        .direct => ProxyEndpoint{ .host = target_host, .port = target_port },
        .socks5 => |proxy| proxy,
        .http_connect => |proxy| proxy,
    };
    var stream = try connectEndpoint(io, endpoint, options.connect_timeout_ms);
    errdefer stream.close(io);
    switch (options.proxy) {
        .direct => {},
        .socks5 => try performSocks5(gpa, io, &stream, target_host, target_port, options.connect_timeout_ms),
        .http_connect => try performHttpConnect(gpa, io, &stream, target_host, target_port, options.connect_timeout_ms),
    }
    return stream;
}

fn connectEndpoint(io: std.Io, endpoint: ProxyEndpoint, timeout_ms: u32) !net.Stream {
    // Zig 0.17's Threaded POSIX backend currently panics when netConnectIp is
    // given a non-null timeout. The whole connect runs on Connector's owned
    // worker, so keep the UI nonblocking and let the bounded proxy reads and
    // TLS handshake enforce their own deadlines until std supports this.
    _ = timeout_ms;
    if (net.IpAddress.resolve(io, endpoint.host, endpoint.port)) |address| {
        return address.connect(io, .{ .mode = .stream });
    } else |_| {}
    const host_name = try net.HostName.init(endpoint.host);
    // HostName.connect resolves both families, races a bounded 32-address
    // queue concurrently, cancels losing sockets, and returns the first
    // successful stream.
    return host_name.connect(io, endpoint.port, .{ .mode = .stream });
}

fn streamWriteAll(io: std.Io, stream: *const net.Stream, bytes: []const u8) !void {
    var offset: usize = 0;
    while (offset < bytes.len) {
        const n = try io.vtable.netWrite(
            io.userdata,
            stream.socket.handle,
            "",
            &[_][]const u8{bytes[offset..]},
            1,
        );
        if (n == 0) return error.WriteZero;
        offset += n;
    }
}

fn streamReadTimeout(io: std.Io, stream: *const net.Stream, dst: []u8, timeout_ms: u32) !usize {
    var iov = [_][]u8{dst};
    const result = io.operateTimeout(.{ .net_read = .{
        .socket_handle = stream.socket.handle,
        .data = iov[0..],
    } }, .{ .duration = .{
        .raw = std.Io.Duration.fromMilliseconds(timeout_ms),
        .clock = .awake,
    } }) catch |err| switch (err) {
        error.Timeout => return error.ProxyHandshakeTimeout,
        else => return err,
    };
    const received = try result.net_read;
    if (received == 0) return error.ProxyClosed;
    return received;
}

fn streamReadExact(io: std.Io, stream: *const net.Stream, dst: []u8, timeout_ms: u32) !void {
    var offset: usize = 0;
    while (offset < dst.len) offset += try streamReadTimeout(io, stream, dst[offset..], timeout_ms);
}

fn performSocks5(
    gpa: std.mem.Allocator,
    io: std.Io,
    stream: *const net.Stream,
    host: []const u8,
    port: u16,
    timeout_ms: u32,
) !void {
    var wire: std.ArrayList(u8) = .empty;
    defer wire.deinit(gpa);
    try policy.socks5.appendGreeting(&wire, gpa);
    try streamWriteAll(io, stream, wire.items);
    var greeting: [2]u8 = undefined;
    try streamReadExact(io, stream, &greeting, timeout_ms);
    try policy.socks5.parseGreeting(&greeting);

    wire.clearRetainingCapacity();
    try policy.socks5.appendConnect(&wire, gpa, host, port);
    try streamWriteAll(io, stream, wire.items);
    var prefix: [4]u8 = undefined;
    try streamReadExact(io, stream, &prefix, timeout_ms);
    try policy.socks5.parseConnectPrefix(&prefix);
    var address_bytes: usize = switch (prefix[3]) {
        0x01 => 4,
        0x04 => 16,
        0x03 => domain: {
            var length: [1]u8 = undefined;
            try streamReadExact(io, stream, &length, timeout_ms);
            break :domain length[0];
        },
        else => return error.InvalidProxyResponse,
    };
    // Discard the bounded bind address and port from the successful reply.
    var discard: [258]u8 = undefined;
    address_bytes += 2;
    try streamReadExact(io, stream, discard[0..address_bytes], timeout_ms);
}

fn performHttpConnect(
    gpa: std.mem.Allocator,
    io: std.Io,
    stream: *const net.Stream,
    host: []const u8,
    port: u16,
    timeout_ms: u32,
) !void {
    var request: std.ArrayList(u8) = .empty;
    defer request.deinit(gpa);
    try policy.http_connect.appendRequest(&request, gpa, host, port);
    try streamWriteAll(io, stream, request.items);

    const max_headers = 32 * 1024;
    var headers: std.ArrayList(u8) = .empty;
    defer headers.deinit(gpa);
    var chunk: [1024]u8 = undefined;
    while (std.mem.indexOf(u8, headers.items, "\r\n\r\n") == null) {
        if (headers.items.len >= max_headers) return error.ProxyResponseTooLarge;
        const length = try streamReadTimeout(io, stream, chunk[0..@min(chunk.len, max_headers - headers.items.len)], timeout_ms);
        try headers.appendSlice(gpa, chunk[0..length]);
    }
    try policy.http_connect.parseResponse(headers.items);
}

/// Heap-pinned, single-shot asynchronous connector. Blocking DNS, proxy, and
/// TLS setup stays on its worker; UI threads only call `poll` and `cancel`.
pub const Connector = struct {
    const State = enum(u8) { pending, succeeded, failed, canceled, consumed };
    const Driver = *const fn (*Connector) anyerror!*Transport;

    gpa: std.mem.Allocator,
    host: []u8,
    port: u16,
    options: ConnectOptions,
    owned_ca: ?[]u8 = null,
    owned_client_cert: ?[]u8 = null,
    owned_proxy_host: ?[]u8 = null,
    thread: std.Thread = undefined,
    state: std.atomic.Value(State) = .init(.pending),
    canceled: std.atomic.Value(bool) = .init(false),
    references: std.atomic.Value(u8) = .init(2),
    transport: ?*Transport = null,
    failure: ?anyerror = null,
    driver: Driver,

    pub fn start(gpa: std.mem.Allocator, host: []const u8, port: u16, options: ConnectOptions) !*Connector {
        try validateConnectInputs(host, port, options);
        return startWithDriver(gpa, host, port, options, defaultDriver);
    }

    fn startWithDriver(
        gpa: std.mem.Allocator,
        host: []const u8,
        port: u16,
        options: ConnectOptions,
        driver: Driver,
    ) !*Connector {
        const self = try gpa.create(Connector);
        errdefer gpa.destroy(self);
        self.* = .{
            .gpa = gpa,
            .host = try gpa.dupe(u8, host),
            .port = port,
            .options = options,
            .driver = driver,
        };
        errdefer gpa.free(self.host);
        if (options.ca_file) |path| {
            self.owned_ca = try gpa.dupe(u8, path);
            self.options.ca_file = self.owned_ca;
        }
        errdefer if (self.owned_ca) |path| gpa.free(path);
        if (options.client_cert_file) |path| {
            self.owned_client_cert = try gpa.dupe(u8, path);
            self.options.client_cert_file = self.owned_client_cert;
        }
        errdefer if (self.owned_client_cert) |path| gpa.free(path);
        switch (options.proxy) {
            .direct => {},
            .socks5, .http_connect => |endpoint| {
                self.owned_proxy_host = try gpa.dupe(u8, endpoint.host);
                const owned = ProxyEndpoint{ .host = self.owned_proxy_host.?, .port = endpoint.port };
                self.options.proxy = switch (options.proxy) {
                    .socks5 => .{ .socks5 = owned },
                    .http_connect => .{ .http_connect = owned },
                    .direct => unreachable,
                };
            },
        }
        errdefer if (self.owned_proxy_host) |proxy_host| gpa.free(proxy_host);
        self.thread = std.Thread.spawn(.{}, worker, .{self}) catch |err| {
            self.references.store(1, .release);
            return err;
        };
        return self;
    }

    pub fn poll(self: *Connector) !?*Transport {
        return switch (self.state.load(.acquire)) {
            .pending => null,
            .succeeded => result: {
                const connected = self.transport orelse return error.InvalidConnectorState;
                self.transport = null;
                self.state.store(.consumed, .release);
                if (self.canceled.load(.acquire)) {
                    connected.deinit();
                    return error.ConnectCanceled;
                }
                break :result connected;
            },
            .failed => {
                const err = self.failure orelse error.ConnectionFailed;
                self.state.store(.consumed, .release);
                return err;
            },
            .canceled => {
                self.state.store(.consumed, .release);
                return error.ConnectCanceled;
            },
            .consumed => return error.ConnectorAlreadyConsumed,
        };
    }

    pub fn cancel(self: *Connector) void {
        self.canceled.store(true, .release);
    }

    /// Cancel and join the bounded worker. Interactive code calls this only
    /// during teardown; normal UI polling never waits for DNS/TCP/proxy/TLS.
    pub fn deinit(self: *Connector) void {
        self.cancel();
        self.thread.join();
        self.release();
    }

    fn defaultDriver(self: *Connector) anyerror!*Transport {
        return Transport.connectWithOptions(self.gpa, self.host, self.port, self.options) catch |err| switch (err) {
            error.Timeout, error.ProxyHandshakeTimeout => error.ConnectDeadlineExceeded,
            else => |other| other,
        };
    }

    fn worker(self: *Connector) void {
        const result = self.driver(self);
        if (result) |connected| {
            if (self.canceled.load(.acquire)) {
                connected.deinit();
                self.state.store(.canceled, .release);
            } else {
                self.transport = connected;
                self.state.store(.succeeded, .release);
            }
        } else |err| {
            if (self.canceled.load(.acquire)) {
                self.state.store(.canceled, .release);
            } else {
                self.failure = err;
                self.state.store(.failed, .release);
            }
        }
        self.release();
    }

    fn release(self: *Connector) void {
        if (self.references.fetchSub(1, .acq_rel) != 1) return;
        if (self.transport) |connected| connected.deinit();
        if (self.owned_proxy_host) |proxy_host| self.gpa.free(proxy_host);
        if (self.owned_client_cert) |path| self.gpa.free(path);
        if (self.owned_ca) |path| self.gpa.free(path);
        self.gpa.free(self.host);
        const gpa = self.gpa;
        self.* = undefined;
        gpa.destroy(self);
    }
};

fn validateConnectInputs(host: []const u8, port: u16, options: ConnectOptions) !void {
    if (host.len == 0 or host.len > net.HostName.max_len or
        std.mem.indexOfAny(u8, host, " \r\n\x00") != null)
        return error.InvalidConnectHost;
    if (port == 0) return error.InvalidConnectPort;
    if (options.connect_timeout_ms == 0) return error.InvalidConnectTimeout;
    if (options.ca_file) |path| if (path.len == 0 or path.len > 32 * 1024 or
        std.mem.indexOfScalar(u8, path, 0) != null) return error.InvalidCertificatePath;
    if (options.client_cert_file) |path| if (path.len == 0 or path.len > 32 * 1024 or
        std.mem.indexOfScalar(u8, path, 0) != null) return error.InvalidCertificatePath;
    switch (options.proxy) {
        .direct => {},
        .socks5, .http_connect => |endpoint| {
            if (endpoint.host.len == 0 or endpoint.host.len > net.HostName.max_len or
                std.mem.indexOfAny(u8, endpoint.host, " \r\n\x00") != null)
                return error.InvalidProxyTarget;
            if (endpoint.port == 0) return error.InvalidProxyTarget;
        },
    }
}

test "transport security is TLS unless plaintext is explicit" {
    try std.testing.expectEqual(Security.tls, (ConnectOptions{}).security);
    try std.testing.expectEqual(Security.plaintext, (ConnectOptions{ .security = .plaintext }).security);
}

test "connection and proxy inputs are bounded before allocation or DNS" {
    const oversized: [net.HostName.max_len + 1]u8 = @splat('a');
    try std.testing.expectError(
        error.InvalidConnectHost,
        validateConnectInputs(&oversized, 6697, .{}),
    );
    try std.testing.expectError(error.InvalidConnectPort, validateConnectInputs("irc.example", 0, .{}));
    try std.testing.expectError(
        error.InvalidProxyTarget,
        validateConnectInputs("irc.example", 6697, .{ .proxy = .{ .socks5 = .{ .host = "bad proxy", .port = 1080 } } }),
    );
}

fn canceledTestDriver(connector: *Connector) anyerror!*Transport {
    while (!connector.canceled.load(.acquire)) std.atomic.spinLoopHint();
    return error.ConnectCanceled;
}

fn deadlineTestDriver(_: *Connector) anyerror!*Transport {
    return error.ConnectDeadlineExceeded;
}

test "asynchronous connector cancellation never waits on its worker" {
    var connector = try Connector.startWithDriver(std.testing.allocator, "example.test", 6697, .{}, canceledTestDriver);
    connector.cancel();
    for (0..100_000) |_| {
        _ = connector.poll() catch |err| switch (err) {
            error.ConnectCanceled => break,
            else => return err,
        };
        std.Thread.yield() catch {};
    } else return error.ConnectorTestTimeout;
    connector.deinit();
}

test "asynchronous connector publishes deterministic deadline failures" {
    var connector = try Connector.startWithDriver(std.testing.allocator, "example.test", 6697, .{}, deadlineTestDriver);
    defer connector.deinit();
    for (0..100_000) |_| {
        const maybe_transport = connector.poll() catch |err| switch (err) {
            error.ConnectDeadlineExceeded => return,
            else => return err,
        };
        if (maybe_transport != null) return error.UnexpectedTestTransport;
        std.Thread.yield() catch {};
    }
    return error.ConnectorTestTimeout;
}
