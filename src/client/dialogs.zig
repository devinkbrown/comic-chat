//! Portable registry for every dialog/property-page template in chat.rc.
//!
//! The registry is intentionally exhaustive: application routing refers to a
//! typed ID instead of scattering legacy resource numbers through the UI.

const std = @import("std");

pub const Id = enum {
    about,
    room_list,
    settings,
    personal,
    character,
    background,
    kick,
    nickname,
    channel,
    channel_properties,
    ban,
    invite,
    sound,
    set_text_font,
    user_list,
    whisper,
    comics_view,
    automation,
    rules,
    edit_rule,
    channel_create,
    channel_password,
    file_transfer,
    motd,
    setup,
    away,
    text_font,
    choose_color,
    invitation,
    advanced_event_params,
    rule_sets,
    add_to_sets,
    rename_loaded_set,
    rename_set,
    notifications,
    advanced_rule_settings,
    notification_users,
    servers,
    password,
    create_set,
};

pub const Group = enum { connection, rooms, automation, files };

pub const Spec = struct {
    id: Id,
    resource: []const u8,
    title: []const u8,
    group: Group,
    source_w: u16,
    source_h: u16,
};

pub const specs = [_]Spec{
    .{ .id = .about, .resource = "IDD_ABOUTBOX", .title = "About Comic Chat", .group = .files, .source_w = 279, .source_h = 137 },
    .{ .id = .room_list, .resource = "IDD_ROOMLIST", .title = "Room List", .group = .rooms, .source_w = 400, .source_h = 255 },
    .{ .id = .settings, .resource = "IDD_SETTINGSPAGE", .title = "Settings", .group = .connection, .source_w = 252, .source_h = 218 },
    .{ .id = .personal, .resource = "IDD_PERSONALPAGE_IRC", .title = "Personal Profile", .group = .connection, .source_w = 252, .source_h = 218 },
    .{ .id = .character, .resource = "IDD_CHARACTERPAGE", .title = "Character", .group = .connection, .source_w = 252, .source_h = 218 },
    .{ .id = .background, .resource = "IDD_BACKGROUNDPAGE", .title = "Background", .group = .connection, .source_w = 252, .source_h = 218 },
    .{ .id = .kick, .resource = "IDD_KICK", .title = "Kick Member", .group = .rooms, .source_w = 186, .source_h = 89 },
    .{ .id = .nickname, .resource = "IDD_NICKNAME", .title = "Nickname", .group = .connection, .source_w = 188, .source_h = 71 },
    .{ .id = .channel, .resource = "IDD_CHANNEL", .title = "Enter Room", .group = .rooms, .source_w = 144, .source_h = 110 },
    .{ .id = .channel_properties, .resource = "IDD_CHANNELPROP", .title = "Room Properties", .group = .rooms, .source_w = 186, .source_h = 196 },
    .{ .id = .ban, .resource = "IDD_BAN", .title = "Ban or Unban", .group = .rooms, .source_w = 186, .source_h = 170 },
    .{ .id = .invite, .resource = "IDD_INVITE", .title = "Invite Member", .group = .rooms, .source_w = 186, .source_h = 89 },
    .{ .id = .sound, .resource = "IDD_SOUND_DLG", .title = "Send Sound", .group = .rooms, .source_w = 188, .source_h = 228 },
    .{ .id = .set_text_font, .resource = "IDD_SETTEXTFONT", .title = "Set Text Font", .group = .connection, .source_w = 264, .source_h = 261 },
    .{ .id = .user_list, .resource = "IDD_USERLIST", .title = "User List", .group = .rooms, .source_w = 395, .source_h = 263 },
    .{ .id = .whisper, .resource = "IDD_WHISPERBOX", .title = "Whisper Box", .group = .rooms, .source_w = 334, .source_h = 196 },
    .{ .id = .comics_view, .resource = "IDD_COMICS_VIEW", .title = "Comic View", .group = .connection, .source_w = 252, .source_h = 218 },
    .{ .id = .automation, .resource = "IDD_AUTOMATION_PAGE", .title = "Automation", .group = .automation, .source_w = 252, .source_h = 218 },
    .{ .id = .rules, .resource = "IDD_RULESPAGE", .title = "Rules", .group = .automation, .source_w = 252, .source_h = 218 },
    .{ .id = .edit_rule, .resource = "IDD_EDITRULE", .title = "Edit Rule", .group = .automation, .source_w = 265, .source_h = 260 },
    .{ .id = .channel_create, .resource = "IDD_CHANNELCREATE", .title = "Create Room", .group = .rooms, .source_w = 186, .source_h = 194 },
    .{ .id = .channel_password, .resource = "IDD_CHANPASSWORD", .title = "Room Password", .group = .rooms, .source_w = 173, .source_h = 86 },
    .{ .id = .file_transfer, .resource = "IDD_FILE_TRANSFER", .title = "File Transfer", .group = .files, .source_w = 186, .source_h = 93 },
    .{ .id = .motd, .resource = "IDD_MOTD", .title = "Message of the Day", .group = .rooms, .source_w = 298, .source_h = 146 },
    .{ .id = .setup, .resource = "IDD_SETUPDIALOG", .title = "Connection Setup", .group = .connection, .source_w = 252, .source_h = 218 },
    .{ .id = .away, .resource = "IDD_AWAYDLG", .title = "Away", .group = .rooms, .source_w = 186, .source_h = 87 },
    .{ .id = .text_font, .resource = "IDD_TEXTFONTPAGE_IRC", .title = "Text Font", .group = .connection, .source_w = 252, .source_h = 218 },
    .{ .id = .choose_color, .resource = "IDD_CHOOSECOLOR", .title = "Choose Color", .group = .connection, .source_w = 118, .source_h = 38 },
    .{ .id = .invitation, .resource = "IDD_INVITATION", .title = "Invitation", .group = .rooms, .source_w = 186, .source_h = 93 },
    .{ .id = .advanced_event_params, .resource = "IDD_ADVANCEDEVENTPARAMS", .title = "Advanced Event Parameters", .group = .automation, .source_w = 186, .source_h = 85 },
    .{ .id = .rule_sets, .resource = "IDD_RULESETSPAGE", .title = "Rule Sets", .group = .automation, .source_w = 252, .source_h = 218 },
    .{ .id = .add_to_sets, .resource = "IDD_ADDTOSETS", .title = "Add to Rule Sets", .group = .automation, .source_w = 252, .source_h = 161 },
    .{ .id = .rename_loaded_set, .resource = "IDD_RENAMELOADEDSET", .title = "Rename Loaded Set", .group = .automation, .source_w = 258, .source_h = 103 },
    .{ .id = .rename_set, .resource = "IDD_RENAMESET", .title = "Rename Rule Set", .group = .automation, .source_w = 226, .source_h = 79 },
    .{ .id = .notifications, .resource = "IDD_NOTIFICATIONS", .title = "Logon Notifications", .group = .automation, .source_w = 252, .source_h = 218 },
    .{ .id = .advanced_rule_settings, .resource = "IDD_ADVANCEDRULESETTINGS", .title = "Advanced Rule Settings", .group = .automation, .source_w = 186, .source_h = 95 },
    .{ .id = .notification_users, .resource = "IDD_NOTIFICATIONUSERS", .title = "Notification Users", .group = .automation, .source_w = 262, .source_h = 111 },
    .{ .id = .servers, .resource = "IDD_SERVERSPAGE", .title = "Servers", .group = .connection, .source_w = 252, .source_h = 218 },
    .{ .id = .password, .resource = "IDD_PASSWORD", .title = "Password", .group = .connection, .source_w = 198, .source_h = 127 },
    .{ .id = .create_set, .resource = "IDD_CREATESET", .title = "Create Rule Set", .group = .automation, .source_w = 226, .source_h = 79 },
};

pub fn get(id: Id) Spec {
    return specs[@intFromEnum(id)];
}

pub fn fromResource(name: []const u8) ?Id {
    for (specs) |spec| if (std.ascii.eqlIgnoreCase(name, spec.resource)) return spec.id;
    return null;
}

pub fn prompt(id: Id) ?[]const u8 {
    return switch (id) {
        .channel, .channel_create => "Room name",
        .nickname => "Nickname",
        .kick, .ban, .invite, .whisper, .notification_users => "Member nickname",
        .away => "Away message",
        .password, .channel_password => "Password",
        .choose_color => "Color value",
        .sound => "Sound name",
        .set_text_font, .text_font => "Font name and size",
        .rename_loaded_set, .rename_set, .create_set => "Rule set name",
        .advanced_event_params => "Event parameters",
        .file_transfer => "File path",
        .background => "Backdrop name",
        .character => "Character name",
        .personal => "Profile text",
        else => null,
    };
}

pub fn primaryLabel(id: Id) []const u8 {
    return switch (id) {
        .channel, .channel_create => "Join",
        .kick => "Kick",
        .ban => "Apply",
        .invite => "Invite",
        .whisper => "Open",
        .file_transfer => "Send",
        .away => "Set Away",
        else => "OK",
    };
}

test "registry covers all forty Microsoft dialog templates exactly once" {
    try std.testing.expectEqual(@as(usize, 40), specs.len);
    var seen: [specs.len]bool = @splat(false);
    for (specs) |spec| {
        const index = @intFromEnum(spec.id);
        try std.testing.expect(!seen[index]);
        seen[index] = true;
        try std.testing.expectEqual(spec.id, fromResource(spec.resource).?);
    }
}
