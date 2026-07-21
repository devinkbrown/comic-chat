//! Pure IRCv3 CAP 302 negotiation state machine.
//!
//! Protocol reference: the current IRCv3 specification catalog plus the
//! capability surface advertised by the pinned Onyx Server submodule. This
//! module owns no transport. It consumes parsed IRC messages and appends
//! complete client commands to a caller-owned output buffer.

const std = @import("std");
const message = @import("message.zig");

pub const max_req_payload: usize = 450;
pub const max_capabilities: usize = 1024;
pub const max_capability_storage: usize = 256 * 1024;

pub const Error = std.mem.Allocator.Error || error{
    InvalidState,
    InvalidCapMessage,
    InvalidCapability,
    CapabilityRequestTooLong,
    CapabilityBackpressure,
};

pub const Capability = struct {
    name: []u8,
    value: ?[]u8,
};

/// An owning, case-sensitive capability map. CAP names and values are opaque;
/// their storage remains valid until the entry is updated, removed, or the
/// list is deinitialized.
pub const CapabilityList = struct {
    entries: std.ArrayList(Capability) = .empty,
    storage_bytes: usize = 0,

    pub fn deinit(self: *CapabilityList, gpa: std.mem.Allocator) void {
        for (self.entries.items) |entry| {
            gpa.free(entry.name);
            if (entry.value) |value| gpa.free(value);
        }
        self.entries.deinit(gpa);
        self.* = .{};
    }

    pub fn count(self: *const CapabilityList) usize {
        return self.entries.items.len;
    }

    pub fn contains(self: *const CapabilityList, name: []const u8) bool {
        return self.indexOf(name) != null;
    }

    pub fn get(self: *const CapabilityList, name: []const u8) ?*const Capability {
        const index = self.indexOf(name) orelse return null;
        return &self.entries.items[index];
    }

    fn indexOf(self: *const CapabilityList, name: []const u8) ?usize {
        for (self.entries.items, 0..) |entry, index| {
            if (std.mem.eql(u8, entry.name, name)) return index;
        }
        return null;
    }

    fn put(
        self: *CapabilityList,
        gpa: std.mem.Allocator,
        name: []const u8,
        value: ?[]const u8,
    ) Error!void {
        try validateCapabilityName(name, false);
        const value_len = if (value) |raw| raw.len else 0;
        const existing_index = self.indexOf(name);
        const old_value_len = if (existing_index) |index|
            if (self.entries.items[index].value) |old| old.len else 0
        else
            0;
        const next_storage = self.storage_bytes - old_value_len + value_len +
            (if (existing_index == null) name.len else 0);
        if (next_storage > max_capability_storage or
            (existing_index == null and self.entries.items.len >= max_capabilities))
            return error.CapabilityBackpressure;
        const value_copy = if (value) |raw| try gpa.dupe(u8, raw) else null;
        errdefer if (value_copy) |copy| gpa.free(copy);
        if (existing_index) |index| {
            if (self.entries.items[index].value) |old| gpa.free(old);
            self.entries.items[index].value = value_copy;
            self.storage_bytes = next_storage;
            return;
        }
        const name_copy = try gpa.dupe(u8, name);
        errdefer gpa.free(name_copy);
        try self.entries.append(gpa, .{ .name = name_copy, .value = value_copy });
        self.storage_bytes = next_storage;
    }

    fn remove(self: *CapabilityList, gpa: std.mem.Allocator, name: []const u8) bool {
        const index = self.indexOf(name) orelse return false;
        const removed = self.entries.swapRemove(index);
        self.storage_bytes -= removed.name.len + if (removed.value) |value| value.len else 0;
        gpa.free(removed.name);
        if (removed.value) |value| gpa.free(value);
        return true;
    }

    fn clone(self: *const CapabilityList, gpa: std.mem.Allocator) Error!CapabilityList {
        var result: CapabilityList = .{};
        errdefer result.deinit(gpa);
        for (self.entries.items) |entry| try result.put(gpa, entry.name, entry.value);
        return result;
    }

    /// Atomically merge an owning delta. Capacity and bounds are checked
    /// before any entry changes; successful calls transfer every string from
    /// `delta` without another allocation.
    fn mergeOwned(self: *CapabilityList, gpa: std.mem.Allocator, delta: *CapabilityList) Error!void {
        var next_storage = self.storage_bytes;
        var additions: usize = 0;
        for (delta.entries.items) |entry| {
            const value_len = if (entry.value) |value| value.len else 0;
            if (self.indexOf(entry.name)) |index| {
                const old_value_len = if (self.entries.items[index].value) |old| old.len else 0;
                next_storage = next_storage - old_value_len + value_len;
            } else {
                additions += 1;
                next_storage += entry.name.len + value_len;
            }
        }
        if (self.entries.items.len + additions > max_capabilities or next_storage > max_capability_storage)
            return error.CapabilityBackpressure;
        try self.entries.ensureUnusedCapacity(gpa, additions);

        for (delta.entries.items) |entry| {
            if (self.indexOf(entry.name)) |index| {
                gpa.free(entry.name);
                if (self.entries.items[index].value) |old| gpa.free(old);
                self.entries.items[index].value = entry.value;
            } else {
                self.entries.appendAssumeCapacity(entry);
            }
        }
        delta.entries.clearRetainingCapacity();
        delta.storage_bytes = 0;
        self.storage_bytes = next_storage;
    }
};

pub const CapabilitySpec = struct {
    name: []const u8,
    dependencies: []const []const u8 = &.{},
    /// STS is an advertised policy and MUST NOT be requested with CAP REQ.
    requestable: bool = true,
};

const deps_batch = [_][]const u8{"batch"};
const deps_event_playback = [_][]const u8{"draft/chathistory"};

/// Client-applicable IRCv3 capabilities plus the pinned Onyx extensions which
/// have concrete consumers in Client/State. Transport-only features such as
/// WebSocket and server/gateway features such as WebIRC are not CAP requests.
/// STS is policy, and E2EE is cataloged but deliberately not requested until a
/// real encrypt/decrypt implementation exists.
pub const published_client_capabilities = [_]CapabilitySpec{
    .{ .name = "account-notify" },
    .{ .name = "draft/account-registration" },
    .{ .name = "account-tag" },
    .{ .name = "away-notify" },
    .{ .name = "batch" },
    .{ .name = "cap-notify" },
    .{ .name = "draft/channel-rename" },
    .{ .name = "draft/chathistory" },
    .{ .name = "draft/search" },
    .{ .name = "draft/event-playback", .dependencies = &deps_event_playback },
    .{ .name = "chghost" },
    .{ .name = "echo-message" },
    .{ .name = "draft/extended-isupport" },
    .{ .name = "extended-join" },
    .{ .name = "extended-monitor" },
    .{ .name = "invite-notify" },
    .{ .name = "labeled-response", .dependencies = &deps_batch },
    .{ .name = "draft/message-redaction" },
    .{ .name = "draft/message-editing" },
    .{ .name = "message-tags" },
    .{ .name = "draft/metadata-2" },
    .{ .name = "multi-prefix" },
    .{ .name = "draft/multiline", .dependencies = &deps_batch },
    .{ .name = "no-implicit-names" },
    .{ .name = "draft/no-implicit-names" },
    .{ .name = "draft/oper-tag" },
    .{ .name = "draft/pre-away" },
    .{ .name = "draft/read-marker" },
    .{ .name = "draft/typing" },
    .{ .name = "draft/react" },
    .{ .name = "draft/reply" },
    .{ .name = "draft/channel-context" },
    .{ .name = "bot" },
    .{ .name = "account-extban" },
    .{ .name = "utf8-only" },
    .{ .name = "draft/netsplit", .dependencies = &deps_batch },
    .{ .name = "draft/netjoin", .dependencies = &deps_batch },
    .{ .name = "onyx/session-sync" },
    .{ .name = "onyx/bouncer" },
    .{ .name = "onyx/topics" },
    .{ .name = "onyx/e2ee", .requestable = false },
    .{ .name = "sasl" },
    .{ .name = "server-time" },
    .{ .name = "setname" },
    .{ .name = "standard-replies" },
    .{ .name = "userhost-in-names" },
    .{ .name = "sts", .requestable = false },
};

/// Default to every requestable published client capability. Applications can
/// pass a narrower list when a message handler is intentionally incomplete.
pub const default_desired_capabilities = blk: {
    const count = count_requestable: {
        var value: usize = 0;
        for (published_client_capabilities) |spec| if (spec.requestable) {
            value += 1;
        };
        break :count_requestable value;
    };
    var names: [count][]const u8 = undefined;
    var at: usize = 0;
    for (published_client_capabilities) |spec| {
        if (spec.requestable) {
            names[at] = spec.name;
            at += 1;
        }
    }
    break :blk names;
};

pub const Config = struct {
    desired: []const []const u8 = &default_desired_capabilities,
    /// Hold registration open after ACKing SASL until the SASL state machine
    /// reports a terminal numeric.
    hold_cap_end_for_sasl: bool = true,
};

pub const Phase = enum {
    idle,
    listing,
    requesting,
    waiting_sasl,
    complete,
};

pub const Event = enum {
    none,
    request_sent,
    sasl_ready,
    capabilities_changed,
    sasl_lost,
    complete,
};

/// STS is connection policy, not a requestable capability. Call
/// `takeStsPolicyUpdate` after each CAP LS or NEW event. A TLS connection must
/// only use `.tls_verified` after hostname and certificate-chain validation.
pub const TransportSecurity = enum { plaintext, tls_verified };

pub const StsPersistence = struct {
    duration_seconds: u64,
    preload: bool,
};

pub const StsUpdate = union(enum) {
    none,
    invalid,
    upgrade_port: u16,
    persistence: StsPersistence,
};

const AckChange = struct {
    name: []const u8,
    enable: bool,
};

pub const Session = struct {
    gpa: std.mem.Allocator,
    config: Config,
    phase: Phase = .idle,
    offered: CapabilityList = .{},
    enabled: CapabilityList = .{},
    pending_batches: usize = 0,
    pending_names: std.ArrayList([]u8) = .empty,
    pending_batch_sizes: std.ArrayList(usize) = .empty,
    pending_batch_index: usize = 0,
    pending_name_index: usize = 0,
    pending_ack: std.ArrayList(AckChange) = .empty,
    list_snapshot: CapabilityList = .{},
    list_in_progress: bool = false,
    registration_open: bool = false,
    sts_update_pending: bool = false,
    deferred_reselect: bool = false,
    sasl_just_enabled: bool = false,

    pub fn init(gpa: std.mem.Allocator, config: Config) Session {
        return .{ .gpa = gpa, .config = config };
    }

    pub fn deinit(self: *Session) void {
        self.offered.deinit(self.gpa);
        self.enabled.deinit(self.gpa);
        self.clearPendingRequests();
        self.pending_names.deinit(self.gpa);
        self.pending_batch_sizes.deinit(self.gpa);
        self.pending_ack.deinit(self.gpa);
        self.list_snapshot.deinit(self.gpa);
        self.* = undefined;
    }

    pub fn begin(self: *Session, out: *std.ArrayList(u8)) Error!void {
        if (self.phase != .idle) return error.InvalidState;
        try out.appendSlice(self.gpa, "CAP LS 302\r\n");
        try self.enabled.put(self.gpa, "cap-notify", null);
        self.phase = .listing;
        self.registration_open = true;
    }

    pub fn requestList(self: *Session, out: *std.ArrayList(u8)) Error!void {
        if (self.phase == .idle or self.phase == .listing or self.pending_batches != 0 or self.list_in_progress)
            return error.InvalidState;
        self.list_snapshot.deinit(self.gpa);
        self.list_in_progress = true;
        const start = out.items.len;
        out.appendSlice(self.gpa, "CAP LIST\r\n") catch |err| {
            out.items.len = start;
            self.list_in_progress = false;
            return err;
        };
    }

    pub fn handle(self: *Session, out: *std.ArrayList(u8), msg: message.Message) Error!Event {
        if (!std.ascii.eqlIgnoreCase(msg.command, "CAP")) return .none;
        if (msg.param_count < 3) return error.InvalidCapMessage;
        const subcommand = msg.param(1) orelse return error.InvalidCapMessage;

        if (std.ascii.eqlIgnoreCase(subcommand, "LS")) return self.handleLs(out, msg);
        if (std.ascii.eqlIgnoreCase(subcommand, "LIST")) return self.handleList(msg);
        if (std.ascii.eqlIgnoreCase(subcommand, "ACK")) return self.handleAck(out, msg);
        if (std.ascii.eqlIgnoreCase(subcommand, "NAK")) return self.handleNak(out, msg);
        if (std.ascii.eqlIgnoreCase(subcommand, "NEW")) return self.handleNew(out, msg);
        if (std.ascii.eqlIgnoreCase(subcommand, "DEL")) return self.handleDel(msg);
        return .none;
    }

    /// Complete registration only after a SASL terminal numeric (or after the
    /// application has deliberately abandoned authentication).
    pub fn saslComplete(self: *Session, out: *std.ArrayList(u8)) Error!Event {
        if (self.phase != .waiting_sasl) return error.InvalidState;
        return self.finishRegistration(out);
    }

    /// Consume a pending STS advertisement using the security state of the
    /// current connection. Missing/invalid required policy keys produce
    /// `.invalid` and MUST be ignored by the application. Upgrade events must
    /// close the plaintext connection and reconnect with verified TLS.
    pub fn takeStsPolicyUpdate(self: *Session, security: TransportSecurity) StsUpdate {
        if (!self.sts_update_pending) return .none;
        self.sts_update_pending = false;
        const capability = self.offered.get("sts") orelse return .invalid;
        return parseStsPolicy(capability.value, security);
    }

    fn handleLs(self: *Session, out: *std.ArrayList(u8), msg: message.Message) Error!Event {
        if (self.phase != .listing) return error.InvalidState;
        const continuation = msg.param_count >= 4 and std.mem.eql(u8, msg.param(2).?, "*");
        const list = msg.param(msg.param_count - 1) orelse return error.InvalidCapMessage;
        try parseListIntoAtomic(&self.offered, self.gpa, list, true);
        if (continuation) return .none;
        if (self.offered.contains("sts")) self.sts_update_pending = true;

        const requested = try self.requestDesired(out);
        if (requested) return .request_sent;
        return self.finishRegistration(out);
    }

    fn handleList(self: *Session, msg: message.Message) Error!Event {
        if (!self.list_in_progress) return error.InvalidState;
        const continuation = msg.param_count >= 4 and std.mem.eql(u8, msg.param(2).?, "*");
        const list = msg.param(msg.param_count - 1) orelse return error.InvalidCapMessage;
        try parseListIntoAtomic(&self.list_snapshot, self.gpa, list, false);
        if (continuation) return .none;
        var authoritative = try self.list_snapshot.clone(self.gpa);
        errdefer authoritative.deinit(self.gpa);
        for (authoritative.entries.items) |entry| {
            if (self.offered.get(entry.name)) |offered| try authoritative.put(self.gpa, entry.name, offered.value);
        }
        // CAP 302 enables cap-notify implicitly even if an implementation
        // omits it from an otherwise authoritative LIST response.
        try authoritative.put(self.gpa, "cap-notify", null);
        self.enabled.deinit(self.gpa);
        self.enabled = authoritative;
        self.list_snapshot.deinit(self.gpa);
        self.list_snapshot = .{};
        self.list_in_progress = false;
        return .capabilities_changed;
    }

    fn handleAck(self: *Session, out: *std.ArrayList(u8), msg: message.Message) Error!Event {
        if (self.pending_batches == 0) return error.InvalidState;
        const list = msg.param(msg.param_count - 1) orelse return error.InvalidCapMessage;
        const expected = self.currentPendingNames() orelse return error.InvalidState;
        var fields = std.mem.splitScalar(u8, list, ' ');
        while (fields.next()) |field| {
            if (field.len == 0) continue;
            const disable = field[0] == '-';
            const name = if (disable) field[1..] else field;
            try validateCapabilityName(name, false);
            if (disable or !ownedListContains(expected, name)) return error.InvalidCapMessage;
            for (self.pending_ack.items) |change| {
                if (std.mem.eql(u8, change.name, name)) return error.InvalidCapMessage;
            }
            try self.pending_ack.append(self.gpa, .{ .name = findName(expected, name).?, .enable = true });
        }
        if (self.pending_ack.items.len > expected.len) return error.InvalidCapMessage;
        // The specification supplies no continuation marker for split ACKs;
        // the complete response is known only once every item in the atomic
        // REQ set has appeared. Do not expose any change before then.
        if (self.pending_ack.items.len != expected.len) return .none;

        var updated = try self.enabled.clone(self.gpa);
        errdefer updated.deinit(self.gpa);
        const sasl_was_enabled = self.enabled.contains("sasl");
        for (self.pending_ack.items) |change| {
            if (change.enable) {
                if (self.offered.get(change.name)) |cap| {
                    try updated.put(self.gpa, change.name, cap.value);
                }
            } else {
                _ = updated.remove(self.gpa, change.name);
            }
        }
        self.enabled.deinit(self.gpa);
        self.enabled = updated;
        self.sasl_just_enabled = !sasl_was_enabled and self.enabled.contains("sasl");
        self.pending_ack.clearRetainingCapacity();
        self.advancePendingBatch();
        self.pending_batches -= 1;
        if (self.pending_batches != 0) return .none;
        self.clearPendingRequests();
        return self.requestsComplete(out);
    }

    fn handleNak(self: *Session, out: *std.ArrayList(u8), msg: message.Message) Error!Event {
        if (self.pending_batches == 0) return error.InvalidState;
        if (self.pending_ack.items.len != 0) return error.InvalidCapMessage;
        const list = msg.param(msg.param_count - 1) orelse return error.InvalidCapMessage;
        const expected = self.currentPendingNames() orelse return error.InvalidState;
        if (!sameCapabilitySet(expected, list)) return error.InvalidCapMessage;
        self.advancePendingBatch();
        self.pending_batches -= 1;
        if (self.pending_batches != 0) return .none;
        self.clearPendingRequests();
        return self.requestsComplete(out);
    }

    fn handleNew(self: *Session, out: *std.ArrayList(u8), msg: message.Message) Error!Event {
        if (self.phase == .idle or self.phase == .listing) return error.InvalidState;
        const list = msg.param(msg.param_count - 1) orelse return error.InvalidCapMessage;
        var delta: CapabilityList = .{};
        defer delta.deinit(self.gpa);
        try parseListInto(&delta, self.gpa, list, true);
        var updated_enabled = try self.enabled.clone(self.gpa);
        var owns_updated_enabled = true;
        errdefer if (owns_updated_enabled) updated_enabled.deinit(self.gpa);
        // CAP NEW may update the value of an already-enabled capability. Keep
        // the enabled snapshot coherent without requiring a redundant REQ.
        for (delta.entries.items) |entry| {
            if (updated_enabled.contains(entry.name))
                try updated_enabled.put(self.gpa, entry.name, entry.value);
        }
        try self.offered.mergeOwned(self.gpa, &delta);
        self.enabled.deinit(self.gpa);
        self.enabled = updated_enabled;
        owns_updated_enabled = false;
        if (capabilityListContains(list, "sts")) self.sts_update_pending = true;
        if (self.pending_batches != 0) {
            self.deferred_reselect = true;
            return .capabilities_changed;
        }
        if (try self.requestDesired(out)) return .request_sent;
        return .capabilities_changed;
    }

    fn handleDel(self: *Session, msg: message.Message) Error!Event {
        const list = msg.param(msg.param_count - 1) orelse return error.InvalidCapMessage;
        var validation = std.mem.splitScalar(u8, list, ' ');
        while (validation.next()) |name| {
            if (name.len != 0) try validateCapabilityName(name, false);
        }
        var sasl_removed = false;
        var fields = std.mem.splitScalar(u8, list, ' ');
        while (fields.next()) |name| {
            if (name.len == 0) continue;
            // STS persistence is disabled only with `duration=0`; the STS
            // specification forbids servers from cancelling it via CAP DEL.
            if (std.mem.eql(u8, name, "sts")) continue;
            _ = self.offered.remove(self.gpa, name);
            if (self.enabled.remove(self.gpa, name) and std.mem.eql(u8, name, "sasl"))
                sasl_removed = true;
        }
        return if (sasl_removed) .sasl_lost else .capabilities_changed;
    }

    fn requestDesired(self: *Session, out: *std.ArrayList(u8)) Error!bool {
        var selected: std.ArrayList([]const u8) = .empty;
        defer selected.deinit(self.gpa);
        for (self.config.desired) |name| {
            try validateCapabilityName(name, false);
            _ = try self.selectWithDependencies(&selected, name);
        }
        if (selected.items.len == 0) return false;
        try self.emitRequests(out, selected.items);
        self.phase = .requesting;
        return true;
    }

    fn selectWithDependencies(
        self: *const Session,
        selected: *std.ArrayList([]const u8),
        name: []const u8,
    ) Error!bool {
        if (self.enabled.contains(name) or listContains(selected.items, name)) return true;
        const offered = self.offered.get(name) orelse return false;
        const spec = findSpec(name);
        if (spec) |known| if (!known.requestable) return false;

        const rollback = selected.items.len;
        if (spec) |known| {
            for (known.dependencies) |dependency| {
                if (!try self.selectWithDependencies(selected, dependency)) {
                    selected.shrinkRetainingCapacity(rollback);
                    return false;
                }
            }
        }
        try selected.append(self.gpa, offered.name);
        return true;
    }

    fn emitRequests(self: *Session, out: *std.ArrayList(u8), names: []const []const u8) Error!void {
        var first: usize = 0;
        while (first < names.len) {
            var end = first;
            var payload_len: usize = 0;
            while (end < names.len) : (end += 1) {
                const next_len = payload_len + @intFromBool(end != first) + names[end].len;
                if (next_len > max_req_payload) break;
                payload_len = next_len;
            }
            if (end == first) return error.CapabilityRequestTooLong;
            const output_start = out.items.len;
            appendRequestLine(out, self.gpa, names[first..end]) catch |err| {
                out.items.len = output_start;
                return err;
            };
            self.recordPendingBatch(names[first..end]) catch |err| {
                out.items.len = output_start;
                return err;
            };
            first = end;
        }
    }

    fn recordPendingBatch(self: *Session, names: []const []const u8) Error!void {
        const names_start = self.pending_names.items.len;
        errdefer {
            for (self.pending_names.items[names_start..]) |name| self.gpa.free(name);
            self.pending_names.shrinkRetainingCapacity(names_start);
        }
        const sizes_start = self.pending_batch_sizes.items.len;
        errdefer self.pending_batch_sizes.shrinkRetainingCapacity(sizes_start);
        try self.pending_batch_sizes.append(self.gpa, names.len);
        for (names) |name| {
            const copy = try self.gpa.dupe(u8, name);
            self.pending_names.append(self.gpa, copy) catch |err| {
                self.gpa.free(copy);
                return err;
            };
        }
        self.pending_batches += 1;
    }

    fn currentPendingNames(self: *const Session) ?[]const []u8 {
        if (self.pending_batch_index >= self.pending_batch_sizes.items.len) return null;
        const count = self.pending_batch_sizes.items[self.pending_batch_index];
        return self.pending_names.items[self.pending_name_index..][0..count];
    }

    fn advancePendingBatch(self: *Session) void {
        self.pending_name_index += self.pending_batch_sizes.items[self.pending_batch_index];
        self.pending_batch_index += 1;
    }

    fn clearPendingRequests(self: *Session) void {
        for (self.pending_names.items) |name| self.gpa.free(name);
        self.pending_names.clearRetainingCapacity();
        self.pending_batch_sizes.clearRetainingCapacity();
        self.pending_ack.clearRetainingCapacity();
        self.pending_batch_index = 0;
        self.pending_name_index = 0;
    }

    fn requestsComplete(self: *Session, out: *std.ArrayList(u8)) Error!Event {
        if (self.deferred_reselect) {
            self.deferred_reselect = false;
            if (try self.requestDesired(out)) return .request_sent;
        }
        if (self.config.hold_cap_end_for_sasl and self.sasl_just_enabled) {
            self.sasl_just_enabled = false;
            self.phase = .waiting_sasl;
            return .sasl_ready;
        }
        if (self.registration_open) {
            return self.finishRegistration(out);
        }
        self.phase = .complete;
        return .capabilities_changed;
    }

    fn finishRegistration(self: *Session, out: *std.ArrayList(u8)) Error!Event {
        if (self.registration_open) {
            try out.appendSlice(self.gpa, "CAP END\r\n");
            self.registration_open = false;
        }
        self.phase = .complete;
        return .complete;
    }
};

fn findSpec(name: []const u8) ?CapabilitySpec {
    for (published_client_capabilities) |spec| {
        if (std.mem.eql(u8, spec.name, name)) return spec;
    }
    return null;
}

fn listContains(names: []const []const u8, needle: []const u8) bool {
    for (names) |name| if (std.mem.eql(u8, name, needle)) return true;
    return false;
}

fn ownedListContains(names: []const []u8, needle: []const u8) bool {
    return findName(names, needle) != null;
}

fn findName(names: []const []u8, needle: []const u8) ?[]const u8 {
    for (names) |name| if (std.mem.eql(u8, name, needle)) return name;
    return null;
}

fn sameCapabilitySet(expected: []const []u8, raw: []const u8) bool {
    var count: usize = 0;
    var fields = std.mem.splitScalar(u8, raw, ' ');
    while (fields.next()) |field| {
        if (field.len == 0) continue;
        if (!ownedListContains(expected, field)) return false;
        count += 1;
    }
    if (count != expected.len) return false;
    for (expected) |name| {
        var occurrences: usize = 0;
        var check = std.mem.splitScalar(u8, raw, ' ');
        while (check.next()) |field| {
            if (std.mem.eql(u8, field, name)) occurrences += 1;
        }
        if (occurrences != 1) return false;
    }
    return true;
}

fn capabilityListContains(raw: []const u8, needle: []const u8) bool {
    var fields = std.mem.splitScalar(u8, raw, ' ');
    while (fields.next()) |field| {
        const equals = std.mem.indexOfScalar(u8, field, '=') orelse field.len;
        if (std.mem.eql(u8, field[0..equals], needle)) return true;
    }
    return false;
}

fn parseStsPolicy(value: ?[]const u8, security: TransportSecurity) StsUpdate {
    const raw = value orelse return .invalid;
    var port: ?u16 = null;
    var duration: ?u64 = null;
    var preload = false;
    var preload_seen = false;
    var tokens = std.mem.splitScalar(u8, raw, ',');
    while (tokens.next()) |token| {
        if (token.len == 0) return .invalid;
        const equals = std.mem.indexOfScalar(u8, token, '=');
        const key = if (equals) |at| token[0..at] else token;
        const token_value = if (equals) |at| token[at + 1 ..] else null;
        if (std.mem.eql(u8, key, "port")) {
            if (security == .plaintext) {
                if (port != null) return .invalid;
                const text = token_value orelse return .invalid;
                port = parseDecimal(u16, text) orelse return .invalid;
                if (port.? == 0) return .invalid;
            }
        } else if (std.mem.eql(u8, key, "duration")) {
            if (security == .tls_verified) {
                if (duration != null) return .invalid;
                const text = token_value orelse return .invalid;
                duration = parseDecimal(u64, text) orelse return .invalid;
            }
        } else if (std.mem.eql(u8, key, "preload")) {
            if (security == .tls_verified) {
                if (preload_seen) return .invalid;
                preload_seen = true;
                preload = true;
            }
        }
    }
    return switch (security) {
        .plaintext => if (port) |upgrade| .{ .upgrade_port = upgrade } else .invalid,
        .tls_verified => if (duration) |seconds|
            .{ .persistence = .{ .duration_seconds = seconds, .preload = preload } }
        else
            .invalid,
    };
}

fn parseDecimal(comptime T: type, raw: []const u8) ?T {
    if (raw.len == 0) return null;
    for (raw) |byte| if (!std.ascii.isDigit(byte)) return null;
    return std.fmt.parseInt(T, raw, 10) catch null;
}

fn validateCapabilityName(raw: []const u8, allow_disable: bool) Error!void {
    var name = raw;
    if (allow_disable and name.len != 0 and name[0] == '-') name = name[1..];
    if (name.len == 0 or name[0] == '-' or std.mem.indexOfAny(u8, name, " =\r\n\x00") != null)
        return error.InvalidCapability;
}

fn parseListInto(
    list: *CapabilityList,
    gpa: std.mem.Allocator,
    raw: []const u8,
    values_allowed: bool,
) Error!void {
    var fields = std.mem.splitScalar(u8, raw, ' ');
    while (fields.next()) |field| {
        if (field.len == 0) continue;
        const equals = std.mem.indexOfScalar(u8, field, '=');
        const name = if (equals) |at| field[0..at] else field;
        const value = if (equals) |at| field[at + 1 ..] else null;
        if (!values_allowed and value != null) return error.InvalidCapability;
        try list.put(gpa, name, value);
    }
}

fn parseListIntoAtomic(
    list: *CapabilityList,
    gpa: std.mem.Allocator,
    raw: []const u8,
    values_allowed: bool,
) Error!void {
    var delta: CapabilityList = .{};
    defer delta.deinit(gpa);
    try parseListInto(&delta, gpa, raw, values_allowed);
    try list.mergeOwned(gpa, &delta);
}

fn appendRequestLine(
    out: *std.ArrayList(u8),
    gpa: std.mem.Allocator,
    names: []const []const u8,
) Error!void {
    const start = out.items.len;
    errdefer out.items.len = start;
    try out.appendSlice(gpa, "CAP REQ :");
    for (names, 0..) |name, index| {
        if (index != 0) try out.append(gpa, ' ');
        try out.appendSlice(gpa, name);
    }
    try out.appendSlice(gpa, "\r\n");
}

fn exerciseCapAllocationFailures(gpa: std.mem.Allocator) !void {
    const desired = [_][]const u8{"labeled-response"};
    var session = Session.init(gpa, .{ .desired = &desired, .hold_cap_end_for_sasl = false });
    defer session.deinit();
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);

    try session.begin(&out);
    _ = try session.handle(&out, message.parse(":irc CAP * LS * :batch message-tags"));
    _ = try session.handle(&out, message.parse(":irc CAP * LS :labeled-response away-notify=v1"));
    _ = try session.handle(&out, message.parse(":irc CAP me ACK :batch labeled-response"));
    _ = try session.handle(&out, message.parse(":irc CAP me NEW :away-notify=v2"));
    try session.requestList(&out);
    _ = try session.handle(&out, message.parse(":irc CAP me LIST * :batch"));
    _ = try session.handle(&out, message.parse(":irc CAP me LIST :labeled-response"));
}

test "CAP snapshots survive every allocation failure without leaks" {
    try std.testing.checkAllAllocationFailures(
        std.testing.allocator,
        exerciseCapAllocationFailures,
        .{},
    );
}

test "CAP 302 multiline LS preserves values, resolves dependencies, and gates CAP END on SASL" {
    const gpa = std.testing.allocator;
    const desired = [_][]const u8{ "sasl", "labeled-response", "sts" };
    var session = Session.init(gpa, .{ .desired = &desired });
    defer session.deinit();
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);

    try session.begin(&out);
    try std.testing.expectEqualStrings("CAP LS 302\r\n", out.items);
    try std.testing.expect(session.enabled.contains("cap-notify"));

    try std.testing.expectEqual(
        Event.none,
        try session.handle(&out, message.parse(":irc.example CAP * LS * :multi-prefix sasl=SCRAM-SHA-256,PLAIN")),
    );
    try std.testing.expectEqual(
        Event.request_sent,
        try session.handle(&out, message.parse(":irc.example CAP * LS :message-tags batch labeled-response sts")),
    );
    try std.testing.expectEqualStrings(
        "SCRAM-SHA-256,PLAIN",
        session.offered.get("sasl").?.value.?,
    );
    try std.testing.expectEqualStrings(
        "CAP LS 302\r\nCAP REQ :sasl batch labeled-response\r\n",
        out.items,
    );
    try std.testing.expect(std.mem.indexOf(u8, out.items, "sts") == null);

    try std.testing.expectEqual(
        Event.sasl_ready,
        try session.handle(&out, message.parse(":irc.example CAP nick ACK :sasl batch labeled-response")),
    );
    try std.testing.expectEqual(Phase.waiting_sasl, session.phase);
    try std.testing.expect(std.mem.indexOf(u8, out.items, "CAP END") == null);
    try std.testing.expect(session.enabled.contains("labeled-response"));

    try std.testing.expectEqual(Event.complete, try session.saslComplete(&out));
    try std.testing.expect(std.mem.endsWith(u8, out.items, "CAP END\r\n"));
}

test "missing dependencies suppress dependent capability requests" {
    const gpa = std.testing.allocator;
    const desired = [_][]const u8{"labeled-response"};
    var session = Session.init(gpa, .{ .desired = &desired });
    defer session.deinit();
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);

    try session.begin(&out);
    try std.testing.expectEqual(
        Event.complete,
        try session.handle(&out, message.parse(":irc.example CAP * LS :labeled-response message-tags")),
    );
    try std.testing.expectEqualStrings("CAP LS 302\r\nCAP END\r\n", out.items);
}

test "CAP NEW requests newly available desired caps and DEL cancels without REQ" {
    const gpa = std.testing.allocator;
    const desired = [_][]const u8{"away-notify"};
    var session = Session.init(gpa, .{ .desired = &desired });
    defer session.deinit();
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);

    try session.begin(&out);
    try std.testing.expectEqual(
        Event.complete,
        try session.handle(&out, message.parse(":irc.example CAP * LS :batch")),
    );
    out.clearRetainingCapacity();
    try std.testing.expectEqual(
        Event.request_sent,
        try session.handle(&out, message.parse(":irc.example CAP nick NEW :away-notify=presence-v2")),
    );
    try std.testing.expectEqualStrings("CAP REQ :away-notify\r\n", out.items);
    out.clearRetainingCapacity();
    try std.testing.expectEqual(
        Event.capabilities_changed,
        try session.handle(&out, message.parse(":irc.example CAP nick ACK :away-notify")),
    );
    try std.testing.expectEqualStrings("presence-v2", session.enabled.get("away-notify").?.value.?);
    try std.testing.expectEqual(@as(usize, 0), out.items.len);

    try std.testing.expectEqual(
        Event.capabilities_changed,
        try session.handle(&out, message.parse(":irc.example CAP nick DEL :away-notify")),
    );
    try std.testing.expect(!session.offered.contains("away-notify"));
    try std.testing.expect(!session.enabled.contains("away-notify"));
    try std.testing.expectEqual(@as(usize, 0), out.items.len);
}

test "CAP NEW during an outstanding request is reconsidered after its atomic ACK" {
    const gpa = std.testing.allocator;
    const desired = [_][]const u8{ "multi-prefix", "away-notify" };
    var session = Session.init(gpa, .{ .desired = &desired, .hold_cap_end_for_sasl = false });
    defer session.deinit();
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);
    try session.begin(&out);
    try std.testing.expectEqual(
        Event.request_sent,
        try session.handle(&out, message.parse(":irc CAP * LS :multi-prefix")),
    );
    try std.testing.expectEqual(
        Event.capabilities_changed,
        try session.handle(&out, message.parse(":irc CAP nick NEW :away-notify")),
    );
    out.clearRetainingCapacity();
    try std.testing.expectEqual(
        Event.request_sent,
        try session.handle(&out, message.parse(":irc CAP nick ACK :multi-prefix")),
    );
    try std.testing.expectEqualStrings("CAP REQ :away-notify\r\n", out.items);
    out.clearRetainingCapacity();
    try std.testing.expectEqual(
        Event.complete,
        try session.handle(&out, message.parse(":irc CAP nick ACK :away-notify")),
    );
    try std.testing.expectEqualStrings("CAP END\r\n", out.items);
}

test "post-registration SASL NEW produces a fresh sasl-ready gate" {
    const gpa = std.testing.allocator;
    const desired = [_][]const u8{"sasl"};
    var session = Session.init(gpa, .{ .desired = &desired });
    defer session.deinit();
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);
    try session.begin(&out);
    try std.testing.expectEqual(
        Event.complete,
        try session.handle(&out, message.parse(":irc CAP * LS :batch")),
    );
    out.clearRetainingCapacity();
    try std.testing.expectEqual(
        Event.request_sent,
        try session.handle(&out, message.parse(":irc CAP nick NEW :sasl=PLAIN")),
    );
    out.clearRetainingCapacity();
    try std.testing.expectEqual(
        Event.sasl_ready,
        try session.handle(&out, message.parse(":irc CAP nick ACK :sasl")),
    );
    try std.testing.expectEqual(Event.complete, try session.saslComplete(&out));
    try std.testing.expectEqual(@as(usize, 0), out.items.len);
}

test "account-registration is dependency-free and preserves its optional value" {
    const gpa = std.testing.allocator;
    const desired = [_][]const u8{"draft/account-registration"};
    var session = Session.init(gpa, .{ .desired = &desired, .hold_cap_end_for_sasl = false });
    defer session.deinit();
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);
    try session.begin(&out);
    try std.testing.expectEqual(
        Event.request_sent,
        try session.handle(&out, message.parse(":irc CAP * LS :draft/account-registration=before-connect,email-required,min-password-length=12")),
    );
    try std.testing.expect(std.mem.indexOf(u8, out.items, "standard-replies") == null);
    _ = try session.handle(&out, message.parse(":irc CAP nick ACK :draft/account-registration"));
    try std.testing.expectEqualStrings(
        "before-connect,email-required,min-password-length=12",
        session.enabled.get("draft/account-registration").?.value.?,
    );
}

test "CAP ACK and NAK batches apply atomically and finish registration" {
    const gpa = std.testing.allocator;
    var session = Session.init(gpa, .{ .desired = &.{} });
    defer session.deinit();
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);
    session.phase = .requesting;
    session.registration_open = true;
    try session.offered.put(gpa, "batch", null);
    const first = [_][]const u8{"batch"};
    const second = [_][]const u8{"unknown"};
    try session.recordPendingBatch(&first);
    try session.recordPendingBatch(&second);

    try std.testing.expectEqual(
        Event.none,
        try session.handle(&out, message.parse(":irc CAP * ACK :batch")),
    );
    try std.testing.expect(session.enabled.contains("batch"));
    try std.testing.expectEqual(
        Event.complete,
        try session.handle(&out, message.parse(":irc CAP * NAK :unknown")),
    );
    try std.testing.expectEqualStrings("CAP END\r\n", out.items);
}

test "default catalog requests every offered client capability except STS" {
    const gpa = std.testing.allocator;
    var advertised: std.ArrayList(u8) = .empty;
    defer advertised.deinit(gpa);
    try advertised.appendSlice(gpa, ":irc CAP * LS :");
    for (published_client_capabilities, 0..) |spec, index| {
        if (index != 0) try advertised.append(gpa, ' ');
        try advertised.appendSlice(gpa, spec.name);
        if (std.mem.eql(u8, spec.name, "sasl")) try advertised.appendSlice(gpa, "=PLAIN,EXTERNAL");
    }

    var session = Session.init(gpa, .{ .hold_cap_end_for_sasl = false });
    defer session.deinit();
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);
    try session.begin(&out);
    try std.testing.expectEqual(Event.request_sent, try session.handle(&out, message.parse(advertised.items)));

    for (published_client_capabilities) |spec| {
        const needle = if (spec.requestable) spec.name else "sts";
        if (spec.requestable and !std.mem.eql(u8, spec.name, "cap-notify")) {
            try std.testing.expect(std.mem.indexOf(u8, out.items, needle) != null);
        }
    }
    try std.testing.expect(std.mem.indexOf(u8, out.items, " sts") == null);
    try std.testing.expect(session.pending_batches >= 1);
}

test "CAP REQ splits before the IRC line-size boundary" {
    const gpa = std.testing.allocator;
    var name_a: [200]u8 = @splat('a');
    var name_b: [200]u8 = @splat('b');
    var name_c: [200]u8 = @splat('c');
    const names = [_][]const u8{ &name_a, &name_b, &name_c };
    var session = Session.init(gpa, .{});
    defer session.deinit();
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);

    try session.emitRequests(&out, &names);
    try std.testing.expectEqual(@as(usize, 2), session.pending_batches);
    var lines = std.mem.splitSequence(u8, out.items, "\r\n");
    var count: usize = 0;
    while (lines.next()) |line| {
        if (line.len == 0) continue;
        try std.testing.expect(line.len + 2 <= 512);
        count += 1;
    }
    try std.testing.expectEqual(@as(usize, 2), count);
}

test "split CAP ACK remains invisible until the complete atomic set arrives" {
    const gpa = std.testing.allocator;
    const desired = [_][]const u8{ "batch", "multi-prefix", "away-notify" };
    var session = Session.init(gpa, .{ .desired = &desired, .hold_cap_end_for_sasl = false });
    defer session.deinit();
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);
    try session.begin(&out);
    try std.testing.expectEqual(
        Event.request_sent,
        try session.handle(&out, message.parse(":irc CAP * LS :batch multi-prefix away-notify")),
    );

    try std.testing.expectEqual(
        Event.none,
        try session.handle(&out, message.parse(":irc CAP * ACK :batch multi-prefix")),
    );
    try std.testing.expect(!session.enabled.contains("batch"));
    try std.testing.expect(!session.enabled.contains("multi-prefix"));
    try std.testing.expect(!session.enabled.contains("away-notify"));
    try std.testing.expectEqual(
        Event.complete,
        try session.handle(&out, message.parse(":irc CAP * ACK :away-notify")),
    );
    try std.testing.expect(session.enabled.contains("batch"));
    try std.testing.expect(session.enabled.contains("multi-prefix"));
    try std.testing.expect(session.enabled.contains("away-notify"));
}

test "STS advertisements surface policy, are never requested, and ignore DEL" {
    const gpa = std.testing.allocator;
    var session = Session.init(gpa, .{ .desired = &.{} });
    defer session.deinit();
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);
    try session.begin(&out);
    try std.testing.expectEqual(
        Event.complete,
        try session.handle(&out, message.parse(":irc CAP * LS :sts=port=6697,duration=86400,preload")),
    );
    try std.testing.expect(std.mem.indexOf(u8, out.items, "CAP REQ") == null);
    const upgrade = session.takeStsPolicyUpdate(.plaintext);
    try std.testing.expect(upgrade == .upgrade_port);
    try std.testing.expectEqual(@as(u16, 6697), upgrade.upgrade_port);
    try std.testing.expect(session.takeStsPolicyUpdate(.plaintext) == .none);

    try std.testing.expectEqual(
        Event.capabilities_changed,
        try session.handle(&out, message.parse(":irc CAP nick NEW :sts=duration=0,preload")),
    );
    const persistence = session.takeStsPolicyUpdate(.tls_verified);
    try std.testing.expect(persistence == .persistence);
    try std.testing.expectEqual(@as(u64, 0), persistence.persistence.duration_seconds);
    try std.testing.expect(persistence.persistence.preload);
    try std.testing.expectEqual(
        Event.capabilities_changed,
        try session.handle(&out, message.parse(":irc CAP nick DEL :sts")),
    );
    try std.testing.expect(session.offered.contains("sts"));
}

test "malformed or context-incomplete STS policy is ignored" {
    try std.testing.expect(parseStsPolicy("duration=60", .plaintext) == .invalid);
    try std.testing.expect(parseStsPolicy("port=6697", .tls_verified) == .invalid);
    try std.testing.expect(parseStsPolicy("port=0", .plaintext) == .invalid);
    try std.testing.expect(parseStsPolicy("duration=-1", .tls_verified) == .invalid);
    try std.testing.expect(parseStsPolicy("duration=1,duration=2", .tls_verified) == .invalid);
    try std.testing.expect(parseStsPolicy("port=bad,duration=60", .tls_verified) == .persistence);
    try std.testing.expect(parseStsPolicy("port=6697,duration=bad", .plaintext) == .upgrade_port);
}

test "CAP NEW updates enabled values without redundant requests" {
    const gpa = std.testing.allocator;
    const desired = [_][]const u8{"away-notify"};
    var session = Session.init(gpa, .{ .desired = &desired, .hold_cap_end_for_sasl = false });
    defer session.deinit();
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);
    try session.begin(&out);
    _ = try session.handle(&out, message.parse(":irc CAP * LS :away-notify=v1"));
    _ = try session.handle(&out, message.parse(":irc CAP * ACK :away-notify"));
    out.clearRetainingCapacity();
    try std.testing.expectEqual(
        Event.capabilities_changed,
        try session.handle(&out, message.parse(":irc CAP me NEW :away-notify=v2")),
    );
    try std.testing.expectEqualStrings("v2", session.enabled.get("away-notify").?.value.?);
    try std.testing.expectEqual(@as(usize, 0), out.items.len);
}

test "capability map is deterministically bounded under multiline LS abuse" {
    const gpa = std.testing.allocator;
    var list: CapabilityList = .{};
    defer list.deinit(gpa);
    var name_buffer: [32]u8 = undefined;
    for (0..max_capabilities) |index| {
        const name = try std.fmt.bufPrint(&name_buffer, "vendor.example/cap-{d}", .{index});
        try list.put(gpa, name, "value");
    }
    try std.testing.expectEqual(max_capabilities, list.count());
    try std.testing.expectError(error.CapabilityBackpressure, list.put(gpa, "vendor.example/overflow", null));
    try std.testing.expect(list.storage_bytes <= max_capability_storage);
}

test "malformed CAP responses fail closed without partial enablement" {
    const gpa = std.testing.allocator;
    const desired = [_][]const u8{ "batch", "multi-prefix" };
    var session = Session.init(gpa, .{ .desired = &desired });
    defer session.deinit();
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);
    try session.begin(&out);
    try std.testing.expectError(
        error.InvalidCapability,
        session.handle(&out, message.parse(":irc CAP * LS :batch =invalid")),
    );
    try std.testing.expect(!session.offered.contains("batch"));

    // Start a clean request after the malformed line, then reject an ACK for
    // a capability outside the atomic set. Nothing becomes visible.
    _ = try session.handle(&out, message.parse(":irc CAP * LS :batch multi-prefix"));
    try std.testing.expectError(
        error.InvalidCapMessage,
        session.handle(&out, message.parse(":irc CAP * ACK :batch away-notify")),
    );
    try std.testing.expect(!session.enabled.contains("batch"));
    try std.testing.expect(!session.enabled.contains("multi-prefix"));
}

test "CAP LIST 302 refreshes one authoritative enabled snapshot" {
    const gpa = std.testing.allocator;
    const desired = [_][]const u8{ "away-notify", "multi-prefix" };
    var session = Session.init(gpa, .{ .desired = &desired, .hold_cap_end_for_sasl = false });
    defer session.deinit();
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);
    try session.begin(&out);
    _ = try session.handle(&out, message.parse(":irc CAP * LS :away-notify=v2 multi-prefix"));
    _ = try session.handle(&out, message.parse(":irc CAP * ACK :away-notify multi-prefix"));
    out.clearRetainingCapacity();
    try session.requestList(&out);
    try std.testing.expectEqualStrings("CAP LIST\r\n", out.items);
    try std.testing.expectEqual(
        Event.none,
        try session.handle(&out, message.parse(":irc CAP me LIST * :away-notify")),
    );
    try std.testing.expect(session.enabled.contains("multi-prefix"));
    try std.testing.expectEqual(
        Event.capabilities_changed,
        try session.handle(&out, message.parse(":irc CAP me LIST :")),
    );
    try std.testing.expect(session.enabled.contains("away-notify"));
    try std.testing.expectEqualStrings("v2", session.enabled.get("away-notify").?.value.?);
    try std.testing.expect(!session.enabled.contains("multi-prefix"));
    try std.testing.expect(session.enabled.contains("cap-notify"));
}
