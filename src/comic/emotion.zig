//! Microsoft Comic Chat's authored emotion/gesture vocabulary and text rules.
//!
//! The exact AVB index mapping comes from `avatario.cpp::emFloats`; the wheel
//! angles and gesture constants come from `avatar.h`; and the English spotting
//! rules come from `textpose.cpp` plus `chat.rc` IDs 63032..63042.

const std = @import("std");

pub const Emotion = enum {
    // Keep the first nine values stable for the existing renderer command/API.
    neutral,
    happy,
    talking,
    surprised,
    sad,
    angry,
    shouting,
    coy,
    bored,
    laughing,

    // Exact old-client vocabulary not present in the early Zig sketch.
    scared,
    wave,
    point_other,
    point_self,
    double_point,
    shrug,
    walk_three_quarter_rear,
    walk_side,
    walk_three_quarter_front,

    /// Semantic selector consumed by `assets.bgb.decodePoseAuto`. The values
    /// preserve the original Zig CLI while routing to exact AVB emotions.
    pub fn headIndex(self: Emotion) usize {
        return switch (self) {
            .neutral => 0,
            .happy => 1,
            .talking => 2,
            .surprised, .scared => 3,
            .sad => 4,
            .angry => 5,
            .shouting => 6,
            .coy => 7,
            .bored => 8,
            .laughing => 9,
            // Gestures affect torso/whole-body records and do not identify a
            // head. Neutral is the faithful face fallback.
            else => 0,
        };
    }

    /// One-based index serialized in AVB records and IRC expression metadata.
    pub fn assetIndex(self: Emotion) u8 {
        return switch (self) {
            .happy => 1,
            .coy => 2,
            .bored => 3,
            .scared, .surprised => 4,
            .sad => 5,
            .angry => 6,
            .shouting => 7,
            .laughing => 8,
            .neutral, .talking => 9,
            .wave => 10,
            .point_other => 11,
            .point_self => 12,
            .double_point => 13,
            .shrug => 14,
            .walk_three_quarter_rear => 15,
            .walk_side => 16,
            .walk_three_quarter_front => 17,
        };
    }

    pub fn isGesture(self: Emotion) bool {
        return self.assetIndex() >= 10;
    }

    /// Exact eight-spoke wheel angle in radians. Neutral/talking use the
    /// center's zero-angle convention; discrete gestures have no wheel angle.
    pub fn wheelAngle(self: Emotion) ?f32 {
        const index = self.assetIndex();
        if (index >= 10) return null;
        if (index == 9) return 0;
        return @as(f32, @floatFromInt(index - 1)) * (2.0 * std.math.pi / 8.0);
    }

    pub fn fromAssetIndex(index: u8) ?Emotion {
        return switch (index) {
            1 => .happy,
            2 => .coy,
            3 => .bored,
            4 => .scared,
            5 => .sad,
            6 => .angry,
            7 => .shouting,
            8 => .laughing,
            9 => .neutral,
            10 => .wave,
            11 => .point_other,
            12 => .point_self,
            13 => .double_point,
            14 => .shrug,
            15 => .walk_three_quarter_rear,
            16 => .walk_side,
            17 => .walk_three_quarter_front,
            else => null,
        };
    }

    pub fn fromName(name: []const u8) ?Emotion {
        const info = @typeInfo(Emotion).@"enum";
        inline for (info.field_names, info.field_values) |field_name, field_value| {
            if (std.ascii.eqlIgnoreCase(name, field_name)) return @enumFromInt(field_value);
        }
        if (std.ascii.eqlIgnoreCase(name, "laugh")) return .laughing;
        if (std.ascii.eqlIgnoreCase(name, "shout")) return .shouting;
        if (std.ascii.eqlIgnoreCase(name, "afraid")) return .scared;
        return null;
    }
};

pub const EmotionOption = struct {
    emotion: Emotion,
    /// AVB strength byte. The old runtime exposed this as `byte / 255.0`.
    intensity: u8,
    priority: u8,
};

/// `CEmotionOpts` is a bounded, priority-ordered set rather than one guessed
/// face: a sentence may request a face (laugh) and a torso gesture (wave).
pub const TextAnalysis = struct {
    options: [10]EmotionOption = undefined,
    len: u8 = 0,

    pub fn slice(self: *const TextAnalysis) []const EmotionOption {
        return self.options[0..self.len];
    }

    pub fn add(self: *TextAnalysis, emotion: Emotion, intensity: u8, priority: u8) void {
        for (self.options[0..self.len]) |*option| {
            // The C++ key is its float emotion value. Compatibility aliases
            // such as surprised/scared therefore merge by AVB index.
            if (option.emotion.assetIndex() != emotion.assetIndex()) continue;
            if (option.priority < priority) {
                option.priority = priority;
                option.intensity = intensity;
                option.emotion = emotion;
            }
            return;
        }
        if (self.len >= self.options.len) return;
        self.options[self.len] = .{
            .emotion = emotion,
            .intensity = intensity,
            .priority = priority,
        };
        self.len += 1;
    }

    pub fn bestFace(self: *const TextAnalysis) Emotion {
        var best: Emotion = .neutral;
        var priority: u8 = 0;
        for (self.slice()) |option| {
            if (option.emotion.isGesture()) continue;
            if (option.priority > priority) {
                best = option.emotion;
                priority = option.priority;
            }
        }
        return best;
    }

    pub fn find(self: *const TextAnalysis, emotion: Emotion) ?EmotionOption {
        for (self.slice()) |option| {
            if (option.emotion.assetIndex() == emotion.assetIndex()) return option;
        }
        return null;
    }
};

/// Run the original English resource rules. Every match has full intensity;
/// priorities decide which authored face/torso constraints win.
pub fn analyzeText(text: []const u8) TextAnalysis {
    var result: TextAnalysis = .{};

    if (isAllCaps(text)) result.add(.shouting, 255, 9);
    if (std.mem.indexOf(u8, text, "!!!") != null) result.add(.shouting, 255, 9);

    if (containsWordIgnoreCase(text, "ROTFL") or containsWordIgnoreCase(text, "LOL") or
        containsIgnoreCase(text, "HEHE")) result.add(.laughing, 255, 11);
    if (std.mem.indexOf(u8, text, ":)") != null or
        std.mem.indexOf(u8, text, ":-)") != null) result.add(.happy, 255, 10);
    if (std.mem.indexOf(u8, text, ":(") != null or
        std.mem.indexOf(u8, text, ":-(") != null) result.add(.sad, 255, 10);

    if (startsSentenceIgnoreCase(text, "You")) result.add(.point_other, 255, 4);
    if (containsWordIgnoreCase(text, "are you") or containsWordIgnoreCase(text, "will you") or
        containsWordIgnoreCase(text, "did you") or containsWordIgnoreCase(text, "aren't you") or
        containsWordIgnoreCase(text, "don't you")) result.add(.point_other, 255, 8);

    if (startsSentenceIgnoreCase(text, "I")) result.add(.point_self, 255, 3);
    if (containsWordIgnoreCase(text, "i'm") or containsWordIgnoreCase(text, "i will") or
        containsWordIgnoreCase(text, "i'll") or containsWordIgnoreCase(text, "i am"))
        result.add(.point_self, 255, 7);

    if (startsSentenceIgnoreCase(text, "Hi")) result.add(.wave, 255, 2);
    if (startsSentenceIgnoreCase(text, "Bye")) result.add(.wave, 255, 3);
    if (startsSentenceIgnoreCase(text, "Hello") or startsSentenceIgnoreCase(text, "Welcome") or
        startsSentenceIgnoreCase(text, "Howdy")) result.add(.wave, 255, 5);

    if (std.mem.indexOf(u8, text, ";-)") != null or
        std.mem.indexOf(u8, text, ";)") != null) result.add(.coy, 255, 10);

    // Angry, scared, and bored have empty resource rules in chat.rc.
    return result;
}

/// Compatibility helper for callers that only support one face. Gestures stay
/// available through `analyzeText`; ordinary text has the old neutral fallback.
pub fn fromText(text: []const u8) Emotion {
    return analyzeText(text).bestFace();
}

fn isAllCaps(text: []const u8) bool {
    var uppercase: usize = 0;
    for (text) |ch| {
        if (std.ascii.isLower(ch)) return false;
        if (std.ascii.isUpper(ch)) uppercase += 1;
    }
    return uppercase > 1;
}

fn containsIgnoreCase(haystack: []const u8, needle: []const u8) bool {
    if (needle.len == 0 or needle.len > haystack.len) return false;
    var i: usize = 0;
    while (i + needle.len <= haystack.len) : (i += 1) {
        if (std.ascii.eqlIgnoreCase(haystack[i .. i + needle.len], needle)) return true;
    }
    return false;
}

/// `textpose.cpp::CheckWord`: starts after whitespace (or at byte zero), ends
/// before whitespace/punctuation (or at end). It deliberately accepts phrases.
fn containsWordIgnoreCase(haystack: []const u8, needle: []const u8) bool {
    if (needle.len == 0 or needle.len > haystack.len) return false;
    var i: usize = 0;
    while (i + needle.len <= haystack.len) : (i += 1) {
        if (!std.ascii.eqlIgnoreCase(haystack[i .. i + needle.len], needle)) continue;
        const before_ok = i == 0 or std.ascii.isWhitespace(haystack[i - 1]);
        const after_index = i + needle.len;
        const after_ok = after_index == haystack.len or std.ascii.isWhitespace(haystack[after_index]) or
            std.ascii.isPunctuation(haystack[after_index]);
        if (before_ok and after_ok) return true;
    }
    return false;
}

fn startsSentenceIgnoreCase(text: []const u8, prefix: []const u8) bool {
    if (prefix.len > text.len or !std.ascii.eqlIgnoreCase(text[0..prefix.len], prefix)) return false;
    return prefix.len == text.len or !std.ascii.isAlphanumeric(text[prefix.len]);
}

test "exact AVB emotion and gesture indices round-trip" {
    try std.testing.expectEqual(@as(u8, 1), Emotion.happy.assetIndex());
    try std.testing.expectEqual(@as(u8, 8), Emotion.laughing.assetIndex());
    try std.testing.expectEqual(@as(u8, 9), Emotion.neutral.assetIndex());
    try std.testing.expectEqual(@as(u8, 17), Emotion.walk_three_quarter_front.assetIndex());
    try std.testing.expectEqual(Emotion.scared, Emotion.fromAssetIndex(4).?);
    try std.testing.expectEqual(Emotion.angry, Emotion.fromName("ANGRY").?);
    try std.testing.expectEqual(Emotion.laughing, Emotion.fromName("laugh").?);
    try std.testing.expect(Emotion.fromAssetIndex(18) == null);
    try std.testing.expectApproxEqAbs(@as(f32, 0), Emotion.happy.wheelAngle().?, 0.0001);
    try std.testing.expectApproxEqAbs(@as(f32, std.math.pi), Emotion.sad.wheelAngle().?, 0.0001);
    try std.testing.expect(Emotion.wave.wheelAngle() == null);
}

test "head selectors preserve compatibility and map new vocabulary" {
    try std.testing.expectEqual(@as(usize, 0), Emotion.neutral.headIndex());
    try std.testing.expectEqual(@as(usize, 3), Emotion.scared.headIndex());
    try std.testing.expectEqual(@as(usize, 9), Emotion.laughing.headIndex());
    try std.testing.expectEqual(@as(usize, 0), Emotion.wave.headIndex());
}

test "source rules choose exact face priorities" {
    try std.testing.expectEqual(Emotion.neutral, fromText("just a normal line"));
    try std.testing.expectEqual(Emotion.neutral, fromText("what happened?"));
    try std.testing.expectEqual(Emotion.neutral, fromText("hey!"));
    try std.testing.expectEqual(Emotion.shouting, fromText("OK"));
    try std.testing.expectEqual(Emotion.shouting, fromText("watch out!!!"));
    try std.testing.expectEqual(Emotion.happy, fromText("that's great :)"));
    try std.testing.expectEqual(Emotion.sad, fromText("aw man :-("));
    try std.testing.expectEqual(Emotion.coy, fromText("maybe ;-)"));
    try std.testing.expectEqual(Emotion.laughing, fromText("LOL!!!")); // p11 beats shout p9
    try std.testing.expectEqual(Emotion.laughing, fromText("hehe, okay"));
    try std.testing.expectEqual(Emotion.neutral, fromText("lollipop guild"));
    try std.testing.expectEqual(Emotion.neutral, fromText("HAHA no way")); // no HAHA rule in chat.rc
}

test "analysis retains face and torso constraints" {
    const analysis = analyzeText("Hello! LOL");
    try std.testing.expectEqual(@as(u8, 2), analysis.len);
    try std.testing.expectEqual(@as(u8, 11), analysis.find(.laughing).?.priority);
    try std.testing.expectEqual(@as(u8, 5), analysis.find(.wave).?.priority);
    try std.testing.expectEqual(Emotion.laughing, analysis.bestFace());

    const pointing = analyzeText("You said, are you ready?");
    try std.testing.expectEqual(@as(u8, 8), pointing.find(.point_other).?.priority);
}

test "sentence-start rules preserve the released textpose pointer bug" {
    try std.testing.expect(analyzeText("Hello there").find(.wave) != null);
    try std.testing.expect(analyzeText("  Hello there").find(.wave) == null);
    try std.testing.expect(analyzeText("No. Hello there").find(.wave) == null);
}

test "duplicate aliases merge exactly like CEmotionOpts" {
    var analysis: TextAnalysis = .{};
    analysis.add(.surprised, 100, 3);
    analysis.add(.scared, 255, 10);
    analysis.add(.scared, 50, 2);
    try std.testing.expectEqual(@as(u8, 1), analysis.len);
    try std.testing.expectEqual(@as(u8, 255), analysis.slice()[0].intensity);
    try std.testing.expectEqual(@as(u8, 10), analysis.slice()[0].priority);
}
