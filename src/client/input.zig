//! Editable input line for the interactive client. Pure state — no I/O, no
//! rendering — so every editing operation is unit-testable.

const std = @import("std");

pub const Editor = struct {
    pub const Selection = struct { start: usize, end: usize };
    gpa: std.mem.Allocator,
    buf: std.ArrayList(u8) = .empty,
    cursor: usize = 0,
    selection_anchor: ?usize = null,
    undo_stack: std.ArrayList(Snapshot) = .empty,
    redo_stack: std.ArrayList(Snapshot) = .empty,

    const Snapshot = struct {
        text: []u8,
        cursor: usize,
        selection_anchor: ?usize,

        fn deinit(self: *Snapshot, gpa: std.mem.Allocator) void {
            gpa.free(self.text);
        }
    };

    pub fn init(gpa: std.mem.Allocator) Editor {
        return .{ .gpa = gpa };
    }

    pub fn deinit(self: *Editor) void {
        self.buf.deinit(self.gpa);
        self.deinitSnapshots(&self.undo_stack);
        self.deinitSnapshots(&self.redo_stack);
    }

    pub fn text(self: *const Editor) []const u8 {
        return self.buf.items;
    }

    pub fn insert(self: *Editor, codepoint: u21) !void {
        try self.recordUndo();
        self.deleteSelection();
        var encoded: [4]u8 = undefined;
        const len = try std.unicode.utf8Encode(codepoint, &encoded);
        try self.buf.insertSlice(self.gpa, self.cursor, encoded[0..len]);
        self.cursor += len;
    }

    pub fn backspace(self: *Editor) void {
        if (self.hasSelection()) {
            self.recordUndo() catch return;
            self.deleteSelection();
            return;
        }
        if (self.cursor == 0) return;
        self.recordUndo() catch return;
        const previous = previousBoundary(self.buf.items, self.cursor);
        self.buf.replaceRangeAssumeCapacity(previous, self.cursor - previous, "");
        self.cursor = previous;
    }

    pub fn delete(self: *Editor) void {
        if (self.hasSelection()) {
            self.recordUndo() catch return;
            self.deleteSelection();
            return;
        }
        if (self.cursor >= self.buf.items.len) return;
        self.recordUndo() catch return;
        const next = nextBoundary(self.buf.items, self.cursor);
        self.buf.replaceRangeAssumeCapacity(self.cursor, next - self.cursor, "");
    }

    pub fn left(self: *Editor) void {
        self.selection_anchor = null;
        self.cursor = previousBoundary(self.buf.items, self.cursor);
    }

    pub fn extendLeft(self: *Editor) void {
        if (self.selection_anchor == null) self.selection_anchor = self.cursor;
        self.cursor = previousBoundary(self.buf.items, self.cursor);
    }

    pub fn right(self: *Editor) void {
        self.selection_anchor = null;
        self.cursor = nextBoundary(self.buf.items, self.cursor);
    }

    pub fn extendRight(self: *Editor) void {
        if (self.selection_anchor == null) self.selection_anchor = self.cursor;
        self.cursor = nextBoundary(self.buf.items, self.cursor);
    }

    pub fn home(self: *Editor) void {
        self.selection_anchor = null;
        self.cursor = 0;
    }

    pub fn extendHome(self: *Editor) void {
        if (self.selection_anchor == null) self.selection_anchor = self.cursor;
        self.cursor = 0;
    }

    pub fn end(self: *Editor) void {
        self.selection_anchor = null;
        self.cursor = self.buf.items.len;
    }

    pub fn extendEnd(self: *Editor) void {
        if (self.selection_anchor == null) self.selection_anchor = self.cursor;
        self.cursor = self.buf.items.len;
    }

    pub fn clear(self: *Editor) void {
        if (self.buf.items.len > 0) self.recordUndo() catch return;
        self.buf.clearRetainingCapacity();
        self.cursor = 0;
        self.selection_anchor = null;
    }

    pub fn selectAll(self: *Editor) void {
        self.selection_anchor = 0;
        self.cursor = self.buf.items.len;
    }

    pub fn selection(self: *const Editor) ?Selection {
        const anchor = self.selection_anchor orelse return null;
        if (anchor == self.cursor) return null;
        return .{ .start = @min(anchor, self.cursor), .end = @max(anchor, self.cursor) };
    }

    pub fn copySelection(self: *const Editor) !?[]u8 {
        const range = self.selection() orelse return null;
        return try self.gpa.dupe(u8, self.buf.items[range.start..range.end]);
    }

    pub fn cutSelection(self: *Editor) !?[]u8 {
        const copied = try self.copySelection() orelse return null;
        try self.recordUndo();
        self.deleteSelection();
        return copied;
    }

    pub fn paste(self: *Editor, contents: []const u8) !void {
        if (!std.unicode.utf8ValidateSlice(contents)) return error.InvalidUtf8;
        if (contents.len == 0) return;
        try self.recordUndo();
        self.deleteSelection();
        try self.buf.insertSlice(self.gpa, self.cursor, contents);
        self.cursor += contents.len;
    }

    pub fn undo(self: *Editor) void {
        const snapshot = self.undo_stack.pop() orelse return;
        self.pushCurrent(&self.redo_stack) catch {
            self.undo_stack.append(self.gpa, snapshot) catch {};
            return;
        };
        self.restore(snapshot);
    }

    pub fn redo(self: *Editor) void {
        const snapshot = self.redo_stack.pop() orelse return;
        self.pushCurrent(&self.undo_stack) catch {
            self.redo_stack.append(self.gpa, snapshot) catch {};
            return;
        };
        self.restore(snapshot);
    }

    /// Commit the line: returns an owned copy and resets the editor.
    /// Caller frees the result.
    pub fn take(self: *Editor) ![]u8 {
        const line = try self.gpa.dupe(u8, self.buf.items);
        self.buf.clearRetainingCapacity();
        self.cursor = 0;
        self.selection_anchor = null;
        return line;
    }

    fn hasSelection(self: *const Editor) bool {
        return self.selection() != null;
    }

    fn deleteSelection(self: *Editor) void {
        const range = self.selection() orelse return;
        self.buf.replaceRangeAssumeCapacity(range.start, range.end - range.start, "");
        self.cursor = range.start;
        self.selection_anchor = null;
    }

    fn recordUndo(self: *Editor) !void {
        try self.pushCurrent(&self.undo_stack);
        self.deinitSnapshots(&self.redo_stack);
    }

    fn pushCurrent(self: *Editor, stack: *std.ArrayList(Snapshot)) !void {
        const contents = try self.gpa.dupe(u8, self.buf.items);
        errdefer self.gpa.free(contents);
        try stack.append(self.gpa, .{ .text = contents, .cursor = self.cursor, .selection_anchor = self.selection_anchor });
    }

    fn restore(self: *Editor, snapshot: Snapshot) void {
        self.buf.clearRetainingCapacity();
        self.buf.appendSlice(self.gpa, snapshot.text) catch unreachable;
        self.cursor = snapshot.cursor;
        self.selection_anchor = snapshot.selection_anchor;
        var mutable = snapshot;
        mutable.deinit(self.gpa);
    }

    fn deinitSnapshots(self: *Editor, stack: *std.ArrayList(Snapshot)) void {
        for (stack.items) |*snapshot| snapshot.deinit(self.gpa);
        stack.deinit(self.gpa);
        stack.* = .empty;
    }
};

fn previousBoundary(text: []const u8, cursor: usize) usize {
    if (cursor == 0) return 0;
    var index = @min(cursor, text.len) - 1;
    while (index > 0 and text[index] & 0xc0 == 0x80) index -= 1;
    return index;
}

fn nextBoundary(text: []const u8, cursor: usize) usize {
    if (cursor >= text.len) return text.len;
    var index = cursor + 1;
    while (index < text.len and text[index] & 0xc0 == 0x80) index += 1;
    return index;
}

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

test "UTF-8 insertion and editing preserve codepoint boundaries" {
    var ed = Editor.init(std.testing.allocator);
    defer ed.deinit();
    try ed.insert('A');
    try ed.insert(0x20ac);
    try ed.insert(0x1f642);
    try std.testing.expectEqualStrings("A€🙂", ed.text());
    ed.left();
    ed.backspace();
    try std.testing.expectEqualStrings("A🙂", ed.text());
    ed.home();
    ed.right();
    ed.delete();
    try std.testing.expectEqualStrings("A", ed.text());
}

test "selection, cut paste, and undo preserve UTF-8 text" {
    var ed = Editor.init(std.testing.allocator);
    defer ed.deinit();
    try ed.paste("one € two");
    ed.selectAll();
    const copied = (try ed.copySelection()).?;
    defer std.testing.allocator.free(copied);
    try std.testing.expectEqualStrings("one € two", copied);
    const cut = (try ed.cutSelection()).?;
    defer std.testing.allocator.free(cut);
    try std.testing.expectEqualStrings("", ed.text());
    ed.undo();
    try std.testing.expectEqualStrings("one € two", ed.text());
    ed.selectAll();
    try ed.paste("hello");
    try std.testing.expectEqualStrings("hello", ed.text());
    ed.undo();
    try std.testing.expectEqualStrings("one € two", ed.text());
    ed.redo();
    try std.testing.expectEqualStrings("hello", ed.text());
}

test "selection extension stays on UTF-8 boundaries" {
    var ed = Editor.init(std.testing.allocator);
    defer ed.deinit();
    try ed.paste("a€b");
    ed.extendLeft();
    ed.extendLeft();
    const range = ed.selection().?;
    try std.testing.expectEqual(@as(usize, 1), range.start);
    try std.testing.expectEqual(@as(usize, 5), range.end);
}
