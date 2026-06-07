//! TCP transport for the IRC engine, built on Zig 0.16's `std.Io` model.
//!
//! Pure Zig, no C: uses `std.Io.Threaded` as the backing runtime and
//! `std.Io.net` for the socket. This is the only piece that touches the
//! network; the protocol logic lives in `message.zig` / `irc.zig` and is
//! driven by feeding `recv` bytes into a `LineFramer`.
//!
//! The struct pins itself in memory (the `Io` vtable and the reader/writer
//! buffers reference it by pointer), so `connect` returns a heap pointer that
//! must not be moved. Call `deinit` to release it.

const std = @import("std");
const net = std.Io.net;

pub const Transport = struct {
    gpa: std.mem.Allocator,
    threaded: std.Io.Threaded,
    io: std.Io,
    stream: net.Stream,

    /// Resolve `host` and open a TCP stream to `host:port`. Returns a heap
    /// pointer: the `Io` vtable references `self.threaded`, so it must not move.
    pub fn connect(
        gpa: std.mem.Allocator,
        host: []const u8,
        port: u16,
    ) !*Transport {
        const self = try gpa.create(Transport);
        errdefer gpa.destroy(self);

        self.gpa = gpa;
        self.threaded = std.Io.Threaded.init(gpa, .{});
        errdefer self.threaded.deinit();
        self.io = self.threaded.io();

        const addr = try net.IpAddress.resolve(self.io, host, port);
        self.stream = try net.IpAddress.connect(&addr, self.io, .{ .mode = .stream });
        return self;
    }

    pub fn deinit(self: *Transport) void {
        self.stream.close(self.io);
        self.threaded.deinit();
        self.gpa.destroy(self);
    }

    /// Send all bytes (e.g. a CRLF-terminated command). Loops until every byte
    /// is written. Uses the Io vtable directly to avoid buffered-writer flush
    /// semantics.
    pub fn send(self: *Transport, bytes: []const u8) !void {
        const handle = self.stream.socket.handle;
        var off: usize = 0;
        while (off < bytes.len) {
            // header empty; the payload is the sole `data` buffer, written once
            // (splat = 1). netWrite requires a non-empty `data` slice.
            const n = try self.io.vtable.netWrite(
                self.io.userdata,
                handle,
                "",
                &[_][]const u8{bytes[off..]},
                1,
            );
            if (n == 0) return error.WriteZero;
            off += n;
        }
    }

    /// One read into `dst`; returns bytes read, or 0 at end of stream. Does NOT
    /// block waiting to fill `dst` (unlike Reader.readSliceShort).
    pub fn recv(self: *Transport, dst: []u8) !usize {
        var iov = [_][]u8{dst};
        return self.io.vtable.netRead(self.io.userdata, self.stream.socket.handle, iov[0..]);
    }
};
