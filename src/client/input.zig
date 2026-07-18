//! Editable input line for the interactive client. Pure state — no I/O, no
//! rendering — so every editing operation is unit-testable.

const std = @import("std");

pub const Editor = struct {
    gpa: std.mem.Allocator,
    buf: std.ArrayList(u8) = .empty,
    cursor: usize = 0,

    pub fn init(gpa: std.mem.Allocator) Editor {
        return .{ .gpa = gpa };
    }

    pub fn deinit(self: *Editor) void {
        self.buf.deinit(self.gpa);
    }

    pub fn text(self: *const Editor) []const u8 {
        return self.buf.items;
    }

    pub fn insert(self: *Editor, ch: u8) !void {
        try self.buf.insert(self.gpa, self.cursor, ch);
        self.cursor += 1;
    }

    pub fn backspace(self: *Editor) void {
        if (self.cursor == 0) return;
        _ = self.buf.orderedRemove(self.cursor - 1);
        self.cursor -= 1;
    }

    pub fn delete(self: *Editor) void {
        if (self.cursor >= self.buf.items.len) return;
        _ = self.buf.orderedRemove(self.cursor);
    }

    pub fn left(self: *Editor) void {
        if (self.cursor > 0) self.cursor -= 1;
    }

    pub fn right(self: *Editor) void {
        if (self.cursor < self.buf.items.len) self.cursor += 1;
    }

    pub fn home(self: *Editor) void {
        self.cursor = 0;
    }

    pub fn end(self: *Editor) void {
        self.cursor = self.buf.items.len;
    }

    /// Commit the line: returns an owned copy and resets the editor.
    /// Caller frees the result.
    pub fn take(self: *Editor) ![]u8 {
        const line = try self.gpa.dupe(u8, self.buf.items);
        self.buf.clearRetainingCapacity();
        self.cursor = 0;
        return line;
    }
};

// --- Tests --------------------------------------------------------------------

test "insert builds text and advances the cursor" {
    var ed = Editor.init(std.testing.allocator);
    defer ed.deinit();
    for ("hello") |ch| try ed.insert(ch);
    try std.testing.expectEqualStrings("hello", ed.text());
    try std.testing.expectEqual(@as(usize, 5), ed.cursor);
}

test "cursor movement and mid-line insert" {
    var ed = Editor.init(std.testing.allocator);
    defer ed.deinit();
    for ("hlo") |ch| try ed.insert(ch);
    ed.home();
    ed.right();
    try ed.insert('e');
    try ed.insert('l');
    try std.testing.expectEqualStrings("hello", ed.text());
    ed.end();
    try std.testing.expectEqual(@as(usize, 5), ed.cursor);
}

test "backspace and delete respect boundaries" {
    var ed = Editor.init(std.testing.allocator);
    defer ed.deinit();
    ed.backspace(); // empty: no-op
    ed.delete(); // empty: no-op
    for ("abc") |ch| try ed.insert(ch);
    ed.backspace(); // remove 'c'
    try std.testing.expectEqualStrings("ab", ed.text());
    ed.home();
    ed.delete(); // remove 'a'
    try std.testing.expectEqualStrings("b", ed.text());
    ed.delete();
    ed.delete(); // past end: no-op
    try std.testing.expectEqualStrings("", ed.text());
}

test "take returns the committed line and resets" {
    var ed = Editor.init(std.testing.allocator);
    defer ed.deinit();
    for ("send me") |ch| try ed.insert(ch);
    const line = try ed.take();
    defer std.testing.allocator.free(line);
    try std.testing.expectEqualStrings("send me", line);
    try std.testing.expectEqualStrings("", ed.text());
    try std.testing.expectEqual(@as(usize, 0), ed.cursor);
    // Editor is reusable after take.
    try ed.insert('x');
    try std.testing.expectEqualStrings("x", ed.text());
}
