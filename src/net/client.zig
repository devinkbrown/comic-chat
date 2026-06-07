//! High-level IRC client: ties the transport, line framer, and command
//! builders together, with automatic PING/PONG keepalive.
//!
//! The message-handling decision (`autoRespond`) is a pure function tested
//! without a socket; `Client` is the live glue over `Transport`.

const std = @import("std");
const irc = @import("irc.zig");
const message = @import("message.zig");
const Transport = @import("transport.zig").Transport;

pub const Message = message.Message;

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

pub const Client = struct {
    gpa: std.mem.Allocator,
    transport: *Transport,
    framer: irc.LineFramer,
    out: std.ArrayList(u8) = .empty,
    rx: [8192]u8 = undefined,

    pub fn connect(gpa: std.mem.Allocator, host: []const u8, port: u16) !Client {
        return .{
            .gpa = gpa,
            .transport = try Transport.connect(gpa, host, port),
            .framer = irc.LineFramer.init(gpa),
        };
    }

    pub fn deinit(self: *Client) void {
        self.framer.deinit();
        self.out.deinit(self.gpa);
        self.transport.deinit();
    }

    fn flushOut(self: *Client) !void {
        try self.transport.send(self.out.items);
        self.out.clearRetainingCapacity();
    }

    pub fn register(
        self: *Client,
        nick: []const u8,
        user: []const u8,
        realname: []const u8,
        want_ircx: bool,
    ) !void {
        try irc.writeRegister(&self.out, self.gpa, nick, user, realname, want_ircx);
        try self.flushOut();
    }

    pub fn join(self: *Client, channel: []const u8) !void {
        try irc.writeJoin(&self.out, self.gpa, channel);
        try self.flushOut();
    }

    pub fn privmsg(self: *Client, target: []const u8, text: []const u8) !void {
        try irc.writePrivmsg(&self.out, self.gpa, target, text);
        try self.flushOut();
    }

    /// Return the next protocol message, reading from the socket as needed.
    /// PING is answered automatically. Returns null at end of stream. The
    /// returned Message borrows from internal storage and is valid until the
    /// next call to `next`.
    pub fn next(self: *Client) !?Message {
        while (true) {
            if (try self.framer.next()) |line| {
                const msg = message.parse(line);
                if (try autoRespond(&self.out, self.gpa, msg)) try self.flushOut();
                return msg;
            }
            const n = try self.transport.recv(&self.rx);
            if (n == 0) return null;
            try self.framer.push(self.rx[0..n]);
        }
    }
};

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
