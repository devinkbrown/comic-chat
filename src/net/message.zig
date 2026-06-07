//! RFC1459 IRC message parsing and formatting (pure, transport-agnostic).
//!
//! Grammar:  [':' <prefix> SPACE] <command> { SPACE <param> } [SPACE ':' <trailing>]
//!
//! `parse` borrows from the source line (zero-copy); the source must outlive
//! the returned Message.

const std = @import("std");

pub const max_params = 15;

pub const Message = struct {
    /// Sender prefix without the leading ':' (servername or nick!user@host).
    prefix: ?[]const u8 = null,
    /// Command word or 3-digit numeric reply.
    command: []const u8 = "",
    params: [max_params][]const u8 = undefined,
    param_count: usize = 0,

    pub fn param(self: *const Message, i: usize) ?[]const u8 {
        if (i >= self.param_count) return null;
        return self.params[i];
    }

    fn addParam(self: *Message, p: []const u8) void {
        if (self.param_count >= max_params) return;
        self.params[self.param_count] = p;
        self.param_count += 1;
    }
};

/// Consume a leading run of spaces.
fn skipSpaces(s: []const u8) []const u8 {
    return std.mem.trimStart(u8, s, " ");
}

pub fn parse(line: []const u8) Message {
    var rest = std.mem.trimEnd(u8, line, "\r\n");
    var msg = Message{};

    if (rest.len > 0 and rest[0] == ':') {
        const sp = std.mem.indexOfScalar(u8, rest, ' ') orelse rest.len;
        msg.prefix = rest[1..sp];
        rest = if (sp < rest.len) skipSpaces(rest[sp..]) else rest[rest.len..];
    }

    {
        const sp = std.mem.indexOfScalar(u8, rest, ' ') orelse rest.len;
        msg.command = rest[0..sp];
        rest = if (sp < rest.len) skipSpaces(rest[sp..]) else rest[rest.len..];
    }

    while (rest.len > 0) {
        if (rest[0] == ':') {
            msg.addParam(rest[1..]);
            break;
        }
        const sp = std.mem.indexOfScalar(u8, rest, ' ') orelse rest.len;
        msg.addParam(rest[0..sp]);
        rest = if (sp < rest.len) skipSpaces(rest[sp..]) else rest[rest.len..];
    }

    return msg;
}

/// A parameter must be sent as a `:trailing` if it is the last one and is
/// empty, contains a space, or begins with ':'.
fn needsTrailing(p: []const u8) bool {
    return p.len == 0 or p[0] == ':' or std.mem.indexOfScalar(u8, p, ' ') != null;
}

/// Append the wire form (with CRLF) to `out`.
pub fn write(
    out: *std.ArrayList(u8),
    gpa: std.mem.Allocator,
    msg: Message,
) !void {
    if (msg.prefix) |p| {
        try out.append(gpa, ':');
        try out.appendSlice(gpa, p);
        try out.append(gpa, ' ');
    }
    try out.appendSlice(gpa, msg.command);
    var i: usize = 0;
    while (i < msg.param_count) : (i += 1) {
        const p = msg.params[i];
        try out.append(gpa, ' ');
        // Only the final parameter may use the ':trailing' form.
        if (i + 1 == msg.param_count and needsTrailing(p)) {
            try out.append(gpa, ':');
        }
        try out.appendSlice(gpa, p);
    }
    try out.appendSlice(gpa, "\r\n");
}

// --- Tests ----------------------------------------------------------------

test "parse: full message with prefix and trailing" {
    const m = parse(":nick!user@host PRIVMSG #comics :hello world\r\n");
    try std.testing.expectEqualStrings("nick!user@host", m.prefix.?);
    try std.testing.expectEqualStrings("PRIVMSG", m.command);
    try std.testing.expectEqual(@as(usize, 2), m.param_count);
    try std.testing.expectEqualStrings("#comics", m.param(0).?);
    try std.testing.expectEqualStrings("hello world", m.param(1).?);
}

test "parse: numeric reply, no prefix-less trailing colon issues" {
    const m = parse("PING :server1");
    try std.testing.expect(m.prefix == null);
    try std.testing.expectEqualStrings("PING", m.command);
    try std.testing.expectEqualStrings("server1", m.param(0).?);
}

test "parse: multiple middle params" {
    const m = parse(":srv 005 nick PREFIX=(ov)@+ CHANTYPES=# :are supported");
    try std.testing.expectEqualStrings("005", m.command);
    try std.testing.expectEqualStrings("nick", m.param(0).?);
    try std.testing.expectEqualStrings("PREFIX=(ov)@+", m.param(1).?);
    try std.testing.expectEqualStrings("CHANTYPES=#", m.param(2).?);
    try std.testing.expectEqualStrings("are supported", m.param(3).?);
}

test "write: chooses trailing form only when needed" {
    const gpa = std.testing.allocator;
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);

    var m = Message{ .command = "PRIVMSG" };
    m.addParam("#comics");
    m.addParam("hello world"); // has space -> trailing
    try write(&out, gpa, m);
    try std.testing.expectEqualStrings("PRIVMSG #comics :hello world\r\n", out.items);
}

test "write/parse round-trip" {
    const gpa = std.testing.allocator;
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);

    var m = Message{ .prefix = "srv", .command = "NOTICE" };
    m.addParam("bob");
    m.addParam("watch out");
    try write(&out, gpa, m);

    const back = parse(out.items);
    try std.testing.expectEqualStrings("srv", back.prefix.?);
    try std.testing.expectEqualStrings("NOTICE", back.command);
    try std.testing.expectEqualStrings("bob", back.param(0).?);
    try std.testing.expectEqualStrings("watch out", back.param(1).?);
}


test "parse: command-only line, no prefix, no params" {
    const m = parse("QUIT");
    try std.testing.expect(m.prefix == null);
    try std.testing.expectEqualStrings("QUIT", m.command);
    try std.testing.expectEqual(@as(usize, 0), m.param_count);
    try std.testing.expect(m.param(0) == null);
}

test "parse: tolerates extra interior spaces" {
    const m = parse(":srv   PRIVMSG    #c    :hi");
    try std.testing.expectEqualStrings("srv", m.prefix.?);
    try std.testing.expectEqualStrings("PRIVMSG", m.command);
    try std.testing.expectEqualStrings("#c", m.param(0).?);
    try std.testing.expectEqualStrings("hi", m.param(1).?);
}

test "parse: empty trailing parameter is preserved" {
    const m = parse("PRIVMSG #c :");
    try std.testing.expectEqual(@as(usize, 2), m.param_count);
    try std.testing.expectEqualStrings("#c", m.param(0).?);
    try std.testing.expectEqualStrings("", m.param(1).?);
}

test "parse: trailing keeps embedded colons and is not re-split on spaces" {
    const m = parse(":n!u@h PRIVMSG #c :a :b: c");
    try std.testing.expectEqualStrings("a :b: c", m.param(1).?);
    try std.testing.expectEqual(@as(usize, 2), m.param_count);
}

test "parse: empty and whitespace-only lines do not crash" {
    const e = parse("");
    try std.testing.expectEqualStrings("", e.command);
    try std.testing.expectEqual(@as(usize, 0), e.param_count);
    const ws = parse("   \r\n");
    try std.testing.expectEqualStrings("", ws.command);
}

test "parse: param overflow past max_params is dropped, not corrupted" {
    // 17 single-char middle params; only max_params (15) are retained.
    const line = "CMD a b c d e f g h i j k l m n o p q";
    const m = parse(line);
    try std.testing.expectEqual(@as(usize, max_params), m.param_count);
    try std.testing.expectEqualStrings("a", m.param(0).?);
    try std.testing.expectEqualStrings("o", m.param(max_params - 1).?);
}

test "write: middle param with a space stays a middle param (not trailing)" {
    const gpa = std.testing.allocator;
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);
    var m = Message{ .command = "CMD" };
    m.addParam("with space"); // not last -> cannot use trailing
    m.addParam("tail");
    // Only the final param may take ':'; a non-final spaced param would corrupt
    // the wire form, but the writer only colon-prefixes the LAST param.
    try write(&out, gpa, m);
    try std.testing.expectEqualStrings("CMD with space tail\r\n", out.items);
}

test "write: param starting with ':' forces trailing form when last" {
    const gpa = std.testing.allocator;
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);
    var m = Message{ .command = "TOPIC" };
    m.addParam("#c");
    m.addParam(":weird"); // starts with ':' -> needs trailing
    try write(&out, gpa, m);
    try std.testing.expectEqualStrings("TOPIC #c ::weird\r\n", out.items);
}

test "write: empty last param emitted as ':'" {
    const gpa = std.testing.allocator;
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);
    var m = Message{ .command = "AWAY" };
    m.addParam("");
    try write(&out, gpa, m);
    try std.testing.expectEqualStrings("AWAY :\r\n", out.items);
}
