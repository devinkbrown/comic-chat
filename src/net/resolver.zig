// SPDX-FileCopyrightText: 2026 Onyx contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

//! Cross-platform transport for Onyx Server's strict DNS wire resolver.
//!
//! The DNS codec, response validation, and resolv.conf parser come directly
//! from the pinned Onyx Server source. This adapter supplies std.Io UDP on all
//! supported ComicChat targets and Windows DNS-server discovery through the
//! documented IP Helper API.

const std = @import("std");
const builtin = @import("builtin");
const net = std.Io.net;
const ConnectedSocket = @import("socket.zig").ConnectedSocket;
const onyx = @import("onyx_tls");
const dns = onyx.proto.dns;
const resolv_conf = onyx.proto.resolv_conf;

pub const max_results: usize = dns.max_cache_addrs * 2;
const max_nameservers = resolv_conf.max_nameservers;

const Config = struct {
    nameservers: [max_nameservers]dns.Address = undefined,
    count: usize = 0,
    attempts: u8 = resolv_conf.default_attempts,

    fn append(self: *Config, address: dns.Address) void {
        if (self.count >= self.nameservers.len) return;
        self.nameservers[self.count] = address;
        self.count += 1;
    }

    fn slice(self: *const Config) []const dns.Address {
        return self.nameservers[0..self.count];
    }
};

pub fn resolve(
    io: std.Io,
    host: []const u8,
    port: u16,
    timeout_ms: u32,
    out: []net.IpAddress,
) ![]net.IpAddress {
    if (out.len == 0) return error.NoAddressSpace;
    if (net.IpAddress.parse(host, port)) |address| {
        out[0] = address;
        return out[0..1];
    } else |_| {}

    var config = try systemConfig(io);
    if (config.count == 0) return error.NoNameservers;

    var count: usize = 0;
    count += resolveType(io, &config, host, port, .a, timeout_ms, out[count..]) catch 0;
    if (count < out.len) {
        count += resolveType(io, &config, host, port, .aaaa, timeout_ms, out[count..]) catch 0;
    }
    // Wine may expose no DNS servers through GetNetworkParams while its DNS
    // service remains fully functional. Restricted Windows networks can also
    // block direct UDP/53. Preserve the Onyx wire resolver as the primary path
    // and ask the Windows DNS service only after that path is exhausted.
    if (count == 0 and comptime builtin.os.tag == .windows) {
        count = try resolveWindowsSystem(host, port, out);
    }
    if (count == 0) return error.HostNotFound;
    return out[0..count];
}

fn systemConfig(io: std.Io) !Config {
    var config = Config{};
    if (comptime builtin.os.tag == .windows) {
        loadWindowsNameservers(&config);
    } else {
        const text = std.Io.Dir.cwd().readFileAlloc(io, "/etc/resolv.conf", std.heap.page_allocator, .limited(64 * 1024)) catch null;
        if (text) |contents| {
            defer std.heap.page_allocator.free(contents);
            const parsed = resolv_conf.parse(contents);
            config.attempts = parsed.attempts;
            for (parsed.nameserverSlice()) |server| config.append(server);
        }
    }
    // Match Onyx Server's fail-safe resolver policy when the system did not
    // expose a usable nameserver.
    if (config.count == 0) {
        config.append(.{ .ipv4 = .{ 1, 1, 1, 1 } });
        config.append(.{ .ipv4 = .{ 8, 8, 8, 8 } });
    }
    return config;
}

fn resolveType(
    io: std.Io,
    config: *const Config,
    host: []const u8,
    port: u16,
    record_type: dns.RecordType,
    timeout_ms: u32,
    out: []net.IpAddress,
) !usize {
    var id_bytes: [2]u8 = undefined;
    try onyx.substrate.platform.fillOsEntropy(&id_bytes);
    const query_id = std.mem.readInt(u16, &id_bytes, .big);
    var query_buffer: [dns.max_message_len]u8 = undefined;
    const query = try dns.encodeQuery(&query_buffer, query_id, host, record_type);

    var attempt: u8 = 0;
    while (attempt < @max(config.attempts, 1)) : (attempt += 1) {
        for (config.slice()) |server| {
            var response_buffer: [dns.max_message_len]u8 = undefined;
            const packet = exchange(io, server, query, timeout_ms, &response_buffer) catch continue;
            const message = dns.parseMessage(1, dns.max_cache_addrs, packet) catch continue;
            if (message.header.id != query_id) continue;
            if (!dns.responseMatchesQuestion(1, dns.max_cache_addrs, &message, host, record_type)) continue;
            if (message.header.rcode() != 0) continue;

            var count: usize = 0;
            for (message.answerSlice()) |answer| {
                if (count >= out.len) break;
                switch (answer.data) {
                    .a => |bytes| if (record_type == .a) {
                        out[count] = .{ .ip4 = .{ .bytes = bytes, .port = port } };
                        count += 1;
                    },
                    .aaaa => |bytes| if (record_type == .aaaa) {
                        out[count] = .{ .ip6 = .{ .bytes = bytes, .port = port } };
                        count += 1;
                    },
                    else => {},
                }
            }
            if (count != 0) return count;
        }
    }
    return error.HostNotFound;
}

fn exchange(
    io: std.Io,
    server: dns.Address,
    query: []const u8,
    timeout_ms: u32,
    response_buffer: []u8,
) ![]const u8 {
    const destination: net.IpAddress = switch (server) {
        .ipv4 => |bytes| .{ .ip4 = .{ .bytes = bytes, .port = 53 } },
        .ipv6 => |bytes| .{ .ip6 = .{ .bytes = bytes, .port = 53 } },
    };
    const socket = try ConnectedSocket.connect(io, destination, .dgram, timeout_ms);
    defer socket.close();
    try socket.sendAll(query);
    const received = (try socket.recvTimeout(response_buffer, timeout_ms)) orelse return error.Timeout;
    return response_buffer[0..received];
}

const WindowsIpAddressString = extern struct {
    string: [16]u8,
};

const WindowsIpAddrString = extern struct {
    next: ?*WindowsIpAddrString,
    ip_address: WindowsIpAddressString,
    ip_mask: WindowsIpAddressString,
    context: u32,
};

const WindowsFixedInfo = extern struct {
    host_name: [132]u8,
    domain_name: [132]u8,
    current_dns_server: ?*WindowsIpAddrString,
    dns_server_list: WindowsIpAddrString,
    node_type: u32,
    scope_id: [260]u8,
    enable_routing: u32,
    enable_proxy: u32,
    enable_dns: u32,
};

extern "iphlpapi" fn GetNetworkParams(info: *WindowsFixedInfo, size: *u32) callconv(.winapi) u32;

const WindowsDnsRecord = extern struct {
    next: ?*WindowsDnsRecord,
    name: ?[*:0]u8,
    record_type: u16,
    data_length: u16,
    flags: u32,
    ttl: u32,
    reserved: u32,
    data: [16]u8,
};

extern "dnsapi" fn DnsQuery_A(
    name: [*:0]const u8,
    record_type: u16,
    options: u32,
    servers: ?*const anyopaque,
    results: *?*WindowsDnsRecord,
    reserved: ?*?*anyopaque,
) callconv(.winapi) i32;
extern "dnsapi" fn DnsRecordListFree(record: ?*WindowsDnsRecord, free_type: i32) callconv(.winapi) void;

fn loadWindowsNameservers(config: *Config) void {
    var storage: [16 * 1024]u8 align(@alignOf(WindowsFixedInfo)) = undefined;
    var size: u32 = storage.len;
    const info: *WindowsFixedInfo = @ptrCast(&storage);
    if (GetNetworkParams(info, &size) != 0) return;

    var item: ?*WindowsIpAddrString = &info.dns_server_list;
    while (item) |entry| : (item = entry.next) {
        const end = std.mem.indexOfScalar(u8, &entry.ip_address.string, 0) orelse entry.ip_address.string.len;
        if (resolv_conf.parseIp(entry.ip_address.string[0..end])) |address| config.append(address);
    }
}

fn resolveWindowsSystem(host: []const u8, port: u16, out: []net.IpAddress) !usize {
    if (host.len == 0 or host.len > dns.max_domain_text_len) return error.HostNotFound;
    var name_buffer: [dns.max_domain_text_len + 1]u8 = undefined;
    @memcpy(name_buffer[0..host.len], host);
    name_buffer[host.len] = 0;

    var count: usize = 0;
    count += queryWindowsRecords(@ptrCast(&name_buffer), 1, port, out[count..]);
    if (count < out.len) count += queryWindowsRecords(@ptrCast(&name_buffer), 28, port, out[count..]);
    if (count == 0) return error.HostNotFound;
    return count;
}

fn queryWindowsRecords(name: [*:0]const u8, record_type: u16, port: u16, out: []net.IpAddress) usize {
    var records: ?*WindowsDnsRecord = null;
    const status = DnsQuery_A(name, record_type, 0, null, &records, null);
    if (status != 0) return 0;
    defer DnsRecordListFree(records, 1);

    var count: usize = 0;
    var item = records;
    while (item) |record| : (item = record.next) {
        if (count >= out.len) break;
        if (record.record_type == 1 and record.data_length == 4) {
            out[count] = .{ .ip4 = .{ .bytes = record.data[0..4].*, .port = port } };
            count += 1;
        } else if (record.record_type == 28 and record.data_length == 16) {
            out[count] = .{ .ip6 = .{ .bytes = record.data, .port = port } };
            count += 1;
        }
    }
    return count;
}

test "Onyx resolver rejects an empty result buffer" {
    var threaded = std.Io.Threaded.init(std.testing.allocator, .{});
    defer threaded.deinit();
    var empty: [0]net.IpAddress = .{};
    try std.testing.expectError(error.NoAddressSpace, resolve(threaded.io(), "example.com", 6697, 100, &empty));
}

test "Onyx resolver preserves numeric address literals" {
    var threaded = std.Io.Threaded.init(std.testing.allocator, .{});
    defer threaded.deinit();
    var addresses: [1]net.IpAddress = undefined;
    const result = try resolve(threaded.io(), "192.0.2.7", 6697, 100, &addresses);
    try std.testing.expectEqual(@as(usize, 1), result.len);
    try std.testing.expectEqual([4]u8{ 192, 0, 2, 7 }, result[0].ip4.bytes);
    try std.testing.expectEqual(@as(u16, 6697), result[0].ip4.port);
}
