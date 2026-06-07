//! The Comic Chat "emotion wheel": map a named emotion to a head-pose index.
//!
//! Each avatar's head poses are its expressions. Index 0 is the neutral/resting
//! face for every avatar; the remaining indices vary per character but generally
//! progress through expressive states. This mapping is best-effort — exact
//! per-avatar ordering isn't encoded in a portable way — and `figure.assemble`
//! clamps any out-of-range index back to 0, so an unavailable emotion simply
//! falls back to neutral rather than failing.

const std = @import("std");

pub const Emotion = enum {
    neutral,
    happy,
    talking,
    surprised,
    sad,
    angry,
    shouting,
    coy,
    bored,

    pub fn headIndex(e: Emotion) usize {
        return @intFromEnum(e);
    }

    pub fn fromName(name: []const u8) ?Emotion {
        inline for (@typeInfo(Emotion).@"enum".fields) |f| {
            if (std.ascii.eqlIgnoreCase(name, f.name)) return @enumFromInt(f.value);
        }
        return null;
    }
};

test "emotion name round-trips and neutral is index 0" {
    try std.testing.expectEqual(@as(usize, 0), Emotion.neutral.headIndex());
    try std.testing.expectEqual(Emotion.angry, Emotion.fromName("ANGRY").?);
    try std.testing.expectEqual(Emotion.happy, Emotion.fromName("happy").?);
    try std.testing.expect(Emotion.fromName("nonsense") == null);
}
