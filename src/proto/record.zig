//! Comic Chat conversation/transcript record codec.
//!
//! Reverse-engineered from the decompilation of cchat.exe (Comic Chat 2.5).
//! Comic state travels inside ordinary IRC PRIVMSG/NOTICE text — and in saved
//! `.ccc` transcripts — as CRLF-terminated, TAB-delimited tagged records. A
//! plain IRC client just sees text; a comic client decodes the tags.
//!
//! Document headers: `#CHATCONVERSATION`, `#CHATLOCATOR`.
//! Wire example: `comicchar\t<name>\t<data>\r\n`
//!
//! The reader in the original is a keyword-dispatch loop over
//! CArchive::ReadString: read a line, match the first TAB-token against a
//! keyword table, construct the matching record type. We mirror that here.

const std = @import("std");

pub const RecordType = enum {
    unknown,
    chat_conversation, // "#CHATCONVERSATION" document header
    chat_locator, // "#CHATLOCATOR" document header
    comicchar, // character identity/state: comicchar \t name \t data
    changeavatar, // switch the speaker's avatar
    backdrop, // set the panel background
    character, // a character definition record
    text, // spoken line (normal balloon)
    whisper, // whispered line (dashed balloon)
    action, // action / "thought"-style line
    url, // shared hyperlink
    sound, // sound cue
    starthistory, // begin history replay marker
    getinfo, // request peer info
    nick, // nickname record
    irc_server, // "IRCSERVER:" session tag
    irc_channel, // "IRCCHANNEL:" session tag
    comics_data, // "COMICSDATA:" payload tag
};

const KeywordEntry = struct { word: []const u8, type: RecordType };

// Keywords exactly as they appear in the binary's .rdata (case-sensitive;
// both observed casings are listed where the original accepts variants).
const keyword_table = [_]KeywordEntry{
    .{ .word = "#CHATCONVERSATION", .type = .chat_conversation },
    .{ .word = "#CHATLOCATOR", .type = .chat_locator },
    .{ .word = "comicchar", .type = .comicchar },
    .{ .word = "changeavatar", .type = .changeavatar },
    .{ .word = "backdrop", .type = .backdrop },
    .{ .word = "Backdrop", .type = .backdrop },
    .{ .word = "Character", .type = .character },
    .{ .word = "Text", .type = .text },
    .{ .word = "WHISPER", .type = .whisper },
    .{ .word = "ACTION", .type = .action },
    .{ .word = "URL", .type = .url },
    .{ .word = "SOUND", .type = .sound },
    .{ .word = "starthistory", .type = .starthistory },
    .{ .word = "getinfo", .type = .getinfo },
    .{ .word = "nick", .type = .nick },
    .{ .word = "NICK", .type = .nick },
    .{ .word = "IRCSERVER:", .type = .irc_server },
    .{ .word = "IRCCHANNEL:", .type = .irc_channel },
    .{ .word = "COMICSDATA:", .type = .comics_data },
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
    for (keyword_table) |entry| {
        if (std.mem.eql(u8, entry.word, keyword)) return entry.type;
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

/// Append the canonical `comicchar\t<name>\t<data>\r\n` record.
pub fn writeComicchar(
    out: *std.ArrayList(u8),
    gpa: std.mem.Allocator,
    name: []const u8,
    data: []const u8,
) !void {
    try writeRecord(out, gpa, "comicchar", &.{ name, data });
}

// --- Tests ----------------------------------------------------------------

test "parseLine: comicchar with two fields" {
    const rec = parseLine("comicchar\tAnna\tH4sIAAA\r\n");
    try std.testing.expectEqual(RecordType.comicchar, rec.type);
    try std.testing.expectEqual(@as(usize, 2), rec.field_count);
    try std.testing.expectEqualStrings("Anna", rec.field(0).?);
    try std.testing.expectEqualStrings("H4sIAAA", rec.field(1).?);
    try std.testing.expect(rec.field(2) == null);
}

test "parseLine: session tags keep the colon in the keyword" {
    const s = parseLine("IRCSERVER:\tirc.eshmaki.me\r\n");
    try std.testing.expectEqual(RecordType.irc_server, s.type);
    try std.testing.expectEqualStrings("irc.eshmaki.me", s.field(0).?);

    const c = parseLine("IRCCHANNEL:\t#comics");
    try std.testing.expectEqual(RecordType.irc_channel, c.type);
    try std.testing.expectEqualStrings("#comics", c.field(0).?);
}

test "parseLine: casing variants and document headers" {
    try std.testing.expectEqual(RecordType.nick, parseLine("NICK\tbob").type);
    try std.testing.expectEqual(RecordType.nick, parseLine("nick\tbob").type);
    try std.testing.expectEqual(RecordType.backdrop, parseLine("Backdrop\tfield").type);
    try std.testing.expectEqual(RecordType.chat_conversation, parseLine("#CHATCONVERSATION").type);
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

    try writeComicchar(&buf, gpa, "Cosmo", "eNplkk");
    try std.testing.expectEqualStrings("comicchar\tCosmo\teNplkk\r\n", buf.items);

    const rec = parseLine(buf.items);
    try std.testing.expectEqual(RecordType.comicchar, rec.type);
    try std.testing.expectEqualStrings("Cosmo", rec.field(0).?);
    try std.testing.expectEqualStrings("eNplkk", rec.field(1).?);
}

test "DocumentIterator walks a small transcript" {
    const doc =
        "#CHATCONVERSATION\r\n" ++
        "IRCCHANNEL:\t#comics\r\n" ++
        "comicchar\tAnna\tDATA1\r\n" ++
        "\r\n" ++ // blank line skipped
        "Text\tAnna\tHello world\r\n";

    var it = DocumentIterator.init(doc);
    try std.testing.expectEqual(RecordType.chat_conversation, it.next().?.type);
    try std.testing.expectEqual(RecordType.irc_channel, it.next().?.type);
    try std.testing.expectEqual(RecordType.comicchar, it.next().?.type);
    const text = it.next().?;
    try std.testing.expectEqual(RecordType.text, text.type);
    try std.testing.expectEqualStrings("Hello world", text.field(1).?);
    try std.testing.expect(it.next() == null);
}
