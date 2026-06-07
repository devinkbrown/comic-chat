//! Map IRC participants to Comic Chat avatars and collect a transcript, so a
//! live conversation can be rendered as a comic strip.

const std = @import("std");

pub const avatars = [_][]const u8{
    "anna",   "armando", "bolo",    "cro",     "dan",      "denise",
    "hugh",   "jordan",  "kevin",   "kwensa",  "lance",    "lynnea",
    "margaret", "maynard", "mike",  "rebecca", "sage",     "scotty",
    "susan",  "tiki",    "tongtyed", "xeno",
};

/// Deterministic, case-insensitive nick → avatar (FNV-1a hash, mod 22).
pub fn avatarForNick(nick: []const u8) []const u8 {
    var h: u64 = 0xcbf29ce484222325;
    for (nick) |ch| {
        h = (h ^ std.ascii.toLower(ch)) *% 0x100000001b3;
    }
    return avatars[@intCast(h % avatars.len)];
}

/// "nick!user@host" or ":nick!.." → "nick".
pub fn nickFromPrefix(prefix: []const u8) []const u8 {
    var p = prefix;
    if (p.len > 0 and p[0] == ':') p = p[1..];
    const bang = std.mem.indexOfScalar(u8, p, '!') orelse p.len;
    return p[0..bang];
}

pub const Line = struct { nick: []const u8, avatar: []const u8, text: []const u8 };

/// Accumulates conversation lines, owning copies of the text (wire buffers are
/// reused by the framer, so we must dupe).
pub const Transcript = struct {
    gpa: std.mem.Allocator,
    lines: std.ArrayList(Line) = .empty,

    pub fn init(gpa: std.mem.Allocator) Transcript {
        return .{ .gpa = gpa };
    }

    pub fn deinit(self: *Transcript) void {
        for (self.lines.items) |l| {
            self.gpa.free(l.nick);
            self.gpa.free(l.text);
        }
        self.lines.deinit(self.gpa);
    }

    pub fn add(self: *Transcript, nick: []const u8, text: []const u8) !void {
        const n = try self.gpa.dupe(u8, nick);
        errdefer self.gpa.free(n);
        const t = try self.gpa.dupe(u8, text);
        try self.lines.append(self.gpa, .{ .nick = n, .avatar = avatarForNick(n), .text = t });
    }

    pub fn count(self: *const Transcript) usize {
        return self.lines.items.len;
    }
};

test "avatarForNick is deterministic, case-insensitive, and always valid" {
    const a = avatarForNick("Bob");
    try std.testing.expectEqualStrings(a, avatarForNick("bob"));
    try std.testing.expectEqualStrings(a, avatarForNick("BOB"));
    var found = false;
    for (avatars) |av| {
        if (std.mem.eql(u8, av, a)) found = true;
    }
    try std.testing.expect(found);
}

test "nickFromPrefix strips user@host and leading colon" {
    try std.testing.expectEqualStrings("anna", nickFromPrefix(":anna!u@h"));
    try std.testing.expectEqualStrings("bob", nickFromPrefix("bob!user@host"));
    try std.testing.expectEqualStrings("srv", nickFromPrefix("srv"));
}

test "transcript owns its copies" {
    const gpa = std.testing.allocator;
    var t = Transcript.init(gpa);
    defer t.deinit();
    var buf: [16]u8 = undefined;
    @memcpy(buf[0..5], "hello");
    try t.add("zoe", buf[0..5]);
    @memset(buf[0..5], 'x'); // mutate the source; transcript must be unaffected
    try std.testing.expectEqual(@as(usize, 1), t.count());
    try std.testing.expectEqualStrings("hello", t.lines.items[0].text);
    try std.testing.expect(t.lines.items[0].avatar.len > 0);
}
