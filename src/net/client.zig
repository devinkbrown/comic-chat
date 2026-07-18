//! High-level IRC client: ties the transport, line framer, and command
//! builders together, with automatic PING/PONG keepalive.
//!
//! The message-handling decision (`autoRespond`) is a pure function tested
//! without a socket; `Client` is the live glue over `Transport`.

const std = @import("std");
const irc = @import("irc.zig");
const ircv3 = @import("ircv3.zig");
const sasl = @import("sasl.zig");
const features_mod = @import("features.zig");
const policy = @import("connection_policy.zig");
const sts_store = @import("sts_store.zig");
const message = @import("message.zig");
const transport = @import("transport.zig");
const Transport = transport.Transport;

pub const Message = message.Message;
pub const ConnectOptions = transport.ConnectOptions;
pub const Security = transport.Security;
pub const TypingStatus = enum { active, paused, done };

const desired_without_sasl = blk: {
    var names: [ircv3.default_desired_capabilities.len - 1][]const u8 = undefined;
    var at: usize = 0;
    for (ircv3.default_desired_capabilities) |name| {
        if (!std.mem.eql(u8, name, "sasl")) {
            names[at] = name;
            at += 1;
        }
    }
    break :blk names;
};

pub const RegistrationOptions = struct {
    want_ircx: bool = true,
    credentials: ?*sasl.Credentials = null,
    sasl_preference: []const sasl.Mechanism = &sasl.default_preference,
    /// Required when SASL is enabled so SCRAM nonces come from the platform
    /// CSPRNG. Kept optional for registrations which do not authenticate.
    io: ?std.Io = null,
    sts: ?*sts_store.Store = null,
    now_seconds: u64 = 0,
};

/// If `msg` requires an automatic protocol reply (currently just PING→PONG),
/// append it to `out` and return true.
pub fn autoRespond(
    out: *std.ArrayList(u8),
    gpa: std.mem.Allocator,
    msg: Message,
) !bool {
    if (std.ascii.eqlIgnoreCase(msg.command, "PING")) {
        try irc.writePong(out, gpa, msg.param(0) orelse "");
        return true;
    }
    return false;
}

const Registration = struct {
    cap: ircv3.Session,
    sasl_session: ?sasl.Session = null,
    credentials: ?*sasl.Credentials,
    sasl_preference: []const sasl.Mechanism,
    io: ?std.Io,
    store: ?*sts_store.Store,
    now_seconds: u64,
    security: Security,
    host: []const u8,
    want_ircx: bool,
    ircx_sent: bool = false,
    done: bool = false,
    nonce: [24]u8 = undefined,

    fn init(
        gpa: std.mem.Allocator,
        host: []const u8,
        security: Security,
        options: RegistrationOptions,
    ) Registration {
        return .{
            .cap = ircv3.Session.init(gpa, .{
                .desired = if (options.credentials != null)
                    &ircv3.default_desired_capabilities
                else
                    &desired_without_sasl,
            }),
            .credentials = options.credentials,
            .sasl_preference = options.sasl_preference,
            .io = options.io,
            .store = options.sts,
            .now_seconds = options.now_seconds,
            .security = security,
            .host = host,
            .want_ircx = options.want_ircx,
        };
    }

    fn deinit(self: *Registration) void {
        if (self.sasl_session) |*session| session.deinit();
        self.cap.deinit();
        if (self.credentials) |credentials| {
            if (!credentials.zeroized) credentials.zeroize();
        }
        self.* = undefined;
    }

    fn consume(
        self: *Registration,
        out: *std.ArrayList(u8),
        msg: *const Message,
        sts_upgrade_port: *?u16,
    ) !bool {
        if (std.ascii.eqlIgnoreCase(msg.command, "CAP")) {
            const event = try self.cap.handle(out, msg.*);
            try self.applySts(sts_upgrade_port);
            try self.handleCapEvent(out, event);
            return true;
        }

        if (!self.done and std.mem.eql(u8, msg.command, "421") and
            msg.param(1) != null and std.ascii.eqlIgnoreCase(msg.param(1).?, "CAP"))
        {
            try self.finish(out);
            return true;
        }

        if (self.sasl_session) |*session| {
            if (std.ascii.eqlIgnoreCase(msg.command, "AUTHENTICATE") or isSaslNumeric(msg.command)) {
                const event = session.handle(out, msg.*) catch |err| switch (err) {
                    error.ServerSignatureMismatch => return true,
                    else => return err,
                };
                switch (event) {
                    .succeeded, .failed, .already_authenticated => {
                        const cap_event = try self.cap.saslComplete(out);
                        try self.handleCapEvent(out, cap_event);
                    },
                    else => {},
                }
                return true;
            }
        }
        return false;
    }

    fn handleCapEvent(self: *Registration, out: *std.ArrayList(u8), event: ircv3.Event) !void {
        switch (event) {
            .sasl_ready => if (self.credentials) |credentials| {
                if (self.sasl_session) |*old| old.deinit();
                self.sasl_session = sasl.Session.init(self.cap.gpa, credentials, .{ .preference = self.sasl_preference });
                const capability = self.cap.offered.get("sasl");
                self.sasl_session.?.setAdvertisement(if (capability) |entry| entry.value else null);
                var random: [18]u8 = undefined;
                try std.Io.randomSecure(self.io orelse return error.SecureRandomUnavailable, &random);
                defer std.crypto.secureZero(u8, &random);
                _ = std.base64.standard.Encoder.encode(&self.nonce, &random);
                _ = self.sasl_session.?.start(out, &self.nonce) catch |err| switch (err) {
                    error.NoSupportedMechanism => {
                        credentials.zeroize();
                        const completed = try self.cap.saslComplete(out);
                        try self.handleCapEvent(out, completed);
                        return;
                    },
                    else => return err,
                };
            } else {
                const completed = try self.cap.saslComplete(out);
                try self.handleCapEvent(out, completed);
            },
            .complete => try self.finish(out),
            else => {},
        }
    }

    fn finish(self: *Registration, out: *std.ArrayList(u8)) !void {
        if (self.done) return;
        if (self.want_ircx and !self.ircx_sent) {
            try irc.writeIrcx(out, self.cap.gpa);
            self.ircx_sent = true;
        }
        if (self.credentials) |credentials| {
            if (self.sasl_session == null and !credentials.zeroized) credentials.zeroize();
        }
        self.done = true;
    }

    fn applySts(self: *Registration, upgrade_port: *?u16) !void {
        const update = self.cap.takeStsPolicyUpdate(switch (self.security) {
            .plaintext => .plaintext,
            .tls => .tls_verified,
        });
        switch (update) {
            .none, .invalid => {},
            .upgrade_port => |port| {
                upgrade_port.* = port;
                return error.StsUpgradeRequired;
            },
            .persistence => |policy_value| if (self.store) |store| {
                try store.update(self.host, policy_value.duration_seconds, self.now_seconds);
            },
        }
    }
};

fn isSaslNumeric(command: []const u8) bool {
    if (command.len != 3 or command[0] != '9' or command[1] != '0') return false;
    return command[2] >= '0' and command[2] <= '8';
}

pub const Client = struct {
    gpa: std.mem.Allocator,
    transport: *Transport,
    host: []u8,
    port: u16,
    connect_options: ConnectOptions,
    framer: irc.LineFramer,
    out: std.ArrayList(u8) = .empty,
    tx: policy.TxQueue,
    deadlines: policy.Deadlines,
    policy_now_ms: u64 = 0,
    policy_started_ms: u64 = 0,
    registration: ?Registration = null,
    features: ?features_mod.State = null,
    aggregator: features_mod.Aggregator,
    logical_line: ?[]u8 = null,
    sts_upgrade_port: ?u16 = null,
    typing_targets: std.ArrayList(TypingTarget) = .empty,
    rx: [8192]u8 = undefined,

    pub fn connect(gpa: std.mem.Allocator, host: []const u8, port: u16) !Client {
        return connectWithOptions(gpa, host, port, .{});
    }

    pub fn connectWithOptions(
        gpa: std.mem.Allocator,
        host: []const u8,
        port: u16,
        options: ConnectOptions,
    ) !Client {
        const connected = try Transport.connectWithOptions(gpa, host, port, options);
        errdefer connected.deinit();
        return fromTransport(gpa, host, port, options, connected);
    }

    /// Bind a completed async transport to the protocol client. Ownership of
    /// `connected` transfers on success and remains with the caller on error.
    pub fn fromTransport(
        gpa: std.mem.Allocator,
        host: []const u8,
        port: u16,
        options: ConnectOptions,
        connected: *Transport,
    ) !Client {
        const owned_host = try gpa.dupe(u8, host);
        errdefer gpa.free(owned_host);
        return .{
            .gpa = gpa,
            .transport = connected,
            .host = owned_host,
            .port = port,
            .connect_options = options,
            .framer = irc.LineFramer.init(gpa),
            .tx = policy.TxQueue.init(gpa, .{}, 0, 2, 4),
            .deadlines = policy.Deadlines.init(0, .{}),
            .aggregator = features_mod.Aggregator.init(gpa, .{}),
        };
    }

    pub fn deinit(self: *Client) void {
        if (self.registration) |*registration| {
            if (registration.store) |store| {
                const elapsed_seconds = (self.policy_now_ms -| self.policy_started_ms) / 1000;
                store.rescheduleOnDisconnect(self.host, registration.now_seconds +| elapsed_seconds);
            }
            registration.deinit();
        }
        if (self.features) |*state| state.deinit();
        self.aggregator.deinit();
        self.tx.deinit();
        for (self.typing_targets.items) |entry| self.gpa.free(entry.target);
        self.typing_targets.deinit(self.gpa);
        if (self.logical_line) |line| self.gpa.free(line);
        self.framer.deinit();
        sasl.secureClear(&self.out);
        self.out.deinit(self.gpa);
        self.transport.deinit();
        self.gpa.free(self.host);
    }

    fn queueOut(self: *Client, priority: policy.Priority, replay_safe: bool, sensitive: bool) !void {
        if (self.out.items.len == 0) return;
        errdefer if (sensitive) sasl.secureClear(&self.out);
        try self.tx.enqueue(self.out.items, priority, replay_safe, sensitive);
        if (sensitive) sasl.secureClear(&self.out) else self.out.clearRetainingCapacity();
        try self.drainTx();
    }

    fn drainTx(self: *Client) !void {
        while (self.tx.peek(self.policy_now_ms)) |next_item| {
            self.transport.send(next_item.bytes) catch |err| {
                self.tx.markUncertain(next_item.index);
                return err;
            };
            self.tx.confirmSent(next_item.index);
        }
    }

    pub fn tick(self: *Client, now_ms: u64) !void {
        if (self.policy_started_ms == 0) {
            self.policy_started_ms = now_ms;
            self.deadlines = policy.Deadlines.init(now_ms, .{});
        }
        self.policy_now_ms = now_ms;
        try self.drainTx();
        switch (self.deadlines.tick(now_ms)) {
            .none => {},
            .send_ping => {
                try self.out.appendSlice(self.gpa, "PING :comicchat-keepalive\r\n");
                try self.queueOut(.control, false, false);
            },
            .disconnect => return error.ConnectionDeadlineExceeded,
        }
    }

    pub fn register(
        self: *Client,
        nick: []const u8,
        user: []const u8,
        realname: []const u8,
        want_ircx: bool,
    ) !void {
        return self.registerWithOptions(nick, user, realname, .{ .want_ircx = want_ircx });
    }

    /// Start registration without waiting. CAP/SASL advances as receive calls
    /// feed messages through bufferedNext/next.
    pub fn registerWithOptions(
        self: *Client,
        nick: []const u8,
        user: []const u8,
        realname: []const u8,
        options: RegistrationOptions,
    ) !void {
        if (self.registration != null) return error.RegistrationAlreadyStarted;
        self.registration = Registration.init(self.gpa, self.host, self.connect_options.security, options);
        errdefer {
            self.registration.?.deinit();
            self.registration = null;
        }
        self.features = try features_mod.State.init(self.gpa, nick, .{});
        errdefer {
            self.features.?.deinit();
            self.features = null;
        }
        try self.registration.?.cap.begin(&self.out);
        try irc.writeNick(&self.out, self.gpa, nick);
        try irc.writeUser(&self.out, self.gpa, user, realname);
        try self.queueOut(.control, false, false);
    }

    pub fn join(self: *Client, channel: []const u8) !void {
        try self.appendCommand("JOIN", &.{channel});
        if (self.capabilityEnabled("no-implicit-names")) try self.appendCommand("NAMES", &.{channel});
        try self.queueOut(.interactive, true, false);
    }

    pub fn privmsg(self: *Client, target: []const u8, text: []const u8) !void {
        try self.validateOutgoingText(text);
        if (self.capabilityEnabled("echo-message")) if (self.features) |*state| try state.recordEcho(target, text);
        try self.appendCommand("PRIVMSG", &.{ target, text });
        try self.queueOut(.interactive, false, false);
    }

    pub fn reply(self: *Client, target: []const u8, msgid: []const u8, text: []const u8) !void {
        if (!validMessageReference(msgid)) return error.InvalidMessageReference;
        try self.validateOutgoingText(text);
        var tags: std.ArrayList(u8) = .empty;
        defer tags.deinit(self.gpa);
        try tags.appendSlice(self.gpa, "+reply=");
        try message.escapeTagValue(&tags, self.gpa, msgid);
        if (self.capabilityEnabled("echo-message")) if (self.features) |*state| try state.recordEcho(target, text);
        try self.appendCommandWithTags("PRIVMSG", &.{ target, text }, tags.items);
        try self.queueOut(.interactive, false, false);
    }

    pub fn react(self: *Client, target: []const u8, msgid: []const u8, reaction: []const u8, remove: bool) !void {
        if (!validMessageReference(msgid)) return error.InvalidMessageReference;
        if (reaction.len > 256 or !std.unicode.utf8ValidateSlice(reaction)) return error.InvalidReaction;
        var tags: std.ArrayList(u8) = .empty;
        defer tags.deinit(self.gpa);
        try tags.appendSlice(self.gpa, "+reply=");
        try message.escapeTagValue(&tags, self.gpa, msgid);
        try tags.appendSlice(self.gpa, if (remove) ";+draft/unreact=" else ";+draft/react=");
        try message.escapeTagValue(&tags, self.gpa, reaction);
        try self.appendCommandWithTags("TAGMSG", &.{target}, tags.items);
        try self.queueOut(.interactive, false, false);
    }

    /// Send a privacy-sensitive typing indicator. Callers opt in explicitly;
    /// the per-target three-second throttle is required by the pinned spec.
    pub fn typing(self: *Client, target: []const u8, status: TypingStatus) !void {
        if (target.len == 0 or std.mem.indexOfAny(u8, target, " \r\n\x00") != null)
            return error.InvalidIrcParameter;
        for (self.typing_targets.items) |*entry| {
            if (!std.ascii.eqlIgnoreCase(entry.target, target)) continue;
            if (self.policy_now_ms -| entry.last_ms < 3000) return error.TypingRateLimited;
            entry.last_ms = self.policy_now_ms;
            return self.sendTyping(target, status);
        }
        if (self.typing_targets.items.len >= 256) {
            const oldest = self.typing_targets.orderedRemove(0);
            self.gpa.free(oldest.target);
        }
        const owned = try self.gpa.dupe(u8, target);
        errdefer self.gpa.free(owned);
        try self.typing_targets.append(self.gpa, .{ .target = owned, .last_ms = self.policy_now_ms });
        return self.sendTyping(target, status);
    }

    pub fn announceAvatar(self: *Client, target: []const u8, avatar: []const u8) !void {
        if (avatar.len == 0 or std.mem.indexOfAny(u8, avatar, " .\r\n") != null)
            return error.InvalidIrcParameter;
        var text: std.ArrayList(u8) = .empty;
        defer text.deinit(self.gpa);
        try text.appendSlice(self.gpa, "# Appears as ");
        try text.appendSlice(self.gpa, avatar);
        return self.privmsg(target, text.items);
    }

    pub fn comicData(self: *Client, target: []const u8, annotation: []const u8) !void {
        try self.appendCommand("DATA", &.{ target, "CCUDI1", annotation });
        try self.queueOut(.interactive, false, false);
    }

    /// `# GetInfo` (`ChatGetInfo`, protsupp.cpp:3415-3422): request the
    /// target's profile text.
    pub fn requestProfile(self: *Client, target: []const u8) !void {
        return self.privmsg(target, "# GetInfo");
    }

    /// `# HeresInfo: <profile>` (protsupp.cpp:919-920): reply to a profile
    /// request.
    pub fn sendProfile(self: *Client, target: []const u8, profile: []const u8) !void {
        var text: std.ArrayList(u8) = .empty;
        defer text.deinit(self.gpa);
        try text.appendSlice(self.gpa, "# HeresInfo: ");
        try text.appendSlice(self.gpa, profile);
        return self.privmsg(target, text.items);
    }

    /// `# GetCharInfo` (`ChatGetAvatarInfo`, protsupp.cpp:3424-3430): ask the
    /// target to (re)announce its avatar name/URL. This port has no avatar
    /// file-transfer path (`filesend.cpp`), so a reply only ever updates the
    /// name via `announceAvatar`/`parseAvatarAnnouncement`.
    pub fn requestAvatarInfo(self: *Client, target: []const u8) !void {
        return self.privmsg(target, "# GetCharInfo");
    }

    /// `ChatSyncBackDrop` (protsupp.cpp:3432-3453): announce a channel
    /// backdrop change, in both the modern and legacy-compat wire forms so
    /// either kind of receiver picks it up. `name` keeps its extension
    /// (e.g. "cave.bmp"); `url` may be omitted.
    pub fn syncBackdrop(self: *Client, target: []const u8, name: []const u8, url: ?[]const u8) !void {
        if (name.len == 0) return error.InvalidIrcParameter;

        var modern: std.ArrayList(u8) = .empty;
        defer modern.deinit(self.gpa);
        try modern.appendSlice(self.gpa, "# BDrop2: ");
        try modern.appendSlice(self.gpa, name);
        try modern.append(self.gpa, ',');
        if (url) |u| try modern.appendSlice(self.gpa, u);
        try self.privmsg(target, modern.items);

        const dot = std.mem.indexOfScalar(u8, name, '.');
        const base_name = if (dot) |i| name[0..i] else name;
        var legacy: std.ArrayList(u8) = .empty;
        defer legacy.deinit(self.gpa);
        // BACKGRNDPREFIX already ends in a space, and protsupp.cpp:3447's
        // format string ("#%s %s") adds one more, so a real client emits a
        // double space here. Preserved verbatim: the receiver skips all
        // whitespace after the prefix (protsupp.cpp:975-976).
        try legacy.appendSlice(self.gpa, "# BDrop:  ");
        try legacy.appendSlice(self.gpa, base_name);
        try self.privmsg(target, legacy.items);
    }

    pub fn accountRegister(
        self: *Client,
        account_or_star: []const u8,
        email_or_star: []const u8,
        password: []u8,
    ) !void {
        defer std.crypto.secureZero(u8, password);
        if (self.connect_options.security != .tls) return error.AccountRegistrationRequiresTls;
        if (!self.capabilityEnabled("draft/account-registration")) return error.AccountRegistrationNotEnabled;
        try self.appendCommand("REGISTER", &.{ account_or_star, email_or_star, password });
        try self.queueOut(.interactive, false, true);
    }

    pub fn accountVerify(self: *Client, account_or_star: []const u8, code: []u8) !void {
        defer std.crypto.secureZero(u8, code);
        if (self.connect_options.security != .tls) return error.AccountRegistrationRequiresTls;
        if (!self.capabilityEnabled("draft/account-registration")) return error.AccountRegistrationNotEnabled;
        try self.appendCommand("VERIFY", &.{ account_or_star, code });
        try self.queueOut(.interactive, false, true);
    }

    pub fn capabilityEnabled(self: *const Client, name: []const u8) bool {
        if (self.registration) |*registration| return registration.cap.enabled.contains(name);
        return false;
    }

    pub fn refreshCapabilities(self: *Client) !void {
        const registration = if (self.registration) |*value| value else return error.RegistrationNotStarted;
        try registration.cap.requestList(&self.out);
        try self.queueOut(.control, false, false);
    }

    pub fn featureState(self: *const Client) ?*const features_mod.State {
        if (self.features) |*state| return state;
        return null;
    }

    pub fn takeStsUpgradePort(self: *Client) ?u16 {
        const port = self.sts_upgrade_port;
        self.sts_upgrade_port = null;
        return port;
    }

    fn appendCommand(self: *Client, command: []const u8, params: []const []const u8) !void {
        return self.appendCommandWithTags(command, params, null);
    }

    fn appendCommandWithTags(self: *Client, command: []const u8, params: []const []const u8, client_tags: ?[]const u8) !void {
        if (client_tags != null and !self.capabilityEnabled("message-tags")) return error.MessageTagsNotEnabled;
        var msg = Message{ .command = command };
        if (params.len > message.max_params) return error.InvalidIrcParameter;
        for (params, 0..) |param, index| msg.params[index] = param;
        msg.param_count = params.len;
        var tags: std.ArrayList(u8) = .empty;
        defer tags.deinit(self.gpa);
        if (client_tags) |raw| try tags.appendSlice(self.gpa, raw);
        if (self.capabilityEnabled("labeled-response")) if (self.features) |*state| {
            const label = try state.createLabel();
            if (tags.items.len != 0) try tags.append(self.gpa, ';');
            try tags.print(self.gpa, "label={s}", .{label});
        };
        if (tags.items.len != 0) msg.tag_data = tags.items;
        try message.write(&self.out, self.gpa, msg);
    }

    fn sendTyping(self: *Client, target: []const u8, status: TypingStatus) !void {
        var buffer: [32]u8 = undefined;
        const tags = try std.fmt.bufPrint(&buffer, "+typing={s}", .{@tagName(status)});
        try self.appendCommandWithTags("TAGMSG", &.{target}, tags);
        try self.queueOut(.interactive, false, false);
    }

    fn validateOutgoingText(self: *const Client, text: []const u8) !void {
        if (self.features) |*state| {
            if (state.isupport("UTF8ONLY") != null and !std.unicode.utf8ValidateSlice(text))
                return error.InvalidUtf8;
        }
    }

    /// Native socket handle for a platform poll/select loop. The client keeps
    /// ownership; callers use this only to wait for readability.
    pub fn fd(self: *const Client) i32 {
        return self.transport.fd();
    }

    /// Read one available chunk into the line framer. Event-driven callers
    /// invoke this only after their poller reports `fd()` readable. Returns
    /// false when the peer closed the connection.
    pub fn receive(self: *Client) !bool {
        const n = try self.transport.recv(&self.rx);
        if (n == 0) return false;
        try self.framer.push(self.rx[0..n]);
        return true;
    }

    /// Null means the timeout expired, false means EOF, and true means bytes
    /// were added to the line framer.
    pub fn receiveTimeout(self: *Client, milliseconds: i64) !?bool {
        const maybe_n = try self.transport.recvTimeout(&self.rx, milliseconds);
        const n = maybe_n orelse return null;
        if (n == 0) return false;
        try self.framer.push(self.rx[0..n]);
        return true;
    }

    /// Parse one already-buffered message without reading the socket. PING is
    /// answered here so both blocking and event-driven users get keepalives.
    pub fn bufferedNext(self: *Client) !?Message {
        if (self.logical_line) |line| {
            self.gpa.free(line);
            self.logical_line = null;
        }

        while (self.aggregator.takeReady()) |line| {
            self.logical_line = line;
            const msg = message.parse(line);
            if (self.features) |*state| {
                if (try state.observe(&msg)) {
                    self.gpa.free(line);
                    self.logical_line = null;
                    continue;
                }
            }
            return msg;
        }

        while (true) {
            const line = (try self.framer.next()) orelse return null;
            const msg = message.parse(line);
            self.deadlines.observeRx(self.policy_now_ms);
            if (std.mem.eql(u8, msg.command, "001")) self.deadlines.markRegistered();

            if (try autoRespond(&self.out, self.gpa, msg)) {
                try self.queueOut(.control, false, false);
                continue;
            }

            if (self.registration) |*registration| {
                if (try registration.consume(&self.out, &msg, &self.sts_upgrade_port)) {
                    // AUTHENTICATE output may contain reversible credentials.
                    // Always wipe the retained allocation after control sends.
                    try self.queueOut(.control, false, true);
                    continue;
                }
            }

            if (try self.aggregator.ingest(line)) {
                while (self.aggregator.takeCompletedLabel()) |label| {
                    defer self.gpa.free(label);
                    if (self.features) |*state| _ = try state.completeLabel(label);
                }
                while (self.aggregator.takeReady()) |ready| {
                    self.logical_line = ready;
                    const logical = message.parse(ready);
                    if (self.features) |*state| {
                        if (try state.observe(&logical)) {
                            self.gpa.free(ready);
                            self.logical_line = null;
                            continue;
                        }
                    }
                    return logical;
                }
                continue;
            }

            if (self.features) |*state| if (try state.observe(&msg)) continue;
            return msg;
        }
    }

    /// Return the next protocol message, reading from the socket as needed.
    /// PING is answered automatically. Returns null at end of stream. The
    /// returned Message borrows from internal storage and is valid until the
    /// next call to `next`.
    pub fn next(self: *Client) !?Message {
        while (true) {
            if (try self.bufferedNext()) |msg| return msg;
            if (!try self.receive()) return null;
        }
    }
};

const TypingTarget = struct { target: []u8, last_ms: u64 };

fn validMessageReference(reference: []const u8) bool {
    return reference.len != 0 and reference[0] != ':' and
        std.mem.indexOfAny(u8, reference, " \r\n\x00") == null;
}

// --- Tests ----------------------------------------------------------------

test "autoRespond answers PING with matching PONG" {
    const gpa = std.testing.allocator;
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);

    const did = try autoRespond(&out, gpa, message.parse("PING :abc123"));
    try std.testing.expect(did);
    try std.testing.expectEqualStrings("PONG abc123\r\n", out.items);
}

test "autoRespond ignores non-PING" {
    const gpa = std.testing.allocator;
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);

    const did = try autoRespond(&out, gpa, message.parse(":srv PRIVMSG me :hi"));
    try std.testing.expect(!did);
    try std.testing.expectEqual(@as(usize, 0), out.items.len);
}

test "registration advances CAP asynchronously and starts IRCX only after CAP END" {
    const gpa = std.testing.allocator;
    var registration = Registration.init(gpa, "irc.example", .tls, .{});
    defer registration.deinit();
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);
    var upgrade: ?u16 = null;

    try registration.cap.begin(&out);
    out.clearRetainingCapacity();
    var ls = message.parse(":irc CAP * LS :message-tags echo-message");
    try std.testing.expect(try registration.consume(&out, &ls, &upgrade));
    try std.testing.expect(std.mem.startsWith(u8, out.items, "CAP REQ :"));
    try std.testing.expect(std.mem.indexOf(u8, out.items, "IRCX") == null);

    out.clearRetainingCapacity();
    var ack = message.parse(":irc CAP * ACK :echo-message message-tags");
    try std.testing.expect(try registration.consume(&out, &ack, &upgrade));
    try std.testing.expectEqualStrings("CAP END\r\nIRCX\r\n", out.items);
    try std.testing.expect(registration.done);
}

test "registration drives PLAIN SASL and zeroizes caller credentials" {
    const gpa = std.testing.allocator;
    var authzid = [_]u8{};
    var authcid = [_]u8{ 'a', 'l', 'i', 'c', 'e' };
    var password = [_]u8{ 's', 'e', 'c', 'r', 'e', 't' };
    var credentials = sasl.Credentials{
        .authorization_identity = &authzid,
        .authentication_identity = &authcid,
        .password = &password,
    };
    const preference = [_]sasl.Mechanism{.plain};
    var registration = Registration.init(gpa, "irc.example", .tls, .{
        .credentials = &credentials,
        .sasl_preference = &preference,
        .io = std.testing.io,
    });
    defer registration.deinit();
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);
    defer sasl.secureClear(&out);
    var upgrade: ?u16 = null;

    try registration.cap.begin(&out);
    sasl.secureClear(&out);
    var ls = message.parse(":irc CAP * LS :sasl=PLAIN");
    _ = try registration.consume(&out, &ls, &upgrade);
    sasl.secureClear(&out);
    var ack = message.parse(":irc CAP * ACK :sasl");
    _ = try registration.consume(&out, &ack, &upgrade);
    try std.testing.expectEqualStrings("AUTHENTICATE PLAIN\r\n", out.items);

    sasl.secureClear(&out);
    var challenge = message.parse("AUTHENTICATE +");
    _ = try registration.consume(&out, &challenge, &upgrade);
    try std.testing.expect(std.mem.startsWith(u8, out.items, "AUTHENTICATE "));

    sasl.secureClear(&out);
    var success = message.parse(":irc 903 alice :SASL authentication successful");
    _ = try registration.consume(&out, &success, &upgrade);
    try std.testing.expectEqualStrings("CAP END\r\nIRCX\r\n", out.items);
    try std.testing.expect(credentials.zeroized);
    try std.testing.expectEqualSlices(u8, &@as([6]u8, @splat(0)), &password);
}

test "plaintext STS advertisement fails closed with the upgrade port" {
    const gpa = std.testing.allocator;
    var registration = Registration.init(gpa, "irc.example", .plaintext, .{});
    defer registration.deinit();
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);
    var upgrade: ?u16 = null;

    try registration.cap.begin(&out);
    out.clearRetainingCapacity();
    var ls = message.parse(":irc CAP * LS :sts=duration=3600,port=6697");
    try std.testing.expectError(error.StsUpgradeRequired, registration.consume(&out, &ls, &upgrade));
    try std.testing.expectEqual(@as(?u16, 6697), upgrade);
}

test "unsupported advertised SASL mechanisms complete CAP without blocking registration" {
    const gpa = std.testing.allocator;
    var authzid = [_]u8{};
    var authcid = [_]u8{ 'm', 'e' };
    var password = [_]u8{ 'p', 'w' };
    var credentials = sasl.Credentials{
        .authorization_identity = &authzid,
        .authentication_identity = &authcid,
        .password = &password,
    };
    const only_external = [_]sasl.Mechanism{.external};
    var registration = Registration.init(gpa, "irc.example", .tls, .{
        .credentials = &credentials,
        .sasl_preference = &only_external,
        .io = std.testing.io,
    });
    defer registration.deinit();
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);
    var upgrade: ?u16 = null;
    try registration.cap.begin(&out);
    out.clearRetainingCapacity();
    var ls = message.parse(":irc CAP * LS :sasl=EXTERNAL");
    _ = try registration.consume(&out, &ls, &upgrade);
    out.clearRetainingCapacity();
    var ack = message.parse(":irc CAP * ACK :sasl");
    _ = try registration.consume(&out, &ack, &upgrade);
    try std.testing.expectEqualStrings("CAP END\r\nIRCX\r\n", out.items);
    try std.testing.expect(registration.done);
    try std.testing.expect(credentials.zeroized);
}

test "reply reaction and typing commands are bounded tagged client messages" {
    const gpa = std.testing.allocator;
    const owned_host = try gpa.dupe(u8, "irc.example");
    var client = Client{
        .gpa = gpa,
        .transport = undefined,
        .host = owned_host,
        .port = 6697,
        .connect_options = .{},
        .framer = irc.LineFramer.init(gpa),
        // A zero burst keeps commands queued, avoiding a live transport in
        // this deterministic command-generation test.
        .tx = policy.TxQueue.init(gpa, .{}, 0, 1, 0),
        .deadlines = policy.Deadlines.init(0, .{}),
        .aggregator = features_mod.Aggregator.init(gpa, .{}),
        .policy_now_ms = 1000,
    };
    client.registration = Registration.init(gpa, owned_host, .tls, .{});
    client.features = try features_mod.State.init(gpa, "me", .{});
    try client.registration.?.cap.begin(&client.out);
    client.out.clearRetainingCapacity();
    _ = try client.registration.?.cap.handle(&client.out, message.parse(":irc CAP * LS :batch draft/account-registration labeled-response message-tags"));
    client.out.clearRetainingCapacity();
    _ = try client.registration.?.cap.handle(&client.out, message.parse(":irc CAP * ACK :batch draft/account-registration labeled-response message-tags"));
    client.out.clearRetainingCapacity();
    defer {
        client.registration.?.deinit();
        client.features.?.deinit();
        client.aggregator.deinit();
        client.tx.deinit();
        for (client.typing_targets.items) |entry| gpa.free(entry.target);
        client.typing_targets.deinit(gpa);
        client.framer.deinit();
        sasl.secureClear(&client.out);
        client.out.deinit(gpa);
        gpa.free(owned_host);
    }

    try client.reply("#c", "msg-1", "reply text");
    try std.testing.expect(std.mem.indexOf(u8, client.tx.items.items[0].bytes, "+reply=msg-1") != null);
    try std.testing.expect(std.mem.indexOf(u8, client.tx.items.items[0].bytes, "label=cc-1") != null);
    try client.react("#c", "msg-1", "wave", false);
    try std.testing.expect(std.mem.indexOf(u8, client.tx.items.items[1].bytes, "+draft/react=wave") != null);
    try client.typing("#c", .active);
    try std.testing.expectError(error.TypingRateLimited, client.typing("#c", .paused));
    client.policy_now_ms = 4000;
    try client.typing("#c", .done);
    try std.testing.expect(std.mem.indexOf(u8, client.tx.items.items[3].bytes, "+typing=done") != null);

    _ = try client.features.?.observe(&message.parse(":irc 005 me UTF8ONLY :are supported"));
    try std.testing.expectError(error.InvalidUtf8, client.privmsg("#c", &.{0xff}));
    try std.testing.expectError(error.InvalidMessageReference, client.reply("#c", "bad id", "text"));

    var password = [_]u8{ 'p', 'w' };
    try client.accountRegister("*", "user@example.test", &password);
    try std.testing.expectEqualSlices(u8, &.{ 0, 0 }, &password);
    try std.testing.expect(client.tx.items.items[4].sensitive);
    try std.testing.expect(std.mem.indexOf(u8, client.tx.items.items[4].bytes, "REGISTER * user@example.test pw") != null);
}
