//! End-to-end mode acceptance for Microsoft's MIT-licensed Comic Chat 2.5
//! rendering behavior at commit `c7df00f60bc8e9fdef413f139e61f7c37e024684`.
//!
//! Source anchors:
//! - `panel.cpp:1036-1053`: SAY/WHISPER/THINK/ACTION construction and the
//!   normal-balloon fallback for unknown modes.
//! - `panel.cpp:1064-1074`: exact ACTION starts a fresh panel and `<Chr>`
//!   dispatches to a reaction.
//! - `panel.cpp:1142-1185`: reactions replace/fetch a body without a balloon.
//! - `fonts.cpp:72-92` and `balloon.cpp:1779-1816`: whisper italic font,
//!   Woodring nimbus, and dashed trajectory.
//! - `balloon.cpp:1820-1868`: thought cloud and bubble chain.
//! - `balloon.cpp:1871-1907`: rectangular action trajectory.
//!
//! Imported by `src/root.zig`; run with `zig build test`.

const std = @import("std");
const strip = @import("strip.zig");
const page = @import("original_page.zig");
const canvas = @import("../render/canvas.zig");
const black = canvas.black;
const white = canvas.white;

fn pixel(image: strip.Image, x: u32, y: u32) u32 {
    return image.pixels[@as(usize, y) * image.width + x];
}

/// Explicit little-endian FNV-1a over ARGB words. This is intentionally not a
/// library hash whose implementation could change independently of the pixels.
fn pixelHash(pixels: []const u32) u64 {
    var hash: u64 = 0xcbf29ce484222325;
    for (pixels) |argb| {
        inline for (0..4) |byte_index| {
            const byte: u8 = @truncate(argb >> @as(u5, @intCast(byte_index * 8)));
            hash = (hash ^ byte) *% 0x100000001b3;
        }
    }
    return hash;
}

fn countColor(image: strip.Image, x0: u32, y0: u32, x1: u32, y1: u32, color: u32) usize {
    var count: usize = 0;
    var y = y0;
    while (y < y1) : (y += 1) {
        var x = x0;
        while (x < x1) : (x += 1) {
            if (pixel(image, x, y) == color) count += 1;
        }
    }
    return count;
}

test "BM_WHISPER renders the dashed Woodring trajectory" {
    const gpa = std.testing.allocator;
    var say = try strip.render(gpa, &.{.{
        .speaker = "anna",
        .text = "Mode fixture.",
        .modes = page.bm_say,
    }});
    defer say.deinit(gpa);
    var whisper = try strip.render(gpa, &.{.{
        .speaker = "anna",
        .text = "Mode fixture.",
        .modes = page.bm_whisper,
    }});
    defer whisper.deinit(gpa);

    // Pixel lock includes the generated Bold Italic text selected exclusively
    // by CBWoodringWhisper in the Microsoft source.
    try std.testing.expectEqual(@as(u64, 0x5aaa64fe213ff6cd), pixelHash(whisper.pixels));
    // The curved Woodring edge retains a black dash at the first point, while
    // the second exposes the white 100-TWIP nimbus in a dash gap.
    try std.testing.expectEqual(black, pixel(say, 382, 12));
    try std.testing.expectEqual(black, pixel(whisper, 382, 12));
    try std.testing.expectEqual(black, pixel(say, 409, 20));
    try std.testing.expectEqual(white, pixel(whisper, 409, 20));
}

test "BM_THINK renders thought bubbles on top of the same pointed tail as BM_SAY" {
    const gpa = std.testing.allocator;
    var say = try strip.render(gpa, &.{.{
        .speaker = "anna",
        .text = "Mode fixture.",
        .modes = page.bm_say,
    }});
    defer say.deinit(gpa);
    var think = try strip.render(gpa, &.{.{
        .speaker = "anna",
        .text = "Mode fixture.",
        .modes = page.bm_think,
    }});
    defer think.deinit(gpa);

    try std.testing.expectEqual(@as(u64, 0x47917f3e92ef5cea), pixelHash(think.pixels));
    // A fixed bubble has a black ellipse edge and white interior.
    try std.testing.expectEqual(black, pixel(think, 487, 112));
    try std.testing.expectEqual(white, pixel(think, 498, 112));
    // CBWoodringThink overrides only Draw, not SetBalloonTraj
    // (balloon.cpp:1820-1868), so it inherits the exact same AddArrow
    // pointed-tail geometry as a normal say balloon and additively overlays
    // the bubble chain on top ("will draw the cloud properly",
    // balloon.cpp:1828) -- the tail edge here is identical for both kinds.
    try std.testing.expectEqual(black, pixel(say, 508, 91));
    try std.testing.expectEqual(black, pixel(think, 508, 91));
}

test "exact BM_ACTION starts a fresh conversation panel" {
    const gpa = std.testing.allocator;
    var image = try strip.render(gpa, &.{
        .{ .speaker = "anna", .text = "First.", .modes = page.bm_say },
        .{ .speaker = "kevin", .text = "Action.", .modes = page.bm_action },
    });
    defer image.deinit(gpa);

    const second_row = strip.panel_height + strip.device_interstice;
    try std.testing.expectEqual(@as(u32, 650), image.width);
    try std.testing.expectEqual(@as(u32, 650), image.height);
    try std.testing.expectEqual(black, pixel(image, 0, second_row));
    try std.testing.expectEqual(@as(u64, 0xb96fd660fe16b995), pixelHash(image.pixels));
}

test "BM_ACTION|BM_WHISPER renders a dashed box without the exact-action break" {
    const gpa = std.testing.allocator;
    var action = try strip.render(gpa, &.{.{
        .speaker = "anna",
        .text = "Mode fixture.",
        .modes = page.bm_action,
    }});
    defer action.deinit(gpa);
    var dashed_box = try strip.render(gpa, &.{.{
        .speaker = "anna",
        .text = "Mode fixture.",
        .modes = page.bm_action | page.bm_whisper,
    }});
    defer dashed_box.deinit(gpa);

    // This hash intentionally stays on the normal Bold atlas: the combined
    // mode constructs CBWoodringBox, not CBWoodringWhisper.
    try std.testing.expectEqual(@as(u64, 0x23e847a0cc76f9c9), pixelHash(dashed_box.pixels));
    // The same straight right box edge alternates nimbus gap then black dash.
    try std.testing.expectEqual(black, pixel(action, 605, 17));
    try std.testing.expectEqual(white, pixel(dashed_box, 605, 17));
    try std.testing.expectEqual(black, pixel(action, 605, 20));
    try std.testing.expectEqual(black, pixel(dashed_box, 605, 20));

    // AddLine tests equality with BM_ACTION, not a bit test. The combined mode
    // therefore clones the existing conversation instead of starting a row.
    var combined_page = try strip.render(gpa, &.{
        .{ .speaker = "anna", .text = "First.", .modes = page.bm_say },
        .{ .speaker = "kevin", .text = "Action.", .modes = page.bm_action | page.bm_whisper },
    });
    defer combined_page.deinit(gpa);
    try std.testing.expectEqual(@as(u32, 650), combined_page.width);
    try std.testing.expectEqual(@as(u32, 315), combined_page.height);
    try std.testing.expectEqual(@as(u64, 0x34c9e73c353eacf8), pixelHash(combined_page.pixels));
}

test "unknown mode falls back to a normal speech balloon" {
    const gpa = std.testing.allocator;
    var say = try strip.render(gpa, &.{.{
        .speaker = "anna",
        .text = "Mode fixture.",
        .modes = page.bm_say,
    }});
    defer say.deinit(gpa);
    var unknown = try strip.render(gpa, &.{.{
        .speaker = "anna",
        .text = "Mode fixture.",
        .modes = 0x8000,
    }});
    defer unknown.deinit(gpa);

    try std.testing.expectEqual(@as(u64, 0xa163cc26dc400217), pixelHash(say.pixels));
    try std.testing.expectEqual(say.width, unknown.width);
    try std.testing.expectEqual(say.height, unknown.height);
    try std.testing.expectEqualSlices(u32, say.pixels, unknown.pixels);
}

test "Chr reaction renders a body with no balloon element" {
    const gpa = std.testing.allocator;
    var reaction = try strip.render(gpa, &.{.{
        .speaker = "anna",
        .text = "<Chr>",
        .modes = page.bm_say,
    }});
    defer reaction.deinit(gpa);

    try std.testing.expectEqual(@as(u32, 650), reaction.width);
    try std.testing.expectEqual(@as(u32, 315), reaction.height);
    try std.testing.expectEqual(@as(u64, 0xb17e3199b6b60ce9), pixelHash(reaction.pixels));
    // The empty upper panel retains the field where a normal Woodring cloud
    // would paint its black edge and white interior.
    try std.testing.expectEqual(white, pixel(reaction, 408, 20));
    try std.testing.expectEqual(white, pixel(reaction, 640, 25));
    try std.testing.expectEqual(
        @as(usize, 8155),
        countColor(reaction, 343, 8, 642, 150, white),
    );
}
