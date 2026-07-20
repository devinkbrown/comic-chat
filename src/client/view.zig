//! Modern portable Comic Chat client view.
//!
//! The workspace geometry follows the established splitter composition:
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

const Canvas = canvas_mod.Canvas;
const Rect = geometry.Rect;
const TextSelection = input_mod.Editor.Selection;

pub const min_width: u32 = 640;
pub const min_height: u32 = 480;

pub const Action = union(enum) {
    none,
    menu: u8,
    toolbar: u8,
    room_tab: usize,
    send,
    dialog_accept: dialogs.Id,
    dialog_cancel: dialogs.Id,
};

// Modern desktop color roles applied to the established workspace geometry.
const ink = ui.Theme.ink;
const secondary = ui.Theme.secondary;
const chrome = ui.Theme.chrome;
const layer = ui.Theme.layer;
const subtle = ui.Theme.subtle;
const divider = ui.Theme.divider;
const accent = ui.Theme.accent;
const accent_soft = ui.Theme.accent_soft;
const focus_color = ui.Theme.focus;
const ColumnControlHover = enum { decrease, increase };

pub const View = struct {
    gpa: std.mem.Allocator,
    canvas: Canvas,
    shell: shell_mod.State = .{},
    active_dialog: ?dialogs.Id = null,
    dialog_notice: []const u8 = "",
    hovered_dialog_button: ?ui.DialogButton = null,
    active_menu: ?u8 = null,
    hovered_menu: ?u8 = null,
    hovered_menu_item: ?u8 = null,
    hovered_toolbar: ?u8 = null,
    hovered_say_action: ?u8 = null,
    hovered_column_control: ?ColumnControlHover = null,
    hovered_member: ?usize = null,
    emotion_dragging: bool = false,
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
        self.hovered_dialog_button = null;
        self.hovered_say_action = null;
        self.hovered_column_control = null;
        self.hovered_member = null;
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
        self.hovered_dialog_button = null;
        return true;
    }

    /// Update transient pointer affordances.  The original client depended on
    /// raised Win32 controls; this port keeps the source geometry but gives the
    /// custom-drawn controls an equivalent hover response and plain-language
    /// status hint.
    pub fn handlePointerMove(self: *View, pointer: platform_event.Pointer, member_count: usize) bool {
        if (self.active_dialog) |id| {
            const dialog_layout = dialogLayout(self.canvas.width, self.canvas.height, dialogs.get(id));
            const next = ui.dialogButtonAt(dialog_layout, pointer.x, pointer.y);
            const changed = self.hovered_dialog_button != next or self.hovered_menu != null or self.hovered_toolbar != null or self.hovered_say_action != null or self.hovered_column_control != null or self.hovered_member != null;
            self.hovered_dialog_button = next;
            self.hovered_menu = null;
            self.hovered_menu_item = null;
            self.hovered_toolbar = null;
            self.hovered_say_action = null;
            self.hovered_column_control = null;
            self.hovered_member = null;
            return changed;
        }
        if (self.active_menu) |menu| {
            const layout = geometry.Layout.compute(self.canvas.width, self.canvas.height, self.shell.content_mode == .comic, self.shell.show_members);
            const target = hit_test.shell(layout, self.shell.content_mode == .comic, pointer.x, pointer.y, member_count);
            if (target == .menu) {
                const next_menu = target.menu;
                const changed = self.active_menu != next_menu or self.hovered_menu != next_menu or self.hovered_menu_item != null or self.hovered_say_action != null or self.hovered_column_control != null or self.hovered_member != null;
                self.active_menu = next_menu;
                self.hovered_menu = next_menu;
                self.hovered_menu_item = null;
                self.hovered_toolbar = null;
                self.hovered_say_action = null;
                self.hovered_column_control = null;
                self.hovered_member = null;
                return changed;
            }
            const item = menuPopupItem(menu, pointer.x, pointer.y);
            const changed = self.hovered_menu_item != item or self.hovered_menu != null or self.hovered_toolbar != null or self.hovered_say_action != null or self.hovered_column_control != null or self.hovered_member != null;
            self.hovered_menu_item = item;
            self.hovered_menu = null;
            self.hovered_toolbar = null;
            self.hovered_say_action = null;
            self.hovered_column_control = null;
            self.hovered_member = null;
            return changed;
        }
        const comic_mode = self.shell.content_mode == .comic;
        const layout = geometry.Layout.compute(self.canvas.width, self.canvas.height, comic_mode, self.shell.show_members);
        if (self.emotion_dragging) {
            if (comic_mode and self.setEmotionFromPoint(layout, pointer.x, pointer.y)) return true;
            return false;
        }
        const target = hit_test.shell(layout, comic_mode, pointer.x, pointer.y, member_count);
        return switch (target) {
            .menu => |index| self.setHover(index, null),
            .toolbar => |index| self.setHover(null, index),
            .say_action => |index| self.setContentHover(index, null, null),
            .comic_columns_decrease => self.setContentHover(null, .decrease, null),
            .comic_columns_increase => self.setContentHover(null, .increase, null),
            .member => |index| self.setContentHover(null, null, index),
            else => self.setHover(null, null),
        };
    }

    fn setHover(self: *View, menu: ?u8, toolbar: ?u8) bool {
        const changed = self.hovered_menu != menu or self.hovered_menu_item != null or self.hovered_toolbar != toolbar or self.hovered_say_action != null or self.hovered_column_control != null or self.hovered_member != null;
        self.hovered_menu = menu;
        self.hovered_menu_item = null;
        self.hovered_toolbar = toolbar;
        self.hovered_say_action = null;
        self.hovered_column_control = null;
        self.hovered_member = null;
        return changed;
    }

    fn setContentHover(self: *View, say_action: ?u8, column_control: ?ColumnControlHover, member: ?usize) bool {
        const changed = self.hovered_menu != null or self.hovered_menu_item != null or self.hovered_toolbar != null or self.hovered_say_action != say_action or self.hovered_column_control != column_control or self.hovered_member != member;
        self.hovered_menu = null;
        self.hovered_menu_item = null;
        self.hovered_toolbar = null;
        self.hovered_say_action = say_action;
        self.hovered_column_control = column_control;
        self.hovered_member = member;
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
                self.hovered_dialog_button = null;
                return .{ .dialog_cancel = id };
            },
            .enter => {
                return .{ .dialog_accept = id };
            },
            .tab => {
                if (field_count > 0) self.dialog_field = (self.dialog_field + 1) % field_count;
            },
            .char => |ch| if (dialogs.fieldAcceptsText(id, self.dialog_field) and editor.text().len < 512) try editor.insert(ch),
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
        if (pointer.kind == .up) {
            self.emotion_dragging = false;
            return .none;
        }
        if (self.active_dialog) |id| {
            if (pointer.kind != .down or pointer.button != .primary) return .none;
            const spec = dialogs.get(id);
            const dialog_layout = dialogLayout(self.canvas.width, self.canvas.height, spec);
            if (ui.dialogButtonAt(dialog_layout, pointer.x, pointer.y)) |button| {
                switch (button) {
                    .primary => return .{ .dialog_accept = id },
                    .cancel => {
                        self.active_dialog = null;
                        self.hovered_dialog_button = null;
                        return .{ .dialog_cancel = id };
                    },
                }
            }
            if (dialog_layout.fieldIndexAt(pointer.x, pointer.y)) |index| {
                if (index < self.dialog_editors.len) {
                    self.dialog_field = index;
                    const field = dialogs.fields(id)[index];
                    if (field.kind == .choice) self.cycleDialogChoice(id, index);
                }
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
                const first_x = layout.tabs.x + 114;
                const tab_width: i32 = 164;
                const raw = @divTrunc(pointer.x - first_x, tab_width);
                if (raw < 0) break :room .none;
                const index: usize = @intCast(raw);
                if (index >= self.room_tab_count) break :room .none;
                break :room .{ .room_tab = index };
            },
            .comic_columns_decrease => columns: {
                self.shell.decreaseComicColumns();
                break :columns .none;
            },
            .comic_columns_increase => columns: {
                self.shell.increaseComicColumns();
                break :columns .none;
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
                self.emotion_dragging = self.setEmotionFromPoint(layout, pointer.x, pointer.y);
                if (!self.emotion_dragging) self.shell.focus = .emotion;
                break :emotion .none;
            },
        };
    }

    fn cycleDialogChoice(self: *View, id: dialogs.Id, index: usize) void {
        const options = dialogs.choiceOptions(id, index);
        if (options.len == 0 or index >= self.dialog_editors.len) return;
        const editor = &self.dialog_editors[index];
        var next: usize = 0;
        for (options, 0..) |option, option_index| {
            if (std.mem.eql(u8, editor.text(), option)) {
                next = (option_index + 1) % options.len;
                break;
            }
        }
        editor.clear();
        editor.paste(options[next]) catch {};
    }

    fn setEmotionFromPoint(self: *View, layout: geometry.Layout, x: i32, y: i32) bool {
        const dial = emotionDialRect(emotionWheelRect(layout));
        if (dial.w < 72 or dial.h < 72 or !ui.contains(dial, x, y)) return false;
        const cx = dial.x + @divTrunc(dial.w, 2);
        const cy = dial.y + @divTrunc(dial.h, 2);
        const radius = @max(1, @min(@divTrunc(dial.w, 2), @divTrunc(dial.h, 2)) - 9);
        const dx = x - cx;
        const dy = y - cy;
        if (dx * dx + dy * dy > radius * radius) return false;
        self.shell.setEmotionPoint(dx, dy, radius);
        return true;
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
        drawTabBar(&self.canvas, layout, tabs, active_tab, self.shell.focus == .navigation, comic_mode, self.shell.comic_columns, self.hovered_column_control);
        drawSplitters(&self.canvas, layout, comic_mode);

        if (comic_mode) {
            try self.drawComicBuffer(layout.transcript, transcript);
        } else {
            self.drawTextBuffer(layout.transcript, transcript);
        }
        if (layout.right.w > 0) {
            try self.drawMemberList(layout.members, transcript, comic_mode);
            if (comic_mode) try self.drawBodyCamera(layout.body_camera, transcript);
        }
        drawSayWindow(&self.canvas, layout, input, cursor, selection, self.shell.focus == .composer, self.shell.say_mode, self.hovered_say_action);
        drawStatusBar(&self.canvas, layout.status, self.hoveredToolbarLabel() orelse status, transcript.roster.items.len);

        if (self.shell.focus == .transcript) drawFocus(&self.canvas, layout.transcript);
        if (self.shell.focus == .members) drawFocus(&self.canvas, layout.members);
        if (self.shell.focus == .emotion) drawFocus(&self.canvas, layout.body_camera);
        if (self.hovered_toolbar) |index| drawToolbarTooltip(&self.canvas, layout, index);
        if (self.hovered_say_action) |index| drawSayActionTooltip(&self.canvas, layout, index);
        if (self.active_menu) |menu| drawMenuPopup(&self.canvas, menu, self.hovered_menu_item);
        if (self.active_dialog) |id| drawDialog(&self.canvas, dialogs.get(id), &self.dialog_editors, self.dialog_field, self.dialog_notice, self.hovered_dialog_button);
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
        if (comic_mode and layout.transcript.w >= 430) {
            snapshot.append(.{ .id = "comic-columns-decrease", .role = .button, .bounds = geometry.comicColumnDecrease(layout), .label = "Fewer panels across" });
            snapshot.append(.{ .id = "comic-columns-increase", .role = .button, .bounds = geometry.comicColumnIncrease(layout), .label = "More panels across" });
        }
        const first_x = layout.tabs.x + 114;
        for (tabs, 0..) |tab, index| snapshot.append(.{
            .id = "room-tab",
            .role = .tab,
            .bounds = .{ .x = first_x + @as(i32, @intCast(index)) * 164, .y = layout.tabs.y + 5, .w = 164, .h = layout.tabs.h - 5 },
            .label = tab.label,
            .selected = index == active_tab,
            .focused = index == active_tab and self.shell.focus == .navigation,
        });
        snapshot.append(.{ .id = "transcript", .role = .transcript, .bounds = layout.transcript, .label = "Conversation", .focused = self.shell.focus == .transcript });
        if (layout.right.w > 0) snapshot.append(.{ .id = "members", .role = .member_list, .bounds = layout.members, .label = "Members", .focused = self.shell.focus == .members });
        snapshot.append(.{ .id = "composer", .role = .composer, .bounds = layout.say_editor, .label = "Message", .focused = self.shell.focus == .composer });
        var action_index: i32 = 0;
        while (action_index < geometry.say_button_count) : (action_index += 1) snapshot.append(.{
            .id = "say-action",
            .role = .say_action,
            .bounds = .{ .x = layout.say_actions.x + action_index * layout.say_action_size, .y = layout.say_actions.y, .w = layout.say_action_size, .h = layout.say_actions.h },
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
        ui.drawContentSurface(&self.canvas, rect, true);
        if (rect.w <= 0 or rect.h <= 0) return;
        if (transcript.lines.items.len == 0) {
            drawEmptyBuffer(&self.canvas, rect, "No messages yet - type below and press Enter", self.shell.comic_columns);
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

        var page = try strip.renderWithOptions(self.gpa, lines, .{ .title_roster = title_roster, .page_columns = self.shell.comic_columns });
        defer page.deinit(self.gpa);
        blitFit(&self.canvas, page.pixels, page.width, page.height, rect.x + 3, rect.y + 3, rect.w - 6, rect.h - 6);

        if (self.shell.history_offset > 0) {
            const label = "Earlier messages - Page Down returns toward latest";
            ui.drawHistoryBanner(&self.canvas, rect, label);
        }
    }

    fn drawTextBuffer(self: *View, rect: Rect, transcript: *const session.Transcript) void {
        ui.drawContentSurface(&self.canvas, rect, false);
        if (transcript.lines.items.len == 0) {
            drawEmptyBuffer(&self.canvas, rect, "No messages yet - type below and press Enter", 1);
            return;
        }
        const row_h: i32 = 31;
        const capacity: usize = @intCast(@max(1, @divTrunc(rect.h - 12, row_h)));
        const range = self.shell.visibleRange(transcript.lines.items.len, capacity);
        var y = rect.y + 6;
        for (transcript.lines.items[range.start..range.end], 0..) |line, index| {
            ui.drawMessageRow(&self.canvas, .{ .x = rect.x, .y = y, .w = rect.w, .h = row_h }, line.nick, line.text, index % 2 == 0);
            y += row_h;
            if (y + row_h > rect.bottom()) break;
        }
    }

    fn drawMemberList(self: *View, rect: Rect, transcript: *const session.Transcript, icon_mode: bool) !void {
        ui.drawContentSurface(&self.canvas, rect, false);
        if (rect.h <= 0) return;
        ui.drawPaneHeader(&self.canvas, rect, "In this room");
        const content = Rect{ .x = rect.x, .y = rect.y + 30, .w = rect.w, .h = @max(0, rect.h - 30) };
        if (icon_mode) return self.drawMemberIcons(content, transcript);
        var y = content.y + 7;
        for (transcript.roster.items, 0..) |member, index| {
            if (y + 24 > content.bottom()) break;
            const selected = if (self.shell.selected_member) |selected_index| selected_index == index else member.is_self;
            ui.drawMemberRow(&self.canvas, .{ .x = content.x, .y = y, .w = content.w, .h = 24 }, member.nick, selected, member.departed, self.hovered_member == index);
            y += 24;
        }
    }

    fn drawMemberIcons(self: *View, rect: Rect, transcript: *const session.Transcript) !void {
        const columns = @max(1, @divTrunc(rect.w, 88));
        const cell_w = @divTrunc(rect.w, columns);
        const cell_h: i32 = 82;
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
            const selected = if (self.shell.selected_member) |selected_index| selected_index == index else member.is_self;
            ui.drawMemberCard(&self.canvas, cell, selected, member.departed, self.hovered_member == index);
            const avatar = strip.avatarByName(member.avatar) orelse continue;
            var icon = bgb.decodeIcon(self.gpa, avatar) catch continue;
            defer icon.deinit(self.gpa);
            blitHeightBottomAlphaSmooth(&self.canvas, icon.pixels, icon.width, icon.height, cell.x + @divTrunc(cell.w - 52, 2), cell.y + 6, 52, 52);
            const name_w = Canvas.uiTextWidth(member.nick);
            drawTextEllipsized(
                &self.canvas,
                member.nick,
                cell.x + @max(3, @divTrunc(cell.w - name_w, 2)),
                cell.y + 59,
                cell.w - 6,
                if (member.departed) secondary else ink,
            );
        }
    }

    fn drawBodyCamera(self: *View, rect: Rect, transcript: *const session.Transcript) !void {
        if (rect.w <= 0 or rect.h <= 0) return;
        ui.drawCharacterPane(&self.canvas, rect);

        // bodycam.cpp: CacheBullSide uses min(width, 159), disabling the wheel
        // below 93 pixels. The figure occupies the remaining white rectangle.
        const wheel_side = if (rect.w >= 93) @min(rect.w, 159) else 0;
        const body_h = @max(0, rect.h - wheel_side);

        var avatar_name: []const u8 = "anna";
        var character_name: []const u8 = "Character";
        if (self.shell.selected_member) |selected_index| {
            if (selected_index < transcript.roster.items.len) {
                const member = transcript.roster.items[selected_index];
                avatar_name = member.avatar;
                character_name = member.nick;
            }
        } else for (transcript.roster.items) |member| if (member.is_self) {
            avatar_name = member.avatar;
            character_name = member.nick;
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
        blitHeightBottomAlphaSmooth(
            &self.canvas,
            rendered.pixels,
            rendered.width,
            rendered.height,
            rect.x,
            rect.y + 30,
            rect.w,
            @max(0, body_h - 30),
        );
        if (wheel_side > 0) drawEmotionWheel(&self.canvas, emotionWheelRectFromPane(rect), self.shell.emotion_x, self.shell.emotion_y);
        // Avatar pixels can be opaque even around the figure. Draw the card
        // header last so it remains legible on every source avatar.
        var header_buf: [96]u8 = undefined;
        const header = std.fmt.bufPrint(&header_buf, "Character  /  {s}", .{character_name}) catch "Character";
        ui.drawPaneHeader(&self.canvas, rect, header);
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
    var x: i32 = 170;
    var index: u8 = 0;
    while (index < menu and index < menu_labels.len) : (index += 1) x += Canvas.uiTextWidth(menu_labels[index]) + 28;
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
    return .{ .x = menuStart(menu), .y = geometry.menu_height, .w = 210, .h = @as(i32, menuItemCount(menu)) * 29 + 10 };
}

fn menuPopupItem(menu: u8, x: i32, y: i32) ?u8 {
    const rect = menuPopupRect(menu);
    if (x < rect.x or x >= rect.right() or y < rect.y + 4 or y >= rect.bottom() - 4) return null;
    const item = @divTrunc(y - rect.y - 5, 29);
    if (item < 0 or item >= menuItemCount(menu)) return null;
    return @intCast(item);
}

fn drawMenuBar(c: *Canvas, rect: Rect, active: ?u8, hovered: ?u8) void {
    ui.drawMenuBarSurface(c, rect);
    ui.fillRoundedRect(c, rect.x + 10, rect.y + 6, 22, 22, 7, accent);
    _ = c.drawUiText("C", rect.x + 17, rect.y + 8, layer);
    _ = c.drawUiText("Comic Chat", rect.x + 42, rect.y + 8, layer);
    var x = rect.x + 170;
    for (menu_labels, 0..) |item, raw_index| {
        const index: u8 = @intCast(raw_index);
        const selected = active == index or hovered == index;
        const item_w = Canvas.uiTextWidth(item) + 16;
        ui.drawMenuLabel(c, x, rect.y, item_w, item, selected);
        x += Canvas.uiTextWidth(item) + 28;
        if (x >= rect.right() - 40) break;
    }
}

fn drawMenuPopup(c: *Canvas, menu: u8, hovered: ?u8) void {
    const rect = menuPopupRect(menu);
    ui.drawPopupSurface(c, rect);
    var item: u8 = 0;
    while (item < menuItemCount(menu)) : (item += 1) {
        const y = rect.y + 5 + @as(i32, item) * 29;
        ui.drawMenuItem(c, rect.x + 5, y, rect.w - 10, menuItemLabel(menu, item), hovered == item);
    }
}

fn drawToolBar(c: *Canvas, rect: Rect, comic_mode: bool, hovered: ?u8) void {
    ui.drawToolbarSurface(c, rect);
    const group_y = rect.y + 5;
    ui.drawToolbarGroup(c, .{ .x = rect.x + 8, .y = group_y, .w = 118, .h = 36 });
    ui.drawToolbarGroup(c, .{ .x = rect.x + 134, .y = group_y, .w = 78, .h = 36 });
    ui.drawToolbarGroup(c, .{ .x = rect.x + 222, .y = group_y, .w = 80, .h = 36 });
    ui.drawToolbarGroup(c, .{ .x = rect.x + 310, .y = group_y, .w = 118, .h = 36 });
    ui.drawToolbarGroup(c, .{ .x = rect.x + 436, .y = group_y, .w = 80, .h = 36 });
    const primary = [_]struct { glyph: ToolGlyph, index: u8, selected: bool }{
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
    var x = rect.x + 12;
    const y = rect.y + @divTrunc(rect.h - 32, 2);
    for (primary, 0..) |item, position| {
        if (position == 3 or position == 5 or position == 7 or position == 10) x += 12;
        _ = drawModernToolButton(c, item.glyph, x, y, item.selected, hovered == item.index);
        x += 38;
    }
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

fn toolbarButtonX(index: u8) ?i32 {
    const ids = [_]u8{ 0, 2, 4, 5, 6, 7, 8, 10, 11, 13, 17, 18 };
    const starts = [_]i32{ 12, 50, 88, 138, 176, 226, 264, 314, 352, 390, 440, 478 };
    for (ids, starts) |id, start| if (id == index) return start;
    return null;
}

fn drawToolbarTooltip(c: *Canvas, layout: geometry.Layout, index: u8) void {
    const button_x = toolbarButtonX(index) orelse return;
    const label = toolbarLabel(index);
    const width = @min(230, Canvas.uiTextWidth(label) + 20);
    const x = std.math.clamp(button_x - 4, 6, @max(6, layout.toolbar.right() - width - 6));
    ui.drawTooltip(c, .{ .x = x, .y = layout.tabs.bottom() + 7, .w = width, .h = 28 }, label);
}

fn drawModernToolButton(c: *Canvas, glyph: ToolGlyph, x: i32, y: i32, selected: bool, hovered: bool) i32 {
    const glyph_color = ui.drawCommandTile(c, x, y, selected, hovered);
    drawToolGlyph(c, glyph, x + 8, y + 8, glyph_color);
    return x + 32;
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
        .font => _ = c.drawUiText("A", x + 2, y - 3, color),
        .color => {
            drawCircleOutline(c, x + 8, y + 8, 7, color);
            c.fillRect(x + 3, y + 4, 3, 3, 0xff0067c0);
            c.fillRect(x + 8, y + 2, 3, 3, 0xff107c10);
            c.fillRect(x + 11, y + 7, 3, 3, 0xffc42b1c);
        },
        .bold => _ = c.drawUiText("B", x + 2, y - 3, color),
        .italic => _ = c.drawUiText("I", x + 4, y - 3, color),
        .underline => {
            _ = c.drawUiText("U", x + 2, y - 3, color);
            c.drawLine(x + 2, y + 15, x + 13, y + 15, color);
        },
        .fixed => {
            c.drawLine(x + 4, y + 3, x + 1, y + 8, color);
            c.drawLine(x + 1, y + 8, x + 4, y + 13, color);
            c.drawLine(x + 12, y + 3, x + 15, y + 8, color);
            c.drawLine(x + 15, y + 8, x + 12, y + 13, color);
        },
        .symbol => _ = c.drawUiText("#", x + 1, y - 3, color),
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
    return ui.drawToolbarSeparator(c, x, rect);
}

fn drawTabBar(c: *Canvas, layout: geometry.Layout, tabs: []const View.Tab, active: usize, focused: bool, comic_mode: bool, comic_columns: u8, column_hover: ?ColumnControlHover) void {
    const rect = layout.tabs;
    ui.drawTabStrip(c, rect);
    const status_w: i32 = 108;
    ui.drawStatusTab(c, rect);
    drawBubbleGlyph(c, rect.x + 16, rect.y + 11, accent, false);
    _ = c.drawUiText("Status", rect.x + 39, rect.y + 9, ink);
    const first_x = rect.x + status_w + 6;
    const tab_w: i32 = 164;
    for (tabs, 0..) |tab, index| {
        const x = first_x + @as(i32, @intCast(index)) * tab_w;
        if (x >= rect.right()) break;
        const width = @min(tab_w, rect.right() - x);
        ui.drawTab(c, x, rect.y + 5, width, rect.h - 5, index == active);
        drawTextEllipsized(c, tab.label, x + 14, rect.y + 9, width - 36, if (index == active) ink else if (tab.unread > 0) accent else secondary);
        if (tab.unread > 0) {
            var unread_buf: [12]u8 = undefined;
            const unread = std.fmt.bufPrint(&unread_buf, "{d}", .{tab.unread}) catch "!";
            _ = c.drawUiText(unread, x + width - Canvas.uiTextWidth(unread) - 10, rect.y + 9, accent);
        }
        if (focused and index == active) drawFocus(c, .{ .x = x, .y = rect.y + 2, .w = width, .h = rect.h - 2 });
    }
    if (comic_mode and layout.transcript.w >= 430) drawComicColumnControl(c, layout, comic_columns, column_hover);
}

fn drawComicColumnControl(c: *Canvas, layout: geometry.Layout, columns: u8, hovered: ?ColumnControlHover) void {
    const control = geometry.comicColumnControl(layout);
    ui.drawStepper(c, control, hovered == .decrease, hovered == .increase);
    const minus = geometry.comicColumnDecrease(layout);
    const plus = geometry.comicColumnIncrease(layout);
    c.drawLine(minus.x + 10, minus.y + 13, minus.x + 19, minus.y + 13, secondary);
    c.drawLine(plus.x + 10, plus.y + 13, plus.x + 19, plus.y + 13, secondary);
    c.drawLine(plus.x + 14, plus.y + 9, plus.x + 14, plus.y + 18, secondary);
    var label_buf: [16]u8 = undefined;
    const label = std.fmt.bufPrint(&label_buf, "{d} across", .{columns}) catch "4 across";
    const center_x = control.x + @divTrunc(control.w - Canvas.uiTextWidth(label), 2);
    _ = c.drawUiText(label, center_x, control.y + 3, ink);
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
        ui.drawSplitter(c, .{ .x = layout.transcript.right(), .y = layout.buffer.y, .w = geometry.splitter, .h = layout.buffer.h });
    ui.drawSplitter(c, .{ .x = layout.transcript.x, .y = layout.transcript.bottom(), .w = layout.transcript.w, .h = geometry.splitter });
    if (comic_mode) ui.drawSplitter(c, .{ .x = layout.right.x, .y = layout.members.bottom(), .w = layout.right.w, .h = geometry.splitter });
}

fn drawSayWindow(c: *Canvas, layout: geometry.Layout, input: []const u8, cursor: usize, selection: ?TextSelection, focused: bool, say_mode: shell_mod.SayMode, hovered_action: ?u8) void {
    const edit = layout.say_editor;
    ui.drawComposerSurface(c, layout.say);
    ui.drawComposerField(c, edit, focused);
    if (selection) |range| {
        const start = @min(range.start, input.len);
        const end = @min(@max(range.end, start), input.len);
        const x = edit.x + 16 + Canvas.uiTextWidth(input[0..start]);
        const w = Canvas.uiTextWidth(input[start..end]);
        c.fillRect(x, edit.y + 11, @max(1, w), @max(1, edit.h - 22), accent_soft);
    }
    if (input.len == 0) {
        drawTextEllipsized(c, "Write a message...", edit.x + 18, edit.y + 13, edit.w - 36, secondary);
    } else {
        drawTextEllipsized(c, input, edit.x + 18, edit.y + 13, edit.w - 36, ink);
    }
    const safe_cursor = @min(cursor, input.len);
    const caret_x = @min(edit.right() - 12, edit.x + 16 + Canvas.uiTextWidth(input[0..safe_cursor]));
    if (focused) c.fillRect(caret_x, edit.y + 12, 2, @max(1, edit.h - 24), accent);

    const glyphs = [_]SayGlyph{ .say, .think, .whisper, .action, .sound };
    var x = layout.say_actions.x;
    for (glyphs, 0..) |glyph, index| {
        const selected = @intFromEnum(say_mode) == index;
        const glyph_color = ui.drawActionTile(c, x, layout.say_actions.y, layout.say_action_size, layout.say_actions.h, selected, hovered_action == @as(u8, @intCast(index)));
        drawSayGlyph(c, glyph, x + @divTrunc(layout.say_action_size - 16, 2), layout.say_actions.y + 18, glyph_color);
        x += layout.say_action_size;
    }
    if (focused) drawFocus(c, layout.say);
}

fn drawSayActionTooltip(c: *Canvas, layout: geometry.Layout, index: u8) void {
    if (index >= geometry.say_button_count) return;
    const label = sayActionLabel(index);
    const width = Canvas.uiTextWidth(label) + 20;
    const action_x = layout.say_actions.x + @as(i32, index) * layout.say_action_size;
    const x = std.math.clamp(action_x + @divTrunc(layout.say_action_size - width, 2), 6, @max(6, layout.transcript.right() - width - 6));
    ui.drawTooltip(c, .{ .x = x, .y = layout.say.y - 34, .w = width, .h = 28 }, label);
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

fn drawEmptyBuffer(c: *Canvas, rect: Rect, text: []const u8, columns: u8) void {
    ui.drawEmptyState(c, rect.x, rect.y, rect.w, rect.h, text, columns);
}

fn drawFocus(c: *Canvas, rect: Rect) void {
    ui.drawFocusRing(c, rect);
}

fn drawTextEllipsized(c: *Canvas, text: []const u8, x: i32, y: i32, max_w: i32, color: u32) void {
    if (max_w <= 0) return;
    if (Canvas.uiTextWidth(text) <= max_w) {
        _ = c.drawUiText(text, x, y, color);
        return;
    }
    const dots = "...";
    const dots_w = Canvas.uiTextWidth(dots);
    var end = text.len;
    while (end > 0 and Canvas.uiTextWidth(text[0..end]) + dots_w > max_w) end -= 1;
    _ = c.drawUiText(text[0..end], x, y, color);
    _ = c.drawUiText(dots, x + Canvas.uiTextWidth(text[0..end]), y, color);
}

fn blitFit(c: *Canvas, src: []const u32, sw: u32, sh: u32, x: i32, y: i32, max_w: i32, max_h: i32) void {
    var fit = fitRect(sw, sh, x, y, max_w, max_h) orelse return;
    fit.y = y + @min(14, @max(0, max_h - fit.h));
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

fn blitHeightBottomAlphaSmooth(c: *Canvas, src: []const u32, sw: u32, sh: u32, x: i32, y: i32, area_w: i32, area_h: i32) void {
    if (sw == 0 or sh == 0 or area_w <= 0 or area_h <= 0) return;
    var draw_h = area_h;
    var draw_w: i32 = @max(1, @as(i32, @intCast(@divTrunc(@as(i64, sw) * draw_h, sh))));
    if (draw_w > area_w) {
        draw_w = area_w;
        draw_h = @max(1, @as(i32, @intCast(@divTrunc(@as(i64, sh) * draw_w, sw))));
    }
    const draw_x = x + @divTrunc(area_w - draw_w, 2);
    const draw_y = y + area_h - draw_h;
    var oy: i32 = 0;
    while (oy < draw_h) : (oy += 1) {
        const sy_fp: u64 = if (draw_h <= 1 or sh <= 1) 0 else @intCast(@divTrunc(@as(i64, oy) * (@as(i64, sh) - 1) * 65536, draw_h - 1));
        var ox: i32 = 0;
        while (ox < draw_w) : (ox += 1) {
            const sx_fp: u64 = if (draw_w <= 1 or sw <= 1) 0 else @intCast(@divTrunc(@as(i64, ox) * (@as(i64, sw) - 1) * 65536, draw_w - 1));
            const sample = bilinearAlphaSample(src, sw, sh, sx_fp, sy_fp);
            if (sample.alpha != 0) c.blendPixel(draw_x + ox, draw_y + oy, sample.rgb, sample.alpha);
        }
    }
}

const AlphaSample = struct { rgb: u32, alpha: u32 };

fn bilinearAlphaSample(src: []const u32, sw: u32, sh: u32, sx_fp: u64, sy_fp: u64) AlphaSample {
    const x0: u32 = @intCast(@min(@as(u64, sw - 1), sx_fp >> 16));
    const y0: u32 = @intCast(@min(@as(u64, sh - 1), sy_fp >> 16));
    const x1 = @min(sw - 1, x0 + 1);
    const y1 = @min(sh - 1, y0 + 1);
    const fx = sx_fp & 0xffff;
    const fy = sy_fp & 0xffff;
    const one: u64 = 65536;
    const weights = [_]u64{ (one - fx) * (one - fy), fx * (one - fy), (one - fx) * fy, fx * fy };
    const pixels = [_]u32{
        src[@as(usize, y0) * @as(usize, sw) + @as(usize, x0)],
        src[@as(usize, y0) * @as(usize, sw) + @as(usize, x1)],
        src[@as(usize, y1) * @as(usize, sw) + @as(usize, x0)],
        src[@as(usize, y1) * @as(usize, sw) + @as(usize, x1)],
    };
    var weighted_alpha: u64 = 0;
    var red: u64 = 0;
    var green: u64 = 0;
    var blue: u64 = 0;
    for (pixels, weights) |pixel, weight| {
        const alpha = @as(u64, pixel >> 24);
        const alpha_weight = alpha * weight;
        weighted_alpha += alpha_weight;
        red += @as(u64, (pixel >> 16) & 0xff) * alpha_weight;
        green += @as(u64, (pixel >> 8) & 0xff) * alpha_weight;
        blue += @as(u64, pixel & 0xff) * alpha_weight;
    }
    if (weighted_alpha == 0) return .{ .rgb = 0, .alpha = 0 };
    const alpha: u32 = @intCast(@min(@as(u64, 255), weighted_alpha >> 32));
    const rgb = (@as(u32, @intCast(red / weighted_alpha)) << 16) |
        (@as(u32, @intCast(green / weighted_alpha)) << 8) |
        @as(u32, @intCast(blue / weighted_alpha));
    return .{ .rgb = rgb, .alpha = alpha };
}

fn drawEmotionWheel(c: *Canvas, rect: Rect, selector_x: i16, selector_y: i16) void {
    ui.drawExpressionPanel(c, rect, emotionLabel(selector_x, selector_y));
    const dial = emotionDialRect(rect);
    const cx = dial.x + @divTrunc(dial.w, 2);
    const cy = dial.y + @divTrunc(dial.h, 2);
    const radius = @max(1, @min(@divTrunc(dial.w, 2), @divTrunc(dial.h, 2)) - 9);
    fillOutlinedCircle(c, cx, cy, radius, ui.Theme.layer, ui.Theme.divider);
    fillOutlinedCircle(c, cx, cy, @max(1, radius - 6), ui.Theme.chrome, ui.Theme.accent_soft);

    const directions = [_][2]i32{
        .{ -707, -707 }, .{ 0, -1000 }, .{ 707, -707 },
        .{ -1000, 0 },   .{ 1000, 0 },  .{ -707, 707 },
        .{ 0, 1000 },    .{ 707, 707 },
    };
    const glyph_positions = [_][2]i32{ .{ 0, 0 }, .{ 0, 1 }, .{ 0, 2 }, .{ 1, 0 }, .{ 1, 2 }, .{ 2, 0 }, .{ 2, 1 }, .{ 2, 2 } };
    const selected_col = emotionGridCoordinate(selector_x);
    const selected_row = emotionGridCoordinate(selector_y);
    const icon_radius = @max(14, radius - 10);
    for (directions, glyph_positions) |direction, glyph_position| {
        const gx = cx + @divTrunc(direction[0] * icon_radius, 1000);
        const gy = cy + @divTrunc(direction[1] * icon_radius, 1000);
        const selected = glyph_position[1] == selected_col and glyph_position[0] == selected_row;
        drawMoodGlyph(c, gx, gy, glyph_position[0], glyph_position[1], selected);
    }

    const neutral = selected_col == 1 and selected_row == 1;
    drawMoodGlyph(c, cx, cy, 1, 1, neutral);
}

fn emotionWheelRect(layout: geometry.Layout) Rect {
    return emotionWheelRectFromPane(layout.body_camera);
}

fn emotionWheelRectFromPane(pane: Rect) Rect {
    const wheel_side = if (pane.w >= 93) @min(pane.w, 159) else 0;
    return .{ .x = pane.x, .y = pane.bottom() - wheel_side, .w = pane.w, .h = wheel_side };
}

fn emotionDialRect(rect: Rect) Rect {
    return .{ .x = rect.x + 8, .y = rect.y + 30, .w = @max(0, rect.w - 16), .h = @max(0, rect.h - 38) };
}

fn emotionGridCoordinate(value: i16) i32 {
    if (value < -5) return 0;
    if (value > 5) return 2;
    return 1;
}

fn emotionLabel(x: i16, y: i16) []const u8 {
    const col = emotionGridCoordinate(x);
    const row = emotionGridCoordinate(y);
    return switch (row * 3 + col) {
        0 => "Angry",
        1 => "Loud",
        2 => "Laughing",
        3 => "Sad",
        4 => "Neutral",
        5 => "Happy",
        6 => "Uneasy",
        7 => "Bored",
        else => "Coy",
    };
}

fn drawMoodGlyph(c: *Canvas, cx: i32, cy: i32, row: i32, column: i32, selected: bool) void {
    const face_fill = if (selected) accent else layer;
    const face_border = if (selected) accent else divider;
    const feature = if (selected) layer else ink;
    fillOutlinedCircle(c, cx, cy, 11, face_fill, face_border);

    const mood = row * 3 + column;
    switch (mood) {
        0 => { // angry
            drawFeatureLine(c, cx - 6, cy - 5, cx - 2, cy - 3, feature);
            drawFeatureLine(c, cx + 2, cy - 3, cx + 6, cy - 5, feature);
            c.fillRect(cx - 5, cy - 1, 2, 2, feature);
            c.fillRect(cx + 3, cy - 1, 2, 2, feature);
            drawFeatureLine(c, cx - 4, cy + 6, cx, cy + 3, feature);
            drawFeatureLine(c, cx, cy + 3, cx + 4, cy + 6, feature);
        },
        1 => { // loud
            c.fillRect(cx - 5, cy - 3, 3, 2, feature);
            c.fillRect(cx + 2, cy - 3, 3, 2, feature);
            fillOutlinedCircle(c, cx, cy + 4, 4, feature, feature);
            c.fillRect(cx - 2, cy + 3, 4, 2, face_fill);
        },
        2 => { // laughing
            drawFeatureLine(c, cx - 6, cy - 2, cx - 4, cy - 4, feature);
            drawFeatureLine(c, cx - 4, cy - 4, cx - 2, cy - 2, feature);
            drawFeatureLine(c, cx + 2, cy - 2, cx + 4, cy - 4, feature);
            drawFeatureLine(c, cx + 4, cy - 4, cx + 6, cy - 2, feature);
            drawFeatureLine(c, cx - 5, cy + 2, cx, cy + 6, feature);
            drawFeatureLine(c, cx, cy + 6, cx + 5, cy + 2, feature);
        },
        3 => { // sad
            c.fillRect(cx - 5, cy - 3, 2, 2, feature);
            c.fillRect(cx + 3, cy - 3, 2, 2, feature);
            drawFeatureLine(c, cx - 5, cy + 6, cx, cy + 3, feature);
            drawFeatureLine(c, cx, cy + 3, cx + 5, cy + 6, feature);
        },
        4 => { // neutral
            c.fillRect(cx - 5, cy - 3, 2, 2, feature);
            c.fillRect(cx + 3, cy - 3, 2, 2, feature);
            drawFeatureLine(c, cx - 4, cy + 4, cx + 4, cy + 4, feature);
        },
        5 => { // happy
            c.fillRect(cx - 5, cy - 3, 2, 2, feature);
            c.fillRect(cx + 3, cy - 3, 2, 2, feature);
            drawFeatureLine(c, cx - 5, cy + 2, cx, cy + 6, feature);
            drawFeatureLine(c, cx, cy + 6, cx + 5, cy + 2, feature);
        },
        6 => { // uneasy
            c.fillRect(cx - 5, cy - 3, 2, 2, feature);
            c.fillRect(cx + 3, cy - 2, 2, 2, feature);
            drawFeatureLine(c, cx - 5, cy + 5, cx - 1, cy + 3, feature);
            drawFeatureLine(c, cx - 1, cy + 3, cx + 4, cy + 5, feature);
        },
        7 => { // bored
            drawFeatureLine(c, cx - 6, cy - 3, cx - 2, cy - 3, feature);
            drawFeatureLine(c, cx + 2, cy - 3, cx + 6, cy - 3, feature);
            c.fillRect(cx - 5, cy - 1, 2, 1, feature);
            c.fillRect(cx + 3, cy - 1, 2, 1, feature);
            drawFeatureLine(c, cx - 4, cy + 5, cx + 4, cy + 5, feature);
        },
        else => { // coy
            c.fillRect(cx - 5, cy - 3, 2, 2, feature);
            drawFeatureLine(c, cx + 2, cy - 3, cx + 6, cy - 3, feature);
            drawFeatureLine(c, cx - 3, cy + 3, cx + 1, cy + 5, feature);
            drawFeatureLine(c, cx + 1, cy + 5, cx + 5, cy + 2, feature);
        },
    }
}

fn drawFeatureLine(c: *Canvas, x1: i32, y1: i32, x2: i32, y2: i32, color: u32) void {
    c.drawLine(x1, y1, x2, y2, color);
    c.drawLine(x1, y1 + 1, x2, y2 + 1, color);
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

fn drawDialog(c: *Canvas, spec: dialogs.Spec, editors: *const [3]input_mod.Editor, active_field: usize, notice: []const u8, hovered_button: ?ui.DialogButton) void {
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
        const field_active = index == active_field;
        switch (field.kind) {
            .text, .password => ui.drawField(c, field_rect.x, field_y, field_rect.w, field_active),
            .choice => ui.drawChoiceField(c, field_rect.x, field_y, field_rect.w, field_active),
            .list => ui.drawListField(c, field_rect.x, field_y, field_rect.w, field_active),
            .preview => ui.drawPreviewField(c, field_rect.x, field_y, field_rect.w),
            .readonly => ui.drawReadonlyField(c, field_rect.x, field_y, field_rect.w),
        }
        if (index < editors.len) {
            const editor = &editors[index];
            const value = editor.text();
            if (value.len != 0 and field.kind == .password) {
                var mask: [64]u8 = undefined;
                const mask_len = @min(value.len, mask.len);
                @memset(mask[0..mask_len], '*');
                drawTextEllipsized(c, mask[0..mask_len], field_rect.x + 10, field_y + 4, field_rect.w - 20, ink);
            } else if (value.len != 0) {
                const value_x = field_rect.x + if (field.kind == .list or field.kind == .readonly) @as(i32, 28) else @as(i32, 10);
                drawTextEllipsized(c, value, value_x, field_y + 4, field_rect.right() - value_x - 10, ink);
            }
            if (field_active and dialogs.fieldAcceptsText(spec.id, index)) {
                const safe_cursor = @min(editor.cursor, value.len);
                const caret_x = @min(field_rect.right() - 12, field_rect.x + 10 + Canvas.uiTextWidth(value[0..safe_cursor]));
                c.fillRect(caret_x, field_y + 6, 2, 18, accent);
            }
        }
        if (editors[index].text().len == 0) {
            const hint_x = field_rect.x + if (field.kind == .list or field.kind == .readonly) @as(i32, 28) else if (field.kind == .preview) @as(i32, 72) else @as(i32, 10);
            const hint_right = field_rect.right() - if (field.kind == .choice) @as(i32, 42) else @as(i32, 10);
            drawTextEllipsized(c, field.hint, hint_x, field_y + 4, hint_right - hint_x, secondary);
        }
    }

    if (notice.len != 0) ui.drawNotice(c, rect.x + 14, dialog_layout.primary.y - 22, rect.w - 28, notice, .warning);
    drawDialogButton(c, dialog_layout.primary.x, dialog_layout.primary.y, dialog_layout.primary.w, dialogs.primaryLabel(spec.id), .primary, hovered_button == .primary);
    drawDialogButton(c, dialog_layout.cancel.x, dialog_layout.cancel.y, dialog_layout.cancel.w, "Cancel", .secondary, hovered_button == .cancel);
}

fn dialogPrimaryButtonWidth(id: dialogs.Id) i32 {
    return @max(84, Canvas.uiTextWidth(dialogs.primaryLabel(id)) + 24);
}

fn drawDialogButton(c: *Canvas, x: i32, y: i32, width: i32, label: []const u8, kind: ui.ButtonKind, hovered: bool) void {
    ui.drawButton(c, x, y, width, label, kind, hovered);
}

test "view renders modern empty buffer and chrome" {
    const gpa = std.testing.allocator;
    var view = try View.init(gpa, 960, 720);
    defer view.deinit();
    var transcript = session.Transcript.init(gpa);
    defer transcript.deinit();
    try transcript.setSelf("anna");

    try view.render("Comic Chat | #root | anna", "connected", &transcript, "hello", 3);
    const layout = geometry.Layout.compute(960, 720, true, true);
    try std.testing.expectEqual(ui.Theme.navigation, view.pixels()[0]);
    try std.testing.expectEqual(divider, view.pixels()[@as(usize, @intCast(layout.tabs.bottom() - 1)) * 960]);
    try std.testing.expectEqual(ui.Theme.workspace, view.pixels()[@as(usize, @intCast(layout.say.y + 2)) * 960 + 2]);

    const wheel = emotionWheelRect(layout);
    const dial = emotionDialRect(wheel);
    try std.testing.expectEqual(accent, view.pixels()[@as(usize, @intCast(wheel.y + 13)) * 960 + @as(usize, @intCast(layout.body_camera.x + 15))]);
    try std.testing.expectEqual(ui.Theme.chrome, view.pixels()[@as(usize, @intCast(dial.y + @divTrunc(dial.h, 2))) * 960 + @as(usize, @intCast(dial.x + @divTrunc(dial.w, 2) + 20))]);
}

test "emotion dial selects and drags only within its circular control" {
    const gpa = std.testing.allocator;
    var view = try View.init(gpa, 960, 720);
    defer view.deinit();
    const layout = geometry.Layout.compute(960, 720, true, true);
    const dial = emotionDialRect(emotionWheelRect(layout));
    const cx = dial.x + @divTrunc(dial.w, 2);
    const cy = dial.y + @divTrunc(dial.h, 2);

    _ = view.handlePointer(.{ .kind = .down, .x = cx + 12, .y = cy, .button = .primary }, 0, 0);
    try std.testing.expect(view.emotion_dragging);
    try std.testing.expect(view.shell.emotion_x > 0);
    _ = view.handlePointerMove(.{ .kind = .move, .x = cx - 18, .y = cy }, 0);
    try std.testing.expect(view.shell.emotion_x < 0);
    _ = view.handlePointer(.{ .kind = .up, .x = cx - 18, .y = cy, .button = .primary }, 0, 0);
    try std.testing.expect(!view.emotion_dragging);
}

test "view exposes a semantic shell snapshot without inspecting pixels" {
    var view = try View.init(std.testing.allocator, 960, 720);
    defer view.deinit();
    const tabs = [_]View.Tab{ .{ .label = "#root" }, .{ .label = "#onyx", .unread = 2 } };
    const snapshot = view.semanticSnapshot("connected", &tabs, 0);
    try std.testing.expect(snapshot.items().len >= 12);
    try std.testing.expectEqual(accessibility.Role.window, snapshot.items()[0].role);
    var found_selected_tab = false;
    for (snapshot.items()) |item| {
        if (item.role == .tab and item.selected) found_selected_tab = true;
    }
    try std.testing.expect(found_selected_tab);
}

test "menu clicks select navigation without opening a modal dialog" {
    var view = try View.init(std.testing.allocator, 960, 720);
    defer view.deinit();
    const action = view.handlePointer(.{ .kind = .down, .x = 174, .y = 10, .button = .primary }, 0, 0);
    try std.testing.expectEqual(Action{ .menu = 0 }, action);
    try std.testing.expect(view.active_dialog == null);
    try std.testing.expectEqual(shell_mod.Focus.navigation, view.shell.focus);
}

test "comic density stepper changes the live four-across layout" {
    var view = try View.init(std.testing.allocator, 960, 720);
    defer view.deinit();
    const layout = geometry.Layout.compute(960, 720, true, true);
    const increase = geometry.comicColumnIncrease(layout);
    _ = view.handlePointer(.{ .kind = .down, .x = increase.x + 3, .y = increase.y + 3, .button = .primary }, 0, 0);
    try std.testing.expectEqual(@as(u8, 5), view.shell.comic_columns);
    const decrease = geometry.comicColumnDecrease(layout);
    _ = view.handlePointer(.{ .kind = .down, .x = decrease.x + 3, .y = decrease.y + 3, .button = .primary }, 0, 0);
    try std.testing.expectEqual(@as(u8, 4), view.shell.comic_columns);
}

test "composer and member controls expose hover state" {
    var view = try View.init(std.testing.allocator, 960, 720);
    defer view.deinit();
    const layout = geometry.Layout.compute(960, 720, true, true);
    const whisper_x = layout.say_actions.x + 2 * layout.say_action_size + 4;
    try std.testing.expect(view.handlePointerMove(.{ .kind = .move, .x = whisper_x, .y = layout.say_actions.y + 8 }, 2));
    try std.testing.expectEqual(@as(?u8, 2), view.hovered_say_action);
    try std.testing.expect(view.handlePointerMove(.{ .kind = .move, .x = layout.members.x + 8, .y = layout.members.y + 35 }, 2));
    try std.testing.expectEqual(@as(?usize, 0), view.hovered_member);
}

test "typed dialog choices cycle instead of accepting arbitrary text" {
    var view = try View.init(std.testing.allocator, 960, 720);
    defer view.deinit();
    view.openDialog(.settings);
    const layout = dialogLayout(view.width(), view.height(), dialogs.get(.settings));
    const security = layout.fieldRect(2);
    _ = view.handlePointer(.{ .kind = .down, .x = security.x + 4, .y = security.y + 4, .button = .primary }, 0, 0);
    try std.testing.expectEqualStrings("Verified TLS", view.dialogValueAt(2));
    _ = view.handlePointer(.{ .kind = .down, .x = security.x + 4, .y = security.y + 4, .button = .primary }, 0, 0);
    try std.testing.expectEqualStrings("Strict TLS", view.dialogValueAt(2));
}

test "alpha-aware avatar scaling does not pull black from transparent pixels" {
    const source = [_]u32{ 0xffffffff, 0x00000000 };
    const sample = bilinearAlphaSample(&source, 2, 1, 32768, 0);
    try std.testing.expect(sample.alpha >= 127 and sample.alpha <= 128);
    try std.testing.expectEqual(@as(u32, 0x00ffffff), sample.rgb);
}

test "every bundled avatar produces a visible mugshot and full figure" {
    const names = [_][]const u8{
        "anna",   "armando",  "bolo",    "cro",  "dan",     "denise", "hugh",   "jordan", "kevin", "kwensa",   "lance",
        "lynnea", "margaret", "maynard", "mike", "rebecca", "sage",   "scotty", "susan",  "tiki",  "tongtyed", "xeno",
    };
    var canvas = try Canvas.init(std.testing.allocator, 120, 180);
    defer canvas.deinit(std.testing.allocator);
    for (names) |name| {
        const data = strip.avatarByName(name) orelse return error.TestUnexpectedResult;
        var icon = try bgb.decodeIcon(std.testing.allocator, data);
        defer icon.deinit(std.testing.allocator);
        try std.testing.expect(icon.width > 0 and icon.height > 0);
        canvas.clear(layer);
        blitHeightBottomAlphaSmooth(&canvas, icon.pixels, icon.width, icon.height, 30, 10, 60, 60);
        var visible_icon = false;
        for (canvas.px) |pixel| if (pixel != layer) {
            visible_icon = true;
            break;
        };
        try std.testing.expect(visible_icon);

        var full = try figure.assembleForText(std.testing.allocator, data, "");
        defer full.deinit(std.testing.allocator);
        try std.testing.expect(full.width > 0 and full.height > 0);
        canvas.clear(layer);
        blitHeightBottomAlphaSmooth(&canvas, full.pixels, full.width, full.height, 10, 10, 100, 160);
        var visible_figure = false;
        for (canvas.px) |pixel| if (pixel != layer) {
            visible_figure = true;
            break;
        };
        try std.testing.expect(visible_figure);
        try std.testing.expectEqual(layer, canvas.px[0]);
    }
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
