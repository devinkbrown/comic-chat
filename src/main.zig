//! comicchat — source-faithful Microsoft Comic Chat continuation (CLI/app).
//!
//! Subcommands:
//!   (none) / app                         open the desktop client
//!   render-bg | render-panel | render-figure | render-strip | topng
//!                                        source art/render diagnostics
//!   window <avatar>                      native backend smoke
//!   connect | chat-comic | app           IRC and interactive comic clients
//!
//! Platform windows only present the shared software-rendered client view.

const std = @import("std");
const builtin = @import("builtin");
const cc = @import("comicchat");

/// Diagnostics stay on stderr so image subcommands can reserve stdout for
/// binary PPM/PNG data on every supported platform.
fn elog(comptime fmt: []const u8, args: anytype) void {
    std.debug.print(fmt, args);
}

pub fn main(init: std.process.Init) !void {
    const gpa = init.gpa;
    const minimal = init.minimal;

    // Collect argv through Zig's Init parameter. The
    // allocator-based iterator is the cross-platform form (Windows requires it).
    var it = try minimal.args.iterateAllocator(gpa);
    defer it.deinit();
    _ = it.skip(); // program name
    var argv: [32][]const u8 = undefined;
    var argc: usize = 0;
    while (it.next()) |a| : (argc += 1) {
        if (argc >= argv.len) break;
        argv[argc] = a;
    }

    if (argc >= 1 and std.mem.eql(u8, argv[0], "render-bg")) {
        try runRenderBg(gpa, init.io, if (argc >= 2) argv[1] else "field");
        return;
    }

    if (argc >= 1 and std.mem.eql(u8, argv[0], "render-panel")) {
        const bg = if (argc >= 2) argv[1] else "field";
        const speaker = if (argc >= 3) argv[2] else "ANNA";
        const text = if (argc >= 4) argv[3] else "Hello from the source-faithful Comic Chat renderer!";
        try runRenderPanel(gpa, init.io, bg, speaker, text);
        return;
    }

    if (argc >= 1 and std.mem.eql(u8, argv[0], "render-figure")) {
        const emo = if (argc >= 3) (cc.comic.emotion.Emotion.fromName(argv[2]) orelse .neutral) else .neutral;
        try runRenderFigure(gpa, init.io, if (argc >= 2) argv[1] else "anna", emo.headIndex());
        return;
    }

    if (argc >= 1 and std.mem.eql(u8, argv[0], "render-strip")) {
        try runRenderStrip(gpa, init.io);
        return;
    }

    if (argc >= 1 and std.mem.eql(u8, argv[0], "topng")) {
        try runToPng(gpa, init.io, if (argc >= 2) argv[1] else "field");
        return;
    }

    if (argc >= 1 and std.mem.eql(u8, argv[0], "window")) {
        const prefer_wayland = if (comptime builtin.os.tag == .linux)
            minimal.environ.containsUnemptyConstant("WAYLAND_DISPLAY")
        else
            false;
        try runWindow(gpa, if (argc >= 2) argv[1] else "anna", prefer_wayland);
        return;
    }

    if (argc == 0 or (argc >= 1 and std.mem.eql(u8, argv[0], "app"))) {
        const app_args: []const []const u8 = if (argc == 0) &.{} else argv[1..argc];
        const connection = parseConnectionArgs(app_args, false) orelse {
            printConnectionUsage("app", false);
            return;
        };
        var runtime = try ConnectionRuntime.init(gpa, init.io, &connection);
        defer runtime.deinit();
        defer runtime.save() catch |err| elog("STS policy save failed: {s}\n", .{@errorName(err)});
        const prefer_wayland = if (comptime builtin.os.tag == .linux)
            minimal.environ.containsUnemptyConstant("WAYLAND_DISPLAY")
        else
            false;
        try runInteractive(
            gpa,
            connection.host,
            connection.port,
            connection.nick,
            connection.channel,
            prefer_wayland,
            &runtime,
            init.io,
        );
        return;
    }

    if (argc >= 1 and std.mem.eql(u8, argv[0], "chat-comic")) {
        const connection = parseConnectionArgs(argv[1..argc], true) orelse {
            printConnectionUsage("chat-comic", true);
            return;
        };
        const maxlines: usize = if (connection.extra) |value|
            (std.fmt.parseInt(usize, value, 10) catch 6)
        else
            6;
        var runtime = try ConnectionRuntime.init(gpa, init.io, &connection);
        defer runtime.deinit();
        defer runtime.save() catch |err| elog("STS policy save failed: {s}\n", .{@errorName(err)});
        try runChatComic(
            gpa,
            init.io,
            connection.host,
            connection.port,
            connection.nick,
            connection.channel,
            maxlines,
            runtime.connect_options,
            runtime.registrationOptions(),
        );
        return;
    }

    if (argc >= 1 and std.mem.eql(u8, argv[0], "connect")) {
        const connection = parseConnectionArgs(argv[1..argc], false) orelse {
            printConnectionUsage("connect", false);
            return;
        };
        var runtime = try ConnectionRuntime.init(gpa, init.io, &connection);
        defer runtime.deinit();
        defer runtime.save() catch |err| elog("STS policy save failed: {s}\n", .{@errorName(err)});
        try runConnect(
            gpa,
            init.io,
            connection.host,
            connection.port,
            connection.nick,
            connection.channel,
            runtime.connect_options,
            runtime.registrationOptions(),
        );
        return;
    }

    try runCodecDemo(gpa);
}

const default_tls_port: u16 = 6697;
const default_server = "eshmaki.me";
const default_channel = "#root";
const default_nick = "kain";

const AuthArgs = struct {
    user: ?[]const u8 = null,
    authzid: ?[]const u8 = null,
    password_file: ?[]const u8 = null,
    mechanism: ?cc.net.sasl.Mechanism = null,
    external: bool = false,

    fn enabled(self: AuthArgs) bool {
        return self.user != null or self.authzid != null or self.password_file != null or
            self.mechanism != null or self.external;
    }
};

const ConnectionArgs = struct {
    host: []const u8,
    port: u16 = default_tls_port,
    nick: []const u8,
    channel: []const u8,
    extra: ?[]const u8 = null,
    options: cc.net.client.ConnectOptions = .{},
    auth: AuthArgs = .{},
    sts_file: []const u8 = ".comicchat-sts",
    session_file: []const u8 = ".comicchat-session",
};

fn parseConnectionArgs(args: []const []const u8, allow_extra: bool) ?ConnectionArgs {
    var positional: [5][]const u8 = undefined;
    var positional_count: usize = 0;
    var options = cc.net.client.ConnectOptions{};
    var auth: AuthArgs = .{};
    var sts_file: []const u8 = ".comicchat-sts";
    var session_file: []const u8 = ".comicchat-session";
    var index: usize = 0;
    while (index < args.len) : (index += 1) {
        const arg = args[index];
        if (std.mem.eql(u8, arg, "--plaintext")) {
            options.security = .plaintext;
            continue;
        }
        if (std.mem.eql(u8, arg, "--tls")) {
            options.security = .tls;
            continue;
        }
        if (std.mem.eql(u8, arg, "--ca-file")) {
            index += 1;
            if (index >= args.len) return null;
            options.ca_file = args[index];
            continue;
        }
        if (std.mem.startsWith(u8, arg, "--ca-file=")) {
            const value = arg["--ca-file=".len..];
            if (value.len == 0) return null;
            options.ca_file = value;
            continue;
        }
        if (std.mem.eql(u8, arg, "--tls-cert")) {
            index += 1;
            if (index >= args.len or args[index].len == 0) return null;
            options.client_cert_file = args[index];
            continue;
        }
        if (std.mem.startsWith(u8, arg, "--tls-cert=")) {
            options.client_cert_file = nonEmptyValue(arg, "--tls-cert=") orelse return null;
            continue;
        }
        if (std.mem.eql(u8, arg, "--socks5") or std.mem.eql(u8, arg, "--http-proxy")) {
            const use_socks = std.mem.eql(u8, arg, "--socks5");
            index += 1;
            if (index >= args.len) return null;
            const endpoint = parseProxyEndpoint(args[index]) orelse return null;
            options.proxy = if (use_socks) .{ .socks5 = endpoint } else .{ .http_connect = endpoint };
            continue;
        }
        if (std.mem.startsWith(u8, arg, "--socks5=") or std.mem.startsWith(u8, arg, "--http-proxy=")) {
            const use_socks = std.mem.startsWith(u8, arg, "--socks5=");
            const prefix = if (use_socks) "--socks5=" else "--http-proxy=";
            const endpoint = parseProxyEndpoint(arg[prefix.len..]) orelse return null;
            options.proxy = if (use_socks) .{ .socks5 = endpoint } else .{ .http_connect = endpoint };
            continue;
        }
        if (std.mem.eql(u8, arg, "--connect-timeout-ms")) {
            index += 1;
            if (index >= args.len) return null;
            options.connect_timeout_ms = std.fmt.parseInt(u32, args[index], 10) catch return null;
            if (options.connect_timeout_ms == 0) return null;
            continue;
        }
        if (std.mem.startsWith(u8, arg, "--connect-timeout-ms=")) {
            const value = nonEmptyValue(arg, "--connect-timeout-ms=") orelse return null;
            options.connect_timeout_ms = std.fmt.parseInt(u32, value, 10) catch return null;
            if (options.connect_timeout_ms == 0) return null;
            continue;
        }
        if (std.mem.eql(u8, arg, "--sasl-user")) {
            index += 1;
            if (index >= args.len or args[index].len == 0) return null;
            auth.user = args[index];
            continue;
        }
        if (std.mem.startsWith(u8, arg, "--sasl-user=")) {
            auth.user = nonEmptyValue(arg, "--sasl-user=") orelse return null;
            continue;
        }
        if (std.mem.eql(u8, arg, "--sasl-authzid")) {
            index += 1;
            if (index >= args.len) return null;
            auth.authzid = args[index];
            continue;
        }
        if (std.mem.startsWith(u8, arg, "--sasl-authzid=")) {
            auth.authzid = arg["--sasl-authzid=".len..];
            continue;
        }
        if (std.mem.eql(u8, arg, "--sasl-password-file")) {
            index += 1;
            if (index >= args.len or args[index].len == 0) return null;
            auth.password_file = args[index];
            continue;
        }
        if (std.mem.startsWith(u8, arg, "--sasl-password-file=")) {
            auth.password_file = nonEmptyValue(arg, "--sasl-password-file=") orelse return null;
            continue;
        }
        if (std.mem.eql(u8, arg, "--sasl-mechanism")) {
            index += 1;
            if (index >= args.len) return null;
            auth.mechanism = cc.net.sasl.Mechanism.parse(args[index]) orelse return null;
            continue;
        }
        if (std.mem.startsWith(u8, arg, "--sasl-mechanism=")) {
            const value = nonEmptyValue(arg, "--sasl-mechanism=") orelse return null;
            auth.mechanism = cc.net.sasl.Mechanism.parse(value) orelse return null;
            continue;
        }
        if (std.mem.eql(u8, arg, "--sasl-external")) {
            auth.external = true;
            auth.mechanism = .external;
            continue;
        }
        if (std.mem.eql(u8, arg, "--sts-file")) {
            index += 1;
            if (index >= args.len or args[index].len == 0) return null;
            sts_file = args[index];
            continue;
        }
        if (std.mem.startsWith(u8, arg, "--sts-file=")) {
            sts_file = nonEmptyValue(arg, "--sts-file=") orelse return null;
            continue;
        }
        if (std.mem.eql(u8, arg, "--session-file")) {
            index += 1;
            if (index >= args.len or args[index].len == 0) return null;
            session_file = args[index];
            continue;
        }
        if (std.mem.startsWith(u8, arg, "--session-file=")) {
            session_file = nonEmptyValue(arg, "--session-file=") orelse return null;
            continue;
        }
        if (std.mem.startsWith(u8, arg, "--")) return null;
        if (positional_count == positional.len) return null;
        positional[positional_count] = arg;
        positional_count += 1;
    }
    var result: ConnectionArgs = undefined;
    if (positional_count == 0) {
        result = .{
            .host = default_server,
            .nick = default_nick,
            .channel = default_channel,
            .options = options,
            .auth = auth,
            .sts_file = sts_file,
            .session_file = session_file,
        };
    } else if (positional_count == 1) {
        result = .{
            .host = default_server,
            .nick = positional[0],
            .channel = default_channel,
            .options = options,
            .auth = auth,
            .sts_file = sts_file,
            .session_file = session_file,
        };
    } else if (positional_count == 2) {
        result = .{
            .host = positional[0],
            .nick = positional[1],
            .channel = default_channel,
            .options = options,
            .auth = auth,
            .sts_file = sts_file,
            .session_file = session_file,
        };
    } else if (positional_count < 3) {
        return null;
    } else if (std.fmt.parseInt(u16, positional[1], 10)) |port| {
        if (positional_count < 4) return null;
        result = .{
            .host = positional[0],
            .port = port,
            .nick = positional[2],
            .channel = positional[3],
            .options = options,
            .auth = auth,
            .sts_file = sts_file,
            .session_file = session_file,
        };
        if (positional_count > 4) result.extra = positional[4];
    } else |_| {
        result = .{
            .host = positional[0],
            .nick = positional[1],
            .channel = positional[2],
            .options = options,
            .auth = auth,
            .sts_file = sts_file,
            .session_file = session_file,
        };
        if (positional_count > 3) result.extra = positional[3];
        if (positional_count > 4) return null;
    }
    if (!allow_extra and result.extra != null) return null;
    return result;
}

fn nonEmptyValue(arg: []const u8, comptime prefix: []const u8) ?[]const u8 {
    const value = arg[prefix.len..];
    return if (value.len == 0) null else value;
}

fn parseProxyEndpoint(raw: []const u8) ?cc.net.transport.ProxyEndpoint {
    if (raw.len == 0) return null;
    var host: []const u8 = undefined;
    var port_text: []const u8 = undefined;
    if (raw[0] == '[') {
        const close = std.mem.indexOfScalar(u8, raw, ']') orelse return null;
        if (close <= 1 or close + 2 > raw.len or raw[close + 1] != ':') return null;
        host = raw[1..close];
        port_text = raw[close + 2 ..];
    } else {
        const colon = std.mem.lastIndexOfScalar(u8, raw, ':') orelse return null;
        if (colon == 0) return null;
        host = raw[0..colon];
        port_text = raw[colon + 1 ..];
    }
    const port = std.fmt.parseInt(u16, port_text, 10) catch return null;
    if (port == 0 or std.mem.indexOfAny(u8, host, " \r\n\x00") != null) return null;
    return .{ .host = host, .port = port };
}

fn printConnectionUsage(command: []const u8, allow_extra: bool) void {
    std.debug.print(
        "usage: comicchat {s} <nick> (defaults: eshmaki.me #root) | <host> <nick> [#channel] | <host> [port=6697] <nick> <#channel>{s} [--ca-file <pem>] [--tls-cert <cert-and-key.pem>] [--plaintext] [--socks5 host:port|--http-proxy host:port] [--connect-timeout-ms <ms>] [--sasl-user <name> --sasl-password-file <path>] [--sasl-mechanism SCRAM-SHA-256|EXTERNAL|PLAIN] [--sts-file <path>] [--session-file <path>]\n",
        .{ command, if (allow_extra) " [maxlines]" else "" },
    );
}

test "connection defaults use eshmaki root" {
    const args = [_][]const u8{"kain"};
    const connection = parseConnectionArgs(&args, false).?;
    try std.testing.expectEqualStrings("eshmaki.me", connection.host);
    try std.testing.expectEqualStrings("kain", connection.nick);
    try std.testing.expectEqualStrings("#root", connection.channel);
}

test "empty app arguments open the configured desktop default" {
    const connection = parseConnectionArgs(&.{}, false).?;
    try std.testing.expectEqualStrings("eshmaki.me", connection.host);
    try std.testing.expectEqualStrings("kain", connection.nick);
    try std.testing.expectEqualStrings("#root", connection.channel);
}

test "explicit host retains the default channel" {
    const args = [_][]const u8{ "irc.example", "kain" };
    const connection = parseConnectionArgs(&args, false).?;
    try std.testing.expectEqualStrings("irc.example", connection.host);
    try std.testing.expectEqualStrings("#root", connection.channel);
}

const ConnectionRuntime = struct {
    gpa: std.mem.Allocator,
    io: std.Io,
    sts_path: []const u8,
    sts: cc.net.sts_store.Store,
    session_path: []const u8,
    session: cc.net.session_store.Store,
    connect_options: cc.net.client.ConnectOptions,
    now_seconds: u64,
    auth: AuthArgs,
    nick: []const u8,
    authzid_storage: ?[]u8 = null,
    authcid_storage: ?[]u8 = null,
    password_storage: ?[]u8 = null,
    credentials: ?cc.net.sasl.Credentials = null,
    preference: [1]cc.net.sasl.Mechanism = undefined,
    preference_len: usize = 0,

    fn init(gpa: std.mem.Allocator, io: std.Io, args: *const ConnectionArgs) !ConnectionRuntime {
        const wall_seconds = std.Io.Clock.real.now(io).toSeconds();
        const now_seconds: u64 = if (wall_seconds > 0) @intCast(wall_seconds) else 0;
        const stores = stores: {
            var sts = try cc.net.sts_store.Store.loadFile(gpa, io, args.sts_file);
            errdefer sts.deinit();
            const session = try cc.net.session_store.Store.loadFile(
                gpa,
                io,
                args.session_file,
                args.host,
                args.auth.user orelse args.nick,
            );
            break :stores .{ .sts = sts, .session = session };
        };
        var runtime = ConnectionRuntime{
            .gpa = gpa,
            .io = io,
            .sts_path = args.sts_file,
            .sts = stores.sts,
            .session_path = args.session_file,
            .session = stores.session,
            .connect_options = args.options,
            .now_seconds = now_seconds,
            .auth = args.auth,
            .nick = args.nick,
        };
        errdefer runtime.deinit();

        // A cached STS policy always overrides a plaintext command-line
        // request. This is the downgrade protection the persisted policy is
        // intended to provide.
        if (runtime.sts.requiresTls(args.host, now_seconds)) runtime.connect_options.security = .tls;
        try runtime.loadCredentials();
        return runtime;
    }

    fn loadCredentials(self: *ConnectionRuntime) !void {
        if (!self.auth.enabled()) return;
        if (self.connect_options.security == .plaintext) return error.SaslRequiresTls;

        const selected = self.auth.mechanism;
        const external_available = self.auth.external or selected == .external;
        if (!external_available and self.auth.password_file == null) return error.MissingSaslPasswordFile;

        self.authzid_storage = try self.gpa.dupe(u8, self.auth.authzid orelse "");
        self.authcid_storage = try self.gpa.dupe(u8, self.auth.user orelse self.nick);
        if (self.auth.password_file) |path| {
            self.password_storage = try std.Io.Dir.cwd().readFileAlloc(self.io, path, self.gpa, .limited(64 * 1024));
        } else {
            self.password_storage = try self.gpa.dupe(u8, "");
        }
        var password_len = self.password_storage.?.len;
        while (password_len > 0 and
            (self.password_storage.?[password_len - 1] == '\r' or self.password_storage.?[password_len - 1] == '\n'))
        {
            password_len -= 1;
        }
        const password = self.password_storage.?[0..password_len];
        self.credentials = .{
            .authorization_identity = self.authzid_storage.?,
            .authentication_identity = self.authcid_storage.?,
            .password = password,
            .external_available = external_available,
        };
        if (selected) |mechanism| {
            self.preference[0] = mechanism;
            self.preference_len = 1;
        }
    }

    fn registrationOptions(self: *ConnectionRuntime) cc.net.client.RegistrationOptions {
        return .{
            .credentials = if (self.credentials) |*credentials| credentials else null,
            .sasl_preference = if (self.preference_len == 1) self.preference[0..1] else &cc.net.sasl.default_preference,
            .io = self.io,
            .sts = &self.sts,
            .session = &self.session,
            .session_path = self.session_path,
            .now_seconds = self.now_seconds,
        };
    }

    /// Credentials are single-attempt mutable buffers. Reconnects reload the
    /// password file only after the previous SASL session wiped and released
    /// its copy, so no reusable cleartext command or queue entry survives.
    fn registrationOptionsForAttempt(self: *ConnectionRuntime) !cc.net.client.RegistrationOptions {
        if (self.credentials) |credentials| if (credentials.zeroized) {
            self.clearCredentialStorage();
            try self.loadCredentials();
        };
        return self.registrationOptions();
    }

    fn save(self: *ConnectionRuntime) !void {
        try self.sts.saveFile(self.io, self.sts_path);
        try self.session.saveFile(self.io, self.session_path);
    }

    fn deinit(self: *ConnectionRuntime) void {
        if (self.credentials) |*credentials| if (!credentials.zeroized) credentials.zeroize();
        self.clearCredentialStorage();
        self.session.deinit();
        self.sts.deinit();
        self.* = undefined;
    }

    fn clearCredentialStorage(self: *ConnectionRuntime) void {
        if (self.authzid_storage) |storage| {
            std.crypto.secureZero(u8, storage);
            self.gpa.free(storage);
            self.authzid_storage = null;
        }
        if (self.authcid_storage) |storage| {
            std.crypto.secureZero(u8, storage);
            self.gpa.free(storage);
            self.authcid_storage = null;
        }
        if (self.password_storage) |storage| {
            std.crypto.secureZero(u8, storage);
            self.gpa.free(storage);
            self.password_storage = null;
        }
        self.credentials = null;
    }
};

fn monotonicMilliseconds(io: std.Io) u64 {
    const milliseconds = std.Io.Clock.awake.now(io).toMilliseconds();
    return if (milliseconds > 0) @intCast(milliseconds) else 0;
}

fn runCodecDemo(gpa: std.mem.Allocator) !void {
    std.debug.print("Comic Chat portable source port — record codec demo\n\n", .{});

    const record = cc.proto.record;
    var doc: std.ArrayList(u8) = .empty;
    defer doc.deinit(gpa);

    try record.writeRecord(&doc, gpa, "#CHATCONVERSATION", &.{});
    try record.writeRecord(&doc, gpa, "join", &.{ "Anna", "Anna Example" });
    try record.writeComicchar(&doc, gpa, "Anna", "character information unavailable");
    try record.writeSay(&doc, gpa, "Anna", "(G:0 0 0 E:0 0 0 R:1 M:1)", "Hi from Comic Chat!");

    std.debug.print("--- encoded transcript ---\n{s}\n", .{doc.items});
    std.debug.print("--- decoded records ---\n", .{});
    var it = record.DocumentIterator.init(doc.items);
    while (it.next()) |rec| {
        std.debug.print("  {s}", .{@tagName(rec.type)});
        var i: usize = 0;
        while (i < rec.field_count) : (i += 1) std.debug.print(" | {s}", .{rec.fields[i]});
        std.debug.print("\n", .{});
    }
}

fn writeStdout(io: std.Io, bytes: []const u8) !void {
    try std.Io.File.stdout().writeStreamingAll(io, bytes);
}

fn avatarByName(name: []const u8) ?[]const u8 {
    const eql = std.ascii.eqlIgnoreCase;
    if (eql(name, "anna")) return @embedFile("assets/testdata/anna.avb");
    if (eql(name, "armando")) return @embedFile("assets/testdata/armando.avb");
    if (eql(name, "bolo")) return @embedFile("assets/testdata/bolo.avb");
    if (eql(name, "cro")) return @embedFile("assets/testdata/cro.avb");
    if (eql(name, "dan")) return @embedFile("assets/testdata/dan.avb");
    if (eql(name, "denise")) return @embedFile("assets/testdata/denise.avb");
    if (eql(name, "hugh")) return @embedFile("assets/testdata/hugh.avb");
    if (eql(name, "jordan")) return @embedFile("assets/testdata/jordan.avb");
    if (eql(name, "kevin")) return @embedFile("assets/testdata/kevin.avb");
    if (eql(name, "kwensa")) return @embedFile("assets/testdata/kwensa.avb");
    if (eql(name, "lance")) return @embedFile("assets/testdata/lance.avb");
    if (eql(name, "lynnea")) return @embedFile("assets/testdata/lynnea.avb");
    if (eql(name, "margaret")) return @embedFile("assets/testdata/margaret.avb");
    if (eql(name, "maynard")) return @embedFile("assets/testdata/maynard.avb");
    if (eql(name, "mike")) return @embedFile("assets/testdata/mike.avb");
    if (eql(name, "rebecca")) return @embedFile("assets/testdata/rebecca.avb");
    if (eql(name, "sage")) return @embedFile("assets/testdata/sage.avb");
    if (eql(name, "scotty")) return @embedFile("assets/testdata/scotty.avb");
    if (eql(name, "susan")) return @embedFile("assets/testdata/susan.avb");
    if (eql(name, "tiki")) return @embedFile("assets/testdata/tiki.avb");
    if (eql(name, "tongtyed")) return @embedFile("assets/testdata/tongtyed.avb");
    if (eql(name, "xeno")) return @embedFile("assets/testdata/xeno.avb");
    return null;
}

fn bgByName(name: []const u8) ?[]const u8 {
    const eql = std.mem.eql;
    if (eql(u8, name, "field")) return @embedFile("assets/testdata/field.bgb");
    if (eql(u8, name, "volcano")) return @embedFile("assets/testdata/volcano.bgb");
    if (eql(u8, name, "den")) return @embedFile("assets/testdata/den.bgb");
    if (eql(u8, name, "room")) return @embedFile("assets/testdata/room.bgb");
    if (eql(u8, name, "pastoral")) return @embedFile("assets/testdata/pastoral.bgb");
    return null;
}

/// Emit RGBA pixels (0xAARRGGBB, top-down) as a binary PPM (P6) on stdout.
fn emitPpm(gpa: std.mem.Allocator, io: std.Io, pixels: []const u32, w: u32, h: u32) !void {
    var ppm: std.ArrayList(u8) = .empty;
    defer ppm.deinit(gpa);
    var hdr: [64]u8 = undefined;
    try ppm.appendSlice(gpa, try std.fmt.bufPrint(&hdr, "P6\n{d} {d}\n255\n", .{ w, h }));
    for (pixels) |px| {
        try ppm.append(gpa, @intCast((px >> 16) & 0xff));
        try ppm.append(gpa, @intCast((px >> 8) & 0xff));
        try ppm.append(gpa, @intCast(px & 0xff));
    }
    try writeStdout(io, ppm.items);
}

/// Decode a named embedded background and emit it as PPM on stdout.
fn runRenderBg(gpa: std.mem.Allocator, io: std.Io, name: []const u8) !void {
    const data = bgByName(name) orelse {
        elog("unknown background '{s}' (field|volcano|den|room|pastoral)\n", .{name});
        return;
    };
    var img = try cc.assets.bgb.decodeBackground(gpa, data);
    defer img.deinit(gpa);
    try emitPpm(gpa, io, img.pixels, img.width, img.height);
}

/// Render a one-line source page (implicit title plus conversation panel) and
/// emit it as PPM on stdout.
fn runRenderPanel(gpa: std.mem.Allocator, io: std.Io, bg: []const u8, speaker: []const u8, text: []const u8) !void {
    const data = bgByName(bg) orelse {
        elog("unknown background '{s}'\n", .{bg});
        return;
    };
    var page = try cc.comic.strip.renderWithOptions(
        gpa,
        &.{.{ .speaker = speaker, .text = text }},
        .{ .backdrop = data },
    );
    defer page.deinit(gpa);
    try emitPpm(gpa, io, page.pixels, page.width, page.height);
}

/// Composite an avatar's head + body layers into the full standing figure and
/// emit it as PPM on stdout. (Comic Chat stores head expressions and body
/// gestures separately — the "emotion wheel" — and composites at the neck.)
/// Render a single complete pose centered on white (for creature/totem avatars
/// that have no head/body split).
fn renderSolo(gpa: std.mem.Allocator, io: std.Io, img: cc.assets.bgb.Image) !void {
    const pad: i32 = 10;
    const W: u32 = img.width + 2 * @as(u32, @intCast(pad));
    const H: u32 = img.height + 2 * @as(u32, @intCast(pad));
    var cf = try cc.render.canvas.Canvas.init(gpa, W, H);
    defer cf.deinit(gpa);
    cf.clear(cc.render.canvas.white);
    composite(&cf, img.pixels, img.width, img.height, pad, pad, 0);
    try emitPpm(gpa, io, cf.px, W, H);
}

fn runRenderFigure(gpa: std.mem.Allocator, io: std.Io, name: []const u8, emotion: usize) !void {
    const avb = avatarByName(name) orelse {
        elog("unknown avatar '{s}'\n", .{name});
        return;
    };
    var fig = cc.comic.figure.assemble(gpa, avb, emotion, 0) catch {
        elog("could not assemble figure for '{s}'\n", .{name});
        return;
    };
    defer fig.deinit(gpa);

    const pad: i32 = 10;
    const W: u32 = fig.width + 2 * @as(u32, @intCast(pad));
    const H: u32 = fig.height + 2 * @as(u32, @intCast(pad));
    var c = try cc.render.canvas.Canvas.init(gpa, W, H);
    defer c.deinit(gpa);
    c.clear(cc.render.canvas.white);
    cc.comic.figure.composite(&c, fig.pixels, fig.width, fig.height, pad, pad);
    try emitPpm(gpa, io, c.px, W, H);
}

fn rowWidth(img: cc.assets.bgb.Image, y: i32) i32 {
    if (y < 0 or y >= img.height) return 0;
    const row = @as(usize, @intCast(y)) * img.width;
    var n: i32 = 0;
    var x: u32 = 0;
    while (x < img.width) : (x += 1) {
        if (img.pixels[row + x] >> 24 != 0) n += 1;
    }
    return n;
}

/// First row (top→down) where the figure flares wider than its neck — the
/// shoulder line. The head's bottom is seated here.
fn shoulderRow(img: cc.assets.bgb.Image) i32 {
    const t = topInkRow(img);
    var neck: i32 = std.math.maxInt(i32);
    var y: i32 = t;
    while (y < t + 40 and y < img.height) : (y += 1) {
        const w = rowWidth(img, y);
        if (w > 0 and w < neck) neck = w;
    }
    if (neck == std.math.maxInt(i32)) neck = 1;
    const threshold = @max(@divTrunc(neck * 17, 10), neck + 18);
    y = t;
    while (y < img.height) : (y += 1) {
        if (rowWidth(img, y) >= threshold) return y;
    }
    return t + 20;
}

fn topInkRow(img: cc.assets.bgb.Image) i32 {
    var y: u32 = 0;
    while (y < img.height) : (y += 1) {
        var x: u32 = 0;
        while (x < img.width) : (x += 1) if (img.pixels[y * img.width + x] >> 24 != 0) return @intCast(y);
    }
    return 0;
}

/// Lowest row with ink within ±18px of the neck column `nx` — the base of the
/// neck, ignoring hair/ornaments that hang lower on the sides.
fn headNeckBottom(img: cc.assets.bgb.Image, nx: i32) i32 {
    const x0: u32 = @intCast(@max(@as(i32, 0), nx - 18));
    const x1: u32 = @intCast(@min(@as(i32, @intCast(img.width)), nx + 18));
    var y: i32 = @as(i32, @intCast(img.height)) - 1;
    while (y >= 0) : (y -= 1) {
        const row = @as(usize, @intCast(y)) * img.width;
        var x: u32 = x0;
        while (x < x1) : (x += 1) if (img.pixels[row + x] >> 24 != 0) return y;
    }
    return @as(i32, @intCast(img.height)) - 1;
}

fn botInkRow(img: cc.assets.bgb.Image) i32 {
    var y: i32 = @as(i32, @intCast(img.height)) - 1;
    while (y >= 0) : (y -= 1) {
        const row = @as(usize, @intCast(y)) * img.width;
        var x: u32 = 0;
        while (x < img.width) : (x += 1) if (img.pixels[row + x] >> 24 != 0) return y;
    }
    return @as(i32, @intCast(img.height)) - 1;
}

/// Horizontal centroid of opaque pixels over rows [y0, y1).
fn centroidX(img: cc.assets.bgb.Image, y0: i32, y1: i32) i32 {
    var sum: i64 = 0;
    var cnt: i64 = 0;
    var y: i32 = @max(0, y0);
    const ye: i32 = @min(@as(i32, @intCast(img.height)), y1);
    while (y < ye) : (y += 1) {
        const row = @as(usize, @intCast(y)) * img.width;
        var x: u32 = 0;
        while (x < img.width) : (x += 1) {
            if (img.pixels[row + x] >> 24 != 0) {
                sum += x;
                cnt += 1;
            }
        }
    }
    if (cnt == 0) return @intCast(img.width / 2);
    return @intCast(@divTrunc(sum, cnt));
}

/// Composite a transparent-keyed pose image, skipping the right `crop_r`
/// columns. Black ink always wins: a white pixel never paints over existing
/// black, so an upper layer's white "sticker" can't erase the lower layer's
/// linework (e.g. the body's collar/neck lines under the head).
fn composite(c: *cc.render.canvas.Canvas, src: []const u32, sw: u32, sh: u32, dx: i32, dy: i32, crop_r: u32) void {
    var y: u32 = 0;
    while (y < sh) : (y += 1) {
        var x: u32 = 0;
        while (x + crop_r < sw) : (x += 1) {
            const p = src[y * sw + x];
            if (p >> 24 == 0) continue; // transparent
            const ox = dx + @as(i32, @intCast(x));
            const oy = dy + @as(i32, @intCast(y));
            if (ox < 0 or oy < 0 or ox >= c.width or oy >= c.height) continue;
            const di = @as(usize, @intCast(oy)) * c.width + @as(usize, @intCast(ox));
            c.px[di] = p; // upper layer occludes (head drawn on top of body)
        }
    }
}

/// Connect to IRC, gather channel messages, and render the conversation as a
/// comic strip (each speaker mapped to an avatar). Emits PPM on stdout.
fn runChatComic(
    gpa: std.mem.Allocator,
    io: std.Io,
    host: []const u8,
    port: u16,
    nick: []const u8,
    channel: []const u8,
    maxlines: usize,
    connect_options: cc.net.client.ConnectOptions,
    registration_options: cc.net.client.RegistrationOptions,
) !void {
    var client = try cc.net.client.Client.connectWithOptions(gpa, host, port, connect_options);
    defer client.deinit();
    try client.registerWithOptions(nick, nick, "Comic Chat portable", registration_options);

    var transcript = cc.comic.session.Transcript.init(gpa);
    defer transcript.deinit();
    try transcript.setSelf(nick);
    var metadata_state: ChatState = .{};
    defer metadata_state.deinit(gpa);

    var budget: usize = 0;
    while (budget < 400 and transcript.count() < maxlines) : (budget += 1) {
        const msg = (try client.next()) orelse break;
        _ = try transcript.observeIrc(&msg, channel, nick);
        if (!metadata_state.join_requested and std.mem.eql(u8, msg.command, "001")) {
            try client.join(channel);
            metadata_state.join_requested = true;
        } else if (std.mem.eql(u8, msg.command, "JOIN")) {
            const who = if (msg.prefix) |p| cc.comic.session.nickFromPrefix(p) else "";
            const joined_channel = msg.param(0) orelse "";
            if (std.ascii.eqlIgnoreCase(who, nick) and std.ascii.eqlIgnoreCase(joined_channel, channel))
                try finishJoin(&client, &transcript, nick, channel, &metadata_state);
        } else if (std.mem.eql(u8, msg.command, "366")) {
            const joined_channel = msg.param(1) orelse msg.param(0) orelse "";
            if (metadata_state.join_requested and std.ascii.eqlIgnoreCase(joined_channel, channel))
                try finishJoin(&client, &transcript, nick, channel, &metadata_state);
        } else if (std.mem.eql(u8, msg.command, "DATA")) {
            const target = msg.param(0) orelse continue;
            const kind = msg.param(1) orelse continue;
            const wire = msg.param(2) orelse continue;
            if (!std.ascii.eqlIgnoreCase(target, channel) or !std.mem.eql(u8, kind, "CCUDI1")) continue;
            const who = if (msg.prefix) |prefix| cc.comic.session.nickFromPrefix(prefix) else continue;
            _ = cc.proto.udi.parseAnnotation(wire) catch continue;
            try metadata_state.rememberUdi(gpa, target, who, wire);
        } else if (std.mem.eql(u8, msg.command, "PRIVMSG")) {
            const target = msg.param(0) orelse continue;
            if (!std.ascii.eqlIgnoreCase(target, channel)) continue;
            const text = msg.param(1) orelse continue;
            const who = if (msg.prefix) |p| cc.comic.session.nickFromPrefix(p) else "someone";
            if (try transcript.consumeAvatarAnnouncement(who, text)) continue;
            var pending = metadata_state.takeUdi(target, who);
            defer if (pending) |*entry| entry.deinit(gpa);
            try transcript.addWireMessage(who, text, false, if (pending) |entry| entry.wire else null);
        }
    }
    elog("collected {d} lines\n", .{transcript.count()});
    if (transcript.count() == 0) return;

    var lines = try gpa.alloc(cc.comic.strip.Line, transcript.count());
    defer gpa.free(lines);
    var target_count: usize = 0;
    for (transcript.lines.items) |line| target_count += line.talk_targets.len;
    const targets = try gpa.alloc(cc.comic.strip.Participant, target_count);
    defer gpa.free(targets);
    var target_offset: usize = 0;
    for (transcript.lines.items, 0..) |line, index| {
        const target_start = target_offset;
        for (line.talk_targets) |target| {
            targets[target_offset] = .{
                .identity = target.nick,
                .display_name = target.nick,
                .avatar = target.avatar,
            };
            target_offset += 1;
        }
        lines[index] = .{
            .identity = line.nick,
            .display_name = line.nick,
            .avatar = line.avatar,
            .text = line.text,
            .formatting = line.formatting,
            .pose_text = line.pose_text,
            .pose_state = line.pose_state,
            .talk_targets = targets[target_start..target_offset],
            .modes = line.modes,
        };
    }

    const title_roster = try gpa.alloc(cc.comic.strip.TitleParticipant, transcript.roster.items.len);
    defer gpa.free(title_roster);
    for (transcript.roster.items, 0..) |member, index| title_roster[index] = .{
        .identity = member.nick,
        .display_name = member.nick,
        .avatar = member.avatar,
        .is_self = member.is_self,
        .sends = member.sends,
        .departed = member.departed,
    };

    var strip = try cc.comic.strip.renderWithOptions(gpa, lines, .{ .title_roster = title_roster });
    defer strip.deinit(gpa);
    try emitPpm(gpa, io, strip.pixels, strip.width, strip.height);
}

/// Run the real interactive application using the native platform transport.
fn runInteractive(
    gpa: std.mem.Allocator,
    host: []const u8,
    port: u16,
    nick: []const u8,
    channel: []const u8,
    prefer_wayland: bool,
    runtime: *ConnectionRuntime,
    io: std.Io,
) !void {
    if (comptime builtin.os.tag == .linux) {
        if (prefer_wayland) return runInteractiveWayland(gpa, host, port, nick, channel, runtime, io);
        return runInteractiveX11(gpa, host, port, nick, channel, runtime, io);
    } else if (comptime builtin.os.tag == .windows) {
        return runInteractiveWin32(gpa, host, port, nick, channel, runtime, io);
    } else {
        std.debug.print("the interactive window backend is not implemented for {s} yet\n", .{@tagName(builtin.os.tag)});
    }
}

const PendingUdi = struct {
    target: []u8,
    nick: []u8,
    wire: []u8,

    fn deinit(self: *PendingUdi, gpa: std.mem.Allocator) void {
        gpa.free(self.target);
        gpa.free(self.nick);
        gpa.free(self.wire);
        self.* = undefined;
    }
};

const ChatState = struct {
    status: []const u8 = "connecting",
    joined: bool = false,
    join_requested: bool = false,
    avatar_announced: bool = false,
    ircx_data: bool = false,
    pending_udi: std.ArrayList(PendingUdi) = .empty,

    fn deinit(self: *ChatState, gpa: std.mem.Allocator) void {
        for (self.pending_udi.items) |*entry| entry.deinit(gpa);
        self.pending_udi.deinit(gpa);
        self.* = undefined;
    }

    fn rememberUdi(self: *ChatState, gpa: std.mem.Allocator, target: []const u8, nick: []const u8, wire: []const u8) !void {
        for (self.pending_udi.items) |*entry| {
            if (!std.ascii.eqlIgnoreCase(entry.target, target) or !std.ascii.eqlIgnoreCase(entry.nick, nick)) continue;
            const replacement = try gpa.dupe(u8, wire);
            gpa.free(entry.wire);
            entry.wire = replacement;
            return;
        }
        const owned_target = try gpa.dupe(u8, target);
        errdefer gpa.free(owned_target);
        const owned_nick = try gpa.dupe(u8, nick);
        errdefer gpa.free(owned_nick);
        const owned_wire = try gpa.dupe(u8, wire);
        errdefer gpa.free(owned_wire);
        try self.pending_udi.append(gpa, .{ .target = owned_target, .nick = owned_nick, .wire = owned_wire });
    }

    fn takeUdi(self: *ChatState, target: []const u8, nick: []const u8) ?PendingUdi {
        for (self.pending_udi.items, 0..) |entry, index| {
            if (std.ascii.eqlIgnoreCase(entry.target, target) and std.ascii.eqlIgnoreCase(entry.nick, nick))
                return self.pending_udi.orderedRemove(index);
        }
        return null;
    }
};

/// JOIN and end-of-NAMES may both confirm the same join. Announce the current
/// deterministic/selected self avatar only once, after either confirmation.
fn finishJoin(
    client: *cc.net.client.Client,
    transcript: *cc.comic.session.Transcript,
    nick: []const u8,
    channel: []const u8,
    state: *ChatState,
) !void {
    state.joined = true;
    state.status = "connected";
    if (state.avatar_announced) return;
    try client.announceAvatar(channel, transcript.resolvedAvatar(nick));
    state.avatar_announced = true;
}

const UiEventResult = struct {
    keep_running: bool = true,
    redraw: bool = false,
};

const NetworkEvent = enum { none, connecting, transport_ready, retry_scheduled, sts_upgrading };

/// UI-owned nonblocking connection lifecycle. DNS/TCP/proxy/TLS runs inside
/// Transport.Connector; this owner only swaps immutable endpoint snapshots,
/// registers a completed client, and schedules bounded reconnects.
const AsyncNetwork = struct {
    gpa: std.mem.Allocator,
    host: []const u8,
    nick: []const u8,
    base_options: cc.net.client.ConnectOptions,
    runtime: *ConnectionRuntime,
    reconnect: cc.net.connection_policy.ReconnectController,
    connector: ?*cc.net.transport.Connector = null,
    client: ?cc.net.client.Client = null,

    fn init(
        gpa: std.mem.Allocator,
        host: []const u8,
        port: u16,
        nick: []const u8,
        runtime: *ConnectionRuntime,
    ) !AsyncNetwork {
        var self = AsyncNetwork{
            .gpa = gpa,
            .host = host,
            .nick = nick,
            .base_options = runtime.connect_options,
            .runtime = runtime,
            .reconnect = .init(port, 0x434f4d4943434841),
        };
        _ = self.reconnect.start();
        try self.startConnector();
        return self;
    }

    fn deinit(self: *AsyncNetwork) void {
        self.reconnect.cancel();
        if (self.connector) |connector| connector.deinit();
        if (self.client) |*client| client.deinit();
        self.* = undefined;
    }

    fn effectiveOptions(self: *const AsyncNetwork) cc.net.client.ConnectOptions {
        var options = self.base_options;
        if (self.reconnect.force_tls) options.security = .tls;
        return options;
    }

    fn startConnector(self: *AsyncNetwork) !void {
        if (self.connector != null or self.client != null) return error.InvalidReconnectState;
        self.connector = try cc.net.transport.Connector.start(
            self.gpa,
            self.host,
            self.reconnect.port,
            self.effectiveOptions(),
        );
    }

    fn tick(self: *AsyncNetwork, now_ms: u64) !NetworkEvent {
        if (self.client) |*client| {
            client.tick(now_ms) catch |err| return self.fail(now_ms, err);
            return .none;
        }
        if (self.connector) |connector| {
            const maybe_transport = connector.poll() catch {
                connector.deinit();
                self.connector = null;
                self.reconnect.disconnected(now_ms);
                return .retry_scheduled;
            };
            const connected = maybe_transport orelse return .none;
            connector.deinit();
            self.connector = null;
            var client = cc.net.client.Client.fromTransport(
                self.gpa,
                self.host,
                self.reconnect.port,
                self.effectiveOptions(),
                connected,
            ) catch {
                connected.deinit();
                self.reconnect.disconnected(now_ms);
                return .retry_scheduled;
            };
            var owns_client = true;
            defer if (owns_client) client.deinit();
            const registration_options = self.runtime.registrationOptionsForAttempt() catch {
                self.reconnect.disconnected(now_ms);
                return .retry_scheduled;
            };
            client.registerWithOptions(self.nick, self.nick, "Comic Chat for Zig", registration_options) catch {
                self.reconnect.disconnected(now_ms);
                return .retry_scheduled;
            };
            client.tick(now_ms) catch {
                self.reconnect.disconnected(now_ms);
                return .retry_scheduled;
            };
            self.client = client;
            owns_client = false;
            self.reconnect.connected();
            return .transport_ready;
        }
        if (self.reconnect.due(now_ms)) {
            self.startConnector() catch {
                self.reconnect.disconnected(now_ms);
                return .retry_scheduled;
            };
            return .connecting;
        }
        return .none;
    }

    fn fail(self: *AsyncNetwork, now_ms: u64, _: anyerror) NetworkEvent {
        var upgrade_port: ?u16 = null;
        if (self.client) |*client| {
            upgrade_port = client.takeStsUpgradePort();
            client.deinit();
            self.client = null;
        }
        if (upgrade_port) |tls_port| {
            self.reconnect.stsUpgrade(tls_port, now_ms) catch {
                self.reconnect.disconnected(now_ms);
                return .retry_scheduled;
            };
            return .sts_upgrading;
        }
        self.reconnect.disconnected(now_ms);
        return .retry_scheduled;
    }

    fn clientPtr(self: *AsyncNetwork) ?*cc.net.client.Client {
        if (self.client) |*client| return client;
        return null;
    }
};

fn applyNetworkEvent(event: NetworkEvent, state: *ChatState) bool {
    return switch (event) {
        .none => false,
        .connecting => changed: {
            state.status = "connecting";
            break :changed true;
        },
        .transport_ready => changed: {
            state.status = "registering";
            break :changed true;
        },
        .retry_scheduled => changed: {
            resetChatConnectionState(state);
            state.status = "reconnecting";
            break :changed true;
        },
        .sts_upgrading => changed: {
            resetChatConnectionState(state);
            state.status = "upgrading to TLS";
            break :changed true;
        },
    };
}

fn resetChatConnectionState(state: *ChatState) void {
    state.joined = false;
    state.join_requested = false;
    state.avatar_announced = false;
}

fn runInteractiveX11(gpa: std.mem.Allocator, host: []const u8, port: u16, nick: []const u8, channel: []const u8, runtime: *ConnectionRuntime, io: std.Io) !void {
    return runInteractivePollBackend(cc.platform.x11, gpa, host, port, nick, channel, runtime, io);
}

fn runInteractiveWayland(gpa: std.mem.Allocator, host: []const u8, port: u16, nick: []const u8, channel: []const u8, runtime: *ConnectionRuntime, io: std.Io) !void {
    return runInteractivePollBackend(cc.platform.wayland, gpa, host, port, nick, channel, runtime, io);
}

fn runInteractivePollBackend(
    comptime Backend: type,
    gpa: std.mem.Allocator,
    host: []const u8,
    port: u16,
    nick: []const u8,
    channel: []const u8,
    runtime: *ConnectionRuntime,
    io: std.Io,
) !void {
    const posix = std.posix;

    const win = try Backend.Window.open(gpa, 960, 720, "Comic Chat");
    defer win.deinit();
    var view = try cc.client.view.View.init(gpa, win.width, win.height);
    defer view.deinit();
    var workspace = try cc.client.workspace.Workspace.init(gpa, nick);
    defer workspace.deinit();
    _ = try workspace.ensure(channel);
    var state: ChatState = .{};
    defer state.deinit(gpa);
    var network = try AsyncNetwork.init(gpa, host, port, nick, runtime);
    defer network.deinit();

    try presentWorkspace(win, &view, state.status, &workspace);

    var poll_fds = [_]posix.pollfd{
        .{ .fd = win.fd(), .events = posix.POLL.IN | posix.POLL.ERR, .revents = 0 },
        .{ .fd = -1, .events = posix.POLL.IN | posix.POLL.ERR, .revents = 0 },
    };

    // Wayland deliberately leaves key-repeat to the client (see
    // platform/wayland.zig's module doc) — Window.checkRepeat must be
    // polled regularly even with no compositor traffic at all, so a backend
    // that implements it gets a short poll timeout instead of the normal
    // up-to-1000ms one. X11 (no checkRepeat: real auto-repeat arrives as
    // ordinary wire KeyPress events the existing revents check already
    // handles) keeps its current cadence.
    const has_client_side_repeat = @hasDecl(Backend.Window, "checkRepeat");
    const repeat_poll_timeout_ms = 15;

    while (true) {
        var redraw = false;
        const base_timeout: i32 = if (network.clientPtr() == null) 50 else 1000;
        const timeout = if (has_client_side_repeat) @min(base_timeout, repeat_poll_timeout_ms) else base_timeout;
        _ = try posix.poll(&poll_fds, timeout);
        const now_ms = monotonicMilliseconds(io);
        redraw = applyNetworkEvent(try network.tick(now_ms), &state) or redraw;
        poll_fds[1].fd = if (network.clientPtr()) |client| client.fd() else -1;

        if ((poll_fds[0].revents & (posix.POLL.ERR | posix.POLL.HUP | posix.POLL.NVAL)) != 0) return;
        if ((poll_fds[0].revents & posix.POLL.IN) != 0) {
            const event_result = try handleWindowEvent(
                gpa,
                io,
                try win.nextEvent(),
                &view,
                network.clientPtr(),
                &workspace,
                nick,
                channel,
                state.joined,
                state.ircx_data,
            );
            if (!event_result.keep_running) return;
            redraw = redraw or event_result.redraw;
        }
        if (has_client_side_repeat) {
            if (win.checkRepeat()) |repeat_event| {
                const event_result = try handleWindowEvent(
                    gpa,
                    io,
                    repeat_event,
                    &view,
                    network.clientPtr(),
                    &workspace,
                    nick,
                    channel,
                    state.joined,
                    state.ircx_data,
                );
                if (!event_result.keep_running) return;
                redraw = redraw or event_result.redraw;
            }
        }

        if (network.clientPtr()) |client| if ((poll_fds[1].revents & posix.POLL.IN) != 0) {
            const maybe_received: ?bool = client.receive() catch |err| failed: {
                redraw = applyNetworkEvent(network.fail(now_ms, err), &state) or redraw;
                poll_fds[1].fd = -1;
                break :failed null;
            };
            if (maybe_received) |received| {
                if (!received) {
                    redraw = applyNetworkEvent(network.fail(now_ms, error.EndOfStream), &state) or redraw;
                    poll_fds[1].fd = -1;
                } else if (network.clientPtr()) |active| {
                    const processed = processWorkspaceMessages(active, &workspace, nick, channel, &state) catch |err| failed: {
                        redraw = applyNetworkEvent(network.fail(now_ms, err), &state) or redraw;
                        poll_fds[1].fd = -1;
                        break :failed false;
                    };
                    redraw = redraw or processed;
                }
            }
        };
        if (network.clientPtr() != null and
            (poll_fds[1].revents & (posix.POLL.ERR | posix.POLL.HUP | posix.POLL.NVAL)) != 0)
        {
            redraw = applyNetworkEvent(network.fail(now_ms, error.ConnectionResetByPeer), &state) or redraw;
            poll_fds[1].fd = -1;
        }

        if (redraw) try presentWorkspace(win, &view, state.status, &workspace);
    }
}

fn runInteractiveWin32(gpa: std.mem.Allocator, host: []const u8, port: u16, nick: []const u8, channel: []const u8, runtime: *ConnectionRuntime, io: std.Io) !void {
    const Win32 = cc.platform.win32;

    const win = try Win32.Window.open(gpa, 960, 720, "Comic Chat");
    defer win.deinit();
    var view = try cc.client.view.View.init(gpa, win.width, win.height);
    defer view.deinit();
    var workspace = try cc.client.workspace.Workspace.init(gpa, nick);
    defer workspace.deinit();
    _ = try workspace.ensure(channel);
    var state: ChatState = .{};
    defer state.deinit(gpa);
    var network = try AsyncNetwork.init(gpa, host, port, nick, runtime);
    defer network.deinit();
    try presentWorkspace(win, &view, state.status, &workspace);

    while (true) {
        var redraw = false;
        const now_ms = monotonicMilliseconds(io);
        redraw = applyNetworkEvent(try network.tick(now_ms), &state) or redraw;
        while (try win.pollEvent()) |event| {
            const event_result = try handleWindowEvent(
                gpa,
                io,
                event,
                &view,
                network.clientPtr(),
                &workspace,
                nick,
                channel,
                state.joined,
                state.ircx_data,
            );
            if (!event_result.keep_running) return;
            redraw = redraw or event_result.redraw;
        }

        if (network.clientPtr()) |client| {
            const receive_result = client.receiveTimeout(16) catch |err| disconnected: {
                redraw = applyNetworkEvent(network.fail(now_ms, err), &state) or redraw;
                break :disconnected null;
            };
            if (receive_result) |received| {
                if (!received) {
                    redraw = applyNetworkEvent(network.fail(now_ms, error.EndOfStream), &state) or redraw;
                } else if (network.clientPtr()) |active| {
                    const processed = processWorkspaceMessages(active, &workspace, nick, channel, &state) catch |err| failed: {
                        redraw = applyNetworkEvent(network.fail(now_ms, err), &state) or redraw;
                        break :failed false;
                    };
                    redraw = redraw or processed;
                }
            }
        } else {
            try std.Io.sleep(io, std.Io.Duration.fromMilliseconds(16), .awake);
        }

        if (redraw) try presentWorkspace(win, &view, state.status, &workspace);
    }
}

fn handleWindowEvent(
    gpa: std.mem.Allocator,
    io: std.Io,
    event: anytype,
    view: *cc.client.view.View,
    client: ?*cc.net.client.Client,
    workspace: *cc.client.workspace.Workspace,
    nick: []const u8,
    channel: []const u8,
    joined: bool,
    ircx_data: bool,
) !UiEventResult {
    _ = channel;
    const room = workspace.activeRoom() orelse return .{};
    const transcript = &room.transcript;
    const editor = &room.editor;
    return switch (event) {
        .close => .{ .keep_running = false },
        .expose => .{ .redraw = true },
        .resize => |size| resized: {
            try view.resize(size.w, size.h);
            break :resized .{ .redraw = true };
        },
        .key => |key_input| key_result: {
            const key = key_input.key;
            if (view.active_dialog != null) {
                if (try view.handleDialogKey(key)) |action| try applyDialogAction(action, view, client, workspace, nick);
                break :key_result .{ .redraw = true };
            }
            if (key_input.modifiers.control and try handleEditorShortcut(editor, key, workspace))
                break :key_result .{ .redraw = true };
            if (key_input.modifiers.shift and key == .tab) {
                view.cycleFocusBackward();
                break :key_result .{ .redraw = true };
            }
            if (key_input.modifiers.shift and handleEditorSelectionKey(editor, key))
                break :key_result .{ .redraw = true };
            break :key_result .{
                .keep_running = try handleWorkspaceInputKey(gpa, io, key, view, editor, client, workspace, nick, joined, ircx_data),
                .redraw = true,
            };
        },
        .pointer => |pointer| pointer_result: {
            if (pointer.kind == .move) break :pointer_result .{};
            const action = view.handlePointer(pointer, transcript.count(), transcript.roster.items.len);
            const keep_running = switch (action) {
                .send => try handleWorkspaceInputKey(gpa, io, cc.platform.event.Key{ .enter = {} }, view, editor, client, workspace, nick, joined, ircx_data),
                .room_tab => |index| workspace.activate(index),
                .dialog_accept, .dialog_cancel => apply: {
                    try applyDialogAction(action, view, client, workspace, nick);
                    break :apply true;
                },
                else => true,
            };
            break :pointer_result .{ .keep_running = keep_running, .redraw = true };
        },
        .other => .{},
    };
}

fn handleEditorSelectionKey(editor: *cc.client.input.Editor, key: cc.platform.event.Key) bool {
    switch (key) {
        .left => editor.extendLeft(),
        .right => editor.extendRight(),
        .home => editor.extendHome(),
        .end => editor.extendEnd(),
        else => return false,
    }
    return true;
}

fn handleEditorShortcut(
    editor: *cc.client.input.Editor,
    key: cc.platform.event.Key,
    workspace: *cc.client.workspace.Workspace,
) !bool {
    const codepoint = switch (key) {
        .char => |ch| if (ch <= 0x7f) std.ascii.toLower(@intCast(ch)) else return false,
        else => return false,
    };
    switch (codepoint) {
        'a' => editor.selectAll(),
        'c' => if (try editor.copySelection()) |text| {
            defer editor.gpa.free(text);
            try workspace.setClipboard(text);
        },
        'x' => if (try editor.cutSelection()) |text| {
            defer editor.gpa.free(text);
            try workspace.setClipboard(text);
        },
        'v' => try editor.paste(workspace.clipboard.items),
        'z' => editor.undo(),
        'y' => editor.redo(),
        else => return false,
    }
    return true;
}

fn applyDialogAction(
    action: cc.client.view.Action,
    view: *cc.client.view.View,
    maybe_client: ?*cc.net.client.Client,
    workspace: *cc.client.workspace.Workspace,
    nick: []const u8,
) !void {
    const id = switch (action) {
        .dialog_accept => |id| id,
        else => return,
    };
    const value = std.mem.trim(u8, view.dialogValue(), " \t");
    const room = workspace.activeRoom() orelse return;
    switch (id) {
        .channel, .channel_create => {
            const index = workspace.ensure(value) catch return;
            _ = workspace.activate(index);
            if (maybe_client) |client| try client.join(value);
        },
        .character => {
            const selected = cc.comic.session.bundledAvatarByName(value) orelse return;
            try room.transcript.setAvatar(nick, selected);
            if (maybe_client) |client| try client.announceAvatar(room.name, selected);
        },
        .background => if (maybe_client) |client| try client.syncBackdrop(room.name, value, null),
        .nickname => if (maybe_client) |client| try client.changeNick(value),
        .away => if (maybe_client) |client| try client.setAway(value),
        .kick => if (maybe_client) |client| try client.kick(room.name, value),
        .ban => if (maybe_client) |client| try client.setBan(room.name, value),
        .invite => if (maybe_client) |client| try client.invite(value, room.name),
        .whisper => {
            for (room.transcript.roster.items, 0..) |member, index| if (std.ascii.eqlIgnoreCase(member.nick, value)) {
                view.shell.selectMember(index);
                view.shell.setSayMode(.whisper);
                break;
            };
        },
        else => {},
    }
}

fn handleWorkspaceInputKey(
    gpa: std.mem.Allocator,
    io: std.Io,
    key: cc.platform.event.Key,
    view: *cc.client.view.View,
    editor: *cc.client.input.Editor,
    maybe_client: ?*cc.net.client.Client,
    workspace: *cc.client.workspace.Workspace,
    nick: []const u8,
    connected: bool,
    ircx_data: bool,
) !bool {
    if (key == .enter and editor.text().len > 0) {
        const text = editor.text();
        if (std.mem.startsWith(u8, text, "/save ")) {
            const path = std.mem.trim(u8, text[6..], " \t");
            const room = workspace.activeRoom() orelse return true;
            cc.client.files.saveConversation(io, gpa, path, &room.transcript) catch return true;
            const consumed = try editor.take();
            gpa.free(consumed);
            return true;
        }
        if (std.mem.startsWith(u8, text, "/open ")) {
            const path = std.mem.trim(u8, text[6..], " \t");
            var loaded = cc.client.files.loadConversation(io, gpa, path) catch return true;
            errdefer loaded.deinit();
            try loaded.setSelf(nick);
            const room = workspace.activeRoom() orelse return true;
            room.transcript.deinit();
            room.transcript = loaded;
            const consumed = try editor.take();
            gpa.free(consumed);
            view.jumpLatest();
            return true;
        }
        if (std.mem.startsWith(u8, text, "/export ")) {
            const path = std.mem.trim(u8, text[8..], " \t");
            const png = cc.render.png.encode(gpa, view.pixels(), view.width(), view.height()) catch return true;
            defer gpa.free(png);
            cc.client.files.saveBytesAtomic(io, gpa, path, png) catch return true;
            const consumed = try editor.take();
            gpa.free(consumed);
            return true;
        }
        if (std.mem.startsWith(u8, text, "/join ")) {
            const name = std.mem.trim(u8, text[6..], " \t");
            const index = workspace.ensure(name) catch return true;
            _ = workspace.activate(index);
            if (maybe_client) |client| try client.join(name);
            const consumed = try editor.take();
            gpa.free(consumed);
            return true;
        }
        if (std.mem.startsWith(u8, text, "/switch ")) {
            const name = std.mem.trim(u8, text[8..], " \t");
            if (workspace.find(name)) |index| _ = workspace.activate(index);
            const consumed = try editor.take();
            gpa.free(consumed);
            return true;
        }
        if (std.mem.eql(u8, text, "/part")) {
            if (workspace.active) |index| {
                if (workspace.rooms.items.len > 1) {
                    if (maybe_client) |client| try client.part(workspace.rooms.items[index].name);
                    _ = workspace.remove(index);
                }
            }
            const consumed = try editor.take();
            gpa.free(consumed);
            return true;
        }
    }
    const room = workspace.activeRoom() orelse return true;
    return handleInputKey(gpa, key, view, editor, maybe_client, &room.transcript, nick, room.name, room.joined or connected, ircx_data);
}

fn processWorkspaceMessages(
    client: *cc.net.client.Client,
    workspace: *cc.client.workspace.Workspace,
    nick: []const u8,
    channel: []const u8,
    state: *ChatState,
) !bool {
    var redraw = false;
    while (try client.bufferedNext()) |msg| {
        if (std.ascii.eqlIgnoreCase(msg.command, "QUIT") or std.ascii.eqlIgnoreCase(msg.command, "NICK")) {
            for (workspace.rooms.items) |*room| redraw = (try room.transcript.observeIrc(&msg, room.name, nick)) or redraw;
        } else if (messageRoom(&msg)) |room_name| {
            if (room_name.len > 1 and (room_name[0] == '#' or room_name[0] == '&')) {
                const room_index = try workspace.ensure(room_name);
                redraw = (try workspace.rooms.items[room_index].transcript.observeIrc(&msg, room_name, nick)) or redraw;
            }
        }
        if (std.mem.eql(u8, msg.command, "800")) {
            state.ircx_data = true;
        } else if (std.mem.eql(u8, msg.command, "005")) {
            var index: usize = 0;
            while (index < msg.param_count) : (index += 1) {
                if (std.ascii.eqlIgnoreCase(msg.params[index], "COMICCHAT=DATA"))
                    state.ircx_data = true;
            }
        } else if (!state.join_requested and std.mem.eql(u8, msg.command, "001")) {
            try client.join(channel);
            state.join_requested = true;
            state.status = "joining";
            redraw = true;
        } else if (std.mem.eql(u8, msg.command, "JOIN")) {
            const who = if (msg.prefix) |p| cc.comic.session.nickFromPrefix(p) else "";
            const joined_channel = msg.param(0) orelse "";
            if (std.ascii.eqlIgnoreCase(who, nick)) {
                const room_index = try workspace.ensure(joined_channel);
                var room = &workspace.rooms.items[room_index];
                room.joined = true;
                state.joined = true;
                state.status = "connected";
                try client.announceAvatar(room.name, room.transcript.resolvedAvatar(nick));
                redraw = true;
            }
        } else if (std.mem.eql(u8, msg.command, "366")) {
            const joined_channel = msg.param(1) orelse msg.param(0) orelse "";
            if (workspace.find(joined_channel)) |room_index| {
                workspace.rooms.items[room_index].joined = true;
                state.joined = true;
                state.status = "connected";
                redraw = true;
            }
        } else if (std.mem.eql(u8, msg.command, "DATA")) {
            const target = msg.param(0) orelse continue;
            const kind = msg.param(1) orelse continue;
            const wire = msg.param(2) orelse continue;
            if (!std.mem.eql(u8, kind, "CCUDI1")) continue;
            const room_index = workspace.find(target) orelse continue;
            const who = if (msg.prefix) |prefix| cc.comic.session.nickFromPrefix(prefix) else continue;
            _ = cc.proto.udi.parseAnnotation(wire) catch continue;
            try state.rememberUdi(workspace.gpa, target, who, wire);
            _ = room_index;
        } else if (std.mem.eql(u8, msg.command, "PRIVMSG")) {
            const target = msg.param(0) orelse continue;
            const room_index = workspace.find(target) orelse continue;
            var room = &workspace.rooms.items[room_index];
            const transcript = &room.transcript;
            const text = msg.param(1) orelse continue;
            const who = if (msg.prefix) |p| cc.comic.session.nickFromPrefix(p) else "someone";
            if (try transcript.consumeAvatarAnnouncement(who, text)) {
                redraw = true;
                continue;
            }
            var pending = state.takeUdi(target, who);
            defer if (pending) |*entry| entry.deinit(transcript.gpa);
            try transcript.addWireMessage(
                who,
                text,
                false,
                if (pending) |entry| entry.wire else null,
            );
            transcript.trimTo(64);
            if (workspace.active != room_index) room.unread +|= 1;
            redraw = true;
        } else if (std.mem.eql(u8, msg.command, "433")) {
            state.status = "nickname in use";
            redraw = true;
        }
    }
    return redraw;
}

fn messageRoom(msg: *const cc.net.message.Message) ?[]const u8 {
    if (std.ascii.eqlIgnoreCase(msg.command, "353")) {
        if (msg.param_count < 2) return null;
        return msg.params[msg.param_count - 2];
    }
    if (std.ascii.eqlIgnoreCase(msg.command, "366")) return msg.param(1) orelse msg.param(0);
    if (std.ascii.eqlIgnoreCase(msg.command, "JOIN") or
        std.ascii.eqlIgnoreCase(msg.command, "PART") or
        std.ascii.eqlIgnoreCase(msg.command, "DATA") or
        std.ascii.eqlIgnoreCase(msg.command, "PRIVMSG")) return msg.param(0);
    return null;
}

fn presentView(
    win: anytype,
    view: *cc.client.view.View,
    title: []const u8,
    status: []const u8,
    transcript: *const cc.comic.session.Transcript,
    editor: *const cc.client.input.Editor,
) !void {
    try view.render(title, status, transcript, editor.text(), editor.cursor);
    try win.present(view.pixels(), view.width(), view.height());
}

fn presentWorkspace(
    win: anytype,
    view: *cc.client.view.View,
    status: []const u8,
    workspace: *cc.client.workspace.Workspace,
) !void {
    const room = workspace.activeRoom() orelse return;
    var tabs: [cc.client.workspace.max_rooms]cc.client.view.View.Tab = undefined;
    for (workspace.rooms.items, 0..) |item, index| tabs[index] = .{
        .label = item.name,
        .unread = item.unread,
    };
    try view.renderTabs(status, &room.transcript, room.editor.text(), room.editor.cursor, room.editor.selection(), tabs[0..workspace.rooms.items.len], workspace.active.?);
    try win.present(view.pixels(), view.width(), view.height());
}

fn handleInputKey(
    gpa: std.mem.Allocator,
    key: anytype,
    view: *cc.client.view.View,
    editor: *cc.client.input.Editor,
    maybe_client: ?*cc.net.client.Client,
    transcript: *cc.comic.session.Transcript,
    nick: []const u8,
    channel: []const u8,
    joined: bool,
    ircx_data: bool,
) !bool {
    switch (key) {
        .char => |ch| {
            view.focusComposer();
            const encoded_len = std.unicode.utf8CodepointSequenceLength(ch) catch return true;
            if (editor.text().len + encoded_len <= 400) try editor.insert(ch);
        },
        .backspace => editor.backspace(),
        .delete => editor.delete(),
        .left => editor.left(),
        .right => editor.right(),
        .home => editor.home(),
        .end => editor.end(),
        .escape => if (!view.closeDialog()) return false,
        .enter => {
            if (view.active_dialog != null) {
                _ = view.closeDialog();
                return true;
            }
            if (editor.text().len == 0) return true;
            const line = try editor.take();
            defer gpa.free(line);
            if (std.mem.eql(u8, line, "/quit")) return false;
            if (std.mem.eql(u8, line, "/clear")) {
                transcript.trimTo(0);
                view.jumpLatest();
                return true;
            }
            if (std.mem.eql(u8, line, "/view comic") or std.mem.eql(u8, line, "/comic")) {
                view.setContentMode(.comic);
                return true;
            }
            if (std.mem.eql(u8, line, "/view text") or std.mem.eql(u8, line, "/text")) {
                view.setContentMode(.text);
                return true;
            }
            if (std.mem.eql(u8, line, "/members")) {
                view.toggleMembers();
                return true;
            }
            if (std.mem.eql(u8, line, "/latest")) {
                view.jumpLatest();
                return true;
            }
            if (std.mem.startsWith(u8, line, "/dialog ")) {
                _ = view.openDialogByResource(std.mem.trim(u8, line["/dialog ".len..], " \t"));
                return true;
            }
            if (!joined) return true;
            const client = maybe_client orelse return true;
            if (std.mem.eql(u8, line, "/avatar") or std.mem.startsWith(u8, line, "/avatar ")) {
                const requested = if (line.len > "/avatar ".len) line["/avatar ".len..] else "";
                const selected = cc.comic.session.bundledAvatarByName(requested) orelse return true;
                try transcript.setAvatar(nick, selected);
                try client.announceAvatar(channel, selected);
                return true;
            }
            const selected_mode = view.shell.say_mode;
            const action_text: ?[]const u8 = if (std.mem.eql(u8, line, "/me"))
                ""
            else if (std.mem.startsWith(u8, line, "/me "))
                line["/me ".len..]
            else if (selected_mode == .action)
                line
            else
                null;
            if (action_text) |body| if (body.len == 0) return true;
            const visible_text = action_text orelse line;
            const modes: u16 = if (action_text != null) cc.proto.udi.bm_action else switch (selected_mode) {
                .say => cc.proto.udi.bm_say,
                .think => cc.proto.udi.bm_think,
                .whisper => cc.proto.udi.bm_whisper,
                .action => unreachable,
                .sound => cc.proto.udi.bm_sound,
            };
            const target = if (selected_mode == .whisper) whisper: {
                const member_index = view.shell.selected_member orelse return true;
                if (member_index >= transcript.roster.items.len) return true;
                break :whisper transcript.roster.items[member_index].nick;
            } else channel;
            const is_private = selected_mode == .whisper;
            const avatar_name = transcript.resolvedAvatar(nick);
            const avatar = cc.comic.strip.avatarByName(avatar_name) orelse return error.UnknownAvatar;
            const selected_emotion = view.shell.selectedEmotion();
            const pose_state = if (selected_emotion == .neutral)
                try cc.comic.figure.poseStateForText(gpa, avatar, visible_text)
            else
                try cc.comic.figure.poseStateForEmotion(gpa, avatar, selected_emotion, view.shell.selectedEmotionIntensity());
            var comic_message: std.ArrayList(u8) = .empty;
            defer comic_message.deinit(gpa);
            try cc.proto.udi.encode(&comic_message, gpa, .{
                .gesture = pose_state.gesture,
                .expression = pose_state.expression,
                .requested = pose_state.requested,
                .modes = modes,
            }, !ircx_data);
            var chat_message: std.ArrayList(u8) = .empty;
            defer chat_message.deinit(gpa);
            if (action_text) |body| {
                try chat_message.appendSlice(gpa, cc.comic.session.ctcp_action_prefix);
                try chat_message.appendSlice(gpa, body);
                try chat_message.append(gpa, 0x01);
            } else {
                try chat_message.appendSlice(gpa, line);
            }
            if (ircx_data) {
                try client.comicData(target, comic_message.items);
                try client.privmsg(target, chat_message.items);
                try transcript.addWireMessage(nick, chat_message.items, is_private, comic_message.items);
            } else {
                try comic_message.appendSlice(gpa, chat_message.items);
                try client.privmsg(target, comic_message.items);
                try transcript.addWireMessage(nick, comic_message.items, is_private, null);
            }
            view.shell.setSayMode(.say);
            transcript.trimTo(64);
            view.jumpLatest();
        },
        .tab => view.cycleFocus(),
        .page_up => view.pageEarlier(transcript.lines.items.len),
        .page_down => view.pageLater(),
        .up, .down, .other => {},
    }
    return true;
}

fn runRenderStrip(gpa: std.mem.Allocator, io: std.Io) !void {
    const lines = [_]cc.comic.strip.Line{
        .{ .speaker = "anna", .text = "The title panel starts every comic." },
        .{ .speaker = "kevin", .text = "Different speakers may share a panel." },
        .{ .speaker = "anna", .text = "A repeated speaker starts a fresh panel." },
        .{ .speaker = "mike", .text = "Two columns and source-sized interstices." },
        .{ .speaker = "rebecca", .text = "Masks and backdrops follow the old draw order." },
        .{ .speaker = "xeno", .text = "The source renderer returns one complete page." },
    };
    var strip = try cc.comic.strip.render(gpa, &lines);
    defer strip.deinit(gpa);
    try emitPpm(gpa, io, strip.pixels, strip.width, strip.height);
}

fn runToPng(gpa: std.mem.Allocator, io: std.Io, name: []const u8) !void {
    const data = bgByName(name) orelse {
        elog("unknown background '{s}'\n", .{name});
        return;
    };
    var img = try cc.assets.bgb.decodeBackground(gpa, data);
    defer img.deinit(gpa);
    const png = try cc.render.png.encode(gpa, img.pixels, img.width, img.height);
    defer gpa.free(png);
    try writeStdout(io, png);
}

fn runWindow(gpa: std.mem.Allocator, name: []const u8, prefer_wayland: bool) !void {
    const avb = avatarByName(name) orelse {
        elog("unknown avatar '{s}'\n", .{name});
        return;
    };
    var fig = cc.comic.figure.assemble(gpa, avb, 0, 0) catch {
        elog("could not assemble figure for '{s}'\n", .{name});
        return;
    };
    defer fig.deinit(gpa);
    const pad: i32 = 18;
    const W: u32 = fig.width + 2 * @as(u32, @intCast(pad));
    const H: u32 = fig.height + 2 * @as(u32, @intCast(pad));
    var c = try cc.render.canvas.Canvas.init(gpa, W, H);
    defer c.deinit(gpa);
    c.clear(cc.render.canvas.white);
    cc.comic.figure.composite(&c, fig.pixels, fig.width, fig.height, pad, pad);
    if (comptime builtin.os.tag == .linux) {
        if (prefer_wayland) {
            cc.platform.wayland.show(gpa, c.px, W, H) catch |err| {
                elog("wayland: {s}\n", .{@errorName(err)});
                return;
            };
        } else {
            cc.platform.x11.show(gpa, c.px, W, H) catch |err| {
                elog("x11: {s}\n", .{@errorName(err)});
                return;
            };
        }
    } else if (comptime builtin.os.tag == .windows) {
        try cc.platform.win32.show(gpa, c.px, W, H);
    } else {
        return error.UnsupportedPlatform;
    }
}

fn runConnect(
    gpa: std.mem.Allocator,
    io: std.Io,
    host: []const u8,
    port: u16,
    nick: []const u8,
    channel: []const u8,
    connect_options: cc.net.client.ConnectOptions,
    registration_options: cc.net.client.RegistrationOptions,
) !void {
    elog("connecting to {s}:{d} as {s} ...\n", .{ host, port, nick });

    var client = try cc.net.client.Client.connectWithOptions(gpa, host, port, connect_options);
    defer client.deinit();

    try client.registerWithOptions(nick, nick, "Comic Chat portable", registration_options);
    try client.tick(monotonicMilliseconds(io));

    var registered = false;
    var joined = false;
    var post: usize = 0;
    var seen: usize = 0;

    while (seen < 80) : (seen += 1) {
        const msg = (try client.next()) orelse {
            elog("<eof>\n", .{});
            break;
        };

        elog("<- {s}", .{msg.command});
        var i: usize = 0;
        while (i < msg.param_count) : (i += 1) elog(" [{s}]", .{msg.params[i]});
        elog("\n", .{});

        if (!registered and std.mem.eql(u8, msg.command, "001")) {
            registered = true;
            elog("** registered; joining {s}\n", .{channel});
            try client.join(channel);
        } else if (registered and !joined and std.mem.eql(u8, msg.command, "JOIN")) {
            const who = if (msg.prefix) |prefix| cc.comic.session.nickFromPrefix(prefix) else "";
            const joined_channel = msg.param(0) orelse "";
            if (std.ascii.eqlIgnoreCase(who, nick) and std.ascii.eqlIgnoreCase(joined_channel, channel)) {
                joined = true;
                elog("** joined; sending a line\n", .{});
                try client.privmsg(channel, "Hello from Comic Chat!");
            }
        } else if (registered and !joined and std.mem.eql(u8, msg.command, "366")) {
            const joined_channel = msg.param(1) orelse msg.param(0) orelse "";
            if (std.ascii.eqlIgnoreCase(joined_channel, channel)) {
                joined = true;
                elog("** joined; sending a line\n", .{});
                try client.privmsg(channel, "Hello from Comic Chat!");
            }
        }

        if (joined) {
            post += 1;
            if (post >= 3) break;
        }
    }
    elog("done.\n", .{});
}
