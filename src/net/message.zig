//! Modern IRC / IRCv3 message parsing and formatting (transport-agnostic).
//!
//! Grammar: [`@` <tags> SPACE] [':' <prefix> SPACE] <command>
//!          { SPACE <param> } [SPACE ':' <trailing>]
//!
//! `parse` borrows from the source line (zero-copy); the source must outlive
//! the returned Message.

const std = @import("std");

pub const max_params = 15;

pub const Tag = struct {
    key: []const u8,
    /// Escaped wire value. Null distinguishes `@tag` from `@tag=`.
    raw_value: ?[]const u8 = null,
};

pub const TagIterator = struct {
    rest: []const u8,

    pub fn next(self: *TagIterator) ?Tag {
        while (self.rest.len > 0) {
            const separator = std.mem.indexOfScalar(u8, self.rest, ';') orelse self.rest.len;
            const field = self.rest[0..separator];
            self.rest = if (separator < self.rest.len) self.rest[separator + 1 ..] else self.rest[self.rest.len..];
            if (field.len == 0) continue;
            const equals = std.mem.indexOfScalar(u8, field, '=');
            return if (equals) |index|
                .{ .key = field[0..index], .raw_value = field[index + 1 ..] }
            else
                .{ .key = field };
        }
        return null;
    }
};

pub const Message = struct {
    /// Serialized tag data without the leading `@` or trailing space. Values
    /// remain escaped so parsing stays zero-copy; use `unescapeTagValue` when
    /// a decoded value is required.
    tag_data: ?[]const u8 = null,
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

    pub fn tags(self: *const Message) TagIterator {
        return .{ .rest = self.tag_data orelse "" };
    }

    pub fn tag(self: *const Message, key: []const u8) ?Tag {
        var iterator = self.tags();
        var result: ?Tag = null;
        while (iterator.next()) |candidate| {
            // Duplicate keys are malformed, but the IRCv3 robustness rule is
            // to retain only the final occurrence rather than reject a line.
            if (std.mem.eql(u8, candidate.key, key)) result = candidate;
        }
        return result;
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

    if (rest.len > 0 and rest[0] == '@') {
        const sp = std.mem.indexOfScalar(u8, rest, ' ') orelse rest.len;
        msg.tag_data = rest[1..sp];
        rest = if (sp < rest.len) skipSpaces(rest[sp..]) else rest[rest.len..];
    }

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

/// Decode an IRCv3 tag value into `out`. Invalid escape backslashes and a
/// trailing lone backslash are dropped exactly as required by message-tags.
pub fn unescapeTagValue(out: *std.ArrayList(u8), gpa: std.mem.Allocator, raw: []const u8) !void {
    var at: usize = 0;
    while (at < raw.len) : (at += 1) {
        if (raw[at] != '\\') {
            try out.append(gpa, raw[at]);
            continue;
        }
        at += 1;
        if (at >= raw.len) break;
        try out.append(gpa, switch (raw[at]) {
            ':' => ';',
            's' => ' ',
            '\\' => '\\',
            'r' => '\r',
            'n' => '\n',
            else => raw[at],
        });
    }
}

/// Escape one decoded tag value for the wire.
pub fn escapeTagValue(out: *std.ArrayList(u8), gpa: std.mem.Allocator, value: []const u8) !void {
    for (value) |byte| switch (byte) {
        ';' => try out.appendSlice(gpa, "\\:"),
        ' ' => try out.appendSlice(gpa, "\\s"),
        '\\' => try out.appendSlice(gpa, "\\\\"),
        '\r' => try out.appendSlice(gpa, "\\r"),
        '\n' => try out.appendSlice(gpa, "\\n"),
        0 => return error.InvalidTagValue,
        else => try out.append(gpa, byte),
    };
}

/// A parameter must be sent as a `:trailing` if it is the last one and is
/// empty, contains a space, or begins with ':'.
fn needsTrailing(p: []const u8) bool {
    return p.len == 0 or p[0] == ':' or std.mem.indexOfScalar(u8, p, ' ') != null;
}

fn validTagData(tags: []const u8) bool {
    // A client may contribute at most 4094 bytes of tag data. The separate
    // 8191-byte receive allowance includes a server-added section as well.
    if (tags.len == 0 or tags.len > 4094 or !std.unicode.utf8ValidateSlice(tags) or
        std.mem.indexOfAny(u8, tags, " \r\n\x00") != null)
        return false;
    var fields = std.mem.splitScalar(u8, tags, ';');
    while (fields.next()) |field| {
        if (field.len == 0) return false;
        const equals = std.mem.indexOfScalar(u8, field, '=') orelse field.len;
        const key = field[0..equals];
        if (key.len == 0) return false;
        for (key) |byte| {
            if (!std.ascii.isAlphanumeric(byte) and byte != '-' and byte != '.' and
                byte != '/' and byte != '+') return false;
        }
    }
    return true;
}

/// Append the wire form (with CRLF) to `out`.
pub fn write(
    out: *std.ArrayList(u8),
    gpa: std.mem.Allocator,
    msg: Message,
) !void {
    const start = out.items.len;
    errdefer out.shrinkRetainingCapacity(start);
    if (msg.tag_data) |tags| {
        if (!validTagData(tags)) return error.InvalidMessageTags;
        try out.append(gpa, '@');
        try out.appendSlice(gpa, tags);
        try out.append(gpa, ' ');
    }
    const command_start = out.items.len;
    if (msg.command.len == 0 or std.mem.indexOfAny(u8, msg.command, " :\r\n\x00") != null)
        return error.InvalidIrcCommand;
    if (msg.prefix) |p| {
        if (p.len == 0 or std.mem.indexOfAny(u8, p, " \r\n\x00") != null)
            return error.InvalidIrcPrefix;
        try out.append(gpa, ':');
        try out.appendSlice(gpa, p);
        try out.append(gpa, ' ');
    }
    try out.appendSlice(gpa, msg.command);
    var i: usize = 0;
    while (i < msg.param_count) : (i += 1) {
        const p = msg.params[i];
        if (std.mem.indexOfAny(u8, p, "\r\n\x00") != null)
            return error.InvalidIrcParameter;
        if (i + 1 < msg.param_count and
            (p.len == 0 or p[0] == ':' or std.mem.indexOfScalar(u8, p, ' ') != null))
            return error.InvalidIrcParameter;
        try out.append(gpa, ' ');
        // Only the final parameter may use the ':trailing' form.
        if (i + 1 == msg.param_count and needsTrailing(p)) {
            try out.append(gpa, ':');
        }
        try out.appendSlice(gpa, p);
    }
    try out.appendSlice(gpa, "\r\n");
    // The command/prefix/params portion retains IRC's 512-byte limit even
    // when the separately-budgeted IRCv3 tag section is present.
    if (out.items.len - command_start > 512) return error.IrcMessageTooLong;
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

test "IRCv3 tags parse before prefix and preserve null versus empty values" {
    const m = parse("@time=2026-07-16T10:20:30.000Z;account=kain;flag;empty= :nick!u@h PRIVMSG #comic :hello");
    try std.testing.expectEqualStrings("nick!u@h", m.prefix.?);
    try std.testing.expectEqualStrings("PRIVMSG", m.command);
    try std.testing.expectEqualStrings("2026-07-16T10:20:30.000Z", m.tag("time").?.raw_value.?);
    try std.testing.expect(m.tag("flag").?.raw_value == null);
    try std.testing.expectEqualStrings("", m.tag("empty").?.raw_value.?);
    try std.testing.expectEqualStrings("hello", m.param(1).?);
}

test "IRCv3 tag escaping follows message-tags invalid escape rules" {
    const gpa = std.testing.allocator;
    var decoded: std.ArrayList(u8) = .empty;
    defer decoded.deinit(gpa);
    try unescapeTagValue(&decoded, gpa, "raw+:=,escaped\\:\\s\\\\\\r\\n\\btrailing\\");
    try std.testing.expectEqualStrings("raw+:=,escaped; \\\r\nbtrailing", decoded.items);

    var encoded: std.ArrayList(u8) = .empty;
    defer encoded.deinit(gpa);
    try escapeTagValue(&encoded, gpa, "semi; space \\ return\rline\n");
    try std.testing.expectEqualStrings("semi\\:\\sspace\\s\\\\\\sreturn\\rline\\n", encoded.items);
}

test "tagged message writes and parses losslessly" {
    const gpa = std.testing.allocator;
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);
    var m = Message{ .tag_data = "label=42;+typing=active", .command = "TAGMSG" };
    m.addParam("#comic");
    try write(&out, gpa, m);
    try std.testing.expectEqualStrings("@label=42;+typing=active TAGMSG #comic\r\n", out.items);
    const parsed = parse(out.items);
    try std.testing.expectEqualStrings("42", parsed.tag("label").?.raw_value.?);
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

test "write rejects an invalid spaced middle parameter atomically" {
    const gpa = std.testing.allocator;
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);
    var m = Message{ .command = "CMD" };
    m.addParam("with space"); // not last -> cannot use trailing
    m.addParam("tail");
    try std.testing.expectError(error.InvalidIrcParameter, write(&out, gpa, m));
    try std.testing.expectEqual(@as(usize, 0), out.items.len);
}

test "write rejects injection oversized payloads and malformed tag keys" {
    const gpa = std.testing.allocator;
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);

    var injected = Message{ .command = "PRIVMSG" };
    injected.addParam("#c");
    injected.addParam("hello\r\nOPER root password");
    try std.testing.expectError(error.InvalidIrcParameter, write(&out, gpa, injected));
    try std.testing.expectEqual(@as(usize, 0), out.items.len);

    var oversized = Message{ .command = "PRIVMSG" };
    oversized.addParam("#c");
    var long: [600]u8 = undefined;
    @memset(&long, 'x');
    oversized.addParam(&long);
    try std.testing.expectError(error.IrcMessageTooLong, write(&out, gpa, oversized));
    try std.testing.expectEqual(@as(usize, 0), out.items.len);

    try std.testing.expectError(
        error.InvalidMessageTags,
        write(&out, gpa, .{ .tag_data = "bad_key=value", .command = "PING" }),
    );
    try std.testing.expectEqual(@as(usize, 0), out.items.len);
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

test "duplicate tags use the final occurrence and preserve opaque invalid names" {
    const parsed = parse("@dup=first;bad_key=x;dup=last :n PRIVMSG #c hi");
    try std.testing.expectEqualStrings("last", parsed.tag("dup").?.raw_value.?);
    try std.testing.expectEqualStrings("x", parsed.tag("bad_key").?.raw_value.?);
}

test "client tag contribution is bounded at 4094 UTF-8 bytes" {
    const gpa = std.testing.allocator;
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);
    var accepted: [4094]u8 = @splat('a');
    try write(&out, gpa, .{ .tag_data = &accepted, .command = "PING" });
    out.clearRetainingCapacity();
    var rejected: [4095]u8 = @splat('a');
    try std.testing.expectError(error.InvalidMessageTags, write(&out, gpa, .{ .tag_data = &rejected, .command = "PING" }));
    try std.testing.expectEqual(@as(usize, 0), out.items.len);

    const invalid_utf8 = [_]u8{ 'x', '=', 0xff };
    try std.testing.expectError(error.InvalidMessageTags, write(&out, gpa, .{ .tag_data = &invalid_utf8, .command = "PING" }));
}
