//! comicchat — pure-Zig Microsoft Comic Chat reimplementation (CLI entry).
//!
//! Subcommands:
//!   (none)                              headless codec demo
//!   connect <host> <port> <nick> <chan> connect to an IRC server, join, speak
//!
//! The GUI (hand-rolled windowing + software rasterizer) is not wired up yet.

const std = @import("std");
const cc = @import("comicchat");

/// Unbuffered stderr log (so progress is visible even if the process is
/// killed mid-block). Used by the live `connect` path.
fn elog(comptime fmt: []const u8, args: anytype) void {
    var buf: [1024]u8 = undefined;
    const s = std.fmt.bufPrint(&buf, fmt, args) catch return;
    _ = std.os.linux.write(2, s.ptr, s.len); // unbuffered; Linux-only diagnostic
}

pub fn main(init: std.process.Init.Minimal) !void {
    const gpa = std.heap.page_allocator;

    // Collect argv (Zig 0.16 delivers args via the Init parameter).
    var it = init.args.iterate();
    defer it.deinit();
    _ = it.skip(); // program name
    var argv: [8][]const u8 = undefined;
    var argc: usize = 0;
    while (it.next()) |a| : (argc += 1) {
        if (argc >= argv.len) break;
        argv[argc] = a;
    }

    if (argc >= 1 and std.mem.eql(u8, argv[0], "render-bg")) {
        try runRenderBg(gpa, if (argc >= 2) argv[1] else "field");
        return;
    }

    if (argc >= 1 and std.mem.eql(u8, argv[0], "render-panel")) {
        const bg = if (argc >= 2) argv[1] else "field";
        const speaker = if (argc >= 3) argv[2] else "ANNA";
        const text = if (argc >= 4) argv[3] else "Hello from a pure-Zig Comic Chat panel!";
        try runRenderPanel(gpa, bg, speaker, text);
        return;
    }

    if (argc >= 1 and std.mem.eql(u8, argv[0], "render-figure")) {
        try runRenderFigure(gpa, if (argc >= 2) argv[1] else "anna");
        return;
    }

    if (argc >= 1 and std.mem.eql(u8, argv[0], "connect")) {
        if (argc < 5) {
            std.debug.print("usage: comicchat connect <host> <port> <nick> <#channel>\n", .{});
            return;
        }
        const port = std.fmt.parseInt(u16, argv[2], 10) catch {
            std.debug.print("bad port: {s}\n", .{argv[2]});
            return;
        };
        try runConnect(gpa, argv[1], port, argv[3], argv[4]);
        return;
    }

    try runCodecDemo(gpa);
}

fn runCodecDemo(gpa: std.mem.Allocator) !void {
    std.debug.print("comicchat 0.0.0 — pure-Zig Comic Chat (core demo)\n\n", .{});

    const record = cc.proto.record;
    var doc: std.ArrayList(u8) = .empty;
    defer doc.deinit(gpa);

    try record.writeRecord(&doc, gpa, "#CHATCONVERSATION", &.{});
    try record.writeRecord(&doc, gpa, "IRCCHANNEL:", &.{"#comics"});
    try record.writeComicchar(&doc, gpa, "Anna", "eNplkk_demo_state");
    try record.writeRecord(&doc, gpa, "Text", &.{ "Anna", "Hi from pure Zig!" });

    std.debug.print("--- encoded transcript ---\n{s}\n", .{doc.items});
    std.debug.print("--- decoded records ---\n", .{});
    var it = record.DocumentIterator.init(doc.items);
    while (it.next()) |rec| {
        std.debug.print("  {s}", .{@tagName(rec.type)});
        var i: usize = 0;
        while (i < rec.field_count) : (i += 1) std.debug.print(" | {s}", .{rec.fields[i]});
        std.debug.print("\n", .{});
    }
}

fn writeAllFd(fd: i32, bytes: []const u8) void {
    var off: usize = 0;
    while (off < bytes.len) {
        const n = std.os.linux.write(fd, bytes[off..].ptr, bytes.len - off);
        if (n == 0 or n > bytes.len) break; // 0 = EOF; huge = -errno
        off += n;
    }
}

fn avatarByName(name: []const u8) ?[]const u8 {
    const eql = std.ascii.eqlIgnoreCase;
    if (eql(name, "anna")) return @embedFile("assets/testdata/anna.avb");
    if (eql(name, "armando")) return @embedFile("assets/testdata/armando.avb");
    if (eql(name, "bolo")) return @embedFile("assets/testdata/bolo.avb");
    if (eql(name, "cro")) return @embedFile("assets/testdata/cro.avb");
    if (eql(name, "dan")) return @embedFile("assets/testdata/dan.avb");
    if (eql(name, "denise")) return @embedFile("assets/testdata/denise.avb");
    if (eql(name, "hugh")) return @embedFile("assets/testdata/hugh.avb");
    if (eql(name, "jordan")) return @embedFile("assets/testdata/jordan.avb");
    if (eql(name, "kevin")) return @embedFile("assets/testdata/kevin.avb");
    if (eql(name, "kwensa")) return @embedFile("assets/testdata/kwensa.avb");
    if (eql(name, "lance")) return @embedFile("assets/testdata/lance.avb");
    if (eql(name, "lynnea")) return @embedFile("assets/testdata/lynnea.avb");
    if (eql(name, "margaret")) return @embedFile("assets/testdata/margaret.avb");
    if (eql(name, "maynard")) return @embedFile("assets/testdata/maynard.avb");
    if (eql(name, "mike")) return @embedFile("assets/testdata/mike.avb");
    if (eql(name, "rebecca")) return @embedFile("assets/testdata/rebecca.avb");
    if (eql(name, "sage")) return @embedFile("assets/testdata/sage.avb");
    if (eql(name, "scotty")) return @embedFile("assets/testdata/scotty.avb");
    if (eql(name, "susan")) return @embedFile("assets/testdata/susan.avb");
    if (eql(name, "tiki")) return @embedFile("assets/testdata/tiki.avb");
    if (eql(name, "tongtyed")) return @embedFile("assets/testdata/tongtyed.avb");
    if (eql(name, "xeno")) return @embedFile("assets/testdata/xeno.avb");
    return null;
}

fn bgByName(name: []const u8) ?[]const u8 {
    const eql = std.mem.eql;
    if (eql(u8, name, "field")) return @embedFile("assets/testdata/field.bgb");
    if (eql(u8, name, "volcano")) return @embedFile("assets/testdata/volcano.bgb");
    if (eql(u8, name, "den")) return @embedFile("assets/testdata/den.bgb");
    if (eql(u8, name, "room")) return @embedFile("assets/testdata/room.bgb");
    if (eql(u8, name, "pastoral")) return @embedFile("assets/testdata/pastoral.bgb");
    return null;
}

/// Emit RGBA pixels (0xAARRGGBB, top-down) as a binary PPM (P6) on stdout.
fn emitPpm(gpa: std.mem.Allocator, pixels: []const u32, w: u32, h: u32) !void {
    var ppm: std.ArrayList(u8) = .empty;
    defer ppm.deinit(gpa);
    var hdr: [64]u8 = undefined;
    try ppm.appendSlice(gpa, try std.fmt.bufPrint(&hdr, "P6\n{d} {d}\n255\n", .{ w, h }));
    for (pixels) |px| {
        try ppm.append(gpa, @intCast((px >> 16) & 0xff));
        try ppm.append(gpa, @intCast((px >> 8) & 0xff));
        try ppm.append(gpa, @intCast(px & 0xff));
    }
    writeAllFd(1, ppm.items);
}

/// Decode a named embedded background and emit it as PPM on stdout.
fn runRenderBg(gpa: std.mem.Allocator, name: []const u8) !void {
    const data = bgByName(name) orelse {
        elog("unknown background '{s}' (field|volcano|den|room|pastoral)\n", .{name});
        return;
    };
    var img = try cc.assets.bgb.decodeBackground(gpa, data);
    defer img.deinit(gpa);
    try emitPpm(gpa, img.pixels, img.width, img.height);
}

/// Compose a comic panel: background + a speaker placeholder + a speech
/// balloon with wrapped text, and emit it as PPM on stdout.
fn runRenderPanel(gpa: std.mem.Allocator, bg: []const u8, speaker: []const u8, text: []const u8) !void {
    const data = bgByName(bg) orelse {
        elog("unknown background '{s}'\n", .{bg});
        return;
    };
    var img = try cc.assets.bgb.decodeBackground(gpa, data);
    defer img.deinit(gpa);

    const Canvas = cc.render.canvas.Canvas;
    var c = try Canvas.init(gpa, img.width, img.height);
    defer c.deinit(gpa);

    const black = cc.render.canvas.black;
    const white = cc.render.canvas.white;
    const W: i32 = @intCast(img.width);

    // Background.
    c.blit(img.pixels, img.width, img.height, 0, 0);

    // Speech balloon sized to fit the wrapped text.
    const bx: i32 = 18;
    const by: i32 = 14;
    const bw: i32 = W - 36;
    const pad: i32 = 14;
    const text_w = bw - 2 * pad;
    const bh = cc.render.canvas.Canvas.wrappedHeight(text, text_w) + 2 * pad;

    // Character: the avatar's real decoded expression pose (2bpp grayscale,
    // transparent background), composited bottom-left over the scene.
    var head_x: i32 = 70;
    var head_y: i32 = by + bh + 20;

    var drew_pose = false;
    if (avatarByName(speaker)) |avb| {
        if (cc.assets.bgb.decodePoseAuto(gpa, avb, 0, false)) |pose_const| {
            var pose = pose_const;
            defer pose.deinit(gpa);
            const pw: i32 = @intCast(pose.width);
            const ph: i32 = @intCast(pose.height);
            const px: i32 = 6;
            const py: i32 = @as(i32, @intCast(img.height)) - ph - 4;
            // composite with a 12px right-edge crop (trailing strip cleanup)
            composite(&c, pose.pixels, pose.width, pose.height, px, py, 12);
            head_x = px + @divTrunc(pw, 2);
            head_y = py + 6;
            // name tag
            const nw = cc.render.canvas.Canvas.textWidth(speaker) + 12;
            c.fillRect(px + 4, py - 4, nw, 22, white);
            c.fillRect(px + 4, py - 4, nw, 2, black);
            _ = c.drawText(speaker, px + 10, py - 2, black);
            drew_pose = true;
        } else |_| {}
    }
    if (!drew_pose) {
        const cx: i32 = 36;
        const cy: i32 = by + bh + 26;
        c.fillRect(cx, cy, 100, 100, 0xfff4f4f4);
        _ = c.drawText(speaker, cx + 8, cy + 8, black);
        head_x = cx + 50;
        head_y = cy;
    }

    // Balloon body + tail toward the character's head, then the text.
    c.speechBalloon(bx, by, bw, bh, head_x, head_y);
    _ = c.drawTextWrapped(text, bx + pad, by + pad, text_w, black);

    try emitPpm(gpa, c.px, c.width, c.height);
}

/// Composite an avatar's head + body layers into the full standing figure and
/// emit it as PPM on stdout. (Comic Chat stores head expressions and body
/// gestures separately — the "emotion wheel" — and composites at the neck.)
/// Render a single complete pose centered on white (for creature/totem avatars
/// that have no head/body split).
fn renderSolo(gpa: std.mem.Allocator, img: cc.assets.bgb.Image) !void {
    const pad: i32 = 10;
    const W: u32 = img.width + 2 * @as(u32, @intCast(pad));
    const H: u32 = img.height + 2 * @as(u32, @intCast(pad));
    var cf = try cc.render.canvas.Canvas.init(gpa, W, H);
    defer cf.deinit(gpa);
    cf.clear(cc.render.canvas.white);
    composite(&cf, img.pixels, img.width, img.height, pad, pad, 0);
    try emitPpm(gpa, cf.px, W, H);
}

fn runRenderFigure(gpa: std.mem.Allocator, name: []const u8) !void {
    const avb = avatarByName(name) orelse {
        elog("unknown avatar '{s}'\n", .{name});
        return;
    };
    var fig = cc.comic.figure.assemble(gpa, avb, 0, 0) catch {
        elog("could not assemble figure for '{s}'\n", .{name});
        return;
    };
    defer fig.deinit(gpa);

    const pad: i32 = 10;
    const W: u32 = fig.width + 2 * @as(u32, @intCast(pad));
    const H: u32 = fig.height + 2 * @as(u32, @intCast(pad));
    var c = try cc.render.canvas.Canvas.init(gpa, W, H);
    defer c.deinit(gpa);
    c.clear(cc.render.canvas.white);
    cc.comic.figure.composite(&c, fig.pixels, fig.width, fig.height, pad, pad, 0);
    try emitPpm(gpa, c.px, W, H);
}

fn rowWidth(img: cc.assets.bgb.Image, y: i32) i32 {
    if (y < 0 or y >= img.height) return 0;
    const row = @as(usize, @intCast(y)) * img.width;
    var n: i32 = 0;
    var x: u32 = 0;
    while (x < img.width) : (x += 1) {
        if (img.pixels[row + x] >> 24 != 0) n += 1;
    }
    return n;
}

/// First row (top→down) where the figure flares wider than its neck — the
/// shoulder line. The head's bottom is seated here.
fn shoulderRow(img: cc.assets.bgb.Image) i32 {
    const t = topInkRow(img);
    var neck: i32 = std.math.maxInt(i32);
    var y: i32 = t;
    while (y < t + 40 and y < img.height) : (y += 1) {
        const w = rowWidth(img, y);
        if (w > 0 and w < neck) neck = w;
    }
    if (neck == std.math.maxInt(i32)) neck = 1;
    const threshold = @max(@divTrunc(neck * 17, 10), neck + 18);
    y = t;
    while (y < img.height) : (y += 1) {
        if (rowWidth(img, y) >= threshold) return y;
    }
    return t + 20;
}

fn topInkRow(img: cc.assets.bgb.Image) i32 {
    var y: u32 = 0;
    while (y < img.height) : (y += 1) {
        var x: u32 = 0;
        while (x < img.width) : (x += 1) if (img.pixels[y * img.width + x] >> 24 != 0) return @intCast(y);
    }
    return 0;
}

/// Lowest row with ink within ±18px of the neck column `nx` — the base of the
/// neck, ignoring hair/ornaments that hang lower on the sides.
fn headNeckBottom(img: cc.assets.bgb.Image, nx: i32) i32 {
    const x0: u32 = @intCast(@max(@as(i32, 0), nx - 18));
    const x1: u32 = @intCast(@min(@as(i32, @intCast(img.width)), nx + 18));
    var y: i32 = @as(i32, @intCast(img.height)) - 1;
    while (y >= 0) : (y -= 1) {
        const row = @as(usize, @intCast(y)) * img.width;
        var x: u32 = x0;
        while (x < x1) : (x += 1) if (img.pixels[row + x] >> 24 != 0) return y;
    }
    return @as(i32, @intCast(img.height)) - 1;
}

fn botInkRow(img: cc.assets.bgb.Image) i32 {
    var y: i32 = @as(i32, @intCast(img.height)) - 1;
    while (y >= 0) : (y -= 1) {
        const row = @as(usize, @intCast(y)) * img.width;
        var x: u32 = 0;
        while (x < img.width) : (x += 1) if (img.pixels[row + x] >> 24 != 0) return y;
    }
    return @as(i32, @intCast(img.height)) - 1;
}

/// Horizontal centroid of opaque pixels over rows [y0, y1).
fn centroidX(img: cc.assets.bgb.Image, y0: i32, y1: i32) i32 {
    var sum: i64 = 0;
    var cnt: i64 = 0;
    var y: i32 = @max(0, y0);
    const ye: i32 = @min(@as(i32, @intCast(img.height)), y1);
    while (y < ye) : (y += 1) {
        const row = @as(usize, @intCast(y)) * img.width;
        var x: u32 = 0;
        while (x < img.width) : (x += 1) {
            if (img.pixels[row + x] >> 24 != 0) {
                sum += x;
                cnt += 1;
            }
        }
    }
    if (cnt == 0) return @intCast(img.width / 2);
    return @intCast(@divTrunc(sum, cnt));
}

/// Composite a transparent-keyed pose image, skipping the right `crop_r`
/// columns. Black ink always wins: a white pixel never paints over existing
/// black, so an upper layer's white "sticker" can't erase the lower layer's
/// linework (e.g. the body's collar/neck lines under the head).
fn composite(c: *cc.render.canvas.Canvas, src: []const u32, sw: u32, sh: u32, dx: i32, dy: i32, crop_r: u32) void {
    var y: u32 = 0;
    while (y < sh) : (y += 1) {
        var x: u32 = 0;
        while (x + crop_r < sw) : (x += 1) {
            const p = src[y * sw + x];
            if (p >> 24 == 0) continue; // transparent
            const ox = dx + @as(i32, @intCast(x));
            const oy = dy + @as(i32, @intCast(y));
            if (ox < 0 or oy < 0 or ox >= c.width or oy >= c.height) continue;
            const di = @as(usize, @intCast(oy)) * c.width + @as(usize, @intCast(ox));
            c.px[di] = p; // upper layer occludes (head drawn on top of body)
        }
    }
}

fn runConnect(
    gpa: std.mem.Allocator,
    host: []const u8,
    port: u16,
    nick: []const u8,
    channel: []const u8,
) !void {
    elog("connecting to {s}:{d} as {s} ...\n", .{ host, port, nick });

    var client = try cc.net.client.Client.connect(gpa, host, port);
    defer client.deinit();

    try client.register(nick, nick, "pure-Zig Comic Chat", true);

    var registered = false;
    var joined = false;
    var post: usize = 0;
    var seen: usize = 0;

    while (seen < 80) : (seen += 1) {
        const msg = (try client.next()) orelse {
            elog("<eof>\n", .{});
            break;
        };

        elog("<- {s}", .{msg.command});
        var i: usize = 0;
        while (i < msg.param_count) : (i += 1) elog(" [{s}]", .{msg.params[i]});
        elog("\n", .{});

        if (!registered and std.mem.eql(u8, msg.command, "001")) {
            registered = true;
            elog("** registered; joining {s}\n", .{channel});
            try client.join(channel);
        } else if (registered and !joined and
            (std.mem.eql(u8, msg.command, "366") or std.mem.eql(u8, msg.command, "JOIN")))
        {
            joined = true;
            elog("** joined; sending a line\n", .{});
            try client.privmsg(channel, "Hello from pure-Zig Comic Chat!");
        }

        if (joined) {
            post += 1;
            if (post >= 3) break;
        }
    }
    elog("done.\n", .{});
}
