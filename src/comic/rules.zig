//! Microsoft Comic Chat's dynamic auto-response rule engine ("if this
//! happens, do that" automation triggered by chat events).
//!
//! Ports `rules.cpp`/`rules.h`'s data model, text serialization, matching,
//! keyword substitution, flood guard, and background-listing diff logic.
//! Explicitly NOT ported: the MFC rule-editor dialogs (`bindauto.cpp`,
//! `binddcmt.cpp`, `bindipfw.cpp` — pure GUI), the Windows Registry / `.crs`
//! binary file persistence (`bSaveRulesToReg`/`bLoadRulesFromReg`/
//! `CCRuleSet::bSaveToFile`/`bLoadFromFile` — replaced here by the
//! already-text `bUnSerialize` grammar), and RTF formatting-run preservation
//! across keyword substitution (`SzControlFull`/`SzControlLess`/
//! `SzReplaceFormattedString` — a rich-text-editor concern with no portable
//! text model in this client; substitution here is plain text).
//!
//! Source anchors:
//! - `rules.h:72-407`: event/action/param-type enums and their static
//!   per-event/per-action metadata tables.
//! - `rules.h:682-778`: the `CCRule` field layout.
//! - `rules.cpp:1466-1567`: `CCRule::bUnSerialize`, the pipe-delimited text
//!   rule grammar.
//! - `rules.cpp:1761-1776`: `CCRule::bIsFlooding`.
//! - `rules.cpp:2442-2585`: `bReplaceKeyEventParams`/`bReplaceKeyActionParams`,
//!   keyword substitution into event/action parameter text.
//! - `rules.cpp:2588-2901`: `bMatchAndApplyRules`, `iGetFirstMatchingRule`,
//!   `iGetNextMatchingRule`, `bMatchingRule` - the dispatch/match core.
//! - `actions.cpp:189-340`: `bKeyEventParam`/`bRndEventParam`, the actual
//!   per-parameter-type match predicates `bMatchingRule` calls through.
//! - `rules.cpp:401-879`: `CCDaemonExt`'s new/old item diffing, used to
//!   drive `eOnConnect`/`eOnDisconnect`/`eOnNewRoom` (events with no direct
//!   wire trigger; they fire from noticing a WHO/LIST snapshot change).
//!
//! Two pieces are *not* in the pinned historical source snapshot at all
//! (declared in a file outside the imported provenance set), so they are
//! not "ported" in the verified sense the rest of this codebase holds
//! itself to:
//! - `bIsMatch`/`PRUSERMATCH`, the `nick!user@host` ban-style wildcard mask
//!   matcher `bRndEventParam` calls for `ptNickname`. `matchesNickMask`
//!   below implements the standard, universal IRC ban-mask convention
//!   (case-insensitive `*`/`?` glob over `nick!user@host`) instead, since
//!   that convention - not Microsoft's specific implementation - is what
//!   is actually verifiable.
//! - The CTCP low-level quote byte family used elsewhere in this codebase
//!   (`proto/dcc.zig` carries the same flag); not used by this module.

const std = @import("std");

pub const max_event_params: usize = 3;
pub const max_action_params: usize = 3;

pub const Event = enum(u8) {
    on_connect,
    on_disconnect,
    on_invitation,
    on_join,
    on_kick,
    on_leave,
    on_message,
    on_new_host,
    on_new_room,
    on_whisper,
    on_whisper_in_room,
};

pub const Action = enum(u8) {
    ban,
    beep,
    do_not_display,
    execute_macro,
    get_identity,
    get_lag_time,
    get_local_time,
    get_profile,
    get_version,
    highlight_message,
    ignore,
    invite,
    join_room,
    kick,
    leave_room,
    make_host,
    notify_dialog,
    play_sound,
    connect,
    replace_message,
    send_action,
    send_file_line,
    send_message,
    send_sound,
    send_thought,
    send_whisper,
    send_whisper_in_room,
    whisper_file_line,
    disconnect,
    activate_rule_set,
};

/// `ptMax` (rules.h:140) is folded into `.none`: "no parameter at this slot".
pub const ParamType = enum(u8) {
    activate,
    beep_count,
    highlight,
    line_number,
    macro_name,
    message,
    nickname,
    reason,
    room_name,
    rule_set_name,
    server_name,
    sound_file_name,
    text_file_name,
    none,
};

/// `kepMax` (rules.h:153) is folded into `.none`: this event parameter slot
/// holds specific text, not a keyword.
pub const KeyEventParam = enum(u8) {
    any,
    anyone,
    me,
    anyone_but_me,
    any_of_my_rooms,
    my_activated_room,
    my_inactivated_rooms,
    none,
};

/// `kapMax` (rules.h:177) is folded into `.none`.
pub const KeyActionParam = enum(u8) {
    my_activated_room,
    all,
    event_message,
    event_nickname,
    event_room,
    event_server,
    random,
    yes,
    no,
    event_recipients,
    me,
    none,
};

pub const flag_active: u16 = 0x0001;
pub const flag_no_subsequent: u16 = 0x0002;
pub const flag_match_case: u16 = 0x0004;
pub const flag_match_word: u16 = 0x0008;
pub const flag_stopped: u16 = 0x0040;
pub const flag_sort_descending: u16 = 0x0080;
/// The only bits `bUnSerialize` accepts from stored text (rules.cpp:1481);
/// `flag_stopped`/`flag_sort_descending` are runtime-only.
const serializable_flags_mask: u16 = flag_active | flag_no_subsequent | flag_match_case | flag_match_word;

// --- Static per-event metadata (rules.h:218-337) -------------------------

pub const event_param_count = [_]u8{ 2, 2, 2, 2, 2, 2, 3, 2, 1, 2, 3 };

pub const event_needs_daemon = [_]bool{ true, true, false, false, false, false, false, false, true, false, false };

pub const event_param_types = [std.enums.values(Event).len][max_event_params]ParamType{
    .{ .nickname, .server_name, .none }, // on_connect
    .{ .nickname, .server_name, .none }, // on_disconnect
    .{ .nickname, .room_name, .none }, // on_invitation
    .{ .nickname, .room_name, .none }, // on_join
    .{ .nickname, .room_name, .none }, // on_kick
    .{ .nickname, .room_name, .none }, // on_leave
    .{ .nickname, .room_name, .message }, // on_message
    .{ .nickname, .room_name, .none }, // on_new_host
    .{ .room_name, .none, .none }, // on_new_room
    .{ .nickname, .message, .none }, // on_whisper
    .{ .nickname, .room_name, .message }, // on_whisper_in_room
};

/// Bit `1 << @intFromEnum(KeyEventParam)` per allowed keyword, per event
/// parameter slot (rules.h:295-308).
pub const event_key_params = [std.enums.values(Event).len][max_event_params]u8{
    .{ 0x04, 0x01, 0x00 }, // on_connect: Me, Any
    .{ 0x04, 0x01, 0x00 }, // on_disconnect: Me, Any
    .{ 0x08, 0x01, 0x00 }, // on_invitation: AnyoneButMe, Any
    .{ 0x0E, 0x70, 0x00 }, // on_join
    .{ 0x0E, 0x70, 0x00 }, // on_kick
    .{ 0x0E, 0x70, 0x00 }, // on_leave
    .{ 0x08, 0x70, 0x01 }, // on_message
    .{ 0x0E, 0x70, 0x00 }, // on_new_host
    .{ 0x00, 0x00, 0x00 }, // on_new_room
    .{ 0x08, 0x01, 0x00 }, // on_whisper
    .{ 0x08, 0x70, 0x01 }, // on_whisper_in_room
};

pub const event_enabled_actions = [_]u32{
    0x3FF31C0A, 0x2FF7000A, 0x3FF31C0A, 0x3FF3FFFB, 0x3FF35E0B,
    0x3FF35E0B, 0x3FFBFFFF, 0x3FF3F9FB, 0x3FF3100A, 0x3FFB1E0E,
    0x3FFBFFFF,
};

// --- Static per-action metadata (rules.h:246-407) -------------------------

/// Low nibble = param count, next nibble = `ActionDelayOK` (rules.h:432).
pub const action_param_flags = [_]u16{
    0x0010, 0x0011, 0x0000, 0x0011, 0x0010, 0x0010, 0x0011, 0x0010, 0x0010,
    0x0001, 0x0010, 0x0011, 0x0011, 0x0011, 0x0010, 0x0010, 0x0011, 0x0011,
    0x0012, 0x0001, 0x0012, 0x0013, 0x0012, 0x0013, 0x0012, 0x0012, 0x0013,
    0x0013, 0x0010, 0x0012,
};

pub fn actionParamCount(action: Action) u8 {
    return @truncate(action_param_flags[@intFromEnum(action)] & 0x000F);
}

pub fn actionDelayAllowed(action: Action) bool {
    return action_param_flags[@intFromEnum(action)] & 0x00F0 != 0;
}

pub const action_param_types = [std.enums.values(Action).len][max_action_params]ParamType{
    .{ .none, .none, .none }, // ban
    .{ .beep_count, .none, .none }, // beep
    .{ .none, .none, .none }, // do_not_display
    .{ .macro_name, .none, .none }, // execute_macro
    .{ .none, .none, .none }, // get_identity
    .{ .none, .none, .none }, // get_lag_time
    .{ .nickname, .none, .none }, // get_local_time
    .{ .none, .none, .none }, // get_profile
    .{ .none, .none, .none }, // get_version
    .{ .highlight, .none, .none }, // highlight_message
    .{ .none, .none, .none }, // ignore
    .{ .room_name, .none, .none }, // invite
    .{ .room_name, .none, .none }, // join_room
    .{ .reason, .none, .none }, // kick
    .{ .none, .none, .none }, // leave_room
    .{ .none, .none, .none }, // make_host
    .{ .message, .none, .none }, // notify_dialog
    .{ .sound_file_name, .none, .none }, // play_sound
    .{ .nickname, .server_name, .none }, // connect
    .{ .message, .none, .none }, // replace_message
    .{ .room_name, .message, .none }, // send_action
    .{ .room_name, .text_file_name, .line_number }, // send_file_line
    .{ .room_name, .message, .none }, // send_message
    .{ .room_name, .message, .sound_file_name }, // send_sound
    .{ .room_name, .message, .none }, // send_thought
    .{ .nickname, .message, .none }, // send_whisper
    .{ .nickname, .room_name, .message }, // send_whisper_in_room
    .{ .nickname, .text_file_name, .line_number }, // whisper_file_line
    .{ .none, .none, .none }, // disconnect
    .{ .rule_set_name, .activate, .none }, // activate_rule_set
};

/// `RTFParam` (rules.h:430): a `ptMessage` action param counts as "rich" and
/// gets the formatting-preserving substitution path in the source, except
/// for `aNotifyDialog`. This port has no formatting model, so every
/// `ptMessage` param uses plain-text substitution; this flag is kept only
/// to mirror the source's own special-case list for documentation.
pub fn isRichMessageParam(action: Action, param_type: ParamType) bool {
    return action != .notify_dialog and param_type == .message;
}

// --- Rule data model -------------------------------------------------------

pub const EventParam = struct {
    keyword: KeyEventParam = .none,
    /// Present when `keyword == .none`. Borrows from the rule's source text.
    text: []const u8 = "",
};

pub const ActionParam = struct {
    keyword: KeyActionParam = .none,
    /// Present when `keyword == .none`. Borrows from the rule's source text.
    text: []const u8 = "",
};

pub const Rule = struct {
    event: Event,
    action: Action,
    event_params: [max_event_params]EventParam = @splat(.{}),
    action_params: [max_action_params]ActionParam = @splat(.{}),
    flags: u16 = 0,
    delay: u8 = 0,

    /// `bIsFlooding`'s moving-window state (rules.cpp:1761-1776).
    period_start: u16 = 0,
    occurrences: u8 = 0,

    pub fn active(self: Rule) bool {
        return self.flags & flag_active != 0;
    }
    pub fn stopped(self: Rule) bool {
        return self.flags & flag_stopped != 0;
    }
    pub fn noSubsequent(self: Rule) bool {
        return self.flags & flag_no_subsequent != 0;
    }

    /// `CCRule::bIsFlooding` (rules.cpp:1761-1776), with the 16-bit
    /// wall-clock-seconds wraparound preserved exactly and the clock
    /// injected rather than read internally. `interval_s`/`max_occurrences`
    /// are `CCDynaRules::GetFloodingInterval`/`GetFloodingOccurrences`
    /// (config, not modeled here - caller supplies the effective values).
    pub fn isFlooding(self: *Rule, now_s: u32, interval_s: u16, max_occurrences: u8) bool {
        const now_wrapped: u16 = @truncate(now_s);
        const interval: u16 = @truncate(@abs(@as(i32, now_wrapped) - @as(i32, self.period_start)));
        if (interval > interval_s) {
            self.period_start = now_wrapped;
            self.occurrences = 1;
            return false;
        }
        self.occurrences +|= 1;
        return self.occurrences > max_occurrences;
    }
};

pub const DeserializeError = error{
    MissingField,
    InvalidFlags,
    InvalidEvent,
    InvalidAction,
    InvalidKeywordIndex,
};

fn nextPipeToken(text: []const u8) ?struct { token: []const u8, rest: []const u8 } {
    // `GetToken1(szTmp, &szTmp, "|")` (rules.cpp:1476 etc.): split on '|',
    // no whitespace trimming.
    if (text.len == 0) return null;
    const bar = std.mem.indexOfScalar(u8, text, '|') orelse text.len;
    return .{ .token = text[0..bar], .rest = if (bar == text.len) text[bar..] else text[bar + 1 ..] };
}

/// `CCRule::bUnSerialize` (rules.cpp:1466-1567): the pipe-delimited text
/// rule grammar, `flags|eventID|actionID|eventParam...|actionParam...`,
/// where a param is either `$<keywordIndex>` or literal text. Keyword
/// substitution of literal text via `StrFindAndReplaceKeyParams`
/// (rules.cpp:1529, :1553) is a UI-authoring convenience for typing `%Me%`
/// style tokens into an editor and is not replicated here; callers pass
/// already-resolved text.
pub fn deserialize(text: []const u8) DeserializeError!Rule {
    var rest = text;

    const flags_tok = nextPipeToken(rest) orelse return error.MissingField;
    rest = flags_tok.rest;
    const flags = std.fmt.parseInt(u16, flags_tok.token, 10) catch return error.InvalidFlags;
    if (flags & ~serializable_flags_mask != 0) return error.InvalidFlags;

    const event_tok = nextPipeToken(rest) orelse return error.MissingField;
    rest = event_tok.rest;
    const event_index = std.fmt.parseInt(u8, event_tok.token, 10) catch return error.InvalidEvent;
    if (event_index >= std.enums.values(Event).len) return error.InvalidEvent;
    const event: Event = @enumFromInt(event_index);

    const action_tok = nextPipeToken(rest) orelse return error.MissingField;
    rest = action_tok.rest;
    const action_index = std.fmt.parseInt(u8, action_tok.token, 10) catch return error.InvalidAction;
    if (action_index >= std.enums.values(Action).len) return error.InvalidAction;
    const action: Action = @enumFromInt(action_index);

    var rule: Rule = .{ .event = event, .action = action, .flags = flags };

    const n_event_params = event_param_count[@intFromEnum(event)];
    for (0..n_event_params) |i| {
        const tok = nextPipeToken(rest) orelse return error.MissingField;
        rest = tok.rest;
        if (tok.token.len > 0 and tok.token[0] == '$') {
            const kep_index = std.fmt.parseInt(u8, tok.token[1..], 10) catch return error.InvalidKeywordIndex;
            if (kep_index >= @intFromEnum(KeyEventParam.none)) return error.InvalidKeywordIndex;
            rule.event_params[i] = .{ .keyword = @enumFromInt(kep_index) };
        } else {
            rule.event_params[i] = .{ .text = tok.token };
        }
    }

    const n_action_params = actionParamCount(action);
    for (0..n_action_params) |i| {
        const tok = nextPipeToken(rest) orelse return error.MissingField;
        rest = tok.rest;
        if (tok.token.len > 0 and tok.token[0] == '$') {
            const kap_index = std.fmt.parseInt(u8, tok.token[1..], 10) catch return error.InvalidKeywordIndex;
            if (kap_index >= @intFromEnum(KeyActionParam.none)) return error.InvalidKeywordIndex;
            rule.action_params[i] = .{ .keyword = @enumFromInt(kap_index) };
        } else {
            rule.action_params[i] = .{ .text = tok.token };
        }
    }

    return rule;
}

test "deserialize parses flags, event, action, and mixed keyword/text params" {
    // on_message needs 3 event params; send_message needs 2 action params.
    const rule = try deserialize("5|6|22|$3|<general>|hello|<general>|hello there");
    try std.testing.expectEqual(Event.on_message, rule.event);
    try std.testing.expectEqual(Action.send_message, rule.action);
    try std.testing.expect(rule.active());
    try std.testing.expect(rule.flags & flag_match_case != 0);
    try std.testing.expectEqual(KeyEventParam.anyone_but_me, rule.event_params[0].keyword);
    try std.testing.expectEqualStrings("<general>", rule.event_params[1].text);
    try std.testing.expectEqual(KeyEventParam.none, rule.event_params[1].keyword);
    try std.testing.expectEqualStrings("hello", rule.event_params[2].text);
    try std.testing.expectEqualStrings("<general>", rule.action_params[0].text);
    try std.testing.expectEqualStrings("hello there", rule.action_params[1].text);
}

// --- Runtime matching (actions.cpp:189-340, rules.cpp:2840-2901) --------

/// The event-triggering facts `bMatchAndApplyRules` receives
/// (rules.cpp:2588's `strServer`/`strIdentity`/`strChannel`/`strMessage`),
/// plus the small set of live-session facts `bKeyEventParam`/`bRndEventParam`
/// read off the app directly (actions.cpp:189-231) that this port has no
/// session/roster model of its own to consult.
pub const MatchContext = struct {
    event: Event,
    server: []const u8 = "",
    /// Bare nick, or full `nick!user@host` when available (only the latter
    /// enables mask matching; see `matchLiteralEventParam`).
    identity: []const u8 = "",
    channel: []const u8 = "",
    message: []const u8 = "",

    my_nick: []const u8 = "",
    /// `LookupDoc(channel) != NULL` (actions.cpp:213): are we a member of
    /// this channel at all.
    is_member_of_channel: bool = false,
    /// `currentRoom && strCurrentChannel == strParam` (actions.cpp:201): is
    /// this our currently-focused channel.
    is_active_channel: bool = false,
};

fn extractNick(identity: []const u8) []const u8 {
    const bang = std.mem.indexOfScalar(u8, identity, '!') orelse return identity;
    return identity[0..bang];
}

/// `bKeyEventParam` (actions.cpp:189-231).
pub fn matchKeyEventParam(ctx: *const MatchContext, kep: KeyEventParam, value: []const u8) bool {
    return switch (kep) {
        .my_activated_room => ctx.is_active_channel,
        .my_inactivated_rooms => ctx.is_member_of_channel and !ctx.is_active_channel,
        .anyone, .any => true,
        .any_of_my_rooms => ctx.is_member_of_channel,
        .me => std.ascii.eqlIgnoreCase(extractNick(value), ctx.my_nick),
        .anyone_but_me => !std.ascii.eqlIgnoreCase(extractNick(value), ctx.my_nick),
        .none => unreachable,
    };
}

fn isWordChar(c: u8) bool {
    // `CStringInfo::IsDelimiter` (utils.cpp:1458-1462): a delimiter is
    // space/blank/control/punctuation, so - notably - '_' is a delimiter
    // here, not a word character.
    return std.ascii.isAlphanumeric(c);
}

fn isDelimiter(c: u8) bool {
    return !isWordChar(c);
}

/// `StrFindSubString` (utils.cpp:1507-1616), minus its Boyer-Moore skip
/// table and DBCS lead-byte handling (a performance/charset detail; match
/// results are identical for the ASCII text this port handles). The
/// whole-word boundary rule (`IsDelimiter` on the bytes immediately outside
/// the match) is preserved exactly.
pub fn findSubstring(haystack: []const u8, needle: []const u8, whole_word: bool, ignore_case: bool) ?usize {
    if (haystack.len == 0 or needle.len == 0 or needle.len > haystack.len) return null;
    var pos: usize = 0;
    while (pos + needle.len <= haystack.len) : (pos += 1) {
        const window = haystack[pos .. pos + needle.len];
        const matches = if (ignore_case) std.ascii.eqlIgnoreCase(window, needle) else std.mem.eql(u8, window, needle);
        if (!matches) continue;
        if (whole_word) {
            const before_ok = pos == 0 or isDelimiter(haystack[pos - 1]);
            const after_ok = pos + needle.len == haystack.len or isDelimiter(haystack[pos + needle.len]);
            if (!before_ok or !after_ok) continue;
        }
        return pos;
    }
    return null;
}

/// The standard, universal IRC ban-mask convention: case-insensitive `*`
/// (any run) / `?` (one byte) glob over `nick!user@host`. `bIsMatch`'s
/// actual implementation is not in the pinned snapshot (see the module doc
/// comment); this is the verifiable public convention, not Microsoft's
/// specific one.
pub fn globMatchCaseInsensitive(pattern: []const u8, text: []const u8) bool {
    var p: usize = 0;
    var t: usize = 0;
    var star_p: ?usize = null;
    var star_t: usize = 0;
    while (t < text.len) {
        if (p < pattern.len and (pattern[p] == '?' or std.ascii.toLower(pattern[p]) == std.ascii.toLower(text[t]))) {
            p += 1;
            t += 1;
        } else if (p < pattern.len and pattern[p] == '*') {
            star_p = p;
            star_t = t;
            p += 1;
        } else if (star_p) |sp| {
            p = sp + 1;
            star_t += 1;
            t = star_t;
        } else {
            return false;
        }
    }
    while (p < pattern.len and pattern[p] == '*') p += 1;
    return p == pattern.len;
}

pub fn matchesNickMask(mask: []const u8, nick: []const u8, user: []const u8, host: []const u8) bool {
    var buf: [512]u8 = undefined;
    const full = std.fmt.bufPrint(&buf, "{s}!{s}@{s}", .{ nick, user, host }) catch return false;
    return globMatchCaseInsensitive(mask, full);
}

/// `bRndEventParam` (actions.cpp:276-340). `value` is the runtime fact
/// (`ctx.identity`/`.message`/`.channel`/`.server`); `filter` is the rule's
/// literal parameter text.
pub fn matchLiteralEventParam(param_type: ParamType, value: []const u8, filter: []const u8, flags: u16) bool {
    return switch (param_type) {
        .nickname => nick: {
            // Full mask matching needs decoded nick/user/host; a bare nick
            // (no '!'/'@') falls back to an exact case-insensitive compare
            // (actions.cpp:296-310).
            const bang = std.mem.indexOfScalar(u8, value, '!');
            const at = std.mem.indexOfScalar(u8, value, '@');
            if (bang != null and at != null and at.? > bang.?) {
                const nick = value[0..bang.?];
                const user = value[bang.? + 1 .. at.?];
                const host = value[at.? + 1 ..];
                break :nick matchesNickMask(filter, nick, user, host);
            }
            break :nick std.ascii.eqlIgnoreCase(value, filter);
        },
        .message => findSubstring(value, filter, flags & flag_match_word != 0, flags & flag_match_case == 0) != null,
        .room_name => std.ascii.eqlIgnoreCase(value, filter),
        .server_name => std.ascii.eqlIgnoreCase(value, filter), // bNetValid's server-group hierarchy is not modeled
        else => false,
    };
}

/// `CCDynaRules::bMatchingRule` (rules.cpp:2840-2901).
pub fn matchingRule(rule: *const Rule, ctx: *const MatchContext) bool {
    if (ctx.event != rule.event or !rule.active() or rule.stopped()) return false;

    const n = event_param_count[@intFromEnum(rule.event)];
    for (0..n) |i| {
        const pt = event_param_types[@intFromEnum(rule.event)][i];
        const value: []const u8 = switch (pt) {
            .room_name => ctx.channel,
            .message => ctx.message,
            .nickname => ctx.identity,
            .server_name => ctx.server,
            else => unreachable,
        };
        const param = rule.event_params[i];
        const pass = if (param.keyword != .none)
            matchKeyEventParam(ctx, param.keyword, value)
        else
            matchLiteralEventParam(pt, value, param.text, rule.flags);
        if (!pass) return false;
    }
    return true;
}

// --- Rule sets and dispatch order (rules.cpp:2683-2901) --------------------

pub const RuleSet = struct {
    name: []const u8 = "",
    flags: u16 = flag_active,
    rules: []Rule,

    pub fn active(self: RuleSet) bool {
        return self.flags & flag_active != 0;
    }
};

pub const RuleLocation = struct { set_index: usize, rule_index: usize };

/// `bInActionIDs`/`bRuleFilteredOut` (rules.cpp:2813-2837).
fn ruleFilteredOut(rule: *const Rule, approved: ?[]const Action, rejected: ?[]const Action) bool {
    if (approved) |list| {
        for (list) |a| {
            if (a == rule.action) break;
        } else return true;
    }
    if (rejected) |list| {
        for (list) |a| {
            if (a == rule.action) return true;
        }
    }
    return false;
}

/// `iGetFirstMatchingRule` (rules.cpp:2683-2756).
pub fn firstMatchingRule(rule_sets: []const RuleSet, ctx: *const MatchContext, approved: ?[]const Action, rejected: ?[]const Action) ?RuleLocation {
    for (rule_sets, 0..) |set, set_index| {
        if (!set.active()) continue;
        for (set.rules, 0..) |*rule, rule_index| {
            if (!matchingRule(rule, ctx)) continue;
            if (ruleFilteredOut(rule, approved, rejected)) {
                if (rule.noSubsequent()) break; // stop scanning this set only
                continue;
            }
            return .{ .set_index = set_index, .rule_index = rule_index };
        }
    }
    return null;
}

/// `iGetNextMatchingRule` (rules.cpp:2759-2810).
pub fn nextMatchingRule(rule_sets: []const RuleSet, previous: RuleLocation, ctx: *const MatchContext, approved: ?[]const Action, rejected: ?[]const Action) ?RuleLocation {
    var set_index = previous.set_index;
    var start_rule = previous.rule_index + 1;
    while (set_index < rule_sets.len) : (set_index += 1) {
        const set = rule_sets[set_index];
        if (set.active()) {
            for (set.rules[start_rule..], start_rule..) |*rule, rule_index| {
                if (!matchingRule(rule, ctx)) continue;
                if (ruleFilteredOut(rule, approved, rejected)) {
                    if (rule.noSubsequent()) break;
                    continue;
                }
                return .{ .set_index = set_index, .rule_index = rule_index };
            }
        }
        start_rule = 0;
    }
    return null;
}

/// `CCDynaRules::bMatchAndApplyRules` (rules.cpp:2588-2680): find the first
/// matching, non-flooding rule and hand it to `executor.executeAction(rule)`,
/// then - unless that rule is `flag_no_subsequent` - keep finding and
/// executing further matching rules until none remain. A flooding rule is
/// stopped (`flag_stopped` set) instead of executed, and reported via
/// `executor.onFlooding(rule)`. `now_s`/`flood_interval_s`/
/// `flood_max_occurrences` feed `Rule.isFlooding` (`CCDynaRules::
/// GetFloodingInterval`/`GetFloodingOccurrences`, config not modeled here).
pub fn matchAndApplyRules(
    rule_sets: []RuleSet,
    ctx: *const MatchContext,
    approved: ?[]const Action,
    rejected: ?[]const Action,
    now_s: u32,
    flood_interval_s: u16,
    flood_max_occurrences: u8,
    executor: anytype,
) void {
    var maybe_loc = firstMatchingRule(rule_sets, ctx, approved, rejected);
    while (maybe_loc) |loc| {
        const rule = &rule_sets[loc.set_index].rules[loc.rule_index];
        if (rule.isFlooding(now_s, flood_interval_s, flood_max_occurrences)) {
            rule.flags |= flag_stopped;
            executor.onFlooding(rule);
        } else {
            executor.executeAction(rule);
        }
        if (rule.noSubsequent()) break;
        maybe_loc = nextMatchingRule(rule_sets, loc, ctx, approved, rejected);
    }
}

// --- Keyword substitution into rule text (rules.cpp:2442-2585) ------------

/// `strTo += strFrom.Mid(...)` accumulation pattern ported to a Zig writer:
/// replace every case-insensitive occurrence of `keyword` in `text` with
/// `replacement` (source compares via `MakeUpper()`, rules.cpp:2460-2463
/// etc. - always case-insensitive regardless of the rule's own MatchCase
/// flag, which only governs event-parameter matching).
fn replaceKeyword(out: *std.ArrayList(u8), gpa: std.mem.Allocator, text: []const u8, keyword: []const u8, replacement: []const u8) !void {
    if (keyword.len == 0) {
        try out.appendSlice(gpa, text);
        return;
    }
    var rest = text;
    while (findSubstring(rest, keyword, false, true)) |at| {
        try out.appendSlice(gpa, rest[0..at]);
        try out.appendSlice(gpa, replacement);
        rest = rest[at + keyword.len ..];
    }
    try out.appendSlice(gpa, rest);
}

/// A rule's literal event-parameter text may itself embed `%Me%`/
/// `%MyActivatedRoom%`-style keyword tokens (only for `ptMessage` params,
/// `bMatchingRule`'s call at rules.cpp:2893-2894); `bReplaceKeyEventParams`
/// (rules.cpp:2442-2476) only ever substitutes `kepMe`/`kepMyActivatedRoom`
/// here (the source's own `switch` has no other case). `my_nick`/
/// `my_activated_room` are `StrGetKeyEventParam(ptNickname)`/
/// `(ptRoomName)` (actions.cpp:343-360).
pub fn substituteEventKeyParams(
    gpa: std.mem.Allocator,
    text: []const u8,
    me_keyword: []const u8,
    my_activated_room_keyword: []const u8,
    my_nick: []const u8,
    my_activated_room: []const u8,
) ![]u8 {
    var pass1: std.ArrayList(u8) = .empty;
    defer pass1.deinit(gpa);
    try replaceKeyword(&pass1, gpa, text, me_keyword, my_nick);

    var pass2: std.ArrayList(u8) = .empty;
    errdefer pass2.deinit(gpa);
    try replaceKeyword(&pass2, gpa, pass1.items, my_activated_room_keyword, my_activated_room);
    return pass2.toOwnedSlice(gpa);
}

/// The action-side substitution facts `bReplaceKeyActionParams`
/// (rules.cpp:2479-2585) draws from the cached event context plus the app
/// (`StrGetKeyActionParam`, actions.cpp:363+).
pub const ActionSubstitution = struct {
    my_activated_room: []const u8 = "",
    all: []const u8 = "",
    event_message: []const u8 = "",
    event_nickname: []const u8 = "",
    event_room: []const u8 = "",
    event_server: []const u8 = "",
    random: []const u8 = "",
    event_recipients: []const u8 = "",
    me: []const u8 = "",
};

fn actionKeywordReplacement(kap: KeyActionParam, sub: *const ActionSubstitution) []const u8 {
    return switch (kap) {
        .my_activated_room => sub.my_activated_room,
        .all => sub.all,
        .event_message => sub.event_message,
        .event_nickname => sub.event_nickname,
        .event_room => sub.event_room,
        .event_server => sub.event_server,
        .random => sub.random,
        .event_recipients => sub.event_recipients,
        .me => sub.me,
        .yes, .no, .none => unreachable, // callers skip these (rules.cpp:2522-2523)
    };
}

/// `bReplaceKeyActionParams` (rules.cpp:2479-2585), plain-text only (see
/// the module doc comment on RTF formatting-run preservation). Fills
/// `substituted[i]` for every action param the rule's action actually uses,
/// allocating from `gpa`; the caller owns and frees each returned slice
/// (`null` for unused param slots). `key_event_texts`/`key_action_texts`
/// are the literal token spellings (e.g. `"%Me%"`, `"%EventMessage%"`) that
/// may appear in rule text - this port has no built-in string-table
/// loader, so the caller supplies them (mirroring `CCRulesData::
/// GetKeyEventParam`/`GetKeyActionParam`, rules.h:543-547).
pub fn substituteActionParams(
    gpa: std.mem.Allocator,
    rule: *const Rule,
    key_event_texts: *const [@intFromEnum(KeyEventParam.none)][]const u8,
    key_action_texts: *const [@intFromEnum(KeyActionParam.none)][]const u8,
    sub: *const ActionSubstitution,
    substituted: *[max_action_params]?[]u8,
) !void {
    const n = actionParamCount(rule.action);
    for (0..n) |i| {
        const param = rule.action_params[i];
        const source_text = if (param.keyword != .none)
            actionKeywordReplacement(param.keyword, sub)
        else
            param.text;

        var current = try gpa.dupe(u8, source_text);
        errdefer gpa.free(current);

        // Replace a literal "%Me%" token with our nickname first
        // (rules.cpp:2509-2518, unconditional for every action param).
        {
            var next: std.ArrayList(u8) = .empty;
            errdefer next.deinit(gpa);
            try replaceKeyword(&next, gpa, current, key_event_texts[@intFromEnum(KeyEventParam.me)], sub.me);
            gpa.free(current);
            current = try next.toOwnedSlice(gpa);
        }

        // Then every other action keyword token, skipping Yes/No
        // (rules.cpp:2520-2565).
        inline for (std.enums.values(KeyActionParam)) |kap| {
            if (kap == .yes or kap == .no or kap == .none) continue;
            var next: std.ArrayList(u8) = .empty;
            errdefer next.deinit(gpa);
            try replaceKeyword(&next, gpa, current, key_action_texts[@intFromEnum(kap)], actionKeywordReplacement(kap, sub));
            gpa.free(current);
            current = try next.toOwnedSlice(gpa);
        }

        substituted[i] = current;
    }
    for (n..max_action_params) |i| substituted[i] = null;
}

// --- Snapshot diffing for daemon-driven events (rules.cpp:401-879) --------
//
// `eOnConnect`/`eOnDisconnect`/`eOnNewRoom` have no direct wire trigger;
// the source notices them by periodically polling a full WHO (users) or
// LIST (channels) snapshot and diffing it against the previous one
// (`CCDaemonExt::bTreatNewItems`/`bTreatOldItems`, rules.cpp:553-706). This
// port keeps the diffing algorithm - which items appeared/disappeared
// between two snapshots - and drops the source's shared, reference-counted
// double-buffer list plumbing (`m_itemLists`/`m_nCredits`), which exists in
// the original only to let a WHO snapshot be shared between the rules
// daemon and a separate notify-list feature this port does not have.
// Actually issuing the WHO/LIST query is a live-network concern left to
// the caller, exactly as `RULEDAEMON_QUERY_FN` leaves it to the app.

pub const ConnectedUser = struct {
    nickname: []const u8,
    /// `user@host`, matching `pUser->m_strIdentity` (rules.cpp:592, :617).
    identity: []const u8,
};

/// `bTreatNewItems` on `itUser` (rules.cpp:553-644, minus action
/// execution): every user present in `current` but absent from `previous`
/// (matched by `identity`), in `current`'s order.
pub fn newUsers(gpa: std.mem.Allocator, previous: []const ConnectedUser, current: []const ConnectedUser) ![]ConnectedUser {
    var out: std.ArrayList(ConnectedUser) = .empty;
    errdefer out.deinit(gpa);
    outer: for (current) |cur| {
        for (previous) |prev| {
            if (std.mem.eql(u8, cur.identity, prev.identity)) continue :outer;
        }
        try out.append(gpa, cur);
    }
    return out.toOwnedSlice(gpa);
}

/// `bTreatOldItems` on `itUser` (rules.cpp:647-706, minus action
/// execution): every user present in `previous` but absent from `current`.
pub fn goneUsers(gpa: std.mem.Allocator, previous: []const ConnectedUser, current: []const ConnectedUser) ![]ConnectedUser {
    return newUsers(gpa, current, previous);
}

/// `bTreatNewItems` on `itChannel` (rules.cpp:553-644, minus action
/// execution): channel names present in `current` but absent from
/// `previous`. There is no `goneChannels`: the source only ever diffs
/// channels for `eOnNewRoom`, which has no "room closed" counterpart
/// (rules.cpp:655's own comment: "for now there is no OnRoomClosed event").
pub fn newChannels(gpa: std.mem.Allocator, previous: []const []const u8, current: []const []const u8) ![][]const u8 {
    var out: std.ArrayList([]const u8) = .empty;
    errdefer out.deinit(gpa);
    outer: for (current) |cur| {
        for (previous) |prev| {
            if (std.ascii.eqlIgnoreCase(cur, prev)) continue :outer;
        }
        try out.append(gpa, cur);
    }
    return out.toOwnedSlice(gpa);
}

test "deserialize rejects unknown flag bits, out-of-range IDs, and truncated rules" {
    try std.testing.expectError(error.InvalidFlags, deserialize("64|0|0|x|x"));
    try std.testing.expectError(error.InvalidEvent, deserialize("1|11|0|x|x"));
    try std.testing.expectError(error.InvalidAction, deserialize("1|0|30|x|x"));
    try std.testing.expectError(error.MissingField, deserialize("1|0|0|x"));
    try std.testing.expectError(error.MissingField, deserialize("1|0"));
}

test "findSubstring honors case and whole-word flags with the delimiter rule" {
    try std.testing.expectEqual(@as(?usize, 6), findSubstring("hello world", "world", false, true));
    try std.testing.expect(findSubstring("hello WORLD", "world", false, false) == null);
    try std.testing.expectEqual(@as(?usize, 6), findSubstring("hello WORLD", "world", false, true));

    // "cat" inside "concatenate" is not a whole word (no delimiter on
    // either side); "cat" as its own token is.
    try std.testing.expect(findSubstring("concatenate", "cat", true, true) == null);
    try std.testing.expectEqual(@as(?usize, 0), findSubstring("cat concatenate", "cat", true, true));

    // '_' is punctuation (a delimiter), not a word character, matching
    // Windows CT_CTYPE1 - so a token flanked only by underscores still
    // counts as a whole word.
    try std.testing.expectEqual(@as(?usize, 1), findSubstring("_cat_", "cat", true, true));

    try std.testing.expect(findSubstring("", "x", false, true) == null);
    try std.testing.expect(findSubstring("x", "", false, true) == null);
}

test "globMatchCaseInsensitive implements standard * and ? ban-mask wildcards" {
    try std.testing.expect(globMatchCaseInsensitive("*!*@*", "Anna!anna@example.invalid"));
    try std.testing.expect(globMatchCaseInsensitive("ANNA!*@*.invalid", "anna!u@host.invalid"));
    try std.testing.expect(!globMatchCaseInsensitive("bob!*@*", "anna!u@host.invalid"));
    try std.testing.expect(globMatchCaseInsensitive("a?na!*", "anna!u@h"));
    try std.testing.expect(!globMatchCaseInsensitive("a?na!*", "annna!u@h"));
    try std.testing.expect(globMatchCaseInsensitive("*", "anything at all"));
}

test "matchesNickMask assembles nick!user@host before globbing" {
    try std.testing.expect(matchesNickMask("anna!*@*", "Anna", "anna", "example.invalid"));
    try std.testing.expect(!matchesNickMask("bob!*@*", "anna", "anna", "example.invalid"));
}

test "matchKeyEventParam ports bKeyEventParam's six cases" {
    const ctx: MatchContext = .{
        .event = .on_join,
        .my_nick = "anna",
        .is_member_of_channel = true,
        .is_active_channel = true,
    };
    try std.testing.expect(matchKeyEventParam(&ctx, .any, "irrelevant"));
    try std.testing.expect(matchKeyEventParam(&ctx, .anyone, "irrelevant"));
    try std.testing.expect(matchKeyEventParam(&ctx, .me, "Anna!u@h"));
    try std.testing.expect(!matchKeyEventParam(&ctx, .me, "bob!u@h"));
    try std.testing.expect(!matchKeyEventParam(&ctx, .anyone_but_me, "Anna!u@h"));
    try std.testing.expect(matchKeyEventParam(&ctx, .anyone_but_me, "bob!u@h"));
    try std.testing.expect(matchKeyEventParam(&ctx, .any_of_my_rooms, "#general"));
    try std.testing.expect(matchKeyEventParam(&ctx, .my_activated_room, "#general"));
    try std.testing.expect(!matchKeyEventParam(&ctx, .my_inactivated_rooms, "#general"));
}

test "matchLiteralEventParam dispatches by param type" {
    try std.testing.expect(matchLiteralEventParam(.room_name, "#General", "#general", 0));
    try std.testing.expect(matchLiteralEventParam(.server_name, "IRC.EXAMPLE.NET", "irc.example.net", 0));
    try std.testing.expect(matchLiteralEventParam(.message, "well hello there", "hello", 0));
    try std.testing.expect(!matchLiteralEventParam(.message, "well hello there", "HELLO", flag_match_case));

    // Nickname: mask matching when a full identity is available, exact
    // compare otherwise.
    try std.testing.expect(matchLiteralEventParam(.nickname, "Anna!anna@example.invalid", "*!*@*.invalid", 0));
    try std.testing.expect(!matchLiteralEventParam(.nickname, "Anna!anna@example.invalid", "*!*@other.invalid", 0));
    try std.testing.expect(matchLiteralEventParam(.nickname, "Anna", "anna", 0));
}

fn testRule(event: Event, action: Action, flags: u16) Rule {
    var r: Rule = .{ .event = event, .action = action, .flags = flags };
    r.event_params[0] = .{ .keyword = .anyone_but_me };
    r.event_params[1] = .{ .keyword = .any_of_my_rooms };
    return r;
}

test "matchingRule requires active+unstopped and the right event, then all params" {
    var rule = testRule(.on_join, .beep, flag_active);
    var ctx: MatchContext = .{ .event = .on_join, .my_nick = "anna", .identity = "bob!u@h", .channel = "#general", .is_member_of_channel = true };

    try std.testing.expect(matchingRule(&rule, &ctx));

    ctx.event = .on_kick;
    try std.testing.expect(!matchingRule(&rule, &ctx));
    ctx.event = .on_join;

    rule.flags = 0; // not active
    try std.testing.expect(!matchingRule(&rule, &ctx));
    rule.flags = flag_active | flag_stopped;
    try std.testing.expect(!matchingRule(&rule, &ctx));
    rule.flags = flag_active;

    ctx.identity = "anna!u@h"; // now it's "me", fails AnyoneButMe
    try std.testing.expect(!matchingRule(&rule, &ctx));
}

const TestExecutor = struct {
    executed: std.ArrayList(Action) = .empty,
    flooded: std.ArrayList(Action) = .empty,

    fn executeAction(self: *TestExecutor, rule: *Rule) void {
        self.executed.append(std.testing.allocator, rule.action) catch unreachable;
    }
    fn onFlooding(self: *TestExecutor, rule: *Rule) void {
        self.flooded.append(std.testing.allocator, rule.action) catch unreachable;
    }
    fn deinit(self: *TestExecutor) void {
        self.executed.deinit(std.testing.allocator);
        self.flooded.deinit(std.testing.allocator);
    }
};

test "matchAndApplyRules fires every matching rule in order unless NoSubsequent stops it" {
    var rules = [_]Rule{
        testRule(.on_join, .beep, flag_active),
        testRule(.on_join, .play_sound, flag_active),
    };
    var sets = [_]RuleSet{.{ .rules = &rules }};
    const ctx: MatchContext = .{ .event = .on_join, .my_nick = "anna", .identity = "bob!u@h", .channel = "#general", .is_member_of_channel = true };

    var exec: TestExecutor = .{};
    defer exec.deinit();
    matchAndApplyRules(&sets, &ctx, null, null, 0, 4, 12, &exec);
    try std.testing.expectEqualSlices(Action, &.{ .beep, .play_sound }, exec.executed.items);

    // Now the first rule stops after itself.
    rules[0].flags |= flag_no_subsequent;
    exec.executed.clearRetainingCapacity();
    matchAndApplyRules(&sets, &ctx, null, null, 0, 4, 12, &exec);
    try std.testing.expectEqualSlices(Action, &.{.beep}, exec.executed.items);
}

test "matchAndApplyRules honors approved/rejected action-id filters, including the NoSubsequent-filtered stop" {
    var rules = [_]Rule{
        testRule(.on_join, .beep, flag_active | flag_no_subsequent),
        testRule(.on_join, .play_sound, flag_active),
    };
    var sets = [_]RuleSet{.{ .rules = &rules }};
    const ctx: MatchContext = .{ .event = .on_join, .my_nick = "anna", .identity = "bob!u@h", .channel = "#general", .is_member_of_channel = true };

    var exec: TestExecutor = .{};
    defer exec.deinit();
    const rejected = [_]Action{.beep};
    // The first (NoSubsequent) rule is filtered out by the reject list, so
    // this ruleset's scan stops there (rules.cpp:2737) - the second rule
    // never even gets a chance, and nothing fires.
    matchAndApplyRules(&sets, &ctx, null, &rejected, 0, 4, 12, &exec);
    try std.testing.expectEqual(@as(usize, 0), exec.executed.items.len);
}

test "matchAndApplyRules reports flooding instead of executing and stops the rule" {
    var rules = [_]Rule{testRule(.on_join, .beep, flag_active)};
    var sets = [_]RuleSet{.{ .rules = &rules }};
    const ctx: MatchContext = .{ .event = .on_join, .my_nick = "anna", .identity = "bob!u@h", .channel = "#general", .is_member_of_channel = true };

    var exec: TestExecutor = .{};
    defer exec.deinit();
    // occurrences 1..12 (max) at the same instant all count as one burst.
    for (0..12) |_| matchAndApplyRules(&sets, &ctx, null, null, 1000, 4, 12, &exec);
    try std.testing.expectEqual(@as(usize, 12), exec.executed.items.len);

    matchAndApplyRules(&sets, &ctx, null, null, 1000, 4, 12, &exec);
    try std.testing.expectEqual(@as(usize, 1), exec.flooded.items.len);
    try std.testing.expect(rules[0].stopped());

    // A stopped rule no longer matches at all.
    try std.testing.expect(!matchingRule(&rules[0], &ctx));
}

test "Rule.isFlooding resets after the interval and wraps the 16-bit clock" {
    var rule: Rule = .{ .event = .on_message, .action = .beep };
    try std.testing.expect(!rule.isFlooding(0, 4, 2));
    try std.testing.expect(!rule.isFlooding(1, 4, 2));
    try std.testing.expect(rule.isFlooding(2, 4, 2)); // 3rd occurrence within the window, max 2

    // Interval elapsed: resets.
    try std.testing.expect(!rule.isFlooding(10, 4, 2));

    // Wraparound: 0xFFFF then 0x0000 one second later is still a 1-second
    // gap under the u16 truncation, matching `time(NULL) & 0xFFFF` math.
    var wrap_rule: Rule = .{ .event = .on_message, .action = .beep };
    try std.testing.expect(!wrap_rule.isFlooding(0xFFFF, 4, 2));
    try std.testing.expect(!wrap_rule.isFlooding(0x10000, 4, 2));
}

test "substituteEventKeyParams replaces only Me and MyActivatedRoom, case-insensitively" {
    const gpa = std.testing.allocator;
    const out = try substituteEventKeyParams(gpa, "hey %Me%, welcome to %MyActivatedRoom%!", "%me%", "%myactivatedroom%", "anna", "#general");
    defer gpa.free(out);
    try std.testing.expectEqualStrings("hey anna, welcome to #general!", out);
}

test "substituteActionParams replaces action keyword params and embedded %Me% tokens" {
    const gpa = std.testing.allocator;
    var rule: Rule = .{ .event = .on_message, .action = .send_message };
    rule.action_params[0] = .{ .text = "#general" };
    rule.action_params[1] = .{ .keyword = .event_message };

    const key_event_texts: [@intFromEnum(KeyEventParam.none)][]const u8 = .{ "%any%", "%anyone%", "%me%", "%anyonebutme%", "%anyofmyrooms%", "%myactivatedroom%", "%myinactivatedrooms%" };
    const key_action_texts: [@intFromEnum(KeyActionParam.none)][]const u8 = .{ "%myactivatedroom%", "%all%", "%eventmessage%", "%eventnickname%", "%eventroom%", "%eventserver%", "%random%", "%yes%", "%no%", "%eventrecipients%", "%me%" };
    const sub: ActionSubstitution = .{ .event_message = "hi %Me%!", .me = "anna" };

    var substituted: [max_action_params]?[]u8 = @splat(null);
    try substituteActionParams(gpa, &rule, &key_event_texts, &key_action_texts, &sub, &substituted);
    defer for (substituted) |maybe| if (maybe) |s| gpa.free(s);

    try std.testing.expectEqualStrings("#general", substituted[0].?);
    // The keyword param resolves to sub.event_message, and its own
    // embedded "%Me%" token is then substituted too.
    try std.testing.expectEqualStrings("hi anna!", substituted[1].?);
    try std.testing.expect(substituted[2] == null);
}

test "newUsers/goneUsers/newChannels diff two snapshots by identity" {
    const gpa = std.testing.allocator;

    const previous = [_]ConnectedUser{
        .{ .nickname = "anna", .identity = "anna@host1" },
        .{ .nickname = "bob", .identity = "bob@host2" },
    };
    const current = [_]ConnectedUser{
        .{ .nickname = "anna", .identity = "anna@host1" },
        .{ .nickname = "cro", .identity = "cro@host3" },
    };

    const arrived = try newUsers(gpa, &previous, &current);
    defer gpa.free(arrived);
    try std.testing.expectEqual(@as(usize, 1), arrived.len);
    try std.testing.expectEqualStrings("cro", arrived[0].nickname);

    const left = try goneUsers(gpa, &previous, &current);
    defer gpa.free(left);
    try std.testing.expectEqual(@as(usize, 1), left.len);
    try std.testing.expectEqualStrings("bob", left[0].nickname);

    const prev_rooms = [_][]const u8{"#general"};
    const cur_rooms = [_][]const u8{ "#general", "#offtopic" };
    const new_rooms = try newChannels(gpa, &prev_rooms, &cur_rooms);
    defer gpa.free(new_rooms);
    try std.testing.expectEqual(@as(usize, 1), new_rooms.len);
    try std.testing.expectEqualStrings("#offtopic", new_rooms[0]);
}
