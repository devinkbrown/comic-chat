//! Host-keyed IRCv3 STS persistence with a small atomic text store.

const std = @import("std");

pub const header = "# comicchat-sts-v1\n";
pub const max_store_bytes: usize = 1024 * 1024;
pub const max_entries: usize = 4096;

pub const Entry = struct {
    host: []u8,
    expires_at: u64,
    duration_seconds: u64,
};

pub const Store = struct {
    gpa: std.mem.Allocator,
    entries: std.ArrayList(Entry) = .empty,
    dirty: bool = false,

    pub fn init(gpa: std.mem.Allocator) Store {
        return .{ .gpa = gpa };
    }

    pub fn deinit(self: *Store) void {
        for (self.entries.items) |entry| self.gpa.free(entry.host);
        self.entries.deinit(self.gpa);
        self.* = undefined;
    }

    pub fn parse(gpa: std.mem.Allocator, data: []const u8) !Store {
        var store = Store.init(gpa);
        errdefer store.deinit();
        if (data.len == 0) return store;
        var lines = std.mem.splitScalar(u8, data, '\n');
        const first = lines.next() orelse return store;
        if (!std.mem.eql(u8, first, std.mem.trimEnd(u8, header, "\n"))) return error.InvalidStsStore;
        while (lines.next()) |raw_line| {
            const line = std.mem.trim(u8, raw_line, "\r");
            if (line.len == 0) continue;
            var fields = std.mem.splitScalar(u8, line, '\t');
            const host = fields.next() orelse return error.InvalidStsStore;
            const expiry_text = fields.next() orelse return error.InvalidStsStore;
            const duration_text = fields.next() orelse return error.InvalidStsStore;
            if (fields.next() != null or !validHost(host)) return error.InvalidStsStore;
            if (store.entries.items.len >= max_entries) return error.StsStoreTooLarge;
            const expiry = std.fmt.parseInt(u64, expiry_text, 10) catch return error.InvalidStsStore;
            const duration = std.fmt.parseInt(u64, duration_text, 10) catch return error.InvalidStsStore;
            const owned_host = try lowerHost(gpa, host);
            errdefer gpa.free(owned_host);
            try store.entries.append(gpa, .{
                .host = owned_host,
                .expires_at = expiry,
                .duration_seconds = duration,
            });
        }
        return store;
    }

    pub fn loadFile(gpa: std.mem.Allocator, io: std.Io, path: []const u8) !Store {
        const data = std.Io.Dir.cwd().readFileAlloc(io, path, gpa, .limited(max_store_bytes)) catch |err| switch (err) {
            error.FileNotFound => return Store.init(gpa),
            else => return err,
        };
        defer gpa.free(data);
        return parse(gpa, data);
    }

    pub fn serialize(self: *const Store, out: *std.ArrayList(u8)) !void {
        try out.appendSlice(self.gpa, header);
        for (self.entries.items) |entry| {
            try out.print(self.gpa, "{s}\t{d}\t{d}\n", .{ entry.host, entry.expires_at, entry.duration_seconds });
        }
    }

    pub fn saveFile(self: *Store, io: std.Io, path: []const u8) !void {
        if (!self.dirty) return;
        var bytes: std.ArrayList(u8) = .empty;
        defer bytes.deinit(self.gpa);
        try self.serialize(&bytes);
        const temporary = try std.fmt.allocPrint(self.gpa, "{s}.tmp", .{path});
        defer self.gpa.free(temporary);
        const cwd = std.Io.Dir.cwd();
        try cwd.writeFile(io, .{ .sub_path = temporary, .data = bytes.items });
        try cwd.rename(temporary, cwd, path, io);
        self.dirty = false;
    }

    pub fn requiresTls(self: *Store, host: []const u8, now_seconds: u64) bool {
        const index = self.find(host) orelse return false;
        if (self.entries.items[index].expires_at <= now_seconds) {
            const expired = self.entries.orderedRemove(index);
            self.gpa.free(expired.host);
            self.dirty = true;
            return false;
        }
        return true;
    }

    pub fn update(self: *Store, host: []const u8, duration_seconds: u64, now_seconds: u64) !void {
        if (!validHost(host)) return error.InvalidStsHost;
        if (duration_seconds == 0) {
            if (self.find(host)) |index| {
                const removed = self.entries.orderedRemove(index);
                self.gpa.free(removed.host);
                self.dirty = true;
            }
            return;
        }
        const expiry = now_seconds +| duration_seconds;
        if (self.find(host)) |index| {
            self.entries.items[index].expires_at = expiry;
            self.entries.items[index].duration_seconds = duration_seconds;
            self.dirty = true;
            return;
        }
        if (self.entries.items.len >= max_entries) return error.StsStoreTooLarge;
        const owned_host = try lowerHost(self.gpa, host);
        errdefer self.gpa.free(owned_host);
        try self.entries.append(self.gpa, .{
            .host = owned_host,
            .expires_at = expiry,
            .duration_seconds = duration_seconds,
        });
        self.dirty = true;
    }

    /// RFC STS reschedules a live policy from disconnect time so a long-lived
    /// session cannot silently consume its full protection window.
    pub fn rescheduleOnDisconnect(self: *Store, host: []const u8, now_seconds: u64) void {
        const index = self.find(host) orelse return;
        const entry = &self.entries.items[index];
        entry.expires_at = now_seconds +| entry.duration_seconds;
        self.dirty = true;
    }

    fn find(self: *const Store, host: []const u8) ?usize {
        for (self.entries.items, 0..) |entry, index| {
            if (std.ascii.eqlIgnoreCase(entry.host, host)) return index;
        }
        return null;
    }
};

fn validHost(host: []const u8) bool {
    return host.len != 0 and host.len <= 253 and std.mem.indexOfAny(u8, host, "\t\r\n\x00") == null;
}

fn lowerHost(gpa: std.mem.Allocator, host: []const u8) ![]u8 {
    const result = try gpa.alloc(u8, host.len);
    for (host, result) |source, *dest| dest.* = std.ascii.toLower(source);
    return result;
}

test "STS store persists, expires, disables, and reschedules" {
    const gpa = std.testing.allocator;
    var store = Store.init(gpa);
    defer store.deinit();
    try store.update("IRC.Example", 3600, 100);
    try std.testing.expect(store.requiresTls("irc.example", 3699));
    store.rescheduleOnDisconnect("irc.example", 2000);

    var wire: std.ArrayList(u8) = .empty;
    defer wire.deinit(gpa);
    try store.serialize(&wire);
    var loaded = try Store.parse(gpa, wire.items);
    defer loaded.deinit();
    try std.testing.expect(loaded.requiresTls("IRC.EXAMPLE", 5500));
    try std.testing.expect(!loaded.requiresTls("irc.example", 5600));

    try store.update("irc.example", 0, 3000);
    try std.testing.expect(!store.requiresTls("irc.example", 3000));
}

test "STS parser rejects injection and oversized state" {
    const gpa = std.testing.allocator;
    try std.testing.expectError(error.InvalidStsStore, Store.parse(gpa, header ++ "bad\thost\t1\t2\n"));
    try std.testing.expectError(error.InvalidStsStore, Store.parse(gpa, "not-a-store\n"));
}
