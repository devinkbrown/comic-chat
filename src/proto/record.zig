//! Comic Chat conversation/transcript record codec.
//!
//! Ported from Microsoft's released `histent.cpp` at upstream commit
//! `c7df00f60bc8e9fdef413f139e61f7c37e024684`. Saved `.ccc` conversations are
//! CRLF-terminated, TAB-delimited tagged records. Live IRC uses ordinary IRC
//! messages plus the source client's annotation/CTCP path; it is a separate
//! representation from this archive codec.
//!
//! Document headers: `#CHATCONVERSATION`, `#CHATLOCATOR`. Conversation loading
//! accepts the former case-insensitively as a prefix. Locator loading instead
//! scans for the latter with the source's exact case; use `LocatorIterator` for
//! that document-level behavior.
//! Wire example: `comicchar\t<name>\t<info>\r\n`
//!
//! `CChatDoc::ChatLoadConversation` reads through `CArchive::ReadString` and
//! dispatches record keywords with `stricmp`, so lookup here is deliberately
//! ASCII case-insensitive.

const std = @import("std");

pub const RecordType = enum {
    unknown,
    chat_conversation, // "#CHATCONVERSATION" document header
    chat_locator, // "#CHATLOCATOR" document header
    say, // say \t nick \t (gesture/expression/mode metadata) \t message
    join, // join \t nick [\t full name]
    existing_join, // ejoin \t nick [\t full name]
    part, // part \t nick
    comicchar, // unavailable-character information: comicchar \t name \t info
    changeavatar, // switch the speaker's avatar
    backdrop, // set the panel background
    starthistory, // begin history replay marker
    getinfo, // request peer info
    nick, // nickname record
    irc_server, // "IRCSERVER:" session tag
    irc_channel, // "IRCCHANNEL:" session tag
    cx_prompt, // "CXPROMPT:" locator tag
    character, // "CHARACTER:" locator tag
    locator_backdrop, // "BACKDROP:" locator tag (distinct from `.ccc` backdrop)
    comics_data, // "COMICSDATA:" payload tag
    title, // "TITLE:" locator tag
    art_dir, // "ARTDIR:" locator tag
    view, // "VIEW:" locator tag
};

const KeywordEntry = struct { word: []const u8, type: RecordType };

// Conversation keywords written and accepted by histent.cpp.
const conversation_keyword_table = [_]KeywordEntry{
    .{ .word = "#CHATCONVERSATION", .type = .chat_conversation },
    .{ .word = "say", .type = .say },
    .{ .word = "join", .type = .join },
    .{ .word = "ejoin", .type = .existing_join },
    .{ .word = "part", .type = .part },
    .{ .word = "comicchar", .type = .comicchar },
    .{ .word = "changeavatar", .type = .changeavatar },
    .{ .word = "backdrop", .type = .backdrop },
    .{ .word = "starthistory", .type = .starthistory },
    .{ .word = "getinfo", .type = .getinfo },
    .{ .word = "nick", .type = .nick },
};

// Locator tags written or accepted by ChatSaveLocator/ChatLoadLocator in
// setupdlg.cpp. Canonical writers use a TAB after the colon, while the source
// reader accepts arbitrary surrounding whitespace through GetValue.
const locator_keyword_table = [_]KeywordEntry{
    .{ .word = "IRCSERVER:", .type = .irc_server },
    .{ .word = "IRCCHANNEL:", .type = .irc_channel },
    .{ .word = "CXPROMPT:", .type = .cx_prompt },
    .{ .word = "CHARACTER:", .type = .character },
    .{ .word = "BACKDROP:", .type = .locator_backdrop },
    .{ .word = "COMICSDATA:", .type = .comics_data },
    .{ .word = "TITLE:", .type = .title },
    .{ .word = "ARTDIR:", .type = .art_dir },
    .{ .word = "VIEW:", .type = .view },
};

pub const max_fields = 16;

/// A parsed record. Field slices borrow from the source line (zero-copy):
/// the source must outlive the Record.
pub const Record = struct {
    type: RecordType,
    keyword: []const u8,
    fields: [max_fields][]const u8 = undefined,
    field_count: usize = 0,

    pub fn field(self: *const Record, i: usize) ?[]const u8 {
        if (i >= self.field_count) return null;
        return self.fields[i];
    }
};

fn lookup(keyword: []const u8) RecordType {
    // The conversation reader compares only the length of this prefix.
    if (std.ascii.startsWithIgnoreCase(keyword, "#CHATCONVERSATION"))
        return .chat_conversation;
    // Unlike the conversation header, ForwardToKey matches the locator header
    // byte-for-byte. LocatorIterator additionally reproduces its scan-forward
    // document behavior.
    if (std.mem.eql(u8, keyword, "#CHATLOCATOR")) return .chat_locator;
    for (conversation_keyword_table) |entry| {
        if (std.ascii.eqlIgnoreCase(entry.word, keyword)) {
            // Dispatch uses stricmp, but JoinEntry's prior-user flag uses a
            // later case-sensitive strcmp against exactly "ejoin".
            if (entry.type == .existing_join and !std.mem.eql(u8, keyword, "ejoin"))
                return .join;
            return entry.type;
        }
    }
    return lookupLocatorKeyword(keyword);
}

fn lookupLocatorKeyword(keyword: []const u8) RecordType {
    for (locator_keyword_table) |entry| {
        if (std.ascii.eqlIgnoreCase(entry.word, keyword)) return entry.type;
    }
    return .unknown;
}

/// Parse a single record line. A trailing CR/LF is tolerated and stripped.
pub fn parseLine(line: []const u8) Record {
    const trimmed = std.mem.trimEnd(u8, line, "\r\n");
    var it = std.mem.splitScalar(u8, trimmed, '\t');
    const keyword = it.first();
    var rec = Record{ .type = lookup(keyword), .keyword = keyword };
    while (it.next()) |tok| {
        if (rec.field_count >= max_fields) break;
        rec.fields[rec.field_count] = tok;
        rec.field_count += 1;
    }
    return rec;
}

/// Iterates the records of a multi-line document (LF or CRLF separated).
/// Blank lines are skipped.
pub const DocumentIterator = struct {
    lines: std.mem.SplitIterator(u8, .scalar),

    pub fn init(doc: []const u8) DocumentIterator {
        return .{ .lines = std.mem.splitScalar(u8, doc, '\n') };
    }

    pub fn next(self: *DocumentIterator) ?Record {
        while (self.lines.next()) |raw| {
            const line = std.mem.trimEnd(u8, raw, "\r");
            if (line.len == 0) continue;
            return parseLine(line);
        }
        return null;
    }
};

const locator_header = "#CHATLOCATOR";
const locator_whitespace = " \t\r\n\x0b\x0c";

/// Parse one line after a locator header. This mirrors the released loader's
/// `%49s` keyword scan and `GetValue`: leading whitespace is ignored, locator
/// tags are case-insensitive, and the single value is everything after the
/// first colon with surrounding whitespace removed. A whitespace-only line
/// returns null, matching the source loader's end-of-records behavior.
pub fn parseLocatorLine(line: []const u8) ?Record {
    const without_terminator = std.mem.trimEnd(u8, line, "\r\n");
    const content = std.mem.trimStart(u8, without_terminator, locator_whitespace);
    if (content.len == 0) return null;

    const keyword_end = std.mem.indexOfAny(u8, content, locator_whitespace) orelse content.len;
    const keyword = content[0..keyword_end];
    var rec = Record{ .type = lookupLocatorKeyword(keyword), .keyword = keyword };
    if (std.mem.indexOfScalar(u8, content, ':')) |colon| {
        rec.fields[0] = std.mem.trim(u8, content[colon + 1 ..], locator_whitespace);
        rec.field_count = 1;
    }
    return rec;
}

/// Iterate a complete `.ccr` locator using the source's document semantics.
/// `init` returns null unless the exact-case `#CHATLOCATOR` marker occurs. As in
/// `ForwardToKey`, any preamble and the remainder of the marker's line are
/// discarded; records begin on the following line and stop at the first blank
/// line.
pub const LocatorIterator = struct {
    lines: std.mem.SplitIterator(u8, .scalar),
    stopped: bool = false,

    pub fn init(doc: []const u8) ?LocatorIterator {
        const header_start = std.mem.indexOf(u8, doc, locator_header) orelse return null;
        const after_header = header_start + locator_header.len;
        const relative_newline = std.mem.indexOfScalar(u8, doc[after_header..], '\n') orelse
            return .{ .lines = std.mem.splitScalar(u8, doc[doc.len..], '\n') };
        const records_start = after_header + relative_newline + 1;
        return .{ .lines = std.mem.splitScalar(u8, doc[records_start..], '\n') };
    }

    pub fn next(self: *LocatorIterator) ?Record {
        if (self.stopped) return null;
        const raw = self.lines.next() orelse return null;
        const rec = parseLocatorLine(raw) orelse {
            self.stopped = true;
            return null;
        };
        return rec;
    }
};

// --- Encoding -------------------------------------------------------------

/// Append a generic record: `<keyword>[\t<field>]*\r\n`.
pub fn writeRecord(
    out: *std.ArrayList(u8),
    gpa: std.mem.Allocator,
    keyword: []const u8,
    fields: []const []const u8,
) !void {
    try out.appendSlice(gpa, keyword);
    for (fields) |f| {
        try out.append(gpa, '\t');
        try out.appendSlice(gpa, f);
    }
    try out.appendSlice(gpa, "\r\n");
}

/// Append the canonical `comicchar\t<name>\t<info>\r\n` unavailable-character
/// information record. This field is not an encoded avatar-state blob.
pub fn writeComicchar(
    out: *std.ArrayList(u8),
    gpa: std.mem.Allocator,
    name: []const u8,
    info: []const u8,
) !void {
    try writeRecord(out, gpa, "comicchar", &.{ name, info });
}

fn appendEscapedMessage(out: *std.ArrayList(u8), gpa: std.mem.Allocator, message: []const u8) !void {
    for (message) |byte| switch (byte) {
        '\n' => try out.appendSlice(gpa, "\\n"),
        '\r' => try out.appendSlice(gpa, "\\r"),
        '\t' => try out.appendSlice(gpa, "\\t"),
        '\\' => try out.appendSlice(gpa, "\\\\"),
        else => try out.append(gpa, byte),
    };
}

/// Undo `QuoteReturns` from `histent.cpp`. Like the released reader, an
/// unknown escape retains the backslash and discards the following byte.
pub fn unescapeMessageAlloc(gpa: std.mem.Allocator, message: []const u8) ![]u8 {
    var out: std.ArrayList(u8) = .empty;
    errdefer out.deinit(gpa);
    var i: usize = 0;
    while (i < message.len) {
        if (message[i] != '\\') {
            try out.append(gpa, message[i]);
            i += 1;
            continue;
        }
        if (i + 1 >= message.len) {
            try out.append(gpa, '\\');
            i += 1;
            continue;
        }
        try out.append(gpa, switch (message[i + 1]) {
            'n' => '\n',
            'r' => '\r',
            't' => '\t',
            '\\' => '\\',
            else => '\\',
        });
        i += 2;
    }
    return out.toOwnedSlice(gpa);
}

/// Append the archive shape emitted by `SayEntry::WriteSelf`. `metadata` is
/// the source parenthetical `(G:... E:... R:... M:... [T:...])` field.
pub fn writeSay(
    out: *std.ArrayList(u8),
    gpa: std.mem.Allocator,
    nick: []const u8,
    metadata: []const u8,
    message: []const u8,
) !void {
    try out.appendSlice(gpa, "say\t");
    try out.appendSlice(gpa, nick);
    try out.append(gpa, '\t');
    try out.appendSlice(gpa, metadata);
    try out.append(gpa, '\t');
    try appendEscapedMessage(out, gpa, message);
    try out.appendSlice(gpa, "\r\n");
}

// --- Tests ----------------------------------------------------------------

test "parseLine: comicchar with two fields" {
    const rec = parseLine("comicchar\tAnna\tcharacter unavailable\r\n");
    try std.testing.expectEqual(RecordType.comicchar, rec.type);
    try std.testing.expectEqual(@as(usize, 2), rec.field_count);
    try std.testing.expectEqualStrings("Anna", rec.field(0).?);
    try std.testing.expectEqualStrings("character unavailable", rec.field(1).?);
    try std.testing.expect(rec.field(2) == null);
}

test "parseLine: session tags keep the colon in the keyword" {
    const s = parseLine("IRCSERVER:\tirc.eshmaki.me\r\n");
    try std.testing.expectEqual(RecordType.irc_server, s.type);
    try std.testing.expectEqualStrings("irc.eshmaki.me", s.field(0).?);

    const c = parseLine("IRCCHANNEL:\t#comics");
    try std.testing.expectEqual(RecordType.irc_channel, c.type);
    try std.testing.expectEqualStrings("#comics", c.field(0).?);

    const b = parseLine("BACKDROP:\tfield");
    try std.testing.expectEqual(RecordType.locator_backdrop, b.type);
    try std.testing.expectEqualStrings("field", b.field(0).?);
}

test "parseLine: conversation reader is case-insensitive but locator header is exact-case" {
    try std.testing.expectEqual(RecordType.nick, parseLine("NICK\tbob").type);
    try std.testing.expectEqual(RecordType.nick, parseLine("nick\tbob").type);
    try std.testing.expectEqual(RecordType.backdrop, parseLine("Backdrop\tfield").type);
    try std.testing.expectEqual(RecordType.chat_conversation, parseLine("#chatconversation").type);
    try std.testing.expectEqual(RecordType.chat_conversation, parseLine("#CHATCONVERSATION-vNext").type);
    try std.testing.expectEqual(RecordType.existing_join, parseLine("ejoin\tbob").type);
    try std.testing.expectEqual(RecordType.join, parseLine("EJOIN\tbob").type);
    try std.testing.expectEqual(RecordType.chat_locator, parseLine("#CHATLOCATOR").type);
    try std.testing.expectEqual(RecordType.unknown, parseLine("#chatlocator").type);
}

test "parseLocatorLine: source whitespace and colon value rules" {
    const server = parseLocatorLine(" \tIrCsErVeR:   irc.example.test  \t\r\n").?;
    try std.testing.expectEqual(RecordType.irc_server, server.type);
    try std.testing.expectEqualStrings("IrCsErVeR:", server.keyword);
    try std.testing.expectEqual(@as(usize, 1), server.field_count);
    try std.testing.expectEqualStrings("irc.example.test", server.field(0).?);

    const title = parseLocatorLine("TITLE: A title with spaces").?;
    try std.testing.expectEqual(RecordType.title, title.type);
    try std.testing.expectEqualStrings("A title with spaces", title.field(0).?);
    try std.testing.expect(parseLocatorLine(" \t\r\n") == null);
}

test "LocatorIterator: exact header scan, valid tags, and blank-line stop" {
    const doc =
        "ignored preamble\r\n" ++
        "prefix #CHATLOCATOR discarded suffix\r\n" ++
        "IRCSERVER: server.example.test\r\n" ++
        "backdrop:\tfield\r\n" ++
        "\r\n" ++
        "VIEW:\tComics\r\n";

    var it = LocatorIterator.init(doc).?;
    const server = it.next().?;
    try std.testing.expectEqual(RecordType.irc_server, server.type);
    try std.testing.expectEqualStrings("server.example.test", server.field(0).?);
    const backdrop = it.next().?;
    try std.testing.expectEqual(RecordType.locator_backdrop, backdrop.type);
    try std.testing.expectEqualStrings("field", backdrop.field(0).?);
    try std.testing.expect(it.next() == null);
    try std.testing.expect(it.next() == null);
}

test "LocatorIterator: lowercase header is not a source locator" {
    try std.testing.expect(LocatorIterator.init("#chatlocator\r\nIRCSERVER:\tserver\r\n") == null);

    var empty = LocatorIterator.init("#CHATLOCATOR").?;
    try std.testing.expect(empty.next() == null);
}

test "writeRecord: locator output remains canonical TAB-delimited" {
    const gpa = std.testing.allocator;
    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(gpa);
    try writeRecord(&buf, gpa, "IRCSERVER:", &.{"server.example.test"});
    try std.testing.expectEqualStrings("IRCSERVER:\tserver.example.test\r\n", buf.items);
}

test "parseLine: unknown keyword degrades gracefully" {
    const rec = parseLine("frobnicate\tx\ty");
    try std.testing.expectEqual(RecordType.unknown, rec.type);
    try std.testing.expectEqualStrings("frobnicate", rec.keyword);
    try std.testing.expectEqual(@as(usize, 2), rec.field_count);
}

test "writeComicchar round-trips through parseLine" {
    const gpa = std.testing.allocator;
    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(gpa);

    try writeComicchar(&buf, gpa, "Cosmo", "character unavailable");
    try std.testing.expectEqualStrings("comicchar\tCosmo\tcharacter unavailable\r\n", buf.items);

    const rec = parseLine(buf.items);
    try std.testing.expectEqual(RecordType.comicchar, rec.type);
    try std.testing.expectEqualStrings("Cosmo", rec.field(0).?);
    try std.testing.expectEqualStrings("character unavailable", rec.field(1).?);
}

test "DocumentIterator walks a small transcript" {
    const doc =
        "#CHATCONVERSATION\r\n" ++
        "join\tAnna\tAnna Example\r\n" ++
        "comicchar\tAnna\tcharacter unavailable\r\n" ++
        "\r\n" ++ // blank line skipped
        "say\tAnna\t(G:0 0 0 E:0 0 0 R:1 M:0)\tHello world\r\n";

    var it = DocumentIterator.init(doc);
    try std.testing.expectEqual(RecordType.chat_conversation, it.next().?.type);
    try std.testing.expectEqual(RecordType.join, it.next().?.type);
    try std.testing.expectEqual(RecordType.comicchar, it.next().?.type);
    const text = it.next().?;
    try std.testing.expectEqual(RecordType.say, text.type);
    try std.testing.expectEqualStrings("Hello world", text.field(2).?);
    try std.testing.expect(it.next() == null);
}

test "parseLine: keyword with no fields" {
    const r = parseLine("starthistory");
    try std.testing.expectEqual(RecordType.starthistory, r.type);
    try std.testing.expectEqual(@as(usize, 0), r.field_count);
    try std.testing.expect(r.field(0) == null);
}

test "parseLine: empty line is an unknown record with empty keyword" {
    const r = parseLine("");
    try std.testing.expectEqual(RecordType.unknown, r.type);
    try std.testing.expectEqualStrings("", r.keyword);
    try std.testing.expectEqual(@as(usize, 0), r.field_count);
}

test "parseLine: consecutive tabs yield empty fields, none dropped" {
    const r = parseLine("say\tAnna\t\thi");
    try std.testing.expectEqual(RecordType.say, r.type);
    try std.testing.expectEqual(@as(usize, 3), r.field_count);
    try std.testing.expectEqualStrings("Anna", r.field(0).?);
    try std.testing.expectEqualStrings("", r.field(1).?);
    try std.testing.expectEqualStrings("hi", r.field(2).?);
}

test "parseLine: lone LF terminator is stripped" {
    const r = parseLine("nick\tbob\n");
    try std.testing.expectEqual(RecordType.nick, r.type);
    try std.testing.expectEqualStrings("bob", r.field(0).?);
}

test "parseLine: field overflow stops at max_fields without corruption" {
    // keyword + 20 fields; only max_fields are kept.
    const line = "say\t1\t2\t3\t4\t5\t6\t7\t8\t9\t10\t11\t12\t13\t14\t15\t16\t17\t18\t19\t20";
    const r = parseLine(line);
    try std.testing.expectEqual(@as(usize, max_fields), r.field_count);
    try std.testing.expectEqualStrings("1", r.field(0).?);
}

test "writeRecord: keyword with no fields just appends CRLF" {
    const gpa = std.testing.allocator;
    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(gpa);
    try writeRecord(&buf, gpa, "starthistory", &.{});
    try std.testing.expectEqualStrings("starthistory\r\n", buf.items);
}

test "writeRecord: fields preserve embedded spaces (tab-delimited, not space)" {
    const gpa = std.testing.allocator;
    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(gpa);
    try writeSay(&buf, gpa, "Anna", "(G:0 0 0 E:0 0 0 R:1 M:0)", "hello world");
    try std.testing.expectEqualStrings("say\tAnna\t(G:0 0 0 E:0 0 0 R:1 M:0)\thello world\r\n", buf.items);
    const r = parseLine(buf.items);
    try std.testing.expectEqual(RecordType.say, r.type);
    try std.testing.expectEqualStrings("hello world", r.field(2).?);
}

test "writeSay and unescapeMessageAlloc mirror QuoteReturns" {
    const gpa = std.testing.allocator;
    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(gpa);
    try writeSay(&buf, gpa, "Anna", "(G:0 0 0 E:0 0 0 R:1 M:1)", "one\ntwo\r\nthree\\four\tfive");
    const rec = parseLine(buf.items);
    try std.testing.expectEqual(RecordType.say, rec.type);
    try std.testing.expectEqualStrings("one\\ntwo\\r\\nthree\\\\four\\tfive", rec.field(2).?);
    const decoded = try unescapeMessageAlloc(gpa, rec.field(2).?);
    defer gpa.free(decoded);
    try std.testing.expectEqualStrings("one\ntwo\r\nthree\\four\tfive", decoded);

    const malformed = try unescapeMessageAlloc(gpa, "a\\qb\\");
    defer gpa.free(malformed);
    try std.testing.expectEqualStrings("a\\b\\", malformed);
}

test "DocumentIterator: handles LF-only separators and trailing newline" {
    const doc = "nick\tbob\npart\tbob\n";
    var it = DocumentIterator.init(doc);
    try std.testing.expectEqual(RecordType.nick, it.next().?.type);
    const t = it.next().?;
    try std.testing.expectEqual(RecordType.part, t.type);
    try std.testing.expectEqualStrings("bob", t.field(0).?);
    try std.testing.expect(it.next() == null);
}

test "DocumentIterator: a document of only blank lines yields nothing" {
    var it = DocumentIterator.init("\r\n\n\r\n");
    try std.testing.expect(it.next() == null);
}

test "lookup: session-tag keywords are matched with their trailing colon" {
    try std.testing.expectEqual(RecordType.comics_data, parseLine("COMICSDATA:\tblob").type);
    // Without the colon it is no longer a recognized keyword.
    try std.testing.expectEqual(RecordType.unknown, parseLine("COMICSDATA\tblob").type);
}
