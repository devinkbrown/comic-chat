//! Microsoft Comic Chat's notify list / buddy-list feature (watch a set of
//! nicknames/masks, get told when a matching user comes online or goes
//! offline).
//!
//! Ports `notif.cpp`/`notif.h`'s `CCNotif` (one watched entry) and
//! `CCDynaNotifs` (the collection + tracked-user bookkeeping) data model,
//! equality, wildcard-mask assembly, and the notify-specific overload of
//! `CCDaemonExt`'s new/old-item snapshot diffing that drives it.
//!
//! Source anchors:
//! - `notif.h:22-32,52-101`: the `CCNotif` field layout (4 text params, 3
//!   operators, `WORD` flags) and the operator enum
//!   (`g_uAny`/`g_uEquals`/`g_uContains`/`g_uStartsWith`/`g_uEndsWith`).
//! - `notif.h:104-200`: the `CCDynaNotifs` collection/daemon-plumbing field
//!   layout and method surface.
//! - `notif.cpp:58-92`: `CCNotif::operator==`/`CopyNotif`.
//! - `notif.cpp:331-345`: `CCNotif::bDaemonNeeded`.
//! - `notif.cpp:801-899`: `CCDynaNotifs::iFindUserIndex`/
//!   `bAddNotificationUser`/`bModifyNotificationUser` - the tracked-user
//!   online/offline bookkeeping.
//! - `notif.cpp:421-460`: `bRemoveAllUsers`/`bRemoveUsersWithoutFlag`/
//!   `bRemoveFlagsFromAllUsers`.
//! - `rules.cpp:709-879`: `CCDaemonExt::bOnEndOfListing`/`bTreatNewItems`/
//!   `bTreatOldItems`, the `CCDynaNotifs`/`CCNotif` overload of the same
//!   snapshot-diffing pattern `src/comic/rules.zig` already ports for
//!   `CCDynaRules`/`CCRule` - the driver that turns a WHO poll into
//!   appeared/disappeared calls against the tracked-user list above.
//! - `actions.cpp:1013-1041`: `StrAddWildcards`, the operator-to-glob
//!   assembly used to build the nickname mask sent as a WHO query and
//!   re-checked against each reply.
//! - `actions.cpp:1044-1071`: `bNotifDaemonQuery`, assembling the three
//!   per-field masks into one `nick!user@host` mask.
//!
//! `src/comic/rules.zig`'s `newUsers`/`goneUsers` are NOT reused directly:
//! that pair diffs by `identity` alone (the `itUser` overload for
//! `CCDynaRules`, rules.cpp:553-644/647-706). The notify overload
//! (rules.cpp:781-879) diffs by the triple (nickname, identity, pretty
//! room) instead - a user who changed room without changing nickname or
//! identity still counts as "old" in the previous room and "new" in the
//! current one (notif.cpp:807-809, :859-861) - so this module has its own
//! `foldWhoSnapshot`. The tracked-user fold
//! (`bAddNotificationUser`/`bModifyNotificationUser`) then re-keys by
//! (nickname, identity) alone when deciding whether a diffed user is a
//! genuinely new watch entry or an online/offline flip of one already
//! tracked, exactly as `iFindUserIndex` (notif.cpp:812-813) does - so a
//! room change on an already-tracked person updates that one tracked entry
//! in place rather than adding a second.
//!
//! Explicitly NOT ported:
//! - `notipage.cpp` (the MFC notify-list preferences/display dialog -
//!   confirmed by reading its top: it pulls in `whisprbx.h`/`binddoc.h`/
//!   `chatdoc.h`/`actions.h`/`ui.h`/`mmsystem.h` and implements
//!   `CompareNotifs`, a `PFNLVCOMPARE` sort callback for an MFC list-view -
//!   pure GUI, the same category as `bindauto.cpp`/`binddcmt.cpp`/
//!   `bindipfw.cpp` already excluded by `rules.zig`'s module doc comment).
//! - `bSaveNotifsToReg`/`bLoadNotifsFromReg` (notif.cpp:601-711): Windows
//!   Registry persistence. Unlike `CCRule`, `CCNotif` has no text grammar to
//!   fall back to - `Serialize`/`UnSerialize` (notif.cpp:128-289) are a
//!   fixed-layout *binary* blob (version byte + `WORD` flags + counts +
//!   null-terminated field bytes) built specifically for
//!   `RegSetValueEx(..., REG_BINARY, ...)`, not a human text format the way
//!   `CCRule::bUnSerialize`'s pipe-delimited grammar is. Inventing a text
//!   grammar here would not be a port of anything in the pinned source, so
//!   this module exposes the pure data model + matching + diffing only and
//!   leaves persistence to the caller, same as the binary registry blob did
//!   for the app.
//! - `bSortNotifs` (notif.cpp:562-598) and `CompareNotifs`
//!   (notipage.cpp:28-44): list-view sort order is a display concern of the
//!   dialog above, not the data model.
//! - IRCX nickname literal-quoting (`TrimQuotes`, `bExtendedNickname`,
//!   `EncodeNick`) that `bNotifDaemonQuery` (actions.cpp:1058-1064) applies
//!   to the nickname before masking: this port's `buildNicknameMask` takes
//!   an already-decoded plain nickname, matching this codebase's IRCX
//!   encode/decode boundary already living in `proto/udi.zig` rather than
//!   here.
//! - `CUser::m_strRoom` alongside `m_strPrettyRoom` (notif.cpp:877,882):
//!   the source keeps both a raw and a display-formatted room string; this
//!   port keeps only one room string per tracked user (a simplification,
//!   not a fidelity gap in the diff/match logic itself).
//! - Actually issuing the WHO query and running the daemon timer
//!   (`CCDynaNotifs::bStartNotifsDaemon`/`OnNotifsDaemonTimer`,
//!   notif.cpp:732-798) and the `CQueryPtrList`/`CCQuery` WHO-reply
//!   correlation queue (`query.cpp`/`query.h`) it rides on: both are live
//!   IRC I/O plumbing, left to the caller exactly as `RULEDAEMON_QUERY_FN`
//!   is in `rules.zig`.
//! - `bIsMatch`/`PRUSERMATCH`, the wildcard mask matcher `bNotifDaemonQuery`
//!   feeds into for actually filtering a WHO reply (`ircsock.cpp:2593`):
//!   not in the pinned snapshot (the same gap `rules.zig`'s module doc
//!   comment already documents for the identical function). `matchesMask`
//!   below reuses `rules.zig`'s `matchesNickMask`/`globMatchCaseInsensitive`,
//!   the verified standard IRC ban-mask convention already established
//!   there for this exact gap.
//!
//! This module lives in `comic/` rather than `net/` because the source
//! keeps `notif.cpp` beside `rules.cpp` as a peer of the same
//! `CCDaemonExt`-driven rule/automation layer (both built on `rules.h`,
//! both operated on by the identical snapshot-diffing pattern) - matching
//! `rules.zig`'s home rather than the live-transport code in `net/`.

const std = @import("std");
const rules = @import("rules.zig");

// --- CCNotif: one watched entry (notif.h:22-32,52-101; notif.cpp:58-92,331-345) ---

/// `g_uAny`/`g_uEquals`/`g_uContains`/`g_uStartsWith`/`g_uEndsWith`
/// (notif.h:28-32).
pub const NotifOperator = enum(u8) {
    any = 0,
    equals = 1,
    contains = 2,
    starts_with = 3,
    ends_with = 4,
};

/// One watched entry (`CCNotif`'s data fields, notif.h:93-100). `net_name`
/// being `null` is the source's "%Any%" keyword (notif.cpp:152,271-274,341);
/// notify.h counts only 3 operators for 4 params (`g_uNotifParamNum-1`,
/// notif.h:16) because the network field has no operator of its own.
pub const Notif = struct {
    nickname: []const u8 = "",
    user_name: []const u8 = "",
    host_name: []const u8 = "",
    net_name: ?[]const u8 = null,
    nickname_op: NotifOperator = .any,
    user_name_op: NotifOperator = .any,
    host_name_op: NotifOperator = .any,
    active: bool = false,

    /// `CCNotif::operator==` (notif.cpp:58-74): same operators and same
    /// params, case-insensitive. (The source separately re-compares
    /// `net_name` a second time at notif.cpp:70-71 after the loop at
    /// notif.cpp:66-68 already covered it - a harmless duplicate this port
    /// does not reproduce.)
    pub fn eql(a: Notif, b: Notif) bool {
        if (a.nickname_op != b.nickname_op or
            a.user_name_op != b.user_name_op or
            a.host_name_op != b.host_name_op) return false;
        if (!std.ascii.eqlIgnoreCase(a.nickname, b.nickname)) return false;
        if (!std.ascii.eqlIgnoreCase(a.user_name, b.user_name)) return false;
        if (!std.ascii.eqlIgnoreCase(a.host_name, b.host_name)) return false;
        return netNameEqlIgnoreCase(a.net_name, b.net_name);
    }

    /// `CCNotif::bDaemonNeeded` (notif.cpp:331-345): the entry needs a live
    /// WHO poll only while active, and only if its network filter is "any"
    /// or the caller confirms the filtered network is currently valid
    /// (`m_pfNetValid`, a live-network concern left to the caller).
    pub fn daemonNeeded(self: Notif, network_valid: bool) bool {
        if (!self.active) return false;
        return self.net_name == null or network_valid;
    }
};

fn netNameEqlIgnoreCase(a: ?[]const u8, b: ?[]const u8) bool {
    if (a == null and b == null) return true;
    if (a == null or b == null) return false;
    return std.ascii.eqlIgnoreCase(a.?, b.?);
}

/// `StrAddWildcards` (actions.cpp:1013-1041): turn one filter field into a
/// glob fragment per its operator. A `text` starting with `'` keeps that
/// leading quote outside the inserted wildcard for `.contains`/`.ends_with`
/// exactly as the source does (an IRCX-literal-nickname marker this port
/// does not otherwise interpret - see the module doc comment).
pub fn addWildcards(buf: []u8, text: []const u8, op: NotifOperator) error{NoSpaceLeft}![]const u8 {
    return switch (op) {
        .any => "*",
        .equals => text,
        .contains => if (text.len > 0 and text[0] == '\'')
            try std.fmt.bufPrint(buf, "'*{s}*", .{text[1..]})
        else
            try std.fmt.bufPrint(buf, "*{s}*", .{text}),
        .starts_with => try std.fmt.bufPrint(buf, "{s}*", .{text}),
        .ends_with => if (text.len > 0 and text[0] == '\'')
            try std.fmt.bufPrint(buf, "'*{s}", .{text[1..]})
        else
            try std.fmt.bufPrint(buf, "*{s}", .{text}),
    };
}

/// `bNotifDaemonQuery` (actions.cpp:1044-1071), minus its live query
/// dispatch and IRCX nickname encoding (see the module doc comment): builds
/// the `nick!user@host` wildcard mask a watched entry's three fields and
/// operators resolve to, both for a WHO command's search argument and for
/// locally re-verifying each reply (`matchesMask` below).
pub fn buildNicknameMask(buf: []u8, notif: Notif) error{NoSpaceLeft}![]const u8 {
    var nick_buf: [130]u8 = undefined;
    var user_buf: [130]u8 = undefined;
    var host_buf: [130]u8 = undefined;
    const nick = try addWildcards(&nick_buf, notif.nickname, notif.nickname_op);
    const user = try addWildcards(&user_buf, notif.user_name, notif.user_name_op);
    const host = try addWildcards(&host_buf, notif.host_name, notif.host_name_op);
    return std.fmt.bufPrint(buf, "{s}!{s}@{s}", .{ nick, user, host });
}

/// Whether a live `nick!user@host` (a WHO reply, or any candidate) matches
/// a watched entry's mask (`ircsock.cpp:2593`'s `bIsMatch` call, ported via
/// `rules.zig`'s verified glob convention - see the module doc comment).
pub fn matchesMask(notif: Notif, nick: []const u8, user: []const u8, host: []const u8) bool {
    var buf: [400]u8 = undefined;
    const mask = buildNicknameMask(&buf, notif) catch return false;
    return rules.matchesNickMask(mask, nick, user, host);
}

// --- Tracked-user overlay (CCDynaNotifs, notif.cpp:421-460,801-899) -------

pub const flag_visible: u16 = 0x0001;
pub const flag_connected: u16 = 0x0002;
pub const flag_new: u16 = 0x0004;
pub const flag_altered: u16 = 0x1000;

/// One user snapshot from a WHO poll (the fields `CCDaemonExt` actually
/// reads off `CUser`: notif.cpp:807-809/859-861 key the outer diff on
/// nickname+identity+pretty-room; notif.cpp:812-813's `iFindUserIndex`
/// keys the tracked-user lookup on nickname+identity alone).
pub const WhoUser = struct {
    nickname: []const u8,
    /// `user@host`, matching `CUser::m_strIdentity`.
    identity: []const u8,
    /// `CUser::m_strPrettyRoom`; empty when the user is not in a room this
    /// port tracks.
    pretty_room: []const u8 = "",
};

fn sameWatchedPerson(a: WhoUser, b: WhoUser) bool {
    return std.mem.eql(u8, a.nickname, b.nickname) and std.mem.eql(u8, a.identity, b.identity);
}

fn sameSnapshotEntry(a: WhoUser, b: WhoUser) bool {
    return sameWatchedPerson(a, b) and std.mem.eql(u8, a.pretty_room, b.pretty_room);
}

/// A tracked entry in the notify overlay: `CUser`'s nickname/identity/room
/// plus the flags `bAddNotificationUser`/`bModifyNotificationUser` set.
/// Owns its own string storage (the source keeps this alive via `CUser`
/// refcounting across daemon polls; this port copies instead so a tracked
/// entry outlives the transient WHO-poll snapshot it was folded from).
pub const NotifiedUser = struct {
    nickname: []const u8,
    identity: []const u8,
    pretty_room: []const u8,
    flags: u16,

    fn deinit(self: *NotifiedUser, gpa: std.mem.Allocator) void {
        gpa.free(self.nickname);
        gpa.free(self.identity);
        gpa.free(self.pretty_room);
    }
};

/// `CCDynaNotifs::iFindUserIndex` (notif.cpp:801-817).
pub fn findUserIndex(tracked: []const NotifiedUser, nickname: []const u8, identity: []const u8) ?usize {
    for (tracked, 0..) |u, i| {
        if (std.mem.eql(u8, u.nickname, nickname) and std.mem.eql(u8, u.identity, identity)) return i;
    }
    return null;
}

/// `CCDynaNotifs`'s tracked-user list (`m_rgpNotifUsers`) plus the modified-
/// user counter the app polls to know when to show a notification.
pub const NotifyOverlay = struct {
    tracked: std.ArrayList(NotifiedUser) = .empty,
    modified_users_count: usize = 0,

    pub fn deinit(self: *NotifyOverlay, gpa: std.mem.Allocator) void {
        for (self.tracked.items) |*u| u.deinit(gpa);
        self.tracked.deinit(gpa);
    }

    pub fn resetModifiedUsersCount(self: *NotifyOverlay) void {
        self.modified_users_count = 0;
    }

    /// `CCDynaNotifs::bRemoveAllUsers` (notif.cpp:421-425).
    pub fn removeAllUsers(self: *NotifyOverlay, gpa: std.mem.Allocator) void {
        for (self.tracked.items) |*u| u.deinit(gpa);
        self.tracked.clearRetainingCapacity();
    }

    /// `CCDynaNotifs::bRemoveUsersWithoutFlag` (notif.cpp:428-445). The
    /// source's `for (iIndex = 0; iIndex < iUsers; iIndex++)` increments
    /// `iIndex` unconditionally, even on an iteration that just called
    /// `RemoveAt(iIndex)`. A removal shifts the next element down into the
    /// just-vacated slot, but the loop then skips straight past it without
    /// checking it - so an element immediately following a removed one
    /// survives even if it also lacks `flag` (verified by tracing
    /// notif.cpp's real loop: [A(no),B(no),C(yes),D(no)] with `flag`
    /// missing leaves B in the array). This port reproduces that exact
    /// skip via the same unconditional per-iteration increment, rather
    /// than "fixing" it into an every-element scan that the pinned source
    /// does not actually perform.
    pub fn removeUsersWithoutFlag(self: *NotifyOverlay, gpa: std.mem.Allocator, flag: u16) void {
        var i: usize = 0;
        while (i < self.tracked.items.len) : (i += 1) {
            if (self.tracked.items[i].flags & flag == 0) {
                var removed = self.tracked.orderedRemove(i);
                removed.deinit(gpa);
            }
        }
    }

    /// `CCDynaNotifs::bRemoveFlagsFromAllUsers` (notif.cpp:448-460).
    pub fn removeFlagsFromAllUsers(self: *NotifyOverlay, flags: u16) void {
        for (self.tracked.items) |*u| u.flags &= ~flags;
    }

    /// `CCDynaNotifs::bAddNotificationUser` (notif.cpp:820-840).
    pub fn addNotificationUser(self: *NotifyOverlay, gpa: std.mem.Allocator, user: WhoUser) !void {
        if (findUserIndex(self.tracked.items, user.nickname, user.identity)) |idx| {
            try self.modifyNotificationUserAt(gpa, idx, user, flag_connected, 0);
            return;
        }

        const nickname_owned = try gpa.dupe(u8, user.nickname);
        errdefer gpa.free(nickname_owned);
        const identity_owned = try gpa.dupe(u8, user.identity);
        errdefer gpa.free(identity_owned);
        const room_owned = try gpa.dupe(u8, user.pretty_room);
        errdefer gpa.free(room_owned);

        try self.tracked.append(gpa, .{
            .nickname = nickname_owned,
            .identity = identity_owned,
            .pretty_room = room_owned,
            .flags = flag_visible | flag_connected | flag_new | flag_altered,
        });
        self.modified_users_count += 1;
    }

    /// `CCDynaNotifs::bModifyNotificationUser` (notif.cpp:843-899), the
    /// lookup-by-(nickname,identity) path (`iIndex < 0` in the source).
    /// A no-op if no tracked entry matches, exactly as the source returns
    /// `TRUE` without modifying anything (notif.cpp:854-855).
    pub fn modifyNotificationUser(self: *NotifyOverlay, gpa: std.mem.Allocator, user: WhoUser, add_flags: u16, remove_flags: u16) !void {
        const idx = findUserIndex(self.tracked.items, user.nickname, user.identity) orelse return;
        try self.modifyNotificationUserAt(gpa, idx, user, add_flags, remove_flags);
    }

    fn modifyNotificationUserAt(self: *NotifyOverlay, gpa: std.mem.Allocator, index: usize, user: WhoUser, add_flags: u16, remove_flags: u16) !void {
        var entry = self.tracked.items[index];
        const already_altered = entry.flags & flag_altered != 0;

        // notif.cpp:870-871.
        var flags = entry.flags | add_flags | flag_visible | flag_new | flag_altered;
        flags &= ~remove_flags;
        entry.flags = flags;

        // notif.cpp:875-883: room tracks the update only while connected;
        // otherwise it is cleared.
        const new_room = if (flags & flag_connected != 0) user.pretty_room else "";
        const room_owned = try gpa.dupe(u8, new_room);
        gpa.free(entry.pretty_room);
        entry.pretty_room = room_owned;

        // notif.cpp:886-888: "Put this user to the end of the array."
        // Removing then appending never needs to grow the backing store
        // (the slot just freed by the removal covers the append), so this
        // cannot itself fail after the point of no return above; the
        // `errdefer` still guards the case where it somehow does, so a
        // owned-string leak never survives an error return.
        _ = self.tracked.orderedRemove(index);
        errdefer entry.deinit(gpa);
        try self.tracked.append(gpa, entry);

        if (!already_altered) self.modified_users_count += 1;
    }
};

/// The notify overload of `CCDaemonExt::bOnEndOfListing` (rules.cpp:709-778),
/// minus its whos-count/timer/callback bookkeeping (live-daemon plumbing
/// left to the caller, as documented above): fold one WHO poll's
/// `previous`/`current` snapshots into `overlay`'s tracked-user list,
/// offline updates first then online arrivals - exactly the order the
/// source calls `bTreatOldItems` before `bTreatNewItems`
/// (rules.cpp:739,743). Reversing that order is a real bug class: a user
/// who only changed room would have `bTreatNewItems`' online update
/// immediately undone by `bTreatOldItems`' stale-room removal (see the
/// module's deliberate-break test).
pub fn foldWhoSnapshot(
    overlay: *NotifyOverlay,
    gpa: std.mem.Allocator,
    previous: []const WhoUser,
    current: []const WhoUser,
) !void {
    // bTreatOldItems (rules.cpp:831-879): present in `previous`, absent
    // from `current` by the (nickname, identity, pretty room) triple.
    for (previous) |prev| {
        const still_here = for (current) |cur| {
            if (sameSnapshotEntry(prev, cur)) break true;
        } else false;
        if (!still_here) {
            try overlay.modifyNotificationUser(gpa, prev, 0, flag_connected);
        }
    }

    // bTreatNewItems (rules.cpp:781-828): present in `current`, absent
    // from `previous` by the same triple.
    for (current) |cur| {
        const already_there = for (previous) |prev| {
            if (sameSnapshotEntry(prev, cur)) break true;
        } else false;
        if (!already_there) {
            try overlay.addNotificationUser(gpa, cur);
        }
    }
}

// --- Tests ------------------------------------------------------------

const testing = std.testing;

test "Notif.eql compares operators and params case-insensitively" {
    const a: Notif = .{ .nickname = "Anna", .nickname_op = .starts_with, .net_name = "IRCXNet" };
    const b: Notif = .{ .nickname = "anna", .nickname_op = .starts_with, .net_name = "ircxnet" };
    const c: Notif = .{ .nickname = "anna", .nickname_op = .equals, .net_name = "ircxnet" };
    const d: Notif = .{ .nickname = "anna", .nickname_op = .starts_with, .net_name = null };
    try testing.expect(a.eql(b));
    try testing.expect(!a.eql(c));
    try testing.expect(!a.eql(d));
    try testing.expect((Notif{}).eql(Notif{}));
}

test "Notif.daemonNeeded requires active, and network validity unless Any" {
    const inactive: Notif = .{ .active = false };
    try testing.expect(!inactive.daemonNeeded(true));

    const any_net: Notif = .{ .active = true, .net_name = null };
    try testing.expect(any_net.daemonNeeded(false));

    const filtered: Notif = .{ .active = true, .net_name = "IRCXNet" };
    try testing.expect(filtered.daemonNeeded(true));
    try testing.expect(!filtered.daemonNeeded(false));
}

test "addWildcards implements StrAddWildcards's five operators" {
    var buf: [64]u8 = undefined;
    try testing.expectEqualStrings("*", try addWildcards(&buf, "anna", .any));
    try testing.expectEqualStrings("anna", try addWildcards(&buf, "anna", .equals));
    try testing.expectEqualStrings("*anna*", try addWildcards(&buf, "anna", .contains));
    try testing.expectEqualStrings("anna*", try addWildcards(&buf, "anna", .starts_with));
    try testing.expectEqualStrings("*anna", try addWildcards(&buf, "anna", .ends_with));

    // A leading quote (the IRCX-literal-nickname marker) stays outside the
    // inserted wildcard (actions.cpp:1022-1024,1031-1033).
    try testing.expectEqualStrings("'*anna*", try addWildcards(&buf, "'anna", .contains));
    try testing.expectEqualStrings("'*anna", try addWildcards(&buf, "'anna", .ends_with));
}

test "buildNicknameMask assembles nick!user@host per field operator" {
    var buf: [200]u8 = undefined;
    const notif: Notif = .{
        .nickname = "anna",
        .nickname_op = .starts_with,
        .user_name = "anna",
        .user_name_op = .equals,
        .host_name = "example",
        .host_name_op = .contains,
    };
    try testing.expectEqualStrings("anna*!anna@*example*", try buildNicknameMask(&buf, notif));
}

test "matchesMask checks a live identity against the built mask" {
    const notif: Notif = .{ .nickname = "anna", .nickname_op = .starts_with, .user_name = "*" };
    try testing.expect(matchesMask(notif, "Annabelle", "u", "example.invalid"));
    try testing.expect(!matchesMask(notif, "bob", "u", "example.invalid"));
}

test "findUserIndex keys on nickname+identity only" {
    const tracked = [_]NotifiedUser{
        .{ .nickname = "anna", .identity = "a@h", .pretty_room = "#one", .flags = flag_visible },
    };
    try testing.expectEqual(@as(?usize, 0), findUserIndex(&tracked, "anna", "a@h"));
    try testing.expectEqual(@as(?usize, null), findUserIndex(&tracked, "anna", "b@h"));
}

test "NotifyOverlay: a brand-new user is added online and counted once" {
    const gpa = testing.allocator;
    var overlay: NotifyOverlay = .{};
    defer overlay.deinit(gpa);

    try overlay.addNotificationUser(gpa, .{ .nickname = "anna", .identity = "a@h", .pretty_room = "#one" });
    try testing.expectEqual(@as(usize, 1), overlay.tracked.items.len);
    try testing.expectEqual(@as(usize, 1), overlay.modified_users_count);

    const u = overlay.tracked.items[0];
    try testing.expectEqualStrings("#one", u.pretty_room);
    try testing.expect(u.flags & flag_visible != 0);
    try testing.expect(u.flags & flag_connected != 0);
    try testing.expect(u.flags & flag_new != 0);
    try testing.expect(u.flags & flag_altered != 0);
}

test "NotifyOverlay: re-adding an already-tracked identity marks it online in place" {
    const gpa = testing.allocator;
    var overlay: NotifyOverlay = .{};
    defer overlay.deinit(gpa);

    try overlay.addNotificationUser(gpa, .{ .nickname = "anna", .identity = "a@h", .pretty_room = "#one" });
    overlay.resetModifiedUsersCount();
    try overlay.modifyNotificationUser(gpa, .{ .nickname = "anna", .identity = "a@h", .pretty_room = "#one" }, 0, flag_connected);
    try testing.expectEqual(@as(usize, 1), overlay.tracked.items.len);
    try testing.expect(overlay.tracked.items[0].flags & flag_connected == 0);
    try testing.expectEqualStrings("", overlay.tracked.items[0].pretty_room);

    // Already altered (from the add above) - modifying again does not
    // double-count (notif.cpp:868,890).
    try testing.expectEqual(@as(usize, 0), overlay.modified_users_count);

    try overlay.addNotificationUser(gpa, .{ .nickname = "anna", .identity = "a@h", .pretty_room = "#two" });
    try testing.expectEqual(@as(usize, 1), overlay.tracked.items.len);
    try testing.expect(overlay.tracked.items[0].flags & flag_connected != 0);
    try testing.expectEqualStrings("#two", overlay.tracked.items[0].pretty_room);
}

test "removeUsersWithoutFlag/removeFlagsFromAllUsers/removeAllUsers" {
    const gpa = testing.allocator;
    var overlay: NotifyOverlay = .{};
    defer overlay.deinit(gpa);

    try overlay.addNotificationUser(gpa, .{ .nickname = "anna", .identity = "a@h" });
    try overlay.addNotificationUser(gpa, .{ .nickname = "bob", .identity = "b@h" });
    overlay.tracked.items[1].flags &= ~flag_connected; // simulate "bob" going offline

    overlay.removeUsersWithoutFlag(gpa, flag_connected);
    try testing.expectEqual(@as(usize, 1), overlay.tracked.items.len);
    try testing.expectEqualStrings("anna", overlay.tracked.items[0].nickname);

    overlay.removeFlagsFromAllUsers(flag_new | flag_altered);
    try testing.expect(overlay.tracked.items[0].flags & (flag_new | flag_altered) == 0);
    try testing.expect(overlay.tracked.items[0].flags & flag_visible != 0);

    overlay.removeAllUsers(gpa);
    try testing.expectEqual(@as(usize, 0), overlay.tracked.items.len);
}

test "removeUsersWithoutFlag reproduces the source's skip-after-removal quirk" {
    // notif.cpp:428-445's `for` loop always increments `iIndex`, even right
    // after `RemoveAt(iIndex)` shifted the next element down into slot
    // `iIndex` - so that shifted-in element is skipped and survives even
    // without `flag`. [A(no),B(no),C(yes),D(no)]: removing A shifts B into
    // slot 0, but the loop moves on to slot 1 (C) without re-checking B, so
    // B survives; D (the last element) is still reached and removed.
    const gpa = testing.allocator;
    var overlay: NotifyOverlay = .{};
    defer overlay.deinit(gpa);

    try overlay.addNotificationUser(gpa, .{ .nickname = "a", .identity = "a@h" });
    try overlay.addNotificationUser(gpa, .{ .nickname = "b", .identity = "b@h" });
    try overlay.addNotificationUser(gpa, .{ .nickname = "c", .identity = "c@h" });
    try overlay.addNotificationUser(gpa, .{ .nickname = "d", .identity = "d@h" });
    // Every entry starts with flag_connected set (addNotificationUser);
    // knock it off everyone except "c".
    for (overlay.tracked.items) |*u| {
        if (!std.mem.eql(u8, u.nickname, "c")) u.flags &= ~flag_connected;
    }

    overlay.removeUsersWithoutFlag(gpa, flag_connected);

    try testing.expectEqual(@as(usize, 2), overlay.tracked.items.len);
    try testing.expectEqualStrings("b", overlay.tracked.items[0].nickname);
    try testing.expectEqualStrings("c", overlay.tracked.items[1].nickname);
}

test "foldWhoSnapshot: arrival, then a room change stays connected in the new room" {
    const gpa = testing.allocator;
    var overlay: NotifyOverlay = .{};
    defer overlay.deinit(gpa);

    // Poll 1: anna appears in #one.
    try foldWhoSnapshot(&overlay, gpa, &.{}, &.{
        .{ .nickname = "anna", .identity = "a@h", .pretty_room = "#one" },
    });
    try testing.expectEqual(@as(usize, 1), overlay.tracked.items.len);
    try testing.expect(overlay.tracked.items[0].flags & flag_connected != 0);
    try testing.expectEqualStrings("#one", overlay.tracked.items[0].pretty_room);

    // Poll 2: same person, now in #two. The (nickname, identity, room)
    // triple differs, so this is simultaneously "old" (gone from #one) and
    // "new" (arrived in #two) - bTreatOldItems running before bTreatNewItems
    // must leave the single tracked entry connected in the new room, not
    // stuck offline.
    try foldWhoSnapshot(&overlay, gpa, &.{
        .{ .nickname = "anna", .identity = "a@h", .pretty_room = "#one" },
    }, &.{
        .{ .nickname = "anna", .identity = "a@h", .pretty_room = "#two" },
    });

    try testing.expectEqual(@as(usize, 1), overlay.tracked.items.len);
    try testing.expect(overlay.tracked.items[0].flags & flag_connected != 0);
    try testing.expectEqualStrings("#two", overlay.tracked.items[0].pretty_room);
}

test "foldWhoSnapshot: a user who is simply gone loses connected and clears room" {
    const gpa = testing.allocator;
    var overlay: NotifyOverlay = .{};
    defer overlay.deinit(gpa);

    try foldWhoSnapshot(&overlay, gpa, &.{}, &.{
        .{ .nickname = "anna", .identity = "a@h", .pretty_room = "#one" },
    });
    try foldWhoSnapshot(&overlay, gpa, &.{
        .{ .nickname = "anna", .identity = "a@h", .pretty_room = "#one" },
    }, &.{});

    try testing.expectEqual(@as(usize, 1), overlay.tracked.items.len);
    try testing.expect(overlay.tracked.items[0].flags & flag_connected == 0);
    try testing.expectEqualStrings("", overlay.tracked.items[0].pretty_room);
}
