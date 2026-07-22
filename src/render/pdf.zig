//! Dependency-free raster PDF export used by print preview and portable
//! printing. The framebuffer is embedded as one lossless RGB image, keeping
//! authored ComicChat art and geometry byte-exact without a C print stack.

const std = @import("std");

pub const max_pixels: usize = 32 * 1024 * 1024;

pub fn encode(gpa: std.mem.Allocator, pixels: []const u32, width: u32, height: u32) ![]u8 {
    if (width == 0 or height == 0) return error.InvalidDimensions;
    const count = try std.math.mul(usize, width, height);
    if (count > max_pixels or pixels.len != count) return error.InvalidFramebuffer;

    var out: std.ArrayList(u8) = .empty;
    errdefer out.deinit(gpa);
    try out.appendSlice(gpa, "%PDF-1.4\n% ComicChat portable print\n");

    var offsets: [6]usize = @splat(0);
    offsets[1] = out.items.len;
    try out.appendSlice(gpa, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    offsets[2] = out.items.len;
    try out.appendSlice(gpa, "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n");

    offsets[3] = out.items.len;
    try out.print(gpa, "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 {d} {d}] /Resources << /XObject << /Im0 4 0 R >> >> /Contents 5 0 R >>\nendobj\n", .{ width, height });

    const rgb_len = try std.math.mul(usize, count, 3);
    offsets[4] = out.items.len;
    try out.print(gpa, "4 0 obj\n<< /Type /XObject /Subtype /Image /Width {d} /Height {d} /ColorSpace /DeviceRGB /BitsPerComponent 8 /Length {d} >>\nstream\n", .{ width, height, rgb_len });
    try out.ensureUnusedCapacity(gpa, rgb_len);
    for (pixels) |pixel| {
        out.appendAssumeCapacity(@truncate(pixel >> 16));
        out.appendAssumeCapacity(@truncate(pixel >> 8));
        out.appendAssumeCapacity(@truncate(pixel));
    }
    try out.appendSlice(gpa, "\nendstream\nendobj\n");

    var content: [96]u8 = undefined;
    const commands = try std.fmt.bufPrint(&content, "q\n{d} 0 0 {d} 0 0 cm\n/Im0 Do\nQ\n", .{ width, height });
    offsets[5] = out.items.len;
    try out.print(gpa, "5 0 obj\n<< /Length {d} >>\nstream\n{s}endstream\nendobj\n", .{ commands.len, commands });

    const xref = out.items.len;
    try out.appendSlice(gpa, "xref\n0 6\n0000000000 65535 f \n");
    for (offsets[1..]) |offset| try out.print(gpa, "{d:0>10} 00000 n \n", .{offset});
    try out.print(gpa, "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n{d}\n%%EOF\n", .{xref});
    return out.toOwnedSlice(gpa);
}

test "PDF export owns a complete one-page RGB document" {
    const pixels = [_]u32{ 0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff };
    const document = try encode(std.testing.allocator, &pixels, 2, 2);
    defer std.testing.allocator.free(document);
    try std.testing.expect(std.mem.startsWith(u8, document, "%PDF-1.4"));
    try std.testing.expect(std.mem.indexOf(u8, document, "/Width 2 /Height 2") != null);
    try std.testing.expect(std.mem.endsWith(u8, document, "%%EOF\n"));
    try std.testing.expect(std.mem.indexOf(u8, document, "\xff\x00\x00\x00\xff\x00\x00\x00\xff") != null);
}

test "PDF export rejects mismatched framebuffers" {
    try std.testing.expectError(error.InvalidFramebuffer, encode(std.testing.allocator, &.{0}, 2, 2));
    try std.testing.expectError(error.InvalidDimensions, encode(std.testing.allocator, &.{}, 0, 1));
}
