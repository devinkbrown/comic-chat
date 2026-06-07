//! IRC client protocol engine (transport-agnostic, pure Zig).
//!
//! This layer knows how to *frame* the byte stream into lines and how to
//! *build* the commands Comic Chat uses. The actual socket pump (std.Io.net)
//! is a thin wrapper added on top — everything here is testable without a
//! network, which is how the original CIrcProto logic is exercised.

const std = @import("std");
pub const message = @import("message.zig");
pub const Message = message.Message;

// --- Line framing ---------------------------------------------------------

/// Accumulates raw bytes from a socket and yields complete lines (CRLF or LF
/// terminated). The slice returned by `next` is valid until the following
/// call to `next` or `push`.
pub const LineFramer = struct {
    gpa: std.mem.Allocator,
    buf: std.ArrayList(u8) = .empty,
    scratch: std.ArrayList(u8) = .empty,
    cursor: usize = 0,

    pub fn init(gpa: std.mem.Allocator) LineFramer {
        return .{ .gpa = gpa };
    }

    pub fn deinit(self: *LineFramer) void {
        self.buf.deinit(self.gpa);
        self.scratch.deinit(self.gpa);
    }

    /// Feed bytes received from the transport.
    pub fn push(self: *LineFramer, data: []const u8) !void {
        try self.buf.appendSlice(self.gpa, data);
    }

    /// Return the next complete line (CR/LF stripped), or null if none is
    /// buffered yet. Empty lines are returned as zero-length slices.
    pub fn next(self: *LineFramer) !?[]const u8 {
        const data = self.buf.items[self.cursor..];
        const nl = std.mem.indexOfScalar(u8, data, '\n') orelse return null;
        const line = std.mem.trimEnd(u8, data[0..nl], "\r");

        self.scratch.clearRetainingCapacity();
        try self.scratch.appendSlice(self.gpa, line);
        self.cursor += nl + 1;

        if (self.cursor >= self.buf.items.len) {
            self.buf.clearRetainingCapacity();
            self.cursor = 0;
        }
        return self.scratch.items;
    }
};

// --- Command builders ------------------------------------------------------
// Each appends a single CRLF-terminated command to `out`.

pub fn writeNick(out: *std.ArrayList(u8), gpa: std.mem.Allocator, nick: []const u8) !void {
    var m = Message{ .command = "NICK" };
    m.params[0] = nick;
    m.param_count = 1;
    try message.write(out, gpa, m);
}

pub fn writeUser(
    out: *std.ArrayList(u8),
    gpa: std.mem.Allocator,
    user: []const u8,
    realname: []const u8,
) !void {
    // USER <user> <mode> * :<realname>   (mode 0, unused host field '*')
    var m = Message{ .command = "USER" };
    m.params[0] = user;
    m.params[1] = "0";
    m.params[2] = "*";
    m.params[3] = realname;
    m.param_count = 4;
    try message.write(out, gpa, m);
}

pub fn writeJoin(out: *std.ArrayList(u8), gpa: std.mem.Allocator, channel: []const u8) !void {
    var m = Message{ .command = "JOIN" };
    m.params[0] = channel;
    m.param_count = 1;
    try message.write(out, gpa, m);
}

pub fn writePrivmsg(
    out: *std.ArrayList(u8),
    gpa: std.mem.Allocator,
    target: []const u8,
    text: []const u8,
) !void {
    var m = Message{ .command = "PRIVMSG" };
    m.params[0] = target;
    m.params[1] = text;
    m.param_count = 2;
    try message.write(out, gpa, m);
}

pub fn writePong(out: *std.ArrayList(u8), gpa: std.mem.Allocator, token: []const u8) !void {
    var m = Message{ .command = "PONG" };
    m.params[0] = token;
    m.param_count = 1;
    try message.write(out, gpa, m);
}

/// Negotiate Microsoft IRCX extensions. The original Comic Chat sent
/// "MODE ISIRCX", but modern IRCX servers (e.g. ophion) use the standalone
/// `IRCX` command and reply with numeric 800 (IRCX state). Verified against
/// ophion, which advertises the IRCX and COMICCHAT=DATA tokens in 005.
pub fn writeIrcx(out: *std.ArrayList(u8), gpa: std.mem.Allocator) !void {
    try message.write(out, gpa, .{ .command = "IRCX" });
}

/// Full registration handshake: NICK + USER (+ optional IRCX probe).
pub fn writeRegister(
    out: *std.ArrayList(u8),
    gpa: std.mem.Allocator,
    nick: []const u8,
    user: []const u8,
    realname: []const u8,
    want_ircx: bool,
) !void {
    try writeNick(out, gpa, nick);
    try writeUser(out, gpa, user, realname);
    if (want_ircx) try writeIrcx(out, gpa);
}

// --- Tests ----------------------------------------------------------------

test "LineFramer reassembles split and batched lines" {
    const gpa = std.testing.allocator;
    var fr = LineFramer.init(gpa);
    defer fr.deinit();

    // A line arriving in two chunks.
    try fr.push("PING :ser");
    try std.testing.expect((try fr.next()) == null);
    try fr.push("ver1\r\n");
    try std.testing.expectEqualStrings("PING :server1", (try fr.next()).?);
    try std.testing.expect((try fr.next()) == null);

    // Two lines in one chunk, LF-only and CRLF mixed.
    try fr.push("NICK bob\nJOIN #comics\r\n");
    try std.testing.expectEqualStrings("NICK bob", (try fr.next()).?);
    try std.testing.expectEqualStrings("JOIN #comics", (try fr.next()).?);
    try std.testing.expect((try fr.next()) == null);
}

test "register handshake emits NICK, USER and IRCX" {
    const gpa = std.testing.allocator;
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);

    try writeRegister(&out, gpa, "anna", "anna", "Anna Example", true);
    try std.testing.expectEqualStrings(
        "NICK anna\r\n" ++
            "USER anna 0 * :Anna Example\r\n" ++
            "IRCX\r\n",
        out.items,
    );
}

test "privmsg builds trailing form" {
    const gpa = std.testing.allocator;
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);

    try writePrivmsg(&out, gpa, "#comics", "hi there");
    try std.testing.expectEqualStrings("PRIVMSG #comics :hi there\r\n", out.items);
}

test "pong echoes the ping token" {
    const gpa = std.testing.allocator;
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);

    const ping = message.parse("PING :server1");
    try writePong(&out, gpa, ping.param(0).?);
    try std.testing.expectEqualStrings("PONG server1\r\n", out.items);
}


test "LineFramer: empty lines are returned as zero-length slices" {
    const gpa = std.testing.allocator;
    var fr = LineFramer.init(gpa);
    defer fr.deinit();
    try fr.push("\r\n\r\nPING x\r\n");
    try std.testing.expectEqualStrings("", (try fr.next()).?);
    try std.testing.expectEqualStrings("", (try fr.next()).?);
    try std.testing.expectEqualStrings("PING x", (try fr.next()).?);
    try std.testing.expect((try fr.next()) == null);
}

test "LineFramer: a line spanning three pushes reassembles correctly" {
    const gpa = std.testing.allocator;
    var fr = LineFramer.init(gpa);
    defer fr.deinit();
    try fr.push(":nick!u@h PRIV");
    try std.testing.expect((try fr.next()) == null);
    try fr.push("MSG #c :hel");
    try std.testing.expect((try fr.next()) == null);
    try fr.push("lo\n");
    try std.testing.expectEqualStrings(":nick!u@h PRIVMSG #c :hello", (try fr.next()).?);
}

test "LineFramer: trailing partial line stays buffered until terminated" {
    const gpa = std.testing.allocator;
    var fr = LineFramer.init(gpa);
    defer fr.deinit();
    try fr.push("DONE\nPART");
    try std.testing.expectEqualStrings("DONE", (try fr.next()).?);
    try std.testing.expect((try fr.next()) == null); // "PART" has no newline yet
    try fr.push("IAL\n");
    try std.testing.expectEqualStrings("PARTIAL", (try fr.next()).?);
}

test "writeRegister without IRCX omits the IRCX probe" {
    const gpa = std.testing.allocator;
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);
    try writeRegister(&out, gpa, "bob", "bob", "Bob B", false);
    try std.testing.expectEqualStrings(
        "NICK bob\r\n" ++ "USER bob 0 * :Bob B\r\n",
        out.items,
    );
}

test "command builders emit individual CRLF-terminated commands" {
    const gpa = std.testing.allocator;
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);
    try writeNick(&out, gpa, "anna");
    try writeJoin(&out, gpa, "#comics");
    try writePong(&out, gpa, "tok123");
    try std.testing.expectEqualStrings(
        "NICK anna\r\n" ++ "JOIN #comics\r\n" ++ "PONG tok123\r\n",
        out.items,
    );
}

test "writePrivmsg keeps a single-word message as a middle param" {
    const gpa = std.testing.allocator;
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);
    try writePrivmsg(&out, gpa, "#c", "hi");
    // "hi" has no space, but it is the final param -> writer leaves it bare.
    try std.testing.expectEqualStrings("PRIVMSG #c hi\r\n", out.items);
}

test "round-trip: framed line parses back into a Message" {
    const gpa = std.testing.allocator;
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);
    try writePrivmsg(&out, gpa, "#comics", "hello there");

    var fr = LineFramer.init(gpa);
    defer fr.deinit();
    try fr.push(out.items);
    const line = (try fr.next()).?;
    const m = message.parse(line);
    try std.testing.expectEqualStrings("PRIVMSG", m.command);
    try std.testing.expectEqualStrings("#comics", m.param(0).?);
    try std.testing.expectEqualStrings("hello there", m.param(1).?);
}
