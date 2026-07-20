//! Persistent portable application preferences.
//!
//! Microsoft Comic Chat stored these values in the Windows Registry.  The
//! portable client keeps the same ownership in one bounded, human-inspectable
//! file. Values are percent escaped, so untrusted profile/rule text can never
//! inject another record or escape its field.

const std = @import("std");
const files = @import("files.zig");

pub const max_preferences_bytes: usize = 256 * 1024;
pub const max_rules: usize = 64;
pub const max_notifications: usize = 128;

pub const Rule = struct {
    name: []u8,
    event: []u8,
    filter: []u8,
    action: []u8,
    value: []u8,
    enabled: bool,

    fn deinit(self: *Rule, gpa: std.mem.Allocator) void {
        gpa.free(self.name);
        gpa.free(self.event);
        gpa.free(self.filter);
        gpa.free(self.action);
        gpa.free(self.value);
    }
};

pub const Notification = struct {
    nickname: []u8,
    user_mask: []u8,
    host_mask: []u8,
    network: []u8,
    enabled: bool,

    fn deinit(self: *Notification, gpa: std.mem.Allocator) void {
        gpa.free(self.nickname);
        gpa.free(self.user_mask);
        gpa.free(self.host_mask);
        gpa.free(self.network);
    }
};

pub const Store = struct {
    gpa: std.mem.Allocator,
    profile: std.ArrayList(u8) = .empty,
    display_name: std.ArrayList(u8) = .empty,
    homepage: std.ArrayList(u8) = .empty,
    email: std.ArrayList(u8) = .empty,
    backdrop: std.ArrayList(u8) = .empty,
    greeting_mode: std.ArrayList(u8) = .empty,
    greeting: std.ArrayList(u8) = .empty,
    auto_ignore_count: u16 = 8,
    auto_ignore_interval_s: u16 = 10,
    rules: std.ArrayList(Rule) = .empty,
    notifications: std.ArrayList(Notification) = .empty,
    notification_delivery: std.ArrayList(u8) = .empty,

    pub fn init(gpa: std.mem.Allocator) Store {
        return .{ .gpa = gpa };
    }

    pub fn deinit(self: *Store) void {
        self.profile.deinit(self.gpa);
        self.display_name.deinit(self.gpa);
        self.homepage.deinit(self.gpa);
        self.email.deinit(self.gpa);
        self.backdrop.deinit(self.gpa);
        self.greeting_mode.deinit(self.gpa);
        self.greeting.deinit(self.gpa);
        for (self.rules.items) |*rule| rule.deinit(self.gpa);
        self.rules.deinit(self.gpa);
        for (self.notifications.items) |*notification| notification.deinit(self.gpa);
        self.notifications.deinit(self.gpa);
        self.notification_delivery.deinit(self.gpa);
        self.* = undefined;
    }

    pub fn loadFile(gpa: std.mem.Allocator, io: std.Io, path: []const u8) !Store {
        const bytes = std.Io.Dir.cwd().readFileAlloc(io, path, gpa, .limited(max_preferences_bytes)) catch |err| switch (err) {
            error.FileNotFound => return init(gpa),
            else => return err,
        };
        defer gpa.free(bytes);
        return parse(gpa, bytes);
    }

    pub fn saveFile(self: *const Store, io: std.Io, path: []const u8) !void {
        const bytes = try self.encode();
        defer self.gpa.free(bytes);
        try files.saveBytesAtomic(io, self.gpa, path, bytes);
    }

    pub fn profileText(self: *const Store) []const u8 {
        return if (self.profile.items.len == 0)
            "This person is too lazy to create a profile entry."
        else
            self.profile.items;
    }

    pub fn backdropName(self: *const Store) []const u8 {
        return if (self.backdrop.items.len == 0) "field" else self.backdrop.items;
    }

    pub fn greetingMode(self: *const Store) []const u8 {
        return if (self.greeting_mode.items.len == 0) "None" else self.greeting_mode.items;
    }

    pub fn notificationDelivery(self: *const Store) []const u8 {
        return if (self.notification_delivery.items.len == 0) "In-app banner" else self.notification_delivery.items;
    }

    pub fn setProfile(self: *Store, profile: []const u8, display_name: []const u8, homepage: []const u8, email: []const u8) !void {
        try replace(&self.profile, self.gpa, profile);
        try replace(&self.display_name, self.gpa, display_name);
        try replace(&self.homepage, self.gpa, homepage);
        try replace(&self.email, self.gpa, email);
    }

    pub fn setBackdrop(self: *Store, value: []const u8) !void {
        try replace(&self.backdrop, self.gpa, value);
    }

    pub fn setAutomation(self: *Store, mode: []const u8, greeting: []const u8, count: u16, interval_s: u16) !void {
        try replace(&self.greeting_mode, self.gpa, mode);
        try replace(&self.greeting, self.gpa, greeting);
        self.auto_ignore_count = count;
        self.auto_ignore_interval_s = interval_s;
    }

    pub fn setNotificationDelivery(self: *Store, value: []const u8) !void {
        try replace(&self.notification_delivery, self.gpa, value);
    }

    pub fn upsertRule(self: *Store, input: struct {
        name: []const u8,
        event: []const u8,
        filter: []const u8,
        action: []const u8,
        value: []const u8,
        enabled: bool = true,
    }) !void {
        for (self.rules.items) |*rule| if (std.ascii.eqlIgnoreCase(rule.name, input.name)) {
            const replacement = try cloneRule(self.gpa, input);
            rule.deinit(self.gpa);
            rule.* = replacement;
            return;
        };
        if (self.rules.items.len >= max_rules) return error.TooManyRules;
        var rule = try cloneRule(self.gpa, input);
        errdefer rule.deinit(self.gpa);
        try self.rules.append(self.gpa, rule);
    }

    pub fn upsertNotification(self: *Store, input: struct {
        nickname: []const u8,
        user_mask: []const u8,
        host_mask: []const u8,
        network: []const u8,
        enabled: bool = true,
    }) !void {
        for (self.notifications.items) |*notification| if (std.ascii.eqlIgnoreCase(notification.nickname, input.nickname)) {
            const replacement = try cloneNotification(self.gpa, input);
            notification.deinit(self.gpa);
            notification.* = replacement;
            return;
        };
        if (self.notifications.items.len >= max_notifications) return error.TooManyNotifications;
        var notification = try cloneNotification(self.gpa, input);
        errdefer notification.deinit(self.gpa);
        try self.notifications.append(self.gpa, notification);
    }

    fn encode(self: *const Store) ![]u8 {
        var out: std.ArrayList(u8) = .empty;
        errdefer out.deinit(self.gpa);
        try out.appendSlice(self.gpa, "COMICCHAT-PREFERENCES 1\n");
        try appendRecord(&out, self.gpa, "profile", self.profile.items);
        try appendRecord(&out, self.gpa, "display_name", self.display_name.items);
        try appendRecord(&out, self.gpa, "homepage", self.homepage.items);
        try appendRecord(&out, self.gpa, "email", self.email.items);
        try appendRecord(&out, self.gpa, "backdrop", self.backdropName());
        try appendRecord(&out, self.gpa, "greeting_mode", self.greetingMode());
        try appendRecord(&out, self.gpa, "greeting", self.greeting.items);
        var number: [16]u8 = undefined;
        try appendRecord(&out, self.gpa, "auto_ignore_count", try std.fmt.bufPrint(&number, "{d}", .{self.auto_ignore_count}));
        try appendRecord(&out, self.gpa, "auto_ignore_interval", try std.fmt.bufPrint(&number, "{d}", .{self.auto_ignore_interval_s}));
        try appendRecord(&out, self.gpa, "notification_delivery", self.notificationDelivery());
        for (self.rules.items) |rule| try appendComposite(&out, self.gpa, "rule", &.{ rule.name, rule.event, rule.filter, rule.action, rule.value, if (rule.enabled) "1" else "0" });
        for (self.notifications.items) |notification| try appendComposite(&out, self.gpa, "notification", &.{ notification.nickname, notification.user_mask, notification.host_mask, notification.network, if (notification.enabled) "1" else "0" });
        if (out.items.len > max_preferences_bytes) return error.PreferencesTooLarge;
        return out.toOwnedSlice(self.gpa);
    }
};

pub fn parse(gpa: std.mem.Allocator, bytes: []const u8) !Store {
    if (bytes.len > max_preferences_bytes) return error.PreferencesTooLarge;
    var store = Store.init(gpa);
    errdefer store.deinit();
    var lines = std.mem.splitScalar(u8, bytes, '\n');
    const header = std.mem.trimEnd(u8, lines.next() orelse "", "\r");
    if (!std.mem.eql(u8, header, "COMICCHAT-PREFERENCES 1")) return error.InvalidPreferencesHeader;
    while (lines.next()) |raw_line| {
        const line = std.mem.trimEnd(u8, raw_line, "\r");
        if (line.len == 0) continue;
        const equals = std.mem.indexOfScalar(u8, line, '=') orelse continue;
        const key = line[0..equals];
        const value = line[equals + 1 ..];
        if (std.mem.eql(u8, key, "rule")) {
            const parts = try decodeComposite(gpa, value, 6);
            defer freeParts(gpa, parts);
            if (parts.len == 6) try store.upsertRule(.{ .name = parts[0], .event = parts[1], .filter = parts[2], .action = parts[3], .value = parts[4], .enabled = std.mem.eql(u8, parts[5], "1") });
            continue;
        }
        if (std.mem.eql(u8, key, "notification")) {
            const parts = try decodeComposite(gpa, value, 5);
            defer freeParts(gpa, parts);
            if (parts.len == 5) try store.upsertNotification(.{ .nickname = parts[0], .user_mask = parts[1], .host_mask = parts[2], .network = parts[3], .enabled = std.mem.eql(u8, parts[4], "1") });
            continue;
        }
        const decoded = try decode(gpa, value);
        defer gpa.free(decoded);
        if (std.mem.eql(u8, key, "profile")) try replace(&store.profile, gpa, decoded) else if (std.mem.eql(u8, key, "display_name")) try replace(&store.display_name, gpa, decoded) else if (std.mem.eql(u8, key, "homepage")) try replace(&store.homepage, gpa, decoded) else if (std.mem.eql(u8, key, "email")) try replace(&store.email, gpa, decoded) else if (std.mem.eql(u8, key, "backdrop")) try store.setBackdrop(decoded) else if (std.mem.eql(u8, key, "greeting_mode")) try replace(&store.greeting_mode, gpa, decoded) else if (std.mem.eql(u8, key, "greeting")) try replace(&store.greeting, gpa, decoded) else if (std.mem.eql(u8, key, "auto_ignore_count")) store.auto_ignore_count = std.fmt.parseInt(u16, decoded, 10) catch store.auto_ignore_count else if (std.mem.eql(u8, key, "auto_ignore_interval")) store.auto_ignore_interval_s = std.fmt.parseInt(u16, decoded, 10) catch store.auto_ignore_interval_s else if (std.mem.eql(u8, key, "notification_delivery")) try replace(&store.notification_delivery, gpa, decoded);
    }
    return store;
}

fn replace(list: *std.ArrayList(u8), gpa: std.mem.Allocator, value: []const u8) !void {
    if (value.len > 4096) return error.ValueTooLong;
    list.clearRetainingCapacity();
    try list.appendSlice(gpa, value);
}

fn cloneRule(gpa: std.mem.Allocator, input: anytype) !Rule {
    const name = try gpa.dupe(u8, input.name);
    errdefer gpa.free(name);
    const event = try gpa.dupe(u8, input.event);
    errdefer gpa.free(event);
    const filter = try gpa.dupe(u8, input.filter);
    errdefer gpa.free(filter);
    const action = try gpa.dupe(u8, input.action);
    errdefer gpa.free(action);
    const value = try gpa.dupe(u8, input.value);
    return .{ .name = name, .event = event, .filter = filter, .action = action, .value = value, .enabled = input.enabled };
}

fn cloneNotification(gpa: std.mem.Allocator, input: anytype) !Notification {
    const nickname = try gpa.dupe(u8, input.nickname);
    errdefer gpa.free(nickname);
    const user_mask = try gpa.dupe(u8, input.user_mask);
    errdefer gpa.free(user_mask);
    const host_mask = try gpa.dupe(u8, input.host_mask);
    errdefer gpa.free(host_mask);
    const network = try gpa.dupe(u8, input.network);
    return .{ .nickname = nickname, .user_mask = user_mask, .host_mask = host_mask, .network = network, .enabled = input.enabled };
}

fn appendRecord(out: *std.ArrayList(u8), gpa: std.mem.Allocator, key: []const u8, value: []const u8) !void {
    try out.appendSlice(gpa, key);
    try out.append(gpa, '=');
    try appendEscaped(out, gpa, value);
    try out.append(gpa, '\n');
}

fn appendComposite(out: *std.ArrayList(u8), gpa: std.mem.Allocator, key: []const u8, fields: []const []const u8) !void {
    try out.appendSlice(gpa, key);
    try out.append(gpa, '=');
    for (fields, 0..) |field, index| {
        if (index != 0) try out.append(gpa, '\t');
        try appendEscaped(out, gpa, field);
    }
    try out.append(gpa, '\n');
}

fn appendEscaped(out: *std.ArrayList(u8), gpa: std.mem.Allocator, value: []const u8) !void {
    const hex = "0123456789ABCDEF";
    for (value) |byte| {
        if (std.ascii.isAlphanumeric(byte) or std.mem.indexOfScalar(u8, " .,_-:/#*!?@+", byte) != null) {
            try out.append(gpa, byte);
        } else {
            try out.appendSlice(gpa, &.{ '%', hex[byte >> 4], hex[byte & 0x0f] });
        }
    }
}

fn decode(gpa: std.mem.Allocator, value: []const u8) ![]u8 {
    var out: std.ArrayList(u8) = .empty;
    errdefer out.deinit(gpa);
    var index: usize = 0;
    while (index < value.len) {
        if (value[index] == '%' and index + 2 < value.len) {
            const hi = std.fmt.charToDigit(value[index + 1], 16) catch return error.InvalidEscape;
            const lo = std.fmt.charToDigit(value[index + 2], 16) catch return error.InvalidEscape;
            try out.append(gpa, @intCast(hi * 16 + lo));
            index += 3;
        } else {
            try out.append(gpa, value[index]);
            index += 1;
        }
    }
    return out.toOwnedSlice(gpa);
}

fn decodeComposite(gpa: std.mem.Allocator, value: []const u8, max_fields: usize) ![][]u8 {
    var out: std.ArrayList([]u8) = .empty;
    errdefer {
        for (out.items) |part| gpa.free(part);
        out.deinit(gpa);
    }
    var parts = std.mem.splitScalar(u8, value, '\t');
    while (parts.next()) |part| {
        if (out.items.len >= max_fields) return error.TooManyFields;
        try out.append(gpa, try decode(gpa, part));
    }
    return out.toOwnedSlice(gpa);
}

fn freeParts(gpa: std.mem.Allocator, parts: [][]u8) void {
    for (parts) |part| gpa.free(part);
    gpa.free(parts);
}

test "preferences round-trip profile rules notifications and escaped text" {
    const gpa = std.testing.allocator;
    var source = Store.init(gpa);
    defer source.deinit();
    try source.setProfile("hello\nworld", "Comic User", "https://example.test", "");
    try source.setBackdrop("den");
    try source.setAutomation("Whisper", "Welcome, %nick%!", 5, 12);
    try source.setNotificationDelivery("In-app banner");
    try source.upsertRule(.{ .name = "Hello", .event = "Message", .filter = "ping|pong", .action = "Reply", .value = "hello\nthere" });
    try source.upsertNotification(.{ .nickname = "Anna", .user_mask = "*", .host_mask = "*.test", .network = "eshmaki.me" });
    const encoded = try source.encode();
    defer gpa.free(encoded);
    try std.testing.expect(std.mem.indexOfScalar(u8, encoded, '\r') == null);
    var decoded = try parse(gpa, encoded);
    defer decoded.deinit();
    try std.testing.expectEqualStrings("hello\nworld", decoded.profile.items);
    try std.testing.expectEqualStrings("ping|pong", decoded.rules.items[0].filter);
    try std.testing.expectEqualStrings("*.test", decoded.notifications.items[0].host_mask);
    try std.testing.expectEqual(@as(u16, 12), decoded.auto_ignore_interval_s);
}
