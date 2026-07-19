//! Source-shaped portable Comic Chat client view.
//!
//! The buffer geometry follows Microsoft's `CChatView` splitter composition:
//! room tabs above an 80/20 conversation/member split, a fixed-height say
//! window below the page/text view, and (in comic mode) a 30/70 member/bodycam
//! split on the right. The skin is modernized, but the spatial contract is not.

const std = @import("std");
const session = @import("../comic/session.zig");
const strip = @import("../comic/strip.zig");
const figure = @import("../comic/figure.zig");
const bgb = @import("../assets/bgb.zig");
const canvas_mod = @import("../render/canvas.zig");
const geometry = @import("geometry.zig");
const shell_mod = @import("shell.zig");
const hit_test = @import("hit_test.zig");
const dialogs = @import("dialogs.zig");
const input_mod = @import("input.zig");
const accessibility = @import("accessibility.zig");
const ui = @import("ui.zig");
const platform_event = @import("../platform/event.zig");
const source_ui = @import("source_ui_assets");

const Canvas = canvas_mod.Canvas;
const Rect = geometry.Rect;
const TextSelection = input_mod.Editor.Selection;

pub const min_width: u32 = 320;
pub const min_height: u32 = 240;

pub const Action = union(enum) {
    none,
    menu: u8,
    toolbar: u8,
    room_tab: usize,
    send,
    dialog_accept: dialogs.Id,
    dialog_cancel: dialogs.Id,
};

// Windows/Fluent neutral roles applied to the source UI geometry.
const ink = ui.Theme.ink;
const secondary = ui.Theme.secondary;
const chrome = ui.Theme.chrome;
const layer = ui.Theme.layer;
const subtle = ui.Theme.subtle;
const divider = ui.Theme.divider;
const accent = ui.Theme.accent;
const accent_soft = ui.Theme.accent_soft;
const focus_color = ui.Theme.focus;

pub const View = struct {
    gpa: std.mem.Allocator,
    canvas: Canvas,
    shell: shell_mod.State = .{},
    active_dialog: ?dialogs.Id = null,
    dialog_notice: []const u8 = "",
    active_menu: ?u8 = null,
    hovered_menu: ?u8 = null,
    hovered_menu_item: ?u8 = null,
    hovered_toolbar: ?u8 = null,
    dialog_editors: [3]input_mod.Editor,
    dialog_field: usize = 0,
    room_tab_count: usize = 1,

    pub fn init(gpa: std.mem.Allocator, initial_width: u32, initial_height: u32) !View {
        return .{
            .gpa = gpa,
            .canvas = try Canvas.init(gpa, @max(initial_width, min_width), @max(initial_height, min_height)),
            .dialog_editors = .{ input_mod.Editor.init(gpa), input_mod.Editor.init(gpa), input_mod.Editor.init(gpa) },
        };
    }

    pub fn deinit(self: *View) void {
        self.canvas.deinit(self.gpa);
        for (&self.dialog_editors) |*editor| editor.deinit();
    }

    pub fn resize(self: *View, new_width: u32, new_height: u32) !void {
        const w = @max(new_width, min_width);
        const h = @max(new_height, min_height);
        if (w == self.canvas.width and h == self.canvas.height) return;
        const replacement = try Canvas.init(self.gpa, w, h);
        self.canvas.deinit(self.gpa);
        self.canvas = replacement;
    }

    pub fn pixels(self: *const View) []const u32 {
        return self.canvas.px;
    }

    pub fn width(self: *const View) u32 {
        return self.canvas.width;
    }

    pub fn height(self: *const View) u32 {
        return self.canvas.height;
    }

    pub fn cycleFocus(self: *View) void {
        self.shell.cycleFocus();
    }

    pub fn cycleFocusBackward(self: *View) void {
        self.shell.cycleFocusBackward();
    }

    pub fn focusComposer(self: *View) void {
        self.shell.focusComposer();
    }

    pub fn pageEarlier(self: *View, total_lines: usize) void {
        self.shell.pageEarlier(total_lines, 9);
    }

    pub fn pageLater(self: *View) void {
        self.shell.pageLater(9);
    }

    pub fn jumpLatest(self: *View) void {
        self.shell.jumpLatest();
    }

    pub fn setContentMode(self: *View, mode: shell_mod.ContentMode) void {
        self.shell.setContentMode(mode);
    }

    pub fn toggleMembers(self: *View) void {
        self.shell.toggleMembers();
    }

    pub fn openDialog(self: *View, id: dialogs.Id) void {
        self.active_menu = null;
        for (&self.dialog_editors) |*editor| editor.clear();
        self.dialog_field = 0;
        self.dialog_notice = "";
        self.active_dialog = id;
    }

    pub fn openDialogByResource(self: *View, resource: []const u8) bool {
        const id = dialogs.fromResource(resource) orelse return false;
        self.openDialog(id);
        return true;
    }

    pub fn closeDialog(self: *View) bool {
        if (self.active_dialog == null) return false;
        self.active_dialog = null;
        return true;
    }

    /// Update transient pointer affordances.  The original client depended on
    /// raised Win32 controls; this port keeps the source geometry but gives the
    /// custom-drawn controls an equivalent hover response and plain-language
    /// status hint.
    pub fn handlePointerMove(self: *View, pointer: platform_event.Pointer, member_count: usize) bool {
        if (self.active_dialog != null) return self.setHover(null, null);
        if (self.active_menu) |menu| {
            const layout = geometry.Layout.compute(self.canvas.width, self.canvas.height, self.shell.content_mode == .comic, self.shell.show_members);
            const target = hit_test.shell(layout, self.shell.content_mode == .comic, pointer.x, pointer.y, member_count);
            if (target == .menu) {
                const next_menu = target.menu;
                const changed = self.active_menu != next_menu or self.hovered_menu != next_menu or self.hovered_menu_item != null;
                self.active_menu = next_menu;
                self.hovered_menu = next_menu;
                self.hovered_menu_item = null;
                self.hovered_toolbar = null;
                return changed;
            }
            const item = menuPopupItem(menu, pointer.x, pointer.y);
            const changed = self.hovered_menu_item != item or self.hovered_menu != null or self.hovered_toolbar != null;
            self.hovered_menu_item = item;
            self.hovered_menu = null;
            self.hovered_toolbar = null;
            return changed;
        }
        const comic_mode = self.shell.content_mode == .comic;
        const layout = geometry.Layout.compute(self.canvas.width, self.canvas.height, comic_mode, self.shell.show_members);
        const target = hit_test.shell(layout, comic_mode, pointer.x, pointer.y, member_count);
        return switch (target) {
            .menu => |index| self.setHover(index, null),
            .toolbar => |index| self.setHover(null, index),
            else => self.setHover(null, null),
        };
    }

    fn setHover(self: *View, menu: ?u8, toolbar: ?u8) bool {
        const changed = self.hovered_menu != menu or self.hovered_menu_item != null or self.hovered_toolbar != toolbar;
        self.hovered_menu = menu;
        self.hovered_menu_item = null;
        self.hovered_toolbar = toolbar;
        return changed;
    }

    pub fn dialogValue(self: *const View) []const u8 {
        return self.dialog_editors[0].text();
    }

    pub fn dialogValueAt(self: *const View, index: usize) []const u8 {
        return self.dialog_editors[@min(index, self.dialog_editors.len - 1)].text();
    }

    pub fn setDialogNotice(self: *View, notice: []const u8) void {
        self.dialog_notice = notice;
    }

    pub fn handleDialogKey(self: *View, key: platform_event.Key) !?Action {
        const id = self.active_dialog orelse return null;
        const field_count = @min(dialogs.fields(id).len, self.dialog_editors.len);
        const editor = &self.dialog_editors[self.dialog_field];
        switch (key) {
            .escape => {
                self.active_dialog = null;
                return .{ .dialog_cancel = id };
            },
            .enter => {
                return .{ .dialog_accept = id };
            },
            .tab => {
                if (field_count > 0) self.dialog_field = (self.dialog_field + 1) % field_count;
            },
            .char => |ch| if (dialogs.acceptsText(id) and editor.text().len < 512) try editor.insert(ch),
            .backspace => editor.backspace(),
            .delete => editor.delete(),
            .left => editor.left(),
            .right => editor.right(),
            .home => editor.home(),
            .end => editor.end(),
            else => {},
        }
        return .none;
    }

    pub fn handlePointer(self: *View, pointer: platform_event.Pointer, total_lines: usize, member_count: usize) Action {
        if (self.active_dialog) |id| {
            if (pointer.kind != .down or pointer.button != .primary) return .none;
            const spec = dialogs.get(id);
            const dialog_layout = dialogLayout(self.canvas.width, self.canvas.height, spec);
            if (ui.contains(dialog_layout.primary, pointer.x, pointer.y)) return .{ .dialog_accept = id };
            if (ui.contains(dialog_layout.cancel, pointer.x, pointer.y)) {
                self.active_dialog = null;
                return .{ .dialog_cancel = id };
            }
            if (dialog_layout.fieldIndexAt(pointer.y)) |index| {
                if (index < self.dialog_editors.len) self.dialog_field = index;
            }
            return .none;
        }
        if (self.active_menu) |menu| {
            if (pointer.kind != .down or pointer.button != .primary) return .none;
            self.active_menu = null;
            if (menuPopupItem(menu, pointer.x, pointer.y)) |item| self.invokeMenuItem(menu, item);
            return .{ .menu = menu };
        }
        const comic_mode = self.shell.content_mode == .comic;
        const layout = geometry.Layout.compute(self.canvas.width, self.canvas.height, comic_mode, self.shell.show_members);
        const target = hit_test.shell(layout, comic_mode, pointer.x, pointer.y, member_count);
        if (pointer.kind == .wheel) {
            switch (target) {
                .transcript => if (pointer.wheel_y > 0) self.pageEarlier(total_lines) else if (pointer.wheel_y < 0) self.pageLater(),
                else => {},
            }
            return .none;
        }
        if (pointer.kind != .down or pointer.button != .primary) return .none;
        return switch (target) {
            .none => .none,
            .menu => |index| menu: {
                self.active_menu = index;
                self.shell.focus = .navigation;
                break :menu .{ .menu = index };
            },
            .toolbar => |index| toolbar: {
                switch (index) {
                    0 => self.openDialog(.setup),
                    2 => self.openDialog(.channel),
                    4 => self.openDialog(.channel_create),
                    5 => self.setContentMode(.comic),
                    6 => self.setContentMode(.text),
                    7 => self.openDialog(.room_list),
                    8 => self.toggleMembers(),
                    9 => self.openDialog(.room_list),
                    10 => self.openDialog(.away),
                    11 => self.openDialog(.personal),
                    13 => self.openDialog(.whisper),
                    17 => self.openDialog(.set_text_font),
                    18 => self.openDialog(.choose_color),
                    else => {},
                }
                break :toolbar .{ .toolbar = index };
            },
            .room_tab => room: {
                self.shell.focus = .navigation;
                const first_x = layout.tabs.x + 82;
                const tab_width: i32 = 140;
                const raw = @divTrunc(pointer.x - first_x, tab_width);
                if (raw < 0) break :room .none;
                const index: usize = @intCast(raw);
                if (index >= self.room_tab_count) break :room .none;
                break :room .{ .room_tab = index };
            },
            .transcript => focus: {
                self.shell.focus = .transcript;
                break :focus .none;
            },
            .composer => focus: {
                self.shell.focus = .composer;
                break :focus .none;
            },
            .say_action => |index| say: {
                self.shell.setSayMode(@enumFromInt(index));
                break :say .send;
            },
            .member => |index| selected: {
                self.shell.selectMember(index);
                break :selected .none;
            },
            .emotion => emotion: {
                const wheel_side = @min(layout.body_camera.w, 159);
                if (wheel_side >= 93) {
                    const cx = layout.body_camera.x + @divTrunc(layout.body_camera.w, 2);
                    const cy = layout.body_camera.bottom() - @divTrunc(wheel_side, 2);
                    self.shell.setEmotionPoint(pointer.x - cx, pointer.y - cy, @max(1, @divTrunc(wheel_side, 2) - 12));
                } else self.shell.focus = .emotion;
                break :emotion .none;
            },
        };
    }

    /// Render the complete Microsoft-shaped application frame and chat buffer.
    pub fn render(
        self: *View,
        title: []const u8,
        status: []const u8,
        transcript: *const session.Transcript,
        input: []const u8,
        cursor: usize,
    ) !void {
        const tabs = [_]Tab{.{ .label = title }};
        return self.renderTabs(status, transcript, input, cursor, null, &tabs, 0);
    }

    pub const Tab = struct { label: []const u8, unread: u32 = 0 };

    pub fn renderTabs(
        self: *View,
        status: []const u8,
        transcript: *const session.Transcript,
        input: []const u8,
        cursor: usize,
        selection: ?TextSelection,
        tabs: []const Tab,
        active_tab: usize,
    ) !void {
        self.room_tab_count = tabs.len;
        const comic_mode = self.shell.content_mode == .comic;
        const layout = geometry.Layout.compute(self.canvas.width, self.canvas.height, comic_mode, self.shell.show_members);
        self.canvas.clear(chrome);

        drawMenuBar(&self.canvas, layout.menu, self.active_menu, self.hovered_menu);
        drawToolBar(&self.canvas, layout.toolbar, comic_mode, self.hovered_toolbar);
        drawTabBar(&self.canvas, layout.tabs, tabs, active_tab, self.shell.focus == .navigation);
        drawSplitters(&self.canvas, layout, comic_mode);

        if (comic_mode) {
            try self.drawComicBuffer(layout.transcript, transcript);
        } else {
            self.drawTextBuffer(layout.transcript, transcript);
        }
        if (self.shell.show_members) {
            try self.drawMemberList(layout.members, transcript, comic_mode);
            if (comic_mode) try self.drawBodyCamera(layout.body_camera, transcript);
        }
        drawSayWindow(&self.canvas, layout, input, cursor, selection, self.shell.focus == .composer, self.shell.say_mode);
        drawStatusBar(&self.canvas, layout.status, self.hoveredToolbarLabel() orelse status, transcript.roster.items.len);

        if (self.shell.focus == .transcript) drawFocus(&self.canvas, layout.transcript);
        if (self.shell.focus == .members) drawFocus(&self.canvas, layout.members);
        if (self.shell.focus == .emotion) drawFocus(&self.canvas, layout.body_camera);
        if (self.active_menu) |menu| drawMenuPopup(&self.canvas, menu, self.hovered_menu_item);
        if (self.active_dialog) |id| drawDialog(&self.canvas, dialogs.get(id), &self.dialog_editors, self.dialog_field, self.dialog_notice);
    }

    fn hoveredToolbarLabel(self: *const View) ?[]const u8 {
        const index = self.hovered_toolbar orelse return null;
        return toolbarLabel(index);
    }

    pub fn semanticSnapshot(self: *const View, status: []const u8, tabs: []const Tab, active_tab: usize) accessibility.Snapshot {
        const comic_mode = self.shell.content_mode == .comic;
        const layout = geometry.Layout.compute(self.canvas.width, self.canvas.height, comic_mode, self.shell.show_members);
        var snapshot: accessibility.Snapshot = .{ .status = status };
        snapshot.append(.{ .id = "comicchat", .role = .window, .bounds = .{ .x = 0, .y = 0, .w = @intCast(self.canvas.width), .h = @intCast(self.canvas.height) }, .label = "Comic Chat" });
        snapshot.append(.{ .id = "menu", .role = .menu_bar, .bounds = layout.menu, .label = "Application menu", .focused = self.shell.focus == .navigation });
        snapshot.append(.{ .id = "toolbar", .role = .toolbar, .bounds = layout.toolbar, .label = "Application tools" });
        snapshot.append(.{ .id = "rooms", .role = .tab_list, .bounds = layout.tabs, .label = "Rooms", .focused = self.shell.focus == .navigation });
        const first_x = layout.tabs.x + 82;
        for (tabs, 0..) |tab, index| snapshot.append(.{
            .id = "room-tab",
            .role = .tab,
            .bounds = .{ .x = first_x + @as(i32, @intCast(index)) * 140, .y = layout.tabs.y + 2, .w = 140, .h = layout.tabs.h - 2 },
            .label = tab.label,
            .selected = index == active_tab,
            .focused = index == active_tab and self.shell.focus == .navigation,
        });
        snapshot.append(.{ .id = "transcript", .role = .transcript, .bounds = layout.transcript, .label = "Conversation", .focused = self.shell.focus == .transcript });
        if (self.shell.show_members) snapshot.append(.{ .id = "members", .role = .member_list, .bounds = layout.members, .label = "Members", .focused = self.shell.focus == .members });
        snapshot.append(.{ .id = "composer", .role = .composer, .bounds = layout.say_editor, .label = "Message", .focused = self.shell.focus == .composer });
        var action_index: i32 = 0;
        while (action_index < geometry.say_button_count) : (action_index += 1) snapshot.append(.{
            .id = "say-action",
            .role = .say_action,
            .bounds = .{ .x = layout.say_actions.x + action_index * geometry.say_button_size, .y = layout.say_actions.y, .w = geometry.say_button_size, .h = layout.say_actions.h },
            .label = sayActionLabel(@intCast(action_index)),
            .selected = @as(i32, @intFromEnum(self.shell.say_mode)) == action_index,
        });
        snapshot.append(.{ .id = "status", .role = .status, .bounds = layout.status, .label = status });
        if (self.active_dialog) |id| {
            const dialog_layout = dialogLayout(self.canvas.width, self.canvas.height, dialogs.get(id));
            snapshot.append(.{ .id = "dialog", .role = .dialog, .bounds = dialog_layout.rect, .label = dialogs.get(id).title, .focused = true });
            snapshot.append(.{ .id = "dialog-accept", .role = .button, .bounds = dialog_layout.primary, .label = dialogs.primaryLabel(id) });
            snapshot.append(.{ .id = "dialog-cancel", .role = .button, .bounds = dialog_layout.cancel, .label = "Cancel" });
        }
        return snapshot;
    }

    fn drawComicBuffer(self: *View, rect: Rect, transcript: *const session.Transcript) !void {
        self.canvas.fillRect(rect.x, rect.y, rect.w, rect.h, 0xffd8d8d8);
        if (rect.w <= 0 or rect.h <= 0) return;
        if (transcript.lines.items.len == 0) {
            drawEmptyBuffer(&self.canvas, rect, "No messages yet - type below and press Enter");
            return;
        }

        const all = transcript.lines.items;
        const range = self.shell.visibleRange(all.len, 9);
        const visible = all[range.start..range.end];
        const lines = try self.gpa.alloc(strip.Line, visible.len);
        defer self.gpa.free(lines);
        const target_views = try self.gpa.alloc([]strip.Participant, visible.len);
        var target_views_count: usize = 0;
        defer {
            for (target_views[0..target_views_count]) |targets| self.gpa.free(targets);
            self.gpa.free(target_views);
        }
        for (visible, 0..) |line, i| {
            const targets = try self.gpa.alloc(strip.Participant, line.talk_targets.len);
            target_views[i] = targets;
            target_views_count += 1;
            for (line.talk_targets, 0..) |target, target_index| targets[target_index] = .{
                .identity = target.nick,
                .display_name = target.nick,
                .avatar = target.avatar,
            };
            lines[i] = .{
                .identity = line.nick,
                .display_name = line.nick,
                .avatar = line.avatar,
                .text = line.text,
                .formatting = line.formatting,
                .pose_text = line.pose_text,
                .pose_state = line.pose_state,
                .talk_targets = targets,
                .modes = line.modes,
            };
        }

        const title_roster = try self.gpa.alloc(strip.TitleParticipant, transcript.roster.items.len);
        defer self.gpa.free(title_roster);
        for (transcript.roster.items, 0..) |member, index| title_roster[index] = .{
            .identity = member.nick,
            .display_name = member.nick,
            .avatar = member.avatar,
            .is_self = member.is_self,
            .sends = member.sends,
            .departed = member.departed,
        };

        var page = try strip.renderWithOptions(self.gpa, lines, .{ .title_roster = title_roster });
        defer page.deinit(self.gpa);
        blitFit(&self.canvas, page.pixels, page.width, page.height, rect.x + 3, rect.y + 3, rect.w - 6, rect.h - 6);

        if (self.shell.history_offset > 0) {
            const label = "Earlier messages - Page Down returns toward latest";
            self.canvas.fillRect(rect.x + 6, rect.y + 6, @min(rect.w - 12, Canvas.textWidth(label) + 12), 25, layer);
            _ = self.canvas.drawText(label, rect.x + 12, rect.y + 7, secondary);
        }
    }

    fn drawTextBuffer(self: *View, rect: Rect, transcript: *const session.Transcript) void {
        self.canvas.fillRect(rect.x, rect.y, rect.w, rect.h, layer);
        if (transcript.lines.items.len == 0) {
            drawEmptyBuffer(&self.canvas, rect, "No messages yet - type below and press Enter");
            return;
        }
        const row_h: i32 = 31;
        const capacity: usize = @intCast(@max(1, @divTrunc(rect.h - 12, row_h)));
        const range = self.shell.visibleRange(transcript.lines.items.len, capacity);
        var y = rect.y + 6;
        for (transcript.lines.items[range.start..range.end], 0..) |line, index| {
            const nick_w = @min(112, @max(54, Canvas.textWidth(line.nick) + 14));
            self.canvas.fillRect(rect.x + 7, y - 2, rect.w - 14, row_h - 3, if (index % 2 == 0) chrome else layer);
            self.canvas.fillRect(rect.x + 7, y - 2, 3, row_h - 3, accent);
            self.canvas.fillRect(rect.x + 16, y + 2, nick_w - 8, 18, accent_soft);
            drawTextEllipsized(&self.canvas, line.nick, rect.x + 20, y + 3, nick_w - 16, accent);
            drawTextEllipsized(&self.canvas, line.text, rect.x + nick_w + 14, y + 3, rect.w - nick_w - 24, ink);
            y += row_h;
            if (y + row_h > rect.bottom()) break;
        }
    }

    fn drawMemberList(self: *View, rect: Rect, transcript: *const session.Transcript, icon_mode: bool) !void {
        self.canvas.fillRect(rect.x, rect.y, rect.w, rect.h, layer);
        if (rect.h <= 0) return;
        if (icon_mode) return self.drawMemberIcons(rect, transcript);
        var y = rect.y + 7;
        for (transcript.roster.items, 0..) |member, index| {
            if (y + 24 > rect.bottom()) break;
            if (member.is_self or self.shell.selected_member == index) self.canvas.fillRect(rect.x + 3, y - 1, rect.w - 6, 23, accent_soft);
            self.canvas.fillRect(rect.x + 8, y + 5, 8, 8, if (member.departed) divider else 0xff107c10);
            const label_x = rect.x + 24;
            drawTextEllipsized(&self.canvas, member.nick, label_x, y, rect.right() - label_x - 6, if (member.departed) secondary else ink);
            y += 24;
        }
    }

    fn drawMemberIcons(self: *View, rect: Rect, transcript: *const session.Transcript) !void {
        const columns = @max(1, @divTrunc(rect.w, 88));
        const cell_w = @divTrunc(rect.w, columns);
        const cell_h: i32 = 72;
        for (transcript.roster.items, 0..) |member, index| {
            const column: i32 = @intCast(index % @as(usize, @intCast(columns)));
            const row: i32 = @intCast(index / @as(usize, @intCast(columns)));
            const cell = Rect{
                .x = rect.x + column * cell_w,
                .y = rect.y + row * cell_h,
                .w = cell_w,
                .h = cell_h,
            };
            if (cell.bottom() > rect.bottom()) break;
            self.canvas.fillRect(cell.x + 3, cell.y + 3, cell.w - 6, cell.h - 6, chrome);
            drawRectOutline(&self.canvas, cell.x + 3, cell.y + 3, cell.w - 6, cell.h - 6, if (member.is_self or self.shell.selected_member == index) accent else divider);
            self.canvas.fillRect(cell.x + 9, cell.y + 10, 7, 7, if (member.departed) divider else 0xff107c10);
            const avatar = strip.avatarByName(member.avatar) orelse continue;
            var icon = bgb.decodeIcon(self.gpa, avatar) catch continue;
            defer icon.deinit(self.gpa);
            blitHeightBottomAlpha(&self.canvas, icon.pixels, icon.width, icon.height, cell.x + @divTrunc(cell.w - 40, 2), cell.y + 7, 40, 40);
            const name_w = Canvas.textWidth(member.nick);
            drawTextEllipsized(
                &self.canvas,
                member.nick,
                cell.x + @max(3, @divTrunc(cell.w - name_w, 2)),
                cell.y + 47,
                cell.w - 6,
                if (member.departed) secondary else ink,
            );
        }
    }

    fn drawBodyCamera(self: *View, rect: Rect, transcript: *const session.Transcript) !void {
        if (rect.w <= 0 or rect.h <= 0) return;
        self.canvas.fillRect(rect.x, rect.y, rect.w, rect.h, chrome);
        drawRectOutline(&self.canvas, rect.x, rect.y, rect.w, rect.h, divider);

        // bodycam.cpp: CacheBullSide uses min(width, 159), disabling the wheel
        // below 93 pixels. The figure occupies the remaining white rectangle.
        const wheel_side = if (rect.w >= 93) @min(rect.w, 159) else 0;
        const body_h = @max(0, rect.h - wheel_side);

        var avatar_name: []const u8 = "anna";
        for (transcript.roster.items) |member| if (member.is_self) {
            avatar_name = member.avatar;
            break;
        };
        const avb_data = strip.avatarByName(avatar_name) orelse return;
        var rendered = selected: {
            const selected_emotion = self.shell.selectedEmotion();
            if (selected_emotion == .neutral) break :selected figure.assembleForText(self.gpa, avb_data, "") catch return;
            const pose = figure.poseStateForEmotion(self.gpa, avb_data, selected_emotion, self.shell.selectedEmotionIntensity()) catch return;
            const detailed = figure.assembleDetailedForSourcePose(self.gpa, avb_data, pose) catch return;
            break :selected detailed.image;
        };
        defer rendered.deinit(self.gpa);
        blitHeightBottomAlpha(
            &self.canvas,
            rendered.pixels,
            rendered.width,
            rendered.height,
            rect.x,
            rect.y + 20,
            rect.w,
            @max(0, body_h - 20),
        );
        if (wheel_side > 0) drawEmotionWheel(&self.canvas, .{
            .x = rect.x,
            .y = rect.bottom() - wheel_side,
            .w = rect.w,
            .h = wheel_side,
        }, self.shell.emotion_x, self.shell.emotion_y);
        // Avatar pixels can be opaque even around the figure. Draw the card
        // header last so it remains legible on every source avatar.
        self.canvas.fillRect(rect.x, rect.y, rect.w, 21, chrome);
        self.canvas.fillRect(rect.x, rect.y, rect.w, 3, accent);
        _ = self.canvas.drawText("Character", rect.x + 9, rect.y + 5, secondary);
    }

    fn invokeMenuItem(self: *View, menu: u8, item: u8) void {
        switch (menu) {
            0 => if (item == 2) self.openDialog(.about),
            1 => if (item == 0) self.openDialog(.settings),
            2 => switch (item) {
                0 => self.setContentMode(.comic),
                1 => self.setContentMode(.text),
                2 => self.toggleMembers(),
                else => {},
            },
            3 => switch (item) {
                0 => self.openDialog(.set_text_font),
                1 => self.openDialog(.choose_color),
                else => {},
            },
            4 => switch (item) {
                0 => self.openDialog(.room_list),
                1 => self.openDialog(.channel),
                2 => self.openDialog(.channel_create),
                else => {},
            },
            5 => switch (item) {
                0 => self.openDialog(.user_list),
                1 => self.openDialog(.personal),
                else => {},
            },
            6 => switch (item) {
                1 => self.openDialog(.settings),
                2 => self.openDialog(.about),
                else => {},
            },
            else => {},
        }
    }
};

const menu_labels = [_][]const u8{ "File", "Edit", "View", "Format", "Room", "Member", "More" };

fn menuStart(menu: u8) i32 {
    var x: i32 = 12;
    var index: u8 = 0;
    while (index < menu and index < menu_labels.len) : (index += 1) x += Canvas.textWidth(menu_labels[index]) + 28;
    return x;
}

fn menuItemCount(menu: u8) u8 {
    return switch (menu) {
        0, 2, 4, 6 => 3,
        3, 5 => 2,
        else => 1,
    };
}

fn menuItemLabel(menu: u8, item: u8) []const u8 {
    return switch (menu) {
        0 => switch (item) {
            0 => "Open conversation",
            1 => "Save conversation",
            else => "About Comic Chat",
        },
        1 => "Settings",
        2 => switch (item) {
            0 => "Comic view",
            1 => "Text view",
            else => "Show members",
        },
        3 => if (item == 0) "Text font" else "Colors",
        4 => switch (item) {
            0 => "Room list",
            1 => "Enter room",
            else => "Create room",
        },
        5 => if (item == 0) "User list" else "Personal profile",
        6 => switch (item) {
            0 => "Favorites",
            1 => "Settings",
            else => "About Comic Chat",
        },
        else => "About Comic Chat",
    };
}

fn menuPopupRect(menu: u8) Rect {
    return .{ .x = menuStart(menu), .y = 24, .w = 190, .h = @as(i32, menuItemCount(menu)) * 25 + 8 };
}

fn menuPopupItem(menu: u8, x: i32, y: i32) ?u8 {
    const rect = menuPopupRect(menu);
    if (x < rect.x or x >= rect.right() or y < rect.y + 4 or y >= rect.bottom() - 4) return null;
    const item = @divTrunc(y - rect.y - 4, 25);
    if (item < 0 or item >= menuItemCount(menu)) return null;
    return @intCast(item);
}

fn drawMenuBar(c: *Canvas, rect: Rect, active: ?u8, hovered: ?u8) void {
    c.fillRect(rect.x, rect.y, rect.w, rect.h, chrome);
    var x = rect.x + 12;
    for (menu_labels, 0..) |item, raw_index| {
        const index: u8 = @intCast(raw_index);
        const selected = active == index or hovered == index;
        const item_w = Canvas.textWidth(item) + 16;
        if (selected) c.fillRect(x - 8, rect.y + 2, item_w, rect.h - 4, accent_soft);
        _ = c.drawText(item, x, rect.y + 1, ink);
        x += Canvas.textWidth(item) + 28;
        if (x >= rect.right() - 40) break;
    }
    c.fillRect(rect.x, rect.bottom() - 1, rect.w, 1, divider);
}

fn drawMenuPopup(c: *Canvas, menu: u8, hovered: ?u8) void {
    const rect = menuPopupRect(menu);
    c.fillRect(rect.x + 3, rect.y + 3, rect.w, rect.h, 0xffb8c2cc);
    c.fillRect(rect.x, rect.y, rect.w, rect.h, chrome);
    drawRectOutline(c, rect.x, rect.y, rect.w, rect.h, divider);
    var item: u8 = 0;
    while (item < menuItemCount(menu)) : (item += 1) {
        const y = rect.y + 4 + @as(i32, item) * 25;
        if (item != 0) c.fillRect(rect.x + 4, y - 1, rect.w - 8, 1, subtle);
        ui.drawMenuItem(c, rect.x + 4, y, rect.w - 8, menuItemLabel(menu, item), hovered == item);
    }
}

fn drawToolBar(c: *Canvas, rect: Rect, comic_mode: bool, hovered: ?u8) void {
    c.fillRect(rect.x, rect.y, rect.w, rect.h, chrome);
    // At narrow desktop widths, preserve the most-used source commands with
    // breathing room instead of reducing two dozen glyphs into visual noise.
    if (rect.w < 760) {
        const compact = [_]struct { glyph: ToolGlyph, index: u8, selected: bool }{
            .{ .glyph = .connect, .index = 0, .selected = false },
            .{ .glyph = .enter_room, .index = 2, .selected = false },
            .{ .glyph = .create_room, .index = 4, .selected = false },
            .{ .glyph = .comic, .index = 5, .selected = comic_mode },
            .{ .glyph = .text, .index = 6, .selected = !comic_mode },
            .{ .glyph = .rooms, .index = 7, .selected = false },
            .{ .glyph = .members, .index = 8, .selected = false },
            .{ .glyph = .away, .index = 10, .selected = false },
            .{ .glyph = .identity, .index = 11, .selected = false },
            .{ .glyph = .whisper, .index = 13, .selected = false },
            .{ .glyph = .font, .index = 17, .selected = false },
            .{ .glyph = .color, .index = 18, .selected = false },
        };
        var compact_x = rect.x + 8;
        for (compact) |item| {
            _ = drawModernToolButton(c, item.glyph, compact_x, rect.y + 1, item.selected, hovered == item.index);
            compact_x += 28;
        }
        c.fillRect(rect.x, rect.bottom() - 1, rect.w, 1, divider);
        return;
    }
    var x = rect.x + 5;
    var index: u8 = 0;
    // chat.rc's IDR_MAINFRAME: connect, disconnect, enter, leave, create,
    // comic, text, room list, user list, favorites.
    const main_first = [_]ToolGlyph{ .connect, .disconnect, .enter_room, .leave_room, .create_room };
    for (main_first) |glyph| {
        x = drawModernToolButton(c, glyph, x, rect.y + 1, false, hovered == index);
        index += 1;
    }
    x = drawToolbarSeparator(c, x, rect);
    x = drawModernToolButton(c, .comic, x, rect.y + 1, comic_mode, hovered == index);
    index += 1;
    x = drawModernToolButton(c, .text, x, rect.y + 1, !comic_mode, hovered == index);
    index += 1;
    x = drawToolbarSeparator(c, x, rect);
    x = drawModernToolButton(c, .rooms, x, rect.y + 1, false, hovered == index);
    index += 1;
    x = drawModernToolButton(c, .members, x, rect.y + 1, false, hovered == index);
    index += 1;
    x = drawToolbarSeparator(c, x, rect);
    x = drawModernToolButton(c, .favorite, x, rect.y + 1, false, hovered == index);
    index += 1;

    // The source coolbar orders member tools before text-formatting tools.
    x = drawToolbarSeparator(c, x, rect);
    const member_first = [_]ToolGlyph{ .away, .identity, .ignore, .whisper };
    for (member_first) |glyph| {
        x = drawModernToolButton(c, glyph, x, rect.y + 1, false, hovered == index);
        index += 1;
    }
    x = drawToolbarSeparator(c, x, rect);
    const member_last = [_]ToolGlyph{ .email, .home_page, .meeting };
    for (member_last) |glyph| {
        x = drawModernToolButton(c, glyph, x, rect.y + 1, false, hovered == index);
        index += 1;
    }
    x = drawToolbarSeparator(c, x, rect);
    const format = [_]ToolGlyph{ .font, .color, .bold, .italic, .underline, .fixed, .symbol };
    for (format) |glyph| {
        if (x + 24 > rect.right()) break;
        x = drawModernToolButton(c, glyph, x, rect.y + 1, false, hovered == index);
        index += 1;
    }
    c.fillRect(rect.x, rect.bottom() - 1, rect.w, 1, divider);
}

const ToolGlyph = enum {
    connect,
    disconnect,
    enter_room,
    leave_room,
    create_room,
    comic,
    text,
    rooms,
    members,
    favorite,
    away,
    identity,
    ignore,
    whisper,
    email,
    home_page,
    meeting,
    font,
    color,
    bold,
    italic,
    underline,
    fixed,
    symbol,
};

fn toolbarLabel(index: u8) []const u8 {
    return switch (index) {
        0 => "Connection setup",
        1 => "Disconnect",
        2 => "Enter room",
        3 => "Leave room",
        4 => "Create room",
        5 => "Comic view",
        6 => "Text view",
        7 => "Browse rooms",
        8 => "Show or hide members",
        9 => "Favorite rooms",
        10 => "Set away message",
        11 => "Personal profile",
        12 => "Ignore member",
        13 => "Send a whisper",
        14 => "Email member",
        15 => "Open home page",
        16 => "Start meeting",
        17 => "Choose text font",
        18 => "Choose text color",
        19 => "Bold",
        20 => "Italic",
        21 => "Underline",
        22 => "Fixed-width text",
        23 => "Insert symbol",
        else => "Comic Chat tool",
    };
}

fn drawModernToolButton(c: *Canvas, glyph: ToolGlyph, x: i32, y: i32, selected: bool, hovered: bool) i32 {
    const glyph_color = ui.drawCommandTile(c, x, y, selected, hovered);
    drawToolGlyph(c, glyph, x + 4, y + 4, glyph_color);
    return x + 24;
}

fn drawToolGlyph(c: *Canvas, glyph: ToolGlyph, x: i32, y: i32, color: u32) void {
    switch (glyph) {
        .connect, .disconnect => {
            c.drawLine(x + 3, y + 3, x + 12, y + 12, color);
            c.drawLine(x + 2, y + 6, x + 6, y + 2, color);
            c.drawLine(x + 9, y + 14, x + 14, y + 9, color);
            if (glyph == .disconnect) c.drawLine(x + 2, y + 14, x + 14, y + 2, accent);
        },
        .enter_room, .leave_room => {
            drawRectOutline(c, x + 8, y + 2, 6, 13, color);
            const rightward = glyph == .enter_room;
            const from_x = if (rightward) x + 1 else x + 13;
            const to_x = if (rightward) x + 10 else x + 4;
            c.drawLine(from_x, y + 8, to_x, y + 8, color);
            c.drawLine(to_x, y + 8, if (rightward) to_x - 3 else to_x + 3, y + 5, color);
            c.drawLine(to_x, y + 8, if (rightward) to_x - 3 else to_x + 3, y + 11, color);
        },
        .create_room => {
            drawRectOutline(c, x + 2, y + 2, 12, 12, color);
            c.drawLine(x + 5, y + 8, x + 11, y + 8, color);
            c.drawLine(x + 8, y + 5, x + 8, y + 11, color);
        },
        .comic, .whisper => drawBubbleGlyph(c, x, y, color, glyph == .whisper),
        .text => {
            c.drawLine(x + 2, y + 3, x + 14, y + 3, color);
            c.drawLine(x + 2, y + 7, x + 12, y + 7, color);
            c.drawLine(x + 2, y + 11, x + 14, y + 11, color);
            c.drawLine(x + 2, y + 15, x + 9, y + 15, color);
        },
        .rooms => {
            drawRectOutline(c, x + 1, y + 2, 14, 12, color);
            c.drawLine(x + 6, y + 3, x + 6, y + 13, color);
            c.drawLine(x + 2, y + 7, x + 14, y + 7, color);
        },
        .members, .identity, .ignore => {
            drawCircleOutline(c, x + 8, y + 5, 3, color);
            c.drawLine(x + 3, y + 14, x + 5, y + 10, color);
            c.drawLine(x + 5, y + 10, x + 11, y + 10, color);
            c.drawLine(x + 11, y + 10, x + 13, y + 14, color);
            if (glyph == .ignore) c.drawLine(x + 2, y + 14, x + 14, y + 2, accent);
            if (glyph == .identity) drawRectOutline(c, x + 1, y + 1, 14, 14, color);
        },
        .favorite => drawStarGlyph(c, x + 8, y + 8, color),
        .away => {
            drawCircleOutline(c, x + 8, y + 8, 6, color);
            c.fillRect(x + 7, y + 1, 7, 9, chrome);
            c.drawLine(x + 8, y + 14, x + 13, y + 11, color);
        },
        .email => {
            drawRectOutline(c, x + 1, y + 3, 14, 11, color);
            c.drawLine(x + 2, y + 4, x + 8, y + 9, color);
            c.drawLine(x + 14, y + 4, x + 8, y + 9, color);
        },
        .home_page => {
            drawCircleOutline(c, x + 8, y + 8, 7, color);
            c.drawLine(x + 1, y + 8, x + 15, y + 8, color);
            c.drawLine(x + 8, y + 1, x + 8, y + 15, color);
            drawRectOutline(c, x + 4, y + 1, 8, 14, color);
        },
        .meeting => {
            drawRectOutline(c, x + 1, y + 4, 10, 9, color);
            c.fillTriangle(x + 11, y + 7, x + 15, y + 4, x + 15, y + 13, color);
        },
        .font => _ = c.drawText("A", x + 2, y - 3, color),
        .color => {
            drawCircleOutline(c, x + 8, y + 8, 7, color);
            c.fillRect(x + 3, y + 4, 3, 3, 0xff0067c0);
            c.fillRect(x + 8, y + 2, 3, 3, 0xff107c10);
            c.fillRect(x + 11, y + 7, 3, 3, 0xffc42b1c);
        },
        .bold => _ = c.drawText("B", x + 2, y - 3, color),
        .italic => _ = c.drawText("I", x + 4, y - 3, color),
        .underline => {
            _ = c.drawText("U", x + 2, y - 3, color);
            c.drawLine(x + 2, y + 15, x + 13, y + 15, color);
        },
        .fixed => {
            c.drawLine(x + 4, y + 3, x + 1, y + 8, color);
            c.drawLine(x + 1, y + 8, x + 4, y + 13, color);
            c.drawLine(x + 12, y + 3, x + 15, y + 8, color);
            c.drawLine(x + 15, y + 8, x + 12, y + 13, color);
        },
        .symbol => _ = c.drawText("#", x + 1, y - 3, color),
    }
}

fn drawRectOutline(c: *Canvas, x: i32, y: i32, w: i32, h: i32, color: u32) void {
    ui.drawOutline(c, x, y, w, h, color);
}

fn drawBubbleGlyph(c: *Canvas, x: i32, y: i32, color: u32, dotted: bool) void {
    drawRectOutline(c, x + 1, y + 2, 14, 10, color);
    c.drawLine(x + 5, y + 11, x + 3, y + 15, color);
    c.drawLine(x + 5, y + 11, x + 8, y + 11, color);
    if (dotted) {
        c.set(x + 5, y + 7, color);
        c.set(x + 8, y + 7, color);
        c.set(x + 11, y + 7, color);
    }
}

fn drawStarGlyph(c: *Canvas, cx: i32, cy: i32, color: u32) void {
    const points = [_][2]i32{ .{ 0, -7 }, .{ 2, -2 }, .{ 7, -2 }, .{ 3, 1 }, .{ 5, 6 }, .{ 0, 3 }, .{ -5, 6 }, .{ -3, 1 }, .{ -7, -2 }, .{ -2, -2 } };
    for (points, 0..) |point, index| {
        const next = points[(index + 1) % points.len];
        c.drawLine(cx + point[0], cy + point[1], cx + next[0], cy + next[1], color);
    }
}

fn drawToolbarSeparator(c: *Canvas, x: i32, rect: Rect) i32 {
    c.fillRect(x + 3, rect.y + 5, 1, rect.h - 10, divider);
    return x + 8;
}

fn drawTabBar(c: *Canvas, rect: Rect, tabs: []const View.Tab, active: usize, focused: bool) void {
    c.fillRect(rect.x, rect.y, rect.w, rect.h, chrome);
    const status_w: i32 = 76;
    c.fillRect(rect.x + 4, rect.y + 3, status_w, rect.h - 4, subtle);
    drawBubbleGlyph(c, rect.x + 7, rect.y + 6, secondary, false);
    _ = c.drawText("Status", rect.x + 27, rect.y + 4, secondary);
    const first_x = rect.x + status_w + 6;
    const tab_w: i32 = 140;
    for (tabs, 0..) |tab, index| {
        const x = first_x + @as(i32, @intCast(index)) * tab_w;
        if (x >= rect.right()) break;
        const width = @min(tab_w, rect.right() - x);
        ui.drawTab(c, x, rect.y + 2, width, rect.h - 2, index == active);
        drawTextEllipsized(c, tab.label, x + 10, rect.y + 3, width - 30, if (tab.unread > 0) accent else ink);
        if (tab.unread > 0) {
            var unread_buf: [12]u8 = undefined;
            const unread = std.fmt.bufPrint(&unread_buf, "{d}", .{tab.unread}) catch "!";
            _ = c.drawText(unread, x + width - Canvas.textWidth(unread) - 7, rect.y + 3, accent);
        }
        if (focused and index == active) drawFocus(c, .{ .x = x, .y = rect.y + 2, .w = width, .h = rect.h - 2 });
    }
    c.fillRect(rect.x, rect.bottom() - 1, rect.w, 1, divider);
}

fn sayActionLabel(index: u8) []const u8 {
    return switch (index) {
        0 => "Say",
        1 => "Think",
        2 => "Whisper",
        3 => "Action",
        4 => "Sound",
        else => "Action",
    };
}

fn drawSplitters(c: *Canvas, layout: geometry.Layout, comic_mode: bool) void {
    if (layout.right.w > 0)
        c.fillRect(layout.transcript.right(), layout.buffer.y, geometry.splitter, layout.buffer.h, divider);
    c.fillRect(layout.transcript.x, layout.transcript.bottom(), layout.transcript.w, geometry.splitter, divider);
    if (comic_mode) c.fillRect(layout.right.x, layout.members.bottom(), layout.right.w, geometry.splitter, divider);
}

fn drawSayWindow(c: *Canvas, layout: geometry.Layout, input: []const u8, cursor: usize, selection: ?TextSelection, focused: bool, say_mode: shell_mod.SayMode) void {
    const edit = layout.say_editor;
    c.fillRect(layout.say.x, layout.say.y, layout.say.w, layout.say.h, chrome);
    c.fillRect(edit.x, edit.y, edit.w, edit.h, layer);
    drawRectOutline(c, edit.x, edit.y, edit.w, edit.h, if (focused) accent else divider);
    if (selection) |range| {
        const start = @min(range.start, input.len);
        const end = @min(@max(range.end, start), input.len);
        const x = edit.x + 7 + Canvas.textWidth(input[0..start]);
        const w = Canvas.textWidth(input[start..end]);
        c.fillRect(x, edit.y + 2, @max(1, w), @max(1, edit.h - 4), accent_soft);
    }
    drawTextEllipsized(c, input, edit.x + 7, edit.y, edit.w - 14, ink);
    const safe_cursor = @min(cursor, input.len);
    const caret_x = @min(edit.right() - 2, edit.x + 7 + Canvas.textWidth(input[0..safe_cursor]));
    if (focused) c.fillRect(caret_x, edit.y + 2, 2, @max(1, edit.h - 4), accent);

    const glyphs = [_]SayGlyph{ .say, .think, .whisper, .action, .sound };
    var x = layout.say_actions.x;
    for (glyphs, 0..) |glyph, index| {
        const selected = @intFromEnum(say_mode) == index;
        const glyph_color = ui.drawActionTile(c, x, layout.say_actions.y, geometry.say_button_size, layout.say_actions.h, selected);
        drawSayGlyph(c, glyph, x + 4, layout.say_actions.y + 3, glyph_color);
        x += geometry.say_button_size;
    }
    if (focused) drawFocus(c, layout.say);
}

const SayGlyph = enum { say, think, whisper, action, sound };

fn drawSayGlyph(c: *Canvas, glyph: SayGlyph, x: i32, y: i32, color: u32) void {
    switch (glyph) {
        .say => drawBubbleGlyph(c, x, y, color, false),
        .whisper => drawBubbleGlyph(c, x, y, color, true),
        .think => {
            drawCircleOutline(c, x + 5, y + 7, 4, color);
            drawCircleOutline(c, x + 10, y + 6, 4, color);
            drawCircleOutline(c, x + 8, y + 10, 4, color);
            c.set(x + 4, y + 15, color);
        },
        .action => {
            c.drawLine(x + 9, y + 1, x + 4, y + 9, color);
            c.drawLine(x + 4, y + 9, x + 8, y + 9, color);
            c.drawLine(x + 8, y + 9, x + 5, y + 16, color);
            c.drawLine(x + 5, y + 16, x + 14, y + 6, color);
            c.drawLine(x + 14, y + 6, x + 10, y + 6, color);
        },
        .sound => {
            c.fillRect(x + 2, y + 6, 4, 6, color);
            c.fillTriangle(x + 6, y + 6, x + 11, y + 2, x + 11, y + 16, color);
            c.drawLine(x + 13, y + 5, x + 15, y + 8, color);
            c.drawLine(x + 15, y + 8, x + 13, y + 12, color);
        },
    }
}

fn drawStatusBar(c: *Canvas, rect: Rect, status: []const u8, member_count: usize) void {
    ui.drawStatusBar(c, rect.x, rect.y, rect.w, rect.h, status, member_count);
}

fn drawEmptyBuffer(c: *Canvas, rect: Rect, text: []const u8) void {
    ui.drawEmptyState(c, rect.x, rect.y, rect.w, rect.h, text);
}

fn drawFocus(c: *Canvas, rect: Rect) void {
    if (rect.w < 4 or rect.h < 4) return;
    c.fillRect(rect.x, rect.y, rect.w, 2, focus_color);
    c.fillRect(rect.x, rect.bottom() - 2, rect.w, 2, focus_color);
    c.fillRect(rect.x, rect.y, 2, rect.h, focus_color);
    c.fillRect(rect.right() - 2, rect.y, 2, rect.h, focus_color);
}

fn drawTextEllipsized(c: *Canvas, text: []const u8, x: i32, y: i32, max_w: i32, color: u32) void {
    if (max_w <= 0) return;
    if (Canvas.textWidth(text) <= max_w) {
        _ = c.drawText(text, x, y, color);
        return;
    }
    const dots = "...";
    const dots_w = Canvas.textWidth(dots);
    var end = text.len;
    while (end > 0 and Canvas.textWidth(text[0..end]) + dots_w > max_w) end -= 1;
    _ = c.drawText(text[0..end], x, y, color);
    _ = c.drawText(dots, x + Canvas.textWidth(text[0..end]), y, color);
}

fn blitFit(c: *Canvas, src: []const u32, sw: u32, sh: u32, x: i32, y: i32, max_w: i32, max_h: i32) void {
    const fit = fitRect(sw, sh, x, y, max_w, max_h) orelse return;
    var oy: i32 = 0;
    while (oy < fit.h) : (oy += 1) {
        const sy: u32 = @intCast(@divTrunc(@as(i64, oy) * sh, fit.h));
        var ox: i32 = 0;
        while (ox < fit.w) : (ox += 1) {
            const sx: u32 = @intCast(@divTrunc(@as(i64, ox) * sw, fit.w));
            c.set(fit.x + ox, fit.y + oy, src[@as(usize, sy) * sw + sx]);
        }
    }
}

fn blitHeightBottomAlpha(c: *Canvas, src: []const u32, sw: u32, sh: u32, x: i32, y: i32, area_w: i32, area_h: i32) void {
    if (sw == 0 or sh == 0 or area_w <= 0 or area_h <= 0) return;
    const draw_h = area_h;
    const draw_w: i32 = @max(1, @as(i32, @intCast(@divTrunc(@as(i64, sw) * draw_h, sh))));
    const draw_x = x + @divTrunc(area_w - draw_w, 2);
    var oy: i32 = 0;
    while (oy < draw_h) : (oy += 1) {
        const sy: u32 = @intCast(@divTrunc(@as(i64, oy) * sh, draw_h));
        var ox: i32 = 0;
        while (ox < draw_w) : (ox += 1) {
            const sx: u32 = @intCast(@divTrunc(@as(i64, ox) * sw, draw_w));
            const pixel = src[@as(usize, sy) * sw + sx];
            if (pixel >> 24 != 0) c.set(draw_x + ox, y + oy, pixel);
        }
    }
}

fn drawEmotionWheel(c: *Canvas, rect: Rect, selector_x: i16, selector_y: i16) void {
    const wheel_gray: u32 = 0xffd2d2d2;
    c.fillRect(rect.x, rect.y, rect.w, rect.h, wheel_gray);
    const cx = rect.x + @divTrunc(rect.w, 2);
    const cy = rect.y + @divTrunc(rect.h, 2);
    const cursor_radius: i32 = 5;
    const icon_h: i32 = 26;
    const radius = @max(1, @divTrunc(rect.h, 2) - cursor_radius - icon_h);
    fillOutlinedCircle(c, cx, cy, radius, layer, ink);

    const icons = [_][]const u8{
        source_ui.emotion_happy,
        source_ui.emotion_coy,
        source_ui.emotion_bored,
        source_ui.emotion_scared,
        source_ui.emotion_sad,
        source_ui.emotion_angry,
        source_ui.emotion_shout,
        source_ui.emotion_laugh,
    };
    // bodycam.cpp places the eight authored faces every PI/4, beginning at
    // the right and proceeding clockwise in Win32 screen coordinates.
    const directions = [_][2]i32{
        .{ 1000, 0 },
        .{ 707, 707 },
        .{ 0, 1000 },
        .{ -707, 707 },
        .{ -1000, 0 },
        .{ -707, -707 },
        .{ 0, -1000 },
        .{ 707, -707 },
    };
    const icon_offset = radius + 2 * cursor_radius + @divTrunc(icon_h, 2);
    for (icons, directions) |icon, direction| {
        const ix = cx + @divTrunc(direction[0] * icon_offset, 1000) - 10;
        const iy = cy + @divTrunc(direction[1] * icon_offset, 1000) - 13;
        drawBmp8(c, icon, ix, iy);
    }

    const selector_cx = cx + selector_x;
    const selector_cy = cy + selector_y;
    drawCircleOutline(c, selector_cx, selector_cy, cursor_radius, ink);
    c.drawLine(selector_cx - 3, selector_cy, selector_cx + 3, selector_cy, ink);
    c.drawLine(selector_cx, selector_cy - 3, selector_cx, selector_cy + 3, ink);
}

fn fillOutlinedCircle(c: *Canvas, cx: i32, cy: i32, radius: i32, fill: u32, outline: u32) void {
    const outer = radius * radius;
    const inner = @max(0, radius - 1) * @max(0, radius - 1);
    var y: i32 = -radius;
    while (y <= radius) : (y += 1) {
        var x: i32 = -radius;
        while (x <= radius) : (x += 1) {
            const distance = x * x + y * y;
            if (distance <= outer) c.set(cx + x, cy + y, if (distance >= inner) outline else fill);
        }
    }
}

fn drawCircleOutline(c: *Canvas, cx: i32, cy: i32, radius: i32, color: u32) void {
    const outer = radius * radius;
    const inner = @max(0, radius - 1) * @max(0, radius - 1);
    var y: i32 = -radius;
    while (y <= radius) : (y += 1) {
        var x: i32 = -radius;
        while (x <= radius) : (x += 1) {
            const distance = x * x + y * y;
            if (distance <= outer and distance >= inner) c.set(cx + x, cy + y, color);
        }
    }
}

fn bmpU16(bytes: []const u8, offset: usize) u16 {
    return @as(u16, bytes[offset]) | @as(u16, bytes[offset + 1]) << 8;
}

fn bmpU32(bytes: []const u8, offset: usize) u32 {
    return @as(u32, bytes[offset]) |
        @as(u32, bytes[offset + 1]) << 8 |
        @as(u32, bytes[offset + 2]) << 16 |
        @as(u32, bytes[offset + 3]) << 24;
}

fn drawBmp8(c: *Canvas, bytes: []const u8, dx: i32, dy: i32) void {
    if (bytes.len < 54 or bmpU16(bytes, 0) != 0x4d42 or bmpU16(bytes, 28) != 8) return;
    drawBmpRegion(c, bytes, 0, 0, bmpU32(bytes, 18), bmpU32(bytes, 22), dx, dy, false);
}

fn drawBmpRegion(c: *Canvas, bytes: []const u8, source_x: usize, source_y: usize, region_w: usize, region_h: usize, dx: i32, dy: i32, transparent_corner: bool) void {
    if (bytes.len < 54 or bmpU16(bytes, 0) != 0x4d42) return;
    const bits: usize = bmpU32(bytes, 10);
    const width: usize = bmpU32(bytes, 18);
    const height: usize = bmpU32(bytes, 22);
    const depth: usize = bmpU16(bytes, 28);
    if (depth != 4 and depth != 8) return;
    const palette: usize = 14 + bmpU32(bytes, 14);
    const stride = ((width * depth + 31) / 32) * 4;
    const palette_len: usize = (@as(usize, 1) << @intCast(depth)) * 4;
    if (width == 0 or height == 0 or source_x + region_w > width or source_y + region_h > height or
        bits + stride * height > bytes.len or palette + palette_len > bytes.len) return;
    const corner_row = bits + (height - 1 - source_y) * stride;
    const corner_index: u8 = if (depth == 8)
        bytes[corner_row + source_x]
    else if (source_x % 2 == 0)
        bytes[corner_row + source_x / 2] >> 4
    else
        bytes[corner_row + source_x / 2] & 0x0f;
    var y: usize = 0;
    while (y < region_h) : (y += 1) {
        const source_row = bits + (height - 1 - (source_y + y)) * stride;
        var x: usize = 0;
        while (x < region_w) : (x += 1) {
            const pixel_x = source_x + x;
            const color_index: u8 = if (depth == 8)
                bytes[source_row + pixel_x]
            else if (pixel_x % 2 == 0)
                bytes[source_row + pixel_x / 2] >> 4
            else
                bytes[source_row + pixel_x / 2] & 0x0f;
            if (transparent_corner and color_index == corner_index) continue;
            const entry = palette + @as(usize, color_index) * 4;
            const color = 0xff000000 | @as(u32, bytes[entry + 2]) << 16 |
                @as(u32, bytes[entry + 1]) << 8 | bytes[entry];
            c.set(dx + @as(i32, @intCast(x)), dy + @as(i32, @intCast(y)), color);
        }
    }
}

fn fitRect(sw: u32, sh: u32, x: i32, y: i32, max_w: i32, max_h: i32) ?Rect {
    if (sw == 0 or sh == 0 or max_w <= 0 or max_h <= 0) return null;
    var dw = max_w;
    var dh: i32 = @intCast(@divTrunc(@as(i64, sh) * dw, sw));
    if (dh > max_h) {
        dh = max_h;
        dw = @intCast(@divTrunc(@as(i64, sw) * dh, sh));
    }
    dw = @max(dw, 1);
    dh = @max(dh, 1);
    return .{
        .x = x + @divTrunc(max_w - dw, 2),
        .y = y + @divTrunc(max_h - dh, 2),
        .w = dw,
        .h = dh,
    };
}

fn dialogLayout(width: u32, height: u32, spec: dialogs.Spec) ui.DialogLayout {
    return ui.DialogLayout.init(width, height, spec.source_w, spec.source_h, dialogs.fields(spec.id).len, dialogPrimaryButtonWidth(spec.id));
}

fn drawDialog(c: *Canvas, spec: dialogs.Spec, editors: *const [3]input_mod.Editor, active_field: usize, notice: []const u8) void {
    ui.drawModalBackdrop(c);
    const dialog_layout = dialogLayout(c.width, c.height, spec);
    const rect = dialog_layout.rect;
    const group_text = switch (spec.group) {
        .connection => "Connection, identity, and appearance",
        .rooms => "Rooms and member workflow",
        .automation => "Automation and notifications",
        .files => "Application and file workflow",
    };
    ui.drawDialogSurface(c, rect, spec.title, group_text);
    const fields = dialogs.fields(spec.id);
    for (fields, 0..) |field, index| {
        const row_y = dialog_layout.fieldLabelY(index);
        if (row_y + 40 > rect.bottom() - 43) break;
        drawTextEllipsized(c, field.label, rect.x + 20, row_y, rect.w - 40, secondary);
        const field_rect = dialog_layout.fieldRect(index);
        const field_y = field_rect.y;
        ui.drawField(c, field_rect.x, field_y, field_rect.w, index == active_field);
        if (index < editors.len) {
            const editor = &editors[index];
            const value = editor.text();
            drawTextEllipsized(c, value, rect.x + 26, field_y + 1, rect.w - 52, ink);
            if (index == active_field) {
                const safe_cursor = @min(editor.cursor, value.len);
                const caret_x = @min(rect.right() - 27, rect.x + 26 + Canvas.textWidth(value[0..safe_cursor]));
                c.fillRect(caret_x, field_y + 3, 2, 17, accent);
            }
        }
        if (editors[index].text().len == 0) drawTextEllipsized(c, field.hint, rect.x + 26, field_y + 1, rect.w - 52, secondary);
    }

    if (notice.len != 0) drawTextEllipsized(c, notice, rect.x + 14, dialog_layout.primary.y - 19, rect.w - 28, focus_color);
    drawDialogButton(c, dialog_layout.primary.x, dialog_layout.primary.y, dialog_layout.primary.w, dialogs.primaryLabel(spec.id), .primary);
    drawDialogButton(c, dialog_layout.cancel.x, dialog_layout.cancel.y, dialog_layout.cancel.w, "Cancel", .secondary);
}

fn dialogPrimaryButtonWidth(id: dialogs.Id) i32 {
    return @max(84, Canvas.textWidth(dialogs.primaryLabel(id)) + 24);
}

fn drawDialogButton(c: *Canvas, x: i32, y: i32, width: i32, label: []const u8, kind: ui.ButtonKind) void {
    ui.drawButton(c, x, y, width, label, kind, false);
}

test "view renders source-shaped empty buffer and chrome" {
    const gpa = std.testing.allocator;
    var view = try View.init(gpa, 960, 720);
    defer view.deinit();
    var transcript = session.Transcript.init(gpa);
    defer transcript.deinit();
    try transcript.setSelf("anna");

    try view.render("Comic Chat | #root | anna", "connected", &transcript, "hello", 3);
    const layout = geometry.Layout.compute(960, 720, true, true);
    try std.testing.expectEqual(chrome, view.pixels()[0]);
    try std.testing.expectEqual(divider, view.pixels()[@as(usize, @intCast(layout.tabs.bottom() - 1)) * 960]);
    try std.testing.expect(view.pixels()[@as(usize, @intCast(layout.say.y + 2)) * 960 + 2] == layer or
        view.pixels()[@as(usize, @intCast(layout.say.y + 2)) * 960 + 2] == focus_color);

    const wheel_side = @min(layout.body_camera.w, 159);
    const wheel_y = layout.body_camera.bottom() - wheel_side;
    const wheel_cx = layout.body_camera.x + @divTrunc(layout.body_camera.w, 2);
    const wheel_cy = wheel_y + @divTrunc(wheel_side, 2);
    try std.testing.expectEqual(@as(u32, 0xffd2d2d2), view.pixels()[@as(usize, @intCast(wheel_y + 2)) * 960 + @as(usize, @intCast(layout.body_camera.x + 2))]);
    try std.testing.expectEqual(layer, view.pixels()[@as(usize, @intCast(wheel_cy)) * 960 + @as(usize, @intCast(wheel_cx + 20))]);
}

test "view exposes a semantic shell snapshot without inspecting pixels" {
    var view = try View.init(std.testing.allocator, 960, 720);
    defer view.deinit();
    const tabs = [_]View.Tab{ .{ .label = "#root" }, .{ .label = "#onyx", .unread = 2 } };
    const snapshot = view.semanticSnapshot("connected", &tabs, 0);
    try std.testing.expect(snapshot.items().len >= 12);
    try std.testing.expectEqual(accessibility.Role.window, snapshot.items()[0].role);
    try std.testing.expectEqual(accessibility.Role.tab, snapshot.items()[4].role);
    try std.testing.expect(snapshot.items()[4].selected);
}

test "menu clicks select navigation without opening a modal dialog" {
    var view = try View.init(std.testing.allocator, 960, 720);
    defer view.deinit();
    const action = view.handlePointer(.{ .kind = .down, .x = 12, .y = 10, .button = .primary }, 0, 0);
    try std.testing.expectEqual(Action{ .menu = 0 }, action);
    try std.testing.expect(view.active_dialog == null);
    try std.testing.expectEqual(shell_mod.Focus.navigation, view.shell.focus);
}

test "dialog dimmer preserves an opaque visible frame" {
    var view = try View.init(std.testing.allocator, 320, 240);
    defer view.deinit();
    var transcript = session.Transcript.init(std.testing.allocator);
    defer transcript.deinit();
    view.openDialog(.settings);
    try view.render("Comic Chat", "offline", &transcript, "", 0);
    const pixel = view.pixels()[0];
    try std.testing.expectEqual(@as(u32, 0xff), pixel >> 24);
    try std.testing.expect((pixel & 0x00ffffff) != 0);
}

test "view switches text/comic buffers, pages history, and resizes" {
    const gpa = std.testing.allocator;
    var view = try View.init(gpa, 640, 480);
    defer view.deinit();
    var transcript = session.Transcript.init(gpa);
    defer transcript.deinit();
    try transcript.setSelf("anna");
    var index: usize = 0;
    while (index < 14) : (index += 1) try transcript.add(if (index % 2 == 0) "anna" else "kevin", "A live chat buffer line");

    view.setContentMode(.text);
    view.pageEarlier(transcript.lines.items.len);
    try view.render("Comic Chat | #root | anna", "connected", &transcript, "", 0);
    try std.testing.expect(view.shell.history_offset > 0);
    try view.resize(960, 720);
    view.setContentMode(.comic);
    try view.render("Comic Chat | #root | anna", "connected", &transcript, "reply", 5);
    try std.testing.expectEqual(@as(u32, 960), view.width());
    try std.testing.expectEqual(@as(u32, 720), view.height());
}

test "every registered dialog can be opened and rendered" {
    const gpa = std.testing.allocator;
    var view = try View.init(gpa, 960, 720);
    defer view.deinit();
    var transcript = session.Transcript.init(gpa);
    defer transcript.deinit();
    for (dialogs.specs) |spec| {
        view.openDialog(spec.id);
        try view.render("Comic Chat", "connected", &transcript, "", 0);
        try std.testing.expectEqual(spec.id, view.active_dialog.?);
    }
}
