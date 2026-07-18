//! Parser for Microsoft Comic Chat `.avb` avatar and `.bgb` backdrop files.
//!
//! The released Comic Chat 2.5 source defines this format in `avbfile.h` and
//! reads it in `avbfile.cpp`. Files start with a packed six-byte header, then a
//! tagged metadata stream. Complex avatars contain packed face and torso
//! records; simple avatars contain packed whole-body records. Each pose record
//! points directly at its image resource and carries the authored emotion,
//! intensity, and alignment coordinates.

const std = @import("std");

pub const magic: u16 = 0x8181;
pub const old_magic: u16 = 0x0081;

pub const Kind = enum(u16) {
    simple_avatar = 1,
    /// The historical name is retained for source compatibility. This is the
    /// `AT_COMPLEX` avatar type in Microsoft's source.
    avatar = 2,
    background = 3,
    _,

    pub fn isAvatar(self: Kind) bool {
        return self == .simple_avatar or self == .avatar;
    }
};

pub const Asset = struct {
    kind: Kind,
    version: u16,
    /// Avatar display name. Borrows from the input buffer.
    name: ?[]const u8,
    /// Copyright/artist text. Borrows from the input buffer.
    copyright: ?[]const u8,
    /// Low byte of `CAvatarX::m_flags`. The released client truncates the
    /// 16-bit `AK_FLAGS` payload to this byte before `CBodyDouble::DrawBody`
    /// selects mask and head/torso drawing order.
    flags: AvatarFlags = .{},
    /// Low byte of the source-defined `AK_STYLE` value. The 2.5 renderer does
    /// not interpret it, but preserving it is required for old assets.
    style: u8 = 0,
};

/// `avatar.h`'s authored avatar flags, kept bit-for-bit in source order.
pub const AvatarFlags = packed struct(u8) {
    head_mask: bool = false, // HEADMASK
    torso_mask: bool = false, // TORSOMASK
    torso_first: bool = false, // TORSOFIRST
    other_mapped: bool = false, // OTHERMAPPED (normally runtime-only)
    reserved: u4 = 0,

    pub fn fromRaw(value: u8) AvatarFlags {
        return @bitCast(value);
    }

    pub fn raw(self: AvatarFlags) u8 {
        return @bitCast(self);
    }
};

pub const ParseError = error{
    BadMagic,
    Truncated,
    UnsupportedRecord,
    InvalidOffset,
    MissingImage,
};

const Tag = enum(u16) {
    name = 1,
    flags = 2,
    icon = 3,
    faces_old = 4,
    torsos_old = 5,
    start_data = 6,
    end_data = 7,
    style = 8,
    bodies_old = 9,
    faces = 10,
    torsos = 11,
    bodies = 12,

    icon_new = 256,
    color_palette = 257,
    backdrop = 258,
    copyright = 259,
    original_url = 260,
    override_url = 261,
    usage_flags = 262,
    offset_adjustment = 263,
    _,
};

pub const ImageFormat = enum(u8) {
    dib = 0,
    zlib = 1,
    _,
};

pub const PaletteType = enum(u8) {
    none = 0,
    global = 1,
    local = 2,
    monochrome = 3,
    masked_mono = 4,
    dual_mask = 5,
    _,
};

pub const Point = struct { x: i16 = 0, y: i16 = 0 };

pub const ImageRef = struct {
    offset: u32 = 0,
    format: ImageFormat = .dib,
    palette: PaletteType = .none,
};

pub const PoseLayer = enum { face, torso, body };

pub const ImageRole = enum { drawing, mask, aura };

/// How `CPose::Load` turns the referenced bitmap into a logical drawing layer.
/// This preserves the packed-mask cases instead of pretending images[1] and
/// images[2] always contain independent resources.
pub const ImageComponent = enum {
    whole,
    /// AIP_MASKEDMONO image bit, ANDed with its mask bit by the source client.
    masked_mono_drawing,
    low_bit,
    high_bit,
    any_bit,
};

pub const PoseImagePlan = struct {
    image: ImageRef,
    component: ImageComponent = .whole,
};

pub const PoseRecord = struct {
    layer: PoseLayer,
    /// Records with the same pose ID deliberately reuse the previous image.
    pose_id: u32,
    images: [3]ImageRef,
    /// Index into `avatario.cpp`'s exact emotion table (1..9 expressions,
    /// 10..17 authored gestures; zero is an invalid/sentinel value).
    emotion_index: u16,
    /// Authored strength, converted by the old client to `byte / 255.0`.
    intensity: u8,
    /// Face: head center; torso: torso center; simple body: unused.
    center: Point,
    /// Face-only adjustment applied before joining it to the torso.
    delta: Point,
    /// Word-balloon/face point (`x`,`y` in the packed record).
    face: Point,

    /// Resolve the source client's logical drawing, mask, or aura input.
    /// AIP_MASKEDMONO packs all three into image 0; AIP_DUALMASK packs mask
    /// and aura into image 1. Callers therefore need both the reference and
    /// the component rule to reproduce CPose::ConvertMasksCommon.
    pub fn imagePlan(self: PoseRecord, role: ImageRole) ?PoseImagePlan {
        if (self.images[0].offset == 0) return null;
        if (self.images[0].palette == .masked_mono) return .{
            .image = self.images[0],
            .component = switch (role) {
                .drawing => .masked_mono_drawing,
                .mask => .high_bit,
                .aura => .any_bit,
            },
        };
        if (role != .drawing and self.images[1].offset != 0 and self.images[1].palette == .dual_mask) return .{
            .image = self.images[1],
            .component = if (role == .mask) .low_bit else .high_bit,
        };
        const index: usize = @intFromEnum(role);
        if (self.images[index].offset == 0) return null;
        return .{ .image = self.images[index] };
    }
};

pub const PoseTable = struct {
    kind: Kind,
    records: []PoseRecord,

    pub fn deinit(self: *PoseTable, gpa: std.mem.Allocator) void {
        gpa.free(self.records);
        self.* = undefined;
    }
};

fn readU16(b: []const u8, off: usize) ParseError!u16 {
    if (off + 2 > b.len) return error.Truncated;
    return @as(u16, b[off]) | (@as(u16, b[off + 1]) << 8);
}

fn readI16(b: []const u8, off: usize) ParseError!i16 {
    return @bitCast(try readU16(b, off));
}

fn readU32(b: []const u8, off: usize) ParseError!u32 {
    if (off + 4 > b.len) return error.Truncated;
    return @as(u32, b[off]) | (@as(u32, b[off + 1]) << 8) |
        (@as(u32, b[off + 2]) << 16) | (@as(u32, b[off + 3]) << 24);
}

fn readI32(b: []const u8, off: usize) ParseError!i32 {
    return @bitCast(try readU32(b, off));
}

fn cString(b: []const u8, off: usize, limit: usize) ParseError!struct { value: ?[]const u8, next: usize } {
    if (off > b.len or limit > b.len or off > limit) return error.Truncated;
    const end = std.mem.indexOfScalarPos(u8, b[0..limit], off, 0) orelse return error.Truncated;
    return .{ .value = if (end == off) null else b[off..end], .next = end + 1 };
}

fn checkedAdvance(pos: usize, amount: usize, len: usize) ParseError!usize {
    const next = std.math.add(usize, pos, amount) catch return error.Truncated;
    if (next > len) return error.Truncated;
    return next;
}

fn oldRecordSize(tag: Tag) ?usize {
    return switch (tag) {
        .faces_old => 43,
        .torsos_old, .bodies_old => 35,
        else => null,
    };
}

fn newRecordSize(tag: Tag) ?usize {
    return switch (tag) {
        .faces => 33,
        .torsos, .bodies => 25,
        else => null,
    };
}

fn skipOldTag(bytes: []const u8, pos: usize, tag: Tag) ParseError!usize {
    return switch (tag) {
        .name => (try cString(bytes, pos, bytes.len)).next,
        .flags, .style => checkedAdvance(pos, 2, bytes.len),
        .icon => checkedAdvance(pos, 4, bytes.len),
        .faces_old, .torsos_old, .bodies_old, .faces, .torsos, .bodies => blk: {
            const count = try readU16(bytes, pos);
            const stride = oldRecordSize(tag) orelse newRecordSize(tag).?;
            const records_len = std.math.mul(usize, count, stride) catch return error.Truncated;
            break :blk checkedAdvance(pos + 2, records_len, bytes.len);
        },
        .start_data, .end_data => pos,
        else => error.UnsupportedRecord,
    };
}

/// Parse just the common header and metadata. Unlike the former heuristic,
/// copyright is read only from `AK_COPYRIGHT`; arbitrary image bytes containing
/// that word cannot be mistaken for metadata.
pub fn parse(bytes: []const u8) ParseError!Asset {
    if (bytes.len < 6) return error.Truncated;
    const file_magic = try readU16(bytes, 0);
    if (file_magic != magic and file_magic != old_magic) return error.BadMagic;

    const kind: Kind = @enumFromInt(try readU16(bytes, 2));
    const version = try readU16(bytes, 4);
    var name: ?[]const u8 = null;
    var copyright: ?[]const u8 = null;
    var flags: AvatarFlags = .{};
    var style: u8 = 0;
    var pos: usize = 6;

    while (pos < bytes.len) {
        const tag: Tag = @enumFromInt(try readU16(bytes, pos));
        pos += 2;
        if (tag == .start_data) break;

        if (@intFromEnum(tag) >= @intFromEnum(Tag.icon_new)) {
            const size = try readU16(bytes, pos);
            pos += 2;
            const end = try checkedAdvance(pos, size, bytes.len);
            switch (tag) {
                .copyright => copyright = (try cString(bytes, pos, end)).value,
                else => {},
            }
            pos = end;
            continue;
        }

        switch (tag) {
            .name => {
                const value = try cString(bytes, pos, bytes.len);
                if (kind.isAvatar()) name = value.value;
                pos = value.next;
            },
            .flags => {
                flags = .fromRaw(@truncate(try readU16(bytes, pos)));
                pos = try checkedAdvance(pos, 2, bytes.len);
            },
            .style => {
                style = @truncate(try readU16(bytes, pos));
                pos = try checkedAdvance(pos, 2, bytes.len);
            },
            else => pos = try skipOldTag(bytes, pos, tag),
        }
    }

    return .{
        .kind = kind,
        .version = version,
        .name = name,
        .copyright = copyright,
        .flags = flags,
        .style = style,
    };
}

fn adjustedOffset(raw: u32, adjustment: i64) ParseError!u32 {
    if (raw == 0) return 0;
    const value = @as(i64, raw) + adjustment;
    if (value <= 0 or value > std.math.maxInt(u32)) return error.InvalidOffset;
    return @intCast(value);
}

fn recordLayer(tag: Tag) PoseLayer {
    return switch (tag) {
        .faces, .faces_old => .face,
        .torsos, .torsos_old => .torso,
        .bodies, .bodies_old => .body,
        else => unreachable,
    };
}

fn appendPoseRecords(
    gpa: std.mem.Allocator,
    list: *std.ArrayList(PoseRecord),
    bytes: []const u8,
    records_pos: usize,
    count: u16,
    tag: Tag,
    adjustment: i64,
    next_pose_id: *u32,
) !void {
    const stride = oldRecordSize(tag) orelse newRecordSize(tag).?;
    const layer = recordLayer(tag);
    var previous_raw_offset: u32 = 0;
    var previous_pose_id: u32 = 0;

    var n: usize = 0;
    while (n < count) : (n += 1) {
        const off = records_pos + n * stride;
        _ = try checkedAdvance(off, stride, bytes.len);

        var refs: [3]ImageRef = undefined;
        const old_record = oldRecordSize(tag) != null;
        const format_off: usize = switch (layer) {
            .face => 27,
            .torso, .body => 19,
        };
        var image_n: usize = 0;
        while (image_n < 3) : (image_n += 1) {
            const raw = try readU32(bytes, off + image_n * 4);
            refs[image_n] = .{
                .offset = try adjustedOffset(raw, adjustment),
                // AK_NFACES/AK_NTORSOS/AK_NBODIES predate format and palette
                // fields. Their trailing 16 bytes really are padding; the old
                // loader treats every referenced resource as an ordinary BMP
                // file with its own color table, just like AK_ICON.
                .format = if (old_record) .dib else @enumFromInt(bytes[off + format_off + image_n]),
                .palette = if (old_record) .none else @enumFromInt(bytes[off + format_off + 3 + image_n]),
            };
        }

        const raw_primary = try readU32(bytes, off);
        const pose_id = if (n > 0 and raw_primary == previous_raw_offset)
            previous_pose_id
        else blk: {
            const id = next_pose_id.*;
            next_pose_id.* += 1;
            break :blk id;
        };
        previous_raw_offset = raw_primary;
        previous_pose_id = pose_id;

        const center: Point = switch (layer) {
            .face, .torso => .{
                .x = try readI16(bytes, off + 15),
                .y = try readI16(bytes, off + 17),
            },
            .body => .{},
        };
        const delta: Point = if (layer == .face) .{
            .x = try readI16(bytes, off + 19),
            .y = try readI16(bytes, off + 21),
        } else .{};
        const face_off: usize = switch (layer) {
            .face => 23,
            .torso => 0,
            .body => 15,
        };
        const face: Point = if (layer == .torso) .{} else .{
            .x = try readI16(bytes, off + face_off),
            .y = try readI16(bytes, off + face_off + 2),
        };

        try list.append(gpa, .{
            .layer = layer,
            .pose_id = pose_id,
            .images = refs,
            .emotion_index = try readU16(bytes, off + 12),
            .intensity = bytes[off + 14],
            .center = center,
            .delta = delta,
            .face = face,
        });
    }
}

/// Decode the exact packed pose-record tables. Image offsets include every
/// preceding `AK_OFFSET_ADJUSTMENT`, matching `ADJUST_OFFSET` in `avbfile.cpp`.
pub fn parsePoseTable(gpa: std.mem.Allocator, bytes: []const u8) !PoseTable {
    const header = try parse(bytes);
    var list: std.ArrayList(PoseRecord) = .empty;
    errdefer list.deinit(gpa);

    var pos: usize = 6;
    var adjustment: i64 = 0;
    var next_pose_id: u32 = 1;
    while (pos < bytes.len) {
        const tag: Tag = @enumFromInt(try readU16(bytes, pos));
        pos += 2;
        if (tag == .start_data) break;

        if (@intFromEnum(tag) >= @intFromEnum(Tag.icon_new)) {
            const size = try readU16(bytes, pos);
            pos += 2;
            const end = try checkedAdvance(pos, size, bytes.len);
            if (tag == .offset_adjustment) {
                if (size < 4) return error.Truncated;
                adjustment += try readI32(bytes, pos);
            } else if (tag == .icon_new and size >= 4) {
                // The old loader creates the icon pose before face/body poses,
                // so reserve its one-based pose ID.
                next_pose_id += 1;
            }
            pos = end;
            continue;
        }

        switch (tag) {
            .faces_old, .torsos_old, .bodies_old, .faces, .torsos, .bodies => {
                const count = try readU16(bytes, pos);
                pos += 2;
                const stride = oldRecordSize(tag) orelse newRecordSize(tag).?;
                try appendPoseRecords(gpa, &list, bytes, pos, count, tag, adjustment, &next_pose_id);
                pos = try checkedAdvance(pos, @as(usize, count) * stride, bytes.len);
            },
            else => pos = try skipOldTag(bytes, pos, tag),
        }
    }

    return .{ .kind = header.kind, .records = try list.toOwnedSlice(gpa) };
}

/// Return the backdrop image reference from `AK_BACKDROP`, including offset
/// adjustments introduced by metadata editors.
pub fn backdropImage(bytes: []const u8) ParseError!ImageRef {
    const header = try parse(bytes);
    if (header.kind != .background) return error.MissingImage;

    var pos: usize = 6;
    var adjustment: i64 = 0;
    while (pos < bytes.len) {
        const tag: Tag = @enumFromInt(try readU16(bytes, pos));
        pos += 2;
        if (tag == .start_data) break;
        if (@intFromEnum(tag) >= @intFromEnum(Tag.icon_new)) {
            const size = try readU16(bytes, pos);
            pos += 2;
            const end = try checkedAdvance(pos, size, bytes.len);
            if (tag == .offset_adjustment) {
                if (size < 4) return error.Truncated;
                adjustment += try readI32(bytes, pos);
            } else if (tag == .backdrop) {
                if (size < 6) return error.Truncated;
                return .{
                    .offset = try adjustedOffset(try readU32(bytes, pos), adjustment),
                    .format = @enumFromInt(bytes[pos + 4]),
                    .palette = @enumFromInt(bytes[pos + 5]),
                };
            }
            pos = end;
        } else {
            pos = try skipOldTag(bytes, pos, tag);
        }
    }
    return error.MissingImage;
}

/// Return the avatar icon image selected by `AK_ICON`/`AK_ICON_NEW`.
/// The old four-byte record is an uncompressed palette-less DIB; the modern
/// six-byte record carries the same format/palette bytes as pose resources.
pub fn iconImage(bytes: []const u8) ParseError!ImageRef {
    const header = try parse(bytes);
    if (!header.kind.isAvatar()) return error.MissingImage;

    var pos: usize = 6;
    var adjustment: i64 = 0;
    while (pos < bytes.len) {
        const tag: Tag = @enumFromInt(try readU16(bytes, pos));
        pos += 2;
        if (tag == .start_data) break;
        if (@intFromEnum(tag) >= @intFromEnum(Tag.icon_new)) {
            const size = try readU16(bytes, pos);
            pos += 2;
            const end = try checkedAdvance(pos, size, bytes.len);
            if (tag == .offset_adjustment) {
                if (size < 4) return error.Truncated;
                adjustment += try readI32(bytes, pos);
            } else if (tag == .icon_new) {
                if (size < 6) return error.Truncated;
                return .{
                    .offset = try adjustedOffset(try readU32(bytes, pos), adjustment),
                    .format = @enumFromInt(bytes[pos + 4]),
                    .palette = @enumFromInt(bytes[pos + 5]),
                };
            }
            pos = end;
            continue;
        }
        if (tag == .icon) return .{
            .offset = try adjustedOffset(try readU32(bytes, pos), adjustment),
            .format = .dib,
            .palette = .none,
        };
        pos = try skipOldTag(bytes, pos, tag);
    }
    return error.MissingImage;
}

// --- Tests ----------------------------------------------------------------

test "parse tagged metadata for a complex avatar" {
    const data =
        "\x81\x81" ++ "\x02\x00" ++ "\x02\x00" ++
        "\x01\x00Anna\x00" ++
        "\x08\x00\x01\x00" ++
        "\x03\x01\x13\x00Copyright (c) Test\x00" ++
        "\x06\x00";

    const asset = try parse(data);
    try std.testing.expectEqual(Kind.avatar, asset.kind);
    try std.testing.expectEqual(@as(u16, 2), asset.version);
    try std.testing.expectEqualStrings("Anna", asset.name.?);
    try std.testing.expectEqualStrings("Copyright (c) Test", asset.copyright.?);
    try std.testing.expectEqual(@as(u8, 1), asset.style);
}

test "parse recognizes simple avatars as avatars" {
    const data = "\x81\x81\x01\x00\x02\x00\x01\x00Jordan\x00\x06\x00";
    const asset = try parse(data);
    try std.testing.expectEqual(Kind.simple_avatar, asset.kind);
    try std.testing.expectEqualStrings("Jordan", asset.name.?);
}

test "parse rejects bad magic and truncation" {
    try std.testing.expectError(error.BadMagic, parse("\x00\x00\x02\x00\x02\x00"));
    try std.testing.expectError(error.Truncated, parse("\x81\x81\x02"));
}

test "parse real released assets through tagged records" {
    const anna = @embedFile("testdata/anna.avb");
    const asset = try parse(anna);
    try std.testing.expectEqual(Kind.avatar, asset.kind);
    try std.testing.expectEqualStrings("Anna", asset.name.?);
    try std.testing.expect(std.mem.indexOf(u8, asset.copyright.?, "Jim Woodring") != null);
    // anna.avb authors HEADMASK | TORSOFIRST. These bits control the exact
    // compositing sequence in CBodyDouble::DrawBody.
    try std.testing.expect(asset.flags.head_mask);
    try std.testing.expect(!asset.flags.torso_mask);
    try std.testing.expect(asset.flags.torso_first);
    try std.testing.expectEqual(@as(u8, 5), asset.flags.raw());
    try std.testing.expectEqual(@as(u8, 1), asset.style);

    const field = @embedFile("testdata/field.bgb");
    const backdrop = try parse(field);
    try std.testing.expectEqual(Kind.background, backdrop.kind);
    try std.testing.expect(backdrop.name == null);
}

test "old pose tags ignore padding and force embedded DIB resources" {
    // AVATARFACEDATA::olddata is 27 meaningful bytes plus 16 padding bytes.
    // Deliberately fill padding with 0xaa so treating it as new format/palette
    // fields would produce invalid enum values.
    const data =
        "\x81\x00\x02\x00\x01\x00" ++ // old magic, complex, v1
        "\x02\x00\x07\x00" ++ // HEADMASK|TORSOMASK|TORSOFIRST
        "\x08\x00\x34\x12" ++ // style truncates exactly like m_style
        "\x04\x00\x01\x00" ++ // AK_NFACES, count 1
        "\x00\x01\x00\x00\x00\x02\x00\x00\x00\x03\x00\x00" ++
        "\x09\x00\x80" ++ // neutral, intensity
        "\x0a\x00\x14\x00\xfe\xff\x03\x00\x1e\x00\x28\x00" ++
        "\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa" ++
        "\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa" ++
        "\x06\x00";

    const asset = try parse(data);
    try std.testing.expectEqual(@as(u8, 7), asset.flags.raw());
    try std.testing.expectEqual(@as(u8, 0x34), asset.style);

    var table = try parsePoseTable(std.testing.allocator, data);
    defer table.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(usize, 1), table.records.len);
    const record = table.records[0];
    try std.testing.expectEqual(@as(i16, 10), record.center.x);
    try std.testing.expectEqual(@as(i16, -2), record.delta.x);
    try std.testing.expectEqual(@as(i16, 30), record.face.x);
    for (record.images, 0..) |image, index| {
        try std.testing.expectEqual(@as(u32, @intCast((index + 1) * 0x100)), image.offset);
        try std.testing.expectEqual(ImageFormat.dib, image.format);
        try std.testing.expectEqual(PaletteType.none, image.palette);
    }
}

test "real pose records preserve emotion, intensity, offsets, and ditto links" {
    const gpa = std.testing.allocator;
    var table = try parsePoseTable(gpa, @embedFile("testdata/anna.avb"));
    defer table.deinit(gpa);

    try std.testing.expectEqual(Kind.avatar, table.kind);
    try std.testing.expectEqual(@as(usize, 34), table.records.len); // 18 faces + 16 torsos
    const first = table.records[0];
    try std.testing.expectEqual(PoseLayer.face, first.layer);
    try std.testing.expectEqual(@as(u16, 9), first.emotion_index);
    try std.testing.expectEqual(@as(u8, 0), first.intensity);
    try std.testing.expectEqual(@as(u32, 0x603), first.images[0].offset);
    try std.testing.expectEqual(ImageFormat.zlib, first.images[0].format);
    try std.testing.expectEqual(PaletteType.masked_mono, first.images[0].palette);
    try std.testing.expectEqual(@as(i16, 92), first.center.x);
    try std.testing.expectEqual(@as(i16, 111), first.center.y);
    try std.testing.expectEqual(@as(i16, -4), first.delta.x);
    try std.testing.expectEqual(@as(i16, 106), first.face.x);
    try std.testing.expectEqual(@as(i16, 80), first.face.y);
    try std.testing.expectEqual(ImageComponent.masked_mono_drawing, first.imagePlan(.drawing).?.component);
    try std.testing.expectEqual(ImageComponent.high_bit, first.imagePlan(.mask).?.component);
    try std.testing.expectEqual(ImageComponent.any_bit, first.imagePlan(.aura).?.component);
    try std.testing.expectEqual(first.images[0].offset, first.imagePlan(.aura).?.image.offset);

    // Face records 2 and 3 share the same primary image offset; this is the
    // `ditto` case in `LoadFaceRecs`, not two unrelated zlib streams.
    try std.testing.expectEqual(table.records[2].images[0].offset, table.records[3].images[0].offset);
    try std.testing.expectEqual(table.records[2].pose_id, table.records[3].pose_id);
    try std.testing.expectEqual(@as(u16, 9), table.records[2].emotion_index);
    try std.testing.expectEqual(@as(u16, 2), table.records[3].emotion_index);
}

test "backdrop reference follows AK_OFFSET_ADJUSTMENT" {
    const image = try backdropImage(@embedFile("testdata/field.bgb"));
    try std.testing.expectEqual(@as(u32, 0x53), image.offset);
    try std.testing.expectEqual(ImageFormat.zlib, image.format);
    try std.testing.expectEqual(PaletteType.local, image.palette);
}

test "all bundled avatar pose tables are structurally valid" {
    const gpa = std.testing.allocator;
    const blobs = [_][]const u8{
        @embedFile("testdata/anna.avb"),     @embedFile("testdata/armando.avb"),
        @embedFile("testdata/bolo.avb"),     @embedFile("testdata/cro.avb"),
        @embedFile("testdata/dan.avb"),      @embedFile("testdata/denise.avb"),
        @embedFile("testdata/hugh.avb"),     @embedFile("testdata/jordan.avb"),
        @embedFile("testdata/kevin.avb"),    @embedFile("testdata/kwensa.avb"),
        @embedFile("testdata/lance.avb"),    @embedFile("testdata/lynnea.avb"),
        @embedFile("testdata/margaret.avb"), @embedFile("testdata/maynard.avb"),
        @embedFile("testdata/mike.avb"),     @embedFile("testdata/rebecca.avb"),
        @embedFile("testdata/sage.avb"),     @embedFile("testdata/scotty.avb"),
        @embedFile("testdata/susan.avb"),    @embedFile("testdata/tiki.avb"),
        @embedFile("testdata/tongtyed.avb"), @embedFile("testdata/xeno.avb"),
    };
    inline for (blobs) |blob| {
        var table = try parsePoseTable(gpa, blob);
        defer table.deinit(gpa);
        try std.testing.expect(table.kind.isAvatar());
        try std.testing.expect(table.records.len > 0);
        for (table.records) |record| {
            try std.testing.expect(record.images[0].offset < blob.len);
            try std.testing.expect(record.emotion_index >= 1 and record.emotion_index <= 17);
        }
    }
}

test "bundled Xeno remains the byte-exact pinned MIT asset" {
    const expected = [_]u8{
        0x67, 0xf1, 0x76, 0xd2, 0x3c, 0x17, 0x01, 0x91,
        0x84, 0x98, 0xff, 0xfe, 0x14, 0x8d, 0xfb, 0x67,
        0xf2, 0xfc, 0x83, 0x68, 0x50, 0xcc, 0xf2, 0x9f,
        0x26, 0x64, 0x7e, 0xd5, 0x47, 0xfe, 0x29, 0x2d,
    };
    var actual: [std.crypto.hash.sha2.Sha256.digest_length]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash(@embedFile("testdata/xeno.avb"), &actual, .{});
    try std.testing.expectEqualSlices(u8, &expected, &actual);
}
