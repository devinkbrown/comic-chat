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
    open_conversation,
    save_conversation,
    export_image,
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

/// Visible controls for the portable dialog surface. The first editable field
/// is bound to the shared modal editor; remaining rows expose the rest of the
/// established dialog contract without collapsing every dialog into one placeholder.
pub const FieldKind = enum { text, password, choice, list, preview, readonly };
pub const Field = struct { label: []const u8, hint: []const u8 = "", kind: FieldKind = .text };

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
    .{ .id = .open_conversation, .resource = "PORTABLE_OPEN_CONVERSATION", .title = "Open Conversation", .group = .files, .source_w = 300, .source_h = 108 },
    .{ .id = .save_conversation, .resource = "PORTABLE_SAVE_CONVERSATION", .title = "Save Conversation", .group = .files, .source_w = 300, .source_h = 108 },
    .{ .id = .export_image, .resource = "PORTABLE_EXPORT_IMAGE", .title = "Export Comic Image", .group = .files, .source_w = 300, .source_h = 108 },
};

pub const microsoft_dialog_count: usize = 40;

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
        .open_conversation => "Conversation file",
        .save_conversation => "Conversation file",
        .export_image => "PNG file",
        .background => "Backdrop name",
        .character => "Character name",
        .personal => "Profile text",
        else => null,
    };
}

pub fn fields(id: Id) []const Field {
    return switch (id) {
        .setup, .settings, .servers => &.{ .{ .label = "Server", .hint = "Secure IRC endpoint" }, .{ .label = "Port", .hint = "6697" }, .{ .label = "Security", .hint = "Verified TLS", .kind = .choice } },
        .personal => &.{ .{ .label = "Profile text" }, .{ .label = "Display name" }, .{ .label = "Homepage" } },
        .character => &.{ .{ .label = "Character name", .kind = .choice }, .{ .label = "Preview", .hint = "Bundled Comic Chat character", .kind = .preview } },
        .background => &.{ .{ .label = "Backdrop name", .kind = .choice }, .{ .label = "Preview", .hint = "Bundled background", .kind = .preview } },
        .nickname => &.{.{ .label = "Nickname" }},
        .password => &.{ .{ .label = "Account" }, .{ .label = "Password", .kind = .password } },
        .channel, .channel_create => &.{ .{ .label = "Room name" }, .{ .label = "Topic", .hint = "Optional" } },
        .channel_properties => &.{ .{ .label = "Topic" }, .{ .label = "Modes" }, .{ .label = "Limit" } },
        .channel_password => &.{.{ .label = "Room password" }},
        .room_list => &.{ .{ .label = "Room name", .hint = "For example #root" }, .{ .label = "Filter", .hint = "Optional room filter" } },
        .user_list => &.{ .{ .label = "Member nickname", .hint = "Choose a visible room member" }, .{ .label = "Filter", .hint = "Optional nickname filter" } },
        .kick, .ban, .invite, .whisper, .notification_users => &.{ .{ .label = "Member nickname" }, .{ .label = "Reason", .hint = "Optional" } },
        .away => &.{.{ .label = "Away message" }},
        .sound => &.{ .{ .label = "Sound name", .kind = .choice }, .{ .label = "Volume", .hint = "100%", .kind = .choice } },
        .set_text_font, .text_font => &.{ .{ .label = "Font name and size", .kind = .choice }, .{ .label = "Style", .hint = "Bold", .kind = .choice } },
        .choose_color => &.{ .{ .label = "Color value" }, .{ .label = "Preview", .hint = "Current theme color", .kind = .preview } },
        .comics_view => &.{ .{ .label = "View mode", .hint = "Comic", .kind = .choice }, .{ .label = "Panels across", .hint = "4 panels", .kind = .choice } },
        .automation, .rules, .edit_rule, .rule_sets, .add_to_sets, .rename_loaded_set, .rename_set, .create_set, .advanced_event_params, .advanced_rule_settings => &.{ .{ .label = "Rule or set name" }, .{ .label = "Condition", .hint = "Event match" }, .{ .label = "Action", .hint = "Portable action" } },
        .notifications => &.{ .{ .label = "Notify on", .hint = "Join, part, mention", .kind = .choice }, .{ .label = "Delivery", .hint = "Desktop notification", .kind = .choice } },
        .file_transfer => &.{ .{ .label = "File path" }, .{ .label = "Destination", .hint = "Ask before receiving" } },
        .open_conversation => &.{.{ .label = "Conversation file", .hint = "Path to a .ccc file" }},
        .save_conversation => &.{.{ .label = "Conversation file", .hint = "Save as .ccc" }},
        .export_image => &.{.{ .label = "Image file", .hint = "Export as .png" }},
        .motd => &.{.{ .label = "Message of the day", .hint = "Server supplied", .kind = .readonly }},
        .invitation => &.{ .{ .label = "Room" }, .{ .label = "Invitation note" } },
        .about => &.{ .{ .label = "ComicChat", .hint = "Portable Zig client", .kind = .readonly }, .{ .label = "License", .hint = "AGPL-3.0-or-later", .kind = .readonly } },
    };
}

pub fn acceptsText(id: Id) bool {
    for (fields(id)) |field| if (field.kind == .text or field.kind == .password) return true;
    return false;
}

pub fn fieldAcceptsText(id: Id, index: usize) bool {
    const all = fields(id);
    if (index >= all.len) return false;
    return all[index].kind == .text or all[index].kind == .password;
}

pub fn choiceOptions(id: Id, index: usize) []const []const u8 {
    return switch (id) {
        .setup, .settings, .servers => if (index == 2) &.{ "Verified TLS", "Plaintext (unsafe)" } else &.{},
        .character => if (index == 0) &.{
            "Anna",   "Armando",  "Bolo",    "Cro",  "Dan",     "Denise", "Hugh",   "Jordan", "Kevin", "Kwensa",   "Lance",
            "Lynnea", "Margaret", "Maynard", "Mike", "Rebecca", "Sage",   "Scotty", "Susan",  "Tiki",  "Tongtyed", "Xeno",
        } else &.{},
        .background => if (index == 0) &.{ "Field", "Volcano", "Den", "Room", "Pastoral" } else &.{},
        .sound => if (index == 0) &.{ "Chime", "Knock", "Laugh", "Applause" } else &.{ "100%", "75%", "50%", "25%" },
        .set_text_font, .text_font => if (index == 0) &.{ "Comic Neue 14", "Comic Neue 16", "Comic Neue 18" } else &.{ "Regular", "Bold", "Italic" },
        .comics_view => if (index == 0) &.{ "Comic", "Text" } else &.{ "4 panels", "3 panels", "2 panels", "1 panel", "5 panels", "6 panels" },
        .notifications => if (index == 0) &.{ "Mentions", "Joins and parts", "All activity" } else &.{ "Desktop notification", "Sound only", "Disabled" },
        else => &.{},
    };
}

pub fn requiresInput(id: Id) bool {
    return switch (id) {
        .about, .motd, .comics_view, .automation, .rules, .rule_sets, .notifications, .servers, .settings, .setup => false,
        else => true,
    };
}

pub fn primaryLabel(id: Id) []const u8 {
    return switch (id) {
        .setup => "Connect",
        .settings, .servers => "Save changes",
        .personal, .character, .background, .text_font, .set_text_font, .choose_color, .comics_view => "Apply",
        .room_list => "Join room",
        .user_list => "Select",
        .channel, .channel_create => "Join",
        .kick => "Kick",
        .ban => "Apply",
        .invite => "Invite",
        .whisper => "Open",
        .file_transfer => "Send",
        .open_conversation => "Open",
        .save_conversation => "Save",
        .export_image => "Export",
        .away => "Set Away",
        .automation, .rules, .edit_rule, .rule_sets, .add_to_sets, .rename_loaded_set, .rename_set, .create_set, .advanced_event_params, .advanced_rule_settings => "Save rule",
        .notifications, .notification_users => "Save notifications",
        .about, .motd => "Close",
        else => "OK",
    };
}

test "registry covers all forty Microsoft dialog templates plus portable file dialogs" {
    try std.testing.expectEqual(@as(usize, 43), specs.len);
    try std.testing.expectEqual(@as(usize, 40), microsoft_dialog_count);
    var seen: [specs.len]bool = @splat(false);
    for (specs) |spec| {
        const index = @intFromEnum(spec.id);
        try std.testing.expect(!seen[index]);
        seen[index] = true;
        try std.testing.expectEqual(spec.id, fromResource(spec.resource).?);
    }
}
