//! Small native desktop-service bridge for pure-Zig Unix builds.
//!
//! The window backends remain direct protocol implementations. Clipboard,
//! notifications, file selection, document opening, and printing use the
//! desktop's standard command-line bridge when present. Every call is bounded
//! and failure is non-fatal, so minimal BSD installations retain the internal
//! application fallback.

const std = @import("std");

const max_clipboard_bytes = 1024 * 1024;
const max_service_output = 1024 * 1024;

pub const Desktop = enum { x11, wayland };

pub fn writeClipboard(io: std.Io, desktop: Desktop, text: []const u8) !void {
    if (text.len > max_clipboard_bytes) return error.ClipboardTooLarge;
    const argv: []const []const u8 = switch (desktop) {
        .wayland => &.{ "wl-copy", "--type", "text/plain;charset=utf-8" },
        .x11 => &.{ "xclip", "-selection", "clipboard", "-in" },
    };
    var child = try std.process.spawn(io, .{
        .argv = argv,
        .stdin = .pipe,
        .stdout = .ignore,
        .stderr = .ignore,
    });
    defer child.kill(io);
    const stdin = child.stdin orelse return error.MissingChildStdin;
    try stdin.writeStreamingAll(io, text);
    stdin.close(io);
    child.stdin = null;
    const term = try child.wait(io);
    if (!term.success()) return error.DesktopServiceFailed;
}

pub fn readClipboard(gpa: std.mem.Allocator, io: std.Io, desktop: Desktop) !?[]u8 {
    const argv: []const []const u8 = switch (desktop) {
        .wayland => &.{ "wl-paste", "--no-newline", "--type", "text" },
        .x11 => &.{ "xclip", "-selection", "clipboard", "-out" },
    };
    var result = try std.process.run(gpa, io, .{
        .argv = argv,
        .stdout_limit = .limited(max_clipboard_bytes),
        .stderr_limit = .limited(4096),
    });
    defer gpa.free(result.stderr);
    if (!result.term.success()) {
        gpa.free(result.stdout);
        return null;
    }
    return result.stdout;
}

pub fn chooseFile(gpa: std.mem.Allocator, io: std.Io, save: bool, title: []const u8) !?[]u8 {
    const mode = if (save) "--save" else "--file-selection";
    const argv = if (save)
        &[_][]const u8{ "zenity", "--file-selection", mode, "--confirm-overwrite", "--title", title }
    else
        &[_][]const u8{ "zenity", mode, "--title", title };
    var result = std.process.run(gpa, io, .{
        .argv = argv,
        .stdout_limit = .limited(max_service_output),
        .stderr_limit = .limited(4096),
    }) catch return chooseFileKdialog(gpa, io, save, title);
    defer gpa.free(result.stderr);
    if (!result.term.success()) {
        gpa.free(result.stdout);
        return null;
    }
    return trimOwnedResult(gpa, result.stdout);
}

fn chooseFileKdialog(gpa: std.mem.Allocator, io: std.Io, save: bool, title: []const u8) !?[]u8 {
    const action = if (save) "--getsavefilename" else "--getopenfilename";
    const argv = [_][]const u8{ "kdialog", "--title", title, action, "." };
    var result = try std.process.run(gpa, io, .{
        .argv = &argv,
        .stdout_limit = .limited(max_service_output),
        .stderr_limit = .limited(4096),
    });
    defer gpa.free(result.stderr);
    if (!result.term.success()) {
        gpa.free(result.stdout);
        return null;
    }
    return trimOwnedResult(gpa, result.stdout);
}

pub fn notify(gpa: std.mem.Allocator, io: std.Io, title: []const u8, body: []const u8) !void {
    if (title.len > 256 or body.len > 4096) return error.NotificationTooLarge;
    var result = try std.process.run(gpa, io, .{
        .argv = &.{ "notify-send", "--app-name=Comic Chat", title, body },
        .stdout_limit = .limited(4096),
        .stderr_limit = .limited(4096),
    });
    defer gpa.free(result.stdout);
    defer gpa.free(result.stderr);
    if (!result.term.success()) return error.DesktopServiceFailed;
}

pub fn openPath(gpa: std.mem.Allocator, io: std.Io, path: []const u8) !void {
    return runNoOutput(gpa, io, &.{ "xdg-open", path });
}

pub fn printPath(gpa: std.mem.Allocator, io: std.Io, path: []const u8) !void {
    runNoOutput(gpa, io, &.{ "lp", path }) catch return runNoOutput(gpa, io, &.{ "lpr", path });
}

fn runNoOutput(gpa: std.mem.Allocator, io: std.Io, argv: []const []const u8) !void {
    var result = try std.process.run(gpa, io, .{
        .argv = argv,
        .stdout_limit = .limited(4096),
        .stderr_limit = .limited(4096),
    });
    defer gpa.free(result.stdout);
    defer gpa.free(result.stderr);
    if (!result.term.success()) return error.DesktopServiceFailed;
}

fn trimOwnedResult(gpa: std.mem.Allocator, owned: []u8) !?[]u8 {
    const trimmed = std.mem.trim(u8, owned, "\r\n");
    if (trimmed.len == 0) {
        gpa.free(owned);
        return null;
    }
    if (trimmed.ptr == owned.ptr and trimmed.len == owned.len) return owned;
    const result = try gpa.dupe(u8, trimmed);
    gpa.free(owned);
    return result;
}

test "desktop service output trimming preserves an owned result" {
    const gpa = std.testing.allocator;
    const raw = try gpa.dupe(u8, "/tmp/chat.ccr\r\n");
    const trimmed = (try trimOwnedResult(gpa, raw)).?;
    defer gpa.free(trimmed);
    try std.testing.expectEqualStrings("/tmp/chat.ccr", trimmed);
}
