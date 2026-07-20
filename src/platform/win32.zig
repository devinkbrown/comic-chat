//! Minimal pure-Zig Win32 window backend.
//!
//! Uses User32 and GDI directly (no `@cImport`) to present the shared
//! 0xAARRGGBB software framebuffer. `Window` must be used from the OS thread
//! that opened it; Win32 owns a thread-local message queue.
//!
//! The public event and key shapes intentionally match `platform/x11.zig`.

const std = @import("std");
const builtin = @import("builtin");
const shared_event = @import("event.zig");

pub const HWND = ?*anyopaque;

const HINSTANCE = ?*anyopaque;
const HICON = ?*anyopaque;
const HCURSOR = ?*anyopaque;
const HBRUSH = ?*anyopaque;
const HMENU = ?*anyopaque;
const HDC = ?*anyopaque;
const HGLOBAL = ?*anyopaque;
const BOOL = i32;
const WPARAM = usize;
const LPARAM = isize;
const LRESULT = isize;
const ATOM = u16;

const WndProc = *const fn (HWND, u32, WPARAM, LPARAM) callconv(.winapi) LRESULT;

const POINT = extern struct {
    x: i32,
    y: i32,
};

const RECT = extern struct {
    left: i32,
    top: i32,
    right: i32,
    bottom: i32,
};

const MSG = extern struct {
    hwnd: HWND,
    message: u32,
    w_param: WPARAM,
    l_param: LPARAM,
    time: u32,
    pt: POINT,
    private: u32,
};

const PAINTSTRUCT = extern struct {
    hdc: HDC,
    erase: BOOL,
    paint: RECT,
    restore: BOOL,
    incremental_update: BOOL,
    reserved: [32]u8,
};

const CREATESTRUCTW = extern struct {
    create_params: ?*anyopaque,
    instance: HINSTANCE,
    menu: HMENU,
    parent: HWND,
    height: i32,
    width: i32,
    y: i32,
    x: i32,
    style: i32,
    name: ?[*:0]const u16,
    class_name: ?[*:0]const u16,
    ex_style: u32,
};

const WNDCLASSEXW = extern struct {
    size: u32,
    style: u32,
    wnd_proc: WndProc,
    class_extra: i32,
    window_extra: i32,
    instance: HINSTANCE,
    icon: HICON,
    cursor: HCURSOR,
    background: HBRUSH,
    menu_name: ?[*:0]const u16,
    class_name: [*:0]const u16,
    small_icon: HICON,
};

const BITMAPINFOHEADER = extern struct {
    size: u32,
    width: i32,
    height: i32,
    planes: u16,
    bit_count: u16,
    compression: u32,
    image_size: u32,
    x_pixels_per_meter: i32,
    y_pixels_per_meter: i32,
    colors_used: u32,
    colors_important: u32,
};

const RGBQUAD = extern struct {
    blue: u8,
    green: u8,
    red: u8,
    reserved: u8,
};

const BITMAPINFO = extern struct {
    header: BITMAPINFOHEADER,
    colors: [1]RGBQUAD,
};

const OPENFILENAMEW = extern struct {
    struct_size: u32,
    owner: HWND,
    instance: HINSTANCE,
    filter: ?[*:0]const u16,
    custom_filter: ?[*:0]u16,
    max_custom_filter: u32,
    filter_index: u32,
    file: [*:0]u16,
    max_file: u32,
    file_title: ?[*:0]u16,
    max_file_title: u32,
    initial_dir: ?[*:0]const u16,
    title: ?[*:0]const u16,
    flags: u32,
    file_offset: u16,
    file_extension: u16,
    default_extension: ?[*:0]const u16,
    custom_data: LPARAM,
    hook: ?*const anyopaque,
    template_name: ?[*:0]const u16,
    reserved: ?*anyopaque,
    reserved_flags: u32,
    flags_ex: u32,
};

const class_name = std.unicode.utf8ToUtf16LeStringLiteral("ComicChatZigWindow");

const cs_vredraw: u32 = 0x0001;
const cs_hredraw: u32 = 0x0002;
const cs_dblclks: u32 = 0x0008;

const ws_overlapped_window: u32 = 0x00cf0000;
const cw_use_default: i32 = std.math.minInt(i32);
const sw_show: i32 = 5;

const gwlp_userdata: i32 = -21;
const pm_remove: u32 = 0x0001;

const wm_destroy: u32 = 0x0002;
const wm_size: u32 = 0x0005;
const wm_paint: u32 = 0x000f;
const wm_close: u32 = 0x0010;
const wm_erasebkgnd: u32 = 0x0014;
const wm_keydown: u32 = 0x0100;
const wm_char: u32 = 0x0102;
const wm_unichar: u32 = 0x0109;
const wm_ime_char: u32 = 0x0286;
const wm_mousemove: u32 = 0x0200;
const wm_lbuttondown: u32 = 0x0201;
const wm_lbuttonup: u32 = 0x0202;
const wm_lbuttondblclk: u32 = 0x0203;
const wm_rbuttondown: u32 = 0x0204;
const wm_rbuttonup: u32 = 0x0205;
const wm_mbuttondown: u32 = 0x0207;
const wm_mbuttonup: u32 = 0x0208;
const wm_mousewheel: u32 = 0x020a;
const wm_nccreate: u32 = 0x0081;
const wm_ncdestroy: u32 = 0x0082;
const wm_dpichanged: u32 = 0x02e0;
const wm_quit: u32 = 0x0012;

const vk_back: usize = 0x08;
const vk_tab: usize = 0x09;
const vk_shift: usize = 0x10;
const vk_control: usize = 0x11;
const vk_menu: usize = 0x12;
const vk_return: usize = 0x0d;
const vk_escape: usize = 0x1b;
const vk_page_up: usize = 0x21;
const vk_page_down: usize = 0x22;
const vk_end: usize = 0x23;
const vk_home: usize = 0x24;
const vk_left: usize = 0x25;
const vk_up: usize = 0x26;
const vk_right: usize = 0x27;
const vk_down: usize = 0x28;
const vk_delete: usize = 0x2e;

const error_class_already_exists: u32 = 1410;
const idc_arrow: usize = 32512;
const dib_rgb_colors: u32 = 0;
const bi_rgb: u32 = 0;
const srccopy: u32 = 0x00cc0020;
const gdi_error: i32 = -1;
const cf_unicode_text: u32 = 13;
const gmem_moveable: u32 = 0x0002;
const ofn_overwrite_prompt: u32 = 0x00000002;
const ofn_no_change_dir: u32 = 0x00000008;
const ofn_path_must_exist: u32 = 0x00000800;
const ofn_file_must_exist: u32 = 0x00001000;
const ofn_explorer: u32 = 0x00080000;

extern "kernel32" fn GetModuleHandleW(name: ?[*:0]const u16) callconv(.winapi) HINSTANCE;
extern "kernel32" fn GetLastError() callconv(.winapi) u32;
extern "kernel32" fn GlobalAlloc(flags: u32, bytes: usize) callconv(.winapi) HGLOBAL;
extern "kernel32" fn GlobalFree(memory: HGLOBAL) callconv(.winapi) HGLOBAL;
extern "kernel32" fn GlobalLock(memory: HGLOBAL) callconv(.winapi) ?*anyopaque;
extern "kernel32" fn GlobalUnlock(memory: HGLOBAL) callconv(.winapi) BOOL;
extern "kernel32" fn GlobalSize(memory: HGLOBAL) callconv(.winapi) usize;

extern "user32" fn RegisterClassExW(class: *const WNDCLASSEXW) callconv(.winapi) ATOM;
extern "user32" fn LoadCursorW(instance: HINSTANCE, name: [*:0]const u16) callconv(.winapi) HCURSOR;
extern "user32" fn CreateWindowExW(
    ex_style: u32,
    class_name_: [*:0]const u16,
    window_name: [*:0]const u16,
    style: u32,
    x: i32,
    y: i32,
    width: i32,
    height: i32,
    parent: HWND,
    menu: HMENU,
    instance: HINSTANCE,
    param: ?*anyopaque,
) callconv(.winapi) HWND;
extern "user32" fn DefWindowProcW(hwnd: HWND, message: u32, w_param: WPARAM, l_param: LPARAM) callconv(.winapi) LRESULT;
extern "user32" fn DestroyWindow(hwnd: HWND) callconv(.winapi) BOOL;
extern "user32" fn ShowWindow(hwnd: HWND, command: i32) callconv(.winapi) BOOL;
extern "user32" fn UpdateWindow(hwnd: HWND) callconv(.winapi) BOOL;
extern "user32" fn AdjustWindowRectEx(rect: *RECT, style: u32, menu: BOOL, ex_style: u32) callconv(.winapi) BOOL;
extern "user32" fn GetClientRect(hwnd: HWND, rect: *RECT) callconv(.winapi) BOOL;
extern "user32" fn GetMessageW(message: *MSG, hwnd: HWND, first: u32, last: u32) callconv(.winapi) BOOL;
extern "user32" fn PeekMessageW(message: *MSG, hwnd: HWND, first: u32, last: u32, remove: u32) callconv(.winapi) BOOL;
extern "user32" fn TranslateMessage(message: *const MSG) callconv(.winapi) BOOL;
extern "user32" fn DispatchMessageW(message: *const MSG) callconv(.winapi) LRESULT;
extern "user32" fn SetWindowLongPtrW(hwnd: HWND, index: i32, value: isize) callconv(.winapi) isize;
extern "user32" fn GetWindowLongPtrW(hwnd: HWND, index: i32) callconv(.winapi) isize;
extern "user32" fn SetWindowLongW(hwnd: HWND, index: i32, value: i32) callconv(.winapi) i32;
extern "user32" fn GetWindowLongW(hwnd: HWND, index: i32) callconv(.winapi) i32;
extern "user32" fn BeginPaint(hwnd: HWND, paint: *PAINTSTRUCT) callconv(.winapi) HDC;
extern "user32" fn EndPaint(hwnd: HWND, paint: *const PAINTSTRUCT) callconv(.winapi) BOOL;
extern "user32" fn GetDC(hwnd: HWND) callconv(.winapi) HDC;
extern "user32" fn ReleaseDC(hwnd: HWND, dc: HDC) callconv(.winapi) i32;
extern "user32" fn SetWindowTextW(hwnd: HWND, text: [*:0]const u16) callconv(.winapi) BOOL;
extern "user32" fn SetProcessDPIAware() callconv(.winapi) BOOL;
extern "user32" fn SetProcessDpiAwarenessContext(context: ?*anyopaque) callconv(.winapi) BOOL;
extern "user32" fn GetDpiForSystem() callconv(.winapi) u32;
extern "user32" fn GetDpiForWindow(hwnd: HWND) callconv(.winapi) u32;
extern "user32" fn AdjustWindowRectExForDpi(rect: *RECT, style: u32, menu: BOOL, ex_style: u32, dpi: u32) callconv(.winapi) BOOL;
extern "user32" fn SetWindowPos(hwnd: HWND, insert_after: HWND, x: i32, y: i32, width: i32, height: i32, flags: u32) callconv(.winapi) BOOL;
extern "user32" fn ScreenToClient(hwnd: HWND, point: *POINT) callconv(.winapi) BOOL;
extern "user32" fn GetKeyState(key: i32) callconv(.winapi) i16;
extern "user32" fn OpenClipboard(owner: HWND) callconv(.winapi) BOOL;
extern "user32" fn CloseClipboard() callconv(.winapi) BOOL;
extern "user32" fn EmptyClipboard() callconv(.winapi) BOOL;
extern "user32" fn SetClipboardData(format: u32, memory: HGLOBAL) callconv(.winapi) ?*anyopaque;
extern "user32" fn GetClipboardData(format: u32) callconv(.winapi) HGLOBAL;
extern "user32" fn IsClipboardFormatAvailable(format: u32) callconv(.winapi) BOOL;
extern "user32" fn MessageBoxW(owner: HWND, text: [*:0]const u16, caption: [*:0]const u16, kind: u32) callconv(.winapi) i32;
extern "comdlg32" fn GetOpenFileNameW(file: *OPENFILENAMEW) callconv(.winapi) BOOL;
extern "comdlg32" fn GetSaveFileNameW(file: *OPENFILENAMEW) callconv(.winapi) BOOL;
extern "shell32" fn ShellExecuteW(owner: HWND, operation: [*:0]const u16, file: [*:0]const u16, parameters: ?[*:0]const u16, directory: ?[*:0]const u16, show: i32) callconv(.winapi) HINSTANCE;

extern "gdi32" fn StretchDIBits(
    dc: HDC,
    dest_x: i32,
    dest_y: i32,
    dest_width: i32,
    dest_height: i32,
    src_x: i32,
    src_y: i32,
    src_width: i32,
    src_height: i32,
    bits: *const anyopaque,
    info: *const BITMAPINFO,
    usage: u32,
    raster_op: u32,
) callconv(.winapi) i32;

// --- Events -----------------------------------------------------------------

pub const Key = shared_event.Key;
pub const Event = shared_event.Event;

fn currentModifiers() shared_event.Modifiers {
    return .{
        .shift = GetKeyState(@intCast(vk_shift)) < 0,
        .control = GetKeyState(@intCast(vk_control)) < 0,
        .alt = GetKeyState(@intCast(vk_menu)) < 0,
    };
}

/// Translate Win32 virtual-key values. Text-producing keys are delivered from
/// WM_CHAR in the actual event loop so Shift and the user's keyboard layout
/// are applied by Windows.
pub fn virtualKeyToKey(vk: usize) Key {
    return switch (vk) {
        vk_back => .backspace,
        vk_tab => .tab,
        vk_return => .enter,
        vk_escape => .escape,
        vk_left => .left,
        vk_right => .right,
        vk_up => .up,
        vk_down => .down,
        vk_home => .home,
        vk_end => .end,
        vk_page_up => .page_up,
        vk_page_down => .page_down,
        vk_delete => .delete,
        else => .other,
    };
}

/// Convert one standalone WM_CHAR UTF-16 code unit to the shared Key API.
/// Surrogate pairs are joined by Window.queueUtf16 rather than this pure helper.
pub fn charCodeUnitToKey(unit: u16) Key {
    return switch (unit) {
        0x08 => .backspace,
        0x09 => .tab,
        0x0d => .enter,
        0x1b => .escape,
        0x20...0xd7ff, 0xe000...0xffff => .{ .char = @intCast(unit) },
        else => .other,
    };
}

fn keyDownEvent(vk: usize) ?Key {
    return switch (vk) {
        vk_left,
        vk_right,
        vk_up,
        vk_down,
        vk_home,
        vk_end,
        vk_page_up,
        vk_page_down,
        vk_delete,
        => virtualKeyToKey(vk),
        else => null,
    };
}

// --- One-shot viewer ---------------------------------------------------------

/// Open a native window, draw `pixels` (0xAARRGGBB), and run until keypress or
/// close. This mirrors the convenience function in the X11 backend.
pub fn show(gpa: std.mem.Allocator, pixels: []const u32, w: u32, h: u32) !void {
    const window = try Window.open(gpa, w, h, "comicchat");
    defer window.deinit();
    try window.present(pixels, w, h);
    while (true) {
        switch (try window.nextEvent()) {
            .key, .close => return,
            .expose => try window.present(pixels, w, h),
            else => {},
        }
    }
}

// --- Interactive window -----------------------------------------------------

/// A heap-pinned native Win32 window. Open, pump events, present, and destroy
/// it on the same OS thread.
pub const Window = struct {
    gpa: std.mem.Allocator,
    hwnd: HWND,
    width: u32,
    height: u32,
    pixel_width: u32,
    pixel_height: u32,
    dpi: u32,
    pending: ?Event,
    pending_high_surrogate: ?u16,

    pub fn open(gpa: std.mem.Allocator, w: u32, h: u32, title: []const u8) !*Window {
        if (builtin.os.tag != .windows) return error.UnsupportedPlatform;
        if (w == 0 or h == 0 or w > std.math.maxInt(i32) or h > std.math.maxInt(i32)) {
            return error.InvalidWindowSize;
        }

        const instance = GetModuleHandleW(null) orelse return error.ModuleHandleUnavailable;
        const per_monitor_v2: ?*anyopaque = @ptrFromInt(@as(usize, @bitCast(@as(isize, -4))));
        if (SetProcessDpiAwarenessContext(per_monitor_v2) == 0) _ = SetProcessDPIAware();
        try registerWindowClass(instance);

        var outer = RECT{
            .left = 0,
            .top = 0,
            .right = @intCast(w),
            .bottom = @intCast(h),
        };
        const system_dpi = @max(@as(u32, 96), GetDpiForSystem());
        outer.right = @intCast(@divTrunc(@as(u64, w) * system_dpi + 95, 96));
        outer.bottom = @intCast(@divTrunc(@as(u64, h) * system_dpi + 95, 96));
        if (AdjustWindowRectExForDpi(&outer, ws_overlapped_window, 0, 0, system_dpi) == 0 and AdjustWindowRectEx(&outer, ws_overlapped_window, 0, 0) == 0) {
            return error.AdjustWindowRectFailed;
        }
        const outer_width = @as(i64, outer.right) - @as(i64, outer.left);
        const outer_height = @as(i64, outer.bottom) - @as(i64, outer.top);
        if (outer_width <= 0 or outer_width > std.math.maxInt(i32) or
            outer_height <= 0 or outer_height > std.math.maxInt(i32))
        {
            return error.InvalidWindowSize;
        }

        const title_w = try std.unicode.utf8ToUtf16LeAllocZ(gpa, title);
        defer gpa.free(title_w);

        const self = try gpa.create(Window);
        errdefer gpa.destroy(self);
        self.* = .{
            .gpa = gpa,
            .hwnd = null,
            .width = w,
            .height = h,
            .pixel_width = w,
            .pixel_height = h,
            .dpi = system_dpi,
            .pending = null,
            .pending_high_surrogate = null,
        };

        const hwnd = CreateWindowExW(
            0,
            class_name,
            title_w.ptr,
            ws_overlapped_window,
            cw_use_default,
            cw_use_default,
            @intCast(outer_width),
            @intCast(outer_height),
            null,
            null,
            instance,
            self,
        ) orelse return error.CreateWindowFailed;
        self.hwnd = hwnd;
        errdefer _ = DestroyWindow(hwnd);

        _ = ShowWindow(hwnd, sw_show);
        _ = UpdateWindow(hwnd);

        var client: RECT = undefined;
        if (GetClientRect(hwnd, &client) == 0) return error.GetClientRectFailed;
        self.pixel_width = @intCast(@max(client.right - client.left, 0));
        self.pixel_height = @intCast(@max(client.bottom - client.top, 0));
        self.dpi = @max(@as(u32, 96), GetDpiForWindow(hwnd));
        self.width = physicalToLogical(self.pixel_width, self.dpi);
        self.height = physicalToLogical(self.pixel_height, self.dpi);
        // Creation/show may synchronously dispatch resize and paint messages.
        // The first explicit present happens immediately after open.
        self.pending = null;
        return self;
    }

    pub fn deinit(self: *Window) void {
        if (self.hwnd) |hwnd| {
            // Detach first so even a wrong-thread DestroyWindow failure cannot
            // leave Win32 holding a pointer to freed Zig state.
            _ = setWindowLongPtr(hwnd, gwlp_userdata, 0);
            self.hwnd = null;
            _ = DestroyWindow(hwnd);
        }
        self.gpa.destroy(self);
    }

    /// Native handle for integrations that need MsgWaitForMultipleObjects or
    /// other Windows-specific coordination. It is not a pollable file handle.
    pub fn nativeHandle(self: *const Window) HWND {
        return self.hwnd;
    }

    pub fn setTitle(self: *Window, title: []const u8) !void {
        const hwnd = self.hwnd orelse return error.WindowClosed;
        const title_w = try std.unicode.utf8ToUtf16LeAllocZ(self.gpa, title);
        defer self.gpa.free(title_w);
        if (SetWindowTextW(hwnd, title_w.ptr) == 0) return error.SetTitleFailed;
    }

    pub fn writeClipboard(self: *Window, text: []const u8) !void {
        const hwnd = self.hwnd orelse return error.WindowClosed;
        const wide = try std.unicode.utf8ToUtf16LeAllocZ(self.gpa, text);
        defer self.gpa.free(wide);
        const bytes = try std.math.mul(usize, wide.len + 1, @sizeOf(u16));
        const memory = GlobalAlloc(gmem_moveable, bytes) orelse return error.ClipboardAllocationFailed;
        var transferred = false;
        defer if (!transferred) {
            _ = GlobalFree(memory);
        };
        const raw = GlobalLock(memory) orelse return error.ClipboardLockFailed;
        const destination: [*]u16 = @ptrCast(@alignCast(raw));
        @memcpy(destination[0 .. wide.len + 1], wide[0 .. wide.len + 1]);
        _ = GlobalUnlock(memory);
        if (OpenClipboard(hwnd) == 0) return error.ClipboardUnavailable;
        defer _ = CloseClipboard();
        if (EmptyClipboard() == 0) return error.ClipboardUnavailable;
        if (SetClipboardData(cf_unicode_text, memory) == null) return error.ClipboardUnavailable;
        transferred = true;
    }

    pub fn readClipboard(self: *Window, gpa: std.mem.Allocator) !?[]u8 {
        const hwnd = self.hwnd orelse return error.WindowClosed;
        if (IsClipboardFormatAvailable(cf_unicode_text) == 0) return null;
        if (OpenClipboard(hwnd) == 0) return error.ClipboardUnavailable;
        defer _ = CloseClipboard();
        const memory = GetClipboardData(cf_unicode_text) orelse return null;
        const raw = GlobalLock(memory) orelse return error.ClipboardLockFailed;
        defer _ = GlobalUnlock(memory);
        const units = GlobalSize(memory) / @sizeOf(u16);
        if (units == 0 or units > 1024 * 1024) return error.ClipboardTooLarge;
        const source: [*]const u16 = @ptrCast(@alignCast(raw));
        var len: usize = 0;
        while (len < units and source[len] != 0) : (len += 1) {}
        return try std.unicode.utf16LeToUtf8Alloc(gpa, source[0..len]);
    }

    pub fn chooseFile(self: *Window, gpa: std.mem.Allocator, save: bool, title: []const u8) !?[]u8 {
        const hwnd = self.hwnd orelse return error.WindowClosed;
        var path: [32768]u16 = @splat(0);
        const title_w = try std.unicode.utf8ToUtf16LeAllocZ(gpa, title);
        defer gpa.free(title_w);
        const filter = std.unicode.utf8ToUtf16LeStringLiteral("Comic Chat files\x00*.ccc;*.ccr;*.ccrules;*.png;*.pdf\x00All files\x00*.*\x00");
        var options = OPENFILENAMEW{
            .struct_size = @sizeOf(OPENFILENAMEW),
            .owner = hwnd,
            .instance = null,
            .filter = filter,
            .custom_filter = null,
            .max_custom_filter = 0,
            .filter_index = 1,
            .file = @ptrCast(&path),
            .max_file = path.len,
            .file_title = null,
            .max_file_title = 0,
            .initial_dir = null,
            .title = title_w.ptr,
            .flags = ofn_explorer | ofn_no_change_dir | ofn_path_must_exist |
                (if (save) ofn_overwrite_prompt else ofn_file_must_exist),
            .file_offset = 0,
            .file_extension = 0,
            .default_extension = null,
            .custom_data = 0,
            .hook = null,
            .template_name = null,
            .reserved = null,
            .reserved_flags = 0,
            .flags_ex = 0,
        };
        const chosen = if (save) GetSaveFileNameW(&options) else GetOpenFileNameW(&options);
        if (chosen == 0) return null;
        var len: usize = 0;
        while (len < path.len and path[len] != 0) : (len += 1) {}
        if (len == path.len) return error.PathTooLong;
        return try std.unicode.utf16LeToUtf8Alloc(gpa, path[0..len]);
    }

    pub fn openPath(self: *Window, gpa: std.mem.Allocator, path: []const u8) !void {
        return self.shellPath(gpa, "open", path);
    }

    pub fn printPath(self: *Window, gpa: std.mem.Allocator, path: []const u8) !void {
        return self.shellPath(gpa, "print", path);
    }

    fn shellPath(self: *Window, gpa: std.mem.Allocator, operation: []const u8, path: []const u8) !void {
        const hwnd = self.hwnd orelse return error.WindowClosed;
        const operation_w = try std.unicode.utf8ToUtf16LeAllocZ(gpa, operation);
        defer gpa.free(operation_w);
        const path_w = try std.unicode.utf8ToUtf16LeAllocZ(gpa, path);
        defer gpa.free(path_w);
        const result = ShellExecuteW(hwnd, operation_w.ptr, path_w.ptr, null, null, sw_show);
        if (@intFromPtr(result) <= 32) return error.ShellActionFailed;
    }

    pub fn notify(self: *Window, gpa: std.mem.Allocator, title: []const u8, body: []const u8) !void {
        const hwnd = self.hwnd orelse return error.WindowClosed;
        const title_w = try std.unicode.utf8ToUtf16LeAllocZ(gpa, title);
        defer gpa.free(title_w);
        const body_w = try std.unicode.utf8ToUtf16LeAllocZ(gpa, body);
        defer gpa.free(body_w);
        _ = MessageBoxW(hwnd, body_w.ptr, title_w.ptr, 0x00000040);
    }

    /// Upload a complete, top-down 0xAARRGGBB framebuffer. A 32-bit BI_RGB DIB
    /// has the same little-endian B,G,R,A byte order, so no conversion/copy is
    /// needed on Windows.
    pub fn present(self: *Window, pixels: []const u32, w: u32, h: u32) !void {
        const hwnd = self.hwnd orelse return error.WindowClosed;
        if (w == 0 or h == 0 or w > std.math.maxInt(i32) or h > std.math.maxInt(i32)) {
            return error.InvalidFramebufferSize;
        }
        const count = std.math.mul(usize, @as(usize, w), @as(usize, h)) catch {
            return error.InvalidFramebufferSize;
        };
        if (pixels.len != count) return error.BadFramebufferSize;

        const info = bitmapInfo(w, h);
        const dc = GetDC(hwnd) orelse return error.GetDcFailed;
        defer _ = ReleaseDC(hwnd, dc);
        const copied = StretchDIBits(
            dc,
            0,
            0,
            @intCast(self.pixel_width),
            @intCast(self.pixel_height),
            0,
            0,
            @intCast(w),
            @intCast(h),
            pixels.ptr,
            &info,
            dib_rgb_colors,
            srccopy,
        );
        if (copied == 0 or copied == gdi_error) return error.PresentFailed;
    }

    /// Block until the next event for this thread. Unlike X11 there is no
    /// socket fd; use `pollEvent` when coordinating with another event source.
    pub fn nextEvent(self: *Window) !Event {
        if (self.takePending()) |event| return event;
        while (true) {
            var message: MSG = undefined;
            const result = GetMessageW(&message, null, 0, 0);
            if (result == -1) return error.MessagePumpFailed;
            if (result == 0) return .close;
            _ = TranslateMessage(&message);
            _ = DispatchMessageW(&message);
            if (self.takePending()) |event| return event;
        }
    }

    /// Drain queued Win32 messages without blocking. Returns null when the
    /// thread queue is empty, making this the Win32 equivalent of polling the
    /// X11 connection fd before calling `nextEvent`.
    pub fn pollEvent(self: *Window) !?Event {
        if (self.takePending()) |event| return event;
        while (true) {
            var message: MSG = undefined;
            if (PeekMessageW(&message, null, 0, 0, pm_remove) == 0) return null;
            if (message.message == wm_quit) return Event.close;
            _ = TranslateMessage(&message);
            _ = DispatchMessageW(&message);
            if (self.takePending()) |event| return event;
        }
    }

    fn takePending(self: *Window) ?Event {
        const event = self.pending orelse return null;
        self.pending = null;
        return event;
    }

    fn queue(self: *Window, event: Event) void {
        // One DispatchMessage call creates at most one user-facing event. Keep
        // an earlier synchronous event if Windows nests another notification.
        if (self.pending == null) self.pending = event;
    }

    fn queueUtf16(self: *Window, unit: u16) void {
        if (unit >= 0xd800 and unit <= 0xdbff) {
            self.pending_high_surrogate = unit;
            return;
        }
        if (unit >= 0xdc00 and unit <= 0xdfff) {
            if (self.pending_high_surrogate) |high| {
                self.pending_high_surrogate = null;
                const codepoint: u21 = @intCast(0x10000 +
                    (@as(u32, high - 0xd800) << 10) + @as(u32, unit - 0xdc00));
                self.queue(.{ .key = .{ .key = .{ .char = codepoint }, .modifiers = currentModifiers() } });
            }
            return;
        }
        self.pending_high_surrogate = null;
        self.queue(.{ .key = .{ .key = charCodeUnitToKey(unit), .modifiers = currentModifiers() } });
    }
};

fn registerWindowClass(instance: HINSTANCE) !void {
    const cursor_name: [*:0]const u16 = @ptrFromInt(idc_arrow);
    const cursor = LoadCursorW(null, cursor_name) orelse return error.LoadCursorFailed;
    const class = WNDCLASSEXW{
        .size = @sizeOf(WNDCLASSEXW),
        .style = cs_hredraw | cs_vredraw | cs_dblclks,
        .wnd_proc = windowProc,
        .class_extra = 0,
        .window_extra = 0,
        .instance = instance,
        .icon = null,
        .cursor = cursor,
        .background = null,
        .menu_name = null,
        .class_name = class_name,
        .small_icon = null,
    };
    if (RegisterClassExW(&class) == 0 and GetLastError() != error_class_already_exists) {
        return error.RegisterClassFailed;
    }
}

fn windowFromHandle(hwnd: HWND) ?*Window {
    const value = getWindowLongPtr(hwnd, gwlp_userdata);
    if (value == 0) return null;
    const address: usize = @bitCast(value);
    return @ptrFromInt(address);
}

fn windowProc(hwnd: HWND, message: u32, w_param: WPARAM, l_param: LPARAM) callconv(.winapi) LRESULT {
    var self = windowFromHandle(hwnd);
    if (message == wm_nccreate) {
        const address: usize = @bitCast(l_param);
        const create: *const CREATESTRUCTW = @ptrFromInt(address);
        if (create.create_params) |raw| {
            self = @ptrCast(@alignCast(raw));
            self.?.hwnd = hwnd;
            const pointer_bits: isize = @bitCast(@intFromPtr(self.?));
            _ = setWindowLongPtr(hwnd, gwlp_userdata, pointer_bits);
        }
    }

    if (self) |window| switch (message) {
        wm_close => {
            window.queue(.close);
            return 0;
        },
        wm_destroy => {
            window.queue(.close);
            return 0;
        },
        wm_ncdestroy => {
            window.hwnd = null;
            _ = setWindowLongPtr(hwnd, gwlp_userdata, 0);
        },
        wm_size => {
            const dimensions: usize = @bitCast(l_param);
            const w: u32 = @as(u16, @truncate(dimensions));
            const h: u32 = @as(u16, @truncate(dimensions >> 16));
            if (w != 0 and h != 0) {
                window.pixel_width = w;
                window.pixel_height = h;
                const logical_w = physicalToLogical(w, window.dpi);
                const logical_h = physicalToLogical(h, window.dpi);
                if (logical_w != window.width or logical_h != window.height) {
                    window.width = logical_w;
                    window.height = logical_h;
                    window.queue(.{ .resize = .{ .w = logical_w, .h = logical_h } });
                }
            }
            return 0;
        },
        wm_dpichanged => {
            window.dpi = @max(@as(u32, 96), @as(u16, @truncate(w_param)));
            const suggested: *const RECT = @ptrFromInt(@as(usize, @bitCast(l_param)));
            _ = SetWindowPos(hwnd, null, suggested.left, suggested.top, suggested.right - suggested.left, suggested.bottom - suggested.top, 0x0014);
            return 0;
        },
        wm_paint => {
            var paint: PAINTSTRUCT = undefined;
            _ = BeginPaint(hwnd, &paint);
            _ = EndPaint(hwnd, &paint);
            window.queue(.expose);
            return 0;
        },
        wm_erasebkgnd => return 1,
        wm_keydown => {
            if (keyDownEvent(w_param)) |key| {
                window.queue(.{ .key = .{ .key = key, .modifiers = currentModifiers() } });
                return 0;
            }
        },
        wm_char, wm_ime_char => {
            window.queueUtf16(@truncate(w_param));
            return 0;
        },
        wm_unichar => {
            if (w_param == 0xffff) return 1;
            if (w_param <= 0x10ffff) {
                window.queue(.{ .key = .{ .key = .{ .char = @intCast(w_param) }, .modifiers = currentModifiers() } });
                return 0;
            }
        },
        wm_mousemove,
        wm_lbuttondown,
        wm_lbuttonup,
        wm_lbuttondblclk,
        wm_rbuttondown,
        wm_rbuttonup,
        wm_mbuttondown,
        wm_mbuttonup,
        => {
            const bits: usize = @bitCast(l_param);
            const x = physicalPointToLogical(@as(i16, @bitCast(@as(u16, @truncate(bits)))), window.dpi);
            const y = physicalPointToLogical(@as(i16, @bitCast(@as(u16, @truncate(bits >> 16)))), window.dpi);
            const kind: shared_event.PointerKind = if (message == wm_mousemove)
                .move
            else if (message == wm_lbuttondown or message == wm_lbuttondblclk or message == wm_rbuttondown or message == wm_mbuttondown)
                .down
            else
                .up;
            const button: shared_event.PointerButton = switch (message) {
                wm_lbuttondown, wm_lbuttonup, wm_lbuttondblclk => .primary,
                wm_rbuttondown, wm_rbuttonup => .secondary,
                wm_mbuttondown, wm_mbuttonup => .middle,
                else => .none,
            };
            window.queue(.{ .pointer = .{ .kind = kind, .x = x, .y = y, .button = button, .clicks = if (message == wm_lbuttondblclk) 2 else 1 } });
            return 0;
        },
        wm_mousewheel => {
            const bits: usize = @bitCast(l_param);
            var point = POINT{
                .x = @as(i16, @bitCast(@as(u16, @truncate(bits)))),
                .y = @as(i16, @bitCast(@as(u16, @truncate(bits >> 16)))),
            };
            _ = ScreenToClient(hwnd, &point);
            point.x = physicalPointToLogical(point.x, window.dpi);
            point.y = physicalPointToLogical(point.y, window.dpi);
            const delta: i16 = @bitCast(@as(u16, @truncate(w_param >> 16)));
            window.queue(.{ .pointer = .{
                .kind = .wheel,
                .x = point.x,
                .y = point.y,
                .wheel_y = if (delta > 0) 1 else if (delta < 0) -1 else 0,
            } });
            return 0;
        },
        else => {},
    };

    return DefWindowProcW(hwnd, message, w_param, l_param);
}

fn physicalToLogical(value: u32, dpi: u32) u32 {
    return @max(@as(u32, 1), @as(u32, @intCast(@divTrunc(@as(u64, value) * 96 + dpi / 2, dpi))));
}

fn physicalPointToLogical(value: i32, dpi: u32) i32 {
    return @intCast(@divTrunc(@as(i64, value) * 96, dpi));
}

fn setWindowLongPtr(hwnd: HWND, index: i32, value: isize) isize {
    if (comptime @sizeOf(usize) == 4) {
        return SetWindowLongW(hwnd, index, @truncate(value));
    }
    return SetWindowLongPtrW(hwnd, index, value);
}

fn getWindowLongPtr(hwnd: HWND, index: i32) isize {
    if (comptime @sizeOf(usize) == 4) return GetWindowLongW(hwnd, index);
    return GetWindowLongPtrW(hwnd, index);
}

fn bitmapInfo(w: u32, h: u32) BITMAPINFO {
    return .{
        .header = .{
            .size = @sizeOf(BITMAPINFOHEADER),
            .width = @intCast(w),
            // Negative height selects top-down scanline order.
            .height = -@as(i32, @intCast(h)),
            .planes = 1,
            .bit_count = 32,
            .compression = bi_rgb,
            .image_size = 0,
            .x_pixels_per_meter = 0,
            .y_pixels_per_meter = 0,
            .colors_used = 0,
            .colors_important = 0,
        },
        .colors = .{.{ .blue = 0, .green = 0, .red = 0, .reserved = 0 }},
    };
}

// --- Tests ------------------------------------------------------------------

test "Win32 virtual-key and WM_CHAR translation matches shared input API" {
    try std.testing.expectEqual(Key.left, virtualKeyToKey(vk_left));
    try std.testing.expectEqual(Key.page_down, virtualKeyToKey(vk_page_down));
    try std.testing.expectEqual(Key.delete, virtualKeyToKey(vk_delete));
    try std.testing.expectEqual(Key.other, virtualKeyToKey('A'));
    try std.testing.expectEqual(Key{ .char = 'A' }, charCodeUnitToKey('A'));
    try std.testing.expectEqual(Key.backspace, charCodeUnitToKey(0x08));
    try std.testing.expectEqual(Key.enter, charCodeUnitToKey(0x0d));
    try std.testing.expectEqual(Key{ .char = 0x20ac }, charCodeUnitToKey(0x20ac));
}

test "Win32 DIB is 32-bit top-down BGRX-compatible" {
    const info = bitmapInfo(640, 480);
    try std.testing.expectEqual(@as(u32, 40), info.header.size);
    try std.testing.expectEqual(@as(i32, 640), info.header.width);
    try std.testing.expectEqual(@as(i32, -480), info.header.height);
    try std.testing.expectEqual(@as(u16, 1), info.header.planes);
    try std.testing.expectEqual(@as(u16, 32), info.header.bit_count);

    const pixel: u32 = 0xaa112233;
    const bytes = std.mem.asBytes(&pixel);
    try std.testing.expectEqualSlices(u8, &.{ 0x33, 0x22, 0x11, 0xaa }, bytes);
}

test "Win32 x86_64 ABI structure sizes" {
    if (@sizeOf(usize) != 8) return error.SkipZigTest;
    try std.testing.expectEqual(@as(usize, 16), @sizeOf(RECT));
    try std.testing.expectEqual(@as(usize, 40), @sizeOf(BITMAPINFOHEADER));
    try std.testing.expectEqual(@as(usize, 48), @sizeOf(MSG));
    try std.testing.expectEqual(@as(usize, 72), @sizeOf(PAINTSTRUCT));
    try std.testing.expectEqual(@as(usize, 80), @sizeOf(CREATESTRUCTW));
    try std.testing.expectEqual(@as(usize, 80), @sizeOf(WNDCLASSEXW));
    try std.testing.expectEqual(@as(usize, 152), @sizeOf(OPENFILENAMEW));
}

test "Win32 backend entry points compile for a Windows test target" {
    if (builtin.os.tag != .windows) return error.SkipZigTest;

    // This runtime test is intentionally not part of normal cross-compilation
    // execution; referencing the path forces semantic analysis and linkage of
    // the native calls when producing a Windows test binary.
    const window = try Window.open(std.testing.allocator, 64, 64, "Comic Chat");
    defer window.deinit();
    try window.setTitle("Comic Chat test");
    var pixels: [64 * 64]u32 = @splat(0xff000000);
    try window.present(&pixels, 64, 64);
    _ = try window.pollEvent();
}
