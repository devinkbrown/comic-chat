//! Microsoft Comic Chat 2.5 panel-page sequencing, separated from rendering.
//!
//! This is a direct state-machine port of the MIT-licensed source at upstream
//! commit `c7df00f60bc8e9fdef413f139e61f7c37e024684`:
//! - `panel.h:68-91`: `CPage::m_newPanel` and `StartNewPanel`.
//! - `panel.cpp:552-582`: panel seed construction and clone preservation.
//! - `panel.cpp:1058-1138`: `CUnitPanelPage::AddLine`.
//! - `panel.cpp:1141-1182`: `CUnitPanelPage::AddReaction` (`<Chr>`).
//! - `panel.cpp:1279-1297`: the title panel which makes `panel count < 2`
//!   force the first conversational panel.
//!
//! Rendering is deliberately a caller responsibility. `begin` produces a
//! candidate panel, the renderer lays out avatars and balloons against that
//! candidate, and `finish` applies the source's replace/append/retry behavior.
//! Before returning an attempt, `begin` also performs the source's
//! `srand(panel.seed)`, so the same `MsvcrtRand` must be passed to the balloon
//! layout port and any random draws it performs.

const std = @import("std");

pub const max_elements_per_panel: usize = 5;
pub const max_bodies_per_panel: usize = 5;

pub const bm_say: u16 = 0x0001;
pub const bm_whisper: u16 = 0x0002;
pub const bm_think: u16 = 0x0004;
pub const bm_action: u16 = 0x0008;
pub const bm_sound: u16 = 0x0010;

/// The Microsoft Visual C runtime generator used by the original application.
/// Its default process seed is one. The original stores `rand()` in each new
/// panel, while a clone preserves its source panel's seed.
pub const MsvcrtRand = struct {
    state: u32 = 1,

    pub fn init(seed: u32) MsvcrtRand {
        return .{ .state = seed };
    }

    pub fn srand(self: *MsvcrtRand, seed: u32) void {
        self.state = seed;
    }

    pub fn rand(self: *MsvcrtRand) u15 {
        self.state = self.state *% 214013 +% 2531011;
        return @truncate(self.state >> 16);
    }

    /// Direct port of `randfloat()` in `balloon.cpp:428-431`.
    pub fn randFloat(self: *MsvcrtRand) f64 {
        return @as(f64, @floatFromInt(self.rand())) / 32767.0;
    }
};

pub const PanelKind = enum { title, conversation };

/// The page-level facts needed by AddLine. Rich avatar/balloon objects remain
/// in the renderer; it can update `body_ids` on an attempt before `finish`.
pub const Panel = struct {
    kind: PanelKind,
    seed: u32,
    element_count: usize = 0,
    body_count: usize = 0,
    body_ids: [max_bodies_per_panel]u32 = undefined,

    pub fn avatarInPanel(self: Panel, avatar_id: u32) bool {
        return std.mem.indexOfScalar(u32, self.body_ids[0..self.body_count], avatar_id) != null;
    }

    /// Mirrors `FetchSpeaker`: preserve an existing body or append it.
    pub fn fetchSpeaker(self: *Panel, avatar_id: u32) Error!void {
        if (self.avatarInPanel(avatar_id)) return;
        if (self.body_count == max_bodies_per_panel) return error.TooManyBodies;
        self.body_ids[self.body_count] = avatar_id;
        self.body_count += 1;
    }

    /// Lets avatar layout publish the final requested/talk-to body set which
    /// later `AvatarInPanel` and `<Chr>` decisions observe.
    pub fn setBodies(self: *Panel, ids: []const u32) Error!void {
        if (ids.len > max_bodies_per_panel) return error.TooManyBodies;
        for (ids, 0..) |id, i| {
            if (std.mem.indexOfScalar(u32, ids[0..i], id) != null) return error.DuplicateBody;
        }
        @memcpy(self.body_ids[0..ids.len], ids);
        self.body_count = ids.len;
    }
};

/// `cookie` carries renderer-owned continuation state such as split formatting
/// and URL position without coupling this planner to a text implementation.
pub const Line = struct {
    speaker_id: u32,
    words: []const u8,
    modes: u16 = bm_say,
    cookie: usize = 0,
};

pub const AttemptKind = enum { line, reaction };

pub const Attempt = struct {
    kind: AttemptKind,
    request: Line,
    panel: Panel,
    replace_last: bool,
};

pub const Begin = union(enum) {
    /// `<Brk>` is consumed and only arms `StartNewPanel`.
    forced_break,
    attempt: Attempt,
};

pub const Continuation = struct {
    words: []const u8,
    cookie: usize = 0,
};

pub const FitResult = struct {
    fit: bool,
    continuation: ?Continuation = null,
};

pub const Next = union(enum) {
    done,
    /// A cloned/multi-balloon candidate did not fit. The caller must submit
    /// this request to `begin` again; `new_panel` is already armed.
    retry: Line,
    /// A sole balloon was force-fit and split. This is the exact recursive
    /// AddLine request, retaining speaker and modes and replacing split state.
    continuation: Line,
};

pub const Error = error{
    EmptyPage,
    TooManyBodies,
    DuplicateBody,
    UnexpectedContinuation,
} || std.mem.Allocator.Error;

pub const Planner = struct {
    panels: std.ArrayList(Panel) = .empty,
    /// Direct equivalent of `CPage::m_newPanel`, whose constructor initializes
    /// it to TRUE. `AddTitle` does not clear it.
    new_panel: bool = true,

    /// Construct the normal Comic Chat page, including its title panel. The
    /// title CUnitPanel consumes a panel seed just like the source does.
    pub fn init(gpa: std.mem.Allocator, rng: *MsvcrtRand) std.mem.Allocator.Error!Planner {
        var self: Planner = .{};
        errdefer self.deinit(gpa);
        try self.panels.append(gpa, .{
            .kind = .title,
            .seed = rng.rand(),
            // `AddTitle` installs title and "Starring" labels. Stars are art,
            // but are irrelevant to AddLine's title-panel accounting.
            .element_count = 2,
        });
        return self;
    }

    pub fn deinit(self: *Planner, gpa: std.mem.Allocator) void {
        self.panels.deinit(gpa);
        self.* = undefined;
    }

    pub fn startNewPanel(self: *Planner) void {
        self.new_panel = true;
    }

    pub fn panelCount(self: Planner) usize {
        return self.panels.items.len;
    }

    pub fn lastPanel(self: *Planner) Error!*Panel {
        if (self.panels.items.len == 0) return error.EmptyPage;
        return &self.panels.items[self.panels.items.len - 1];
    }

    /// Port of AddLine's preflight and AddReaction dispatch. The returned
    /// candidate is not committed until `finish`, matching the temporary
    /// CUnitPanel allocation/clone in the C++ source.
    pub fn begin(self: *Planner, rng: *MsvcrtRand, request: Line) Error!Begin {
        // Equality is intentional: BM_ACTION|BM_WHISPER made a box but did not
        // trigger this source branch.
        if (request.modes == bm_action) self.startNewPanel();

        if (std.mem.eql(u8, request.words, "<Brk>")) {
            self.startNewPanel();
            return .forced_break;
        }
        if (std.mem.eql(u8, request.words, "<Chr>"))
            return .{ .attempt = try self.beginReaction(rng, request) };

        const old = try self.lastPanel();
        const make_new = self.new_panel or
            old.element_count >= max_elements_per_panel or
            self.panelCount() < 2 or
            old.avatarInPanel(request.speaker_id);

        var attempt = try self.makeAttempt(rng, request, .line, make_new);
        // Source order is MakeBalloon/FetchSpeaker, append balloon, ReplaceBody.
        // Only the page-visible counts/identity are represented here.
        try attempt.panel.fetchSpeaker(request.speaker_id);
        attempt.panel.element_count += 1;
        rng.srand(attempt.panel.seed);
        return .{ .attempt = attempt };
    }

    fn beginReaction(self: *Planner, rng: *MsvcrtRand, request: Line) Error!Attempt {
        const old = try self.lastPanel();
        const make_new = self.new_panel or
            old.body_count >= max_bodies_per_panel or
            self.panelCount() < 2;
        var attempt = try self.makeAttempt(rng, request, .reaction, make_new);
        // AddReaction does ReplaceBody and falls back to FetchSpeaker; it does
        // not append a balloon and has no repeated-avatar fresh-panel rule.
        try attempt.panel.fetchSpeaker(request.speaker_id);
        rng.srand(attempt.panel.seed);
        return attempt;
    }

    fn makeAttempt(
        self: *Planner,
        rng: *MsvcrtRand,
        request: Line,
        kind: AttemptKind,
        make_new: bool,
    ) Error!Attempt {
        if (make_new) {
            self.new_panel = false;
            return .{
                .kind = kind,
                .request = request,
                .panel = .{ .kind = .conversation, .seed = rng.rand() },
                .replace_last = false,
            };
        }
        return .{
            .kind = kind,
            .request = request,
            .panel = (try self.lastPanel()).*,
            .replace_last = true,
        };
    }

    /// Apply `LayoutBalloons`' result. A failed candidate is discarded and
    /// converted into StartNewPanel + recursive AddLine/AddReaction. A success
    /// replaces its clone source or appends a fresh panel before recursively
    /// submitting any force-fit continuation.
    pub fn finish(
        self: *Planner,
        gpa: std.mem.Allocator,
        attempt: Attempt,
        result: FitResult,
    ) Error!Next {
        if (!result.fit) {
            self.startNewPanel();
            return .{ .retry = attempt.request };
        }
        if (attempt.kind == .reaction and result.continuation != null)
            return error.UnexpectedContinuation;

        if (attempt.replace_last) {
            (try self.lastPanel()).* = attempt.panel;
        } else {
            try self.panels.append(gpa, attempt.panel);
        }

        if (result.continuation) |rest| {
            return .{ .continuation = .{
                .speaker_id = attempt.request.speaker_id,
                .words = rest.words,
                .modes = attempt.request.modes,
                .cookie = rest.cookie,
            } };
        }
        return .done;
    }
};

fn expectAttempt(begin_result: Begin) !Attempt {
    return switch (begin_result) {
        .attempt => |attempt| attempt,
        .forced_break => error.TestExpectedEqual,
    };
}

fn accept(
    planner: *Planner,
    gpa: std.mem.Allocator,
    attempt: Attempt,
) !Next {
    return planner.finish(gpa, attempt, .{ .fit = true });
}

test "MSVCRT rand and randfloat match the source runtime" {
    var rng = MsvcrtRand{};
    try std.testing.expectEqual(@as(u15, 41), rng.rand());
    try std.testing.expectEqual(@as(u15, 18467), rng.rand());
    try std.testing.expectEqual(@as(u15, 6334), rng.rand());

    rng.srand(1);
    try std.testing.expectApproxEqAbs(@as(f64, 41.0 / 32767.0), rng.randFloat(), 0.0000001);
}

test "title panel forces first line into a new conversational panel" {
    const gpa = std.testing.allocator;
    var rng = MsvcrtRand{};
    var planner = try Planner.init(gpa, &rng);
    defer planner.deinit(gpa);

    try std.testing.expectEqual(@as(usize, 1), planner.panelCount());
    try std.testing.expectEqual(PanelKind.title, planner.panels.items[0].kind);
    const title_seed = planner.panels.items[0].seed;

    const attempt = try expectAttempt(try planner.begin(&rng, .{ .speaker_id = 7, .words = "hello" }));
    try std.testing.expect(!attempt.replace_last);
    try std.testing.expect(attempt.panel.seed != title_seed);
    try std.testing.expectEqual(@as(usize, 1), attempt.panel.element_count);
    try std.testing.expect(attempt.panel.avatarInPanel(7));
    _ = try accept(&planner, gpa, attempt);
    try std.testing.expectEqual(@as(usize, 2), planner.panelCount());
}

test "different speaker clones while repeated speaker starts fresh" {
    const gpa = std.testing.allocator;
    var rng = MsvcrtRand{};
    var planner = try Planner.init(gpa, &rng);
    defer planner.deinit(gpa);

    const first = try expectAttempt(try planner.begin(&rng, .{ .speaker_id = 1, .words = "one" }));
    _ = try accept(&planner, gpa, first);

    const second = try expectAttempt(try planner.begin(&rng, .{ .speaker_id = 2, .words = "two" }));
    try std.testing.expect(second.replace_last);
    try std.testing.expectEqual(first.panel.seed, second.panel.seed);
    _ = try accept(&planner, gpa, second);
    try std.testing.expectEqual(@as(usize, 2), planner.panelCount());
    try std.testing.expectEqual(@as(usize, 2), (try planner.lastPanel()).element_count);

    const repeat = try expectAttempt(try planner.begin(&rng, .{ .speaker_id = 1, .words = "again" }));
    try std.testing.expect(!repeat.replace_last);
    _ = try accept(&planner, gpa, repeat);
    try std.testing.expectEqual(@as(usize, 3), planner.panelCount());
}

test "five existing elements force a fresh panel" {
    const gpa = std.testing.allocator;
    var rng = MsvcrtRand{};
    var planner = try Planner.init(gpa, &rng);
    defer planner.deinit(gpa);

    const first = try expectAttempt(try planner.begin(&rng, .{ .speaker_id = 1, .words = "one" }));
    _ = try accept(&planner, gpa, first);
    const last = try planner.lastPanel();
    last.element_count = max_elements_per_panel;

    const attempt = try expectAttempt(try planner.begin(&rng, .{ .speaker_id = 2, .words = "six" }));
    try std.testing.expect(!attempt.replace_last);
}

test "failed clone is discarded then retried as a fresh panel" {
    const gpa = std.testing.allocator;
    var rng = MsvcrtRand{};
    var planner = try Planner.init(gpa, &rng);
    defer planner.deinit(gpa);

    const first = try expectAttempt(try planner.begin(&rng, .{ .speaker_id = 1, .words = "one" }));
    _ = try accept(&planner, gpa, first);
    const committed_seed = (try planner.lastPanel()).seed;

    const clone = try expectAttempt(try planner.begin(&rng, .{ .speaker_id = 2, .words = "does not fit" }));
    try std.testing.expect(clone.replace_last);
    // Simulate random draws performed by LayoutBalloons after begin's srand.
    _ = rng.rand();
    const retry = try planner.finish(gpa, clone, .{ .fit = false });
    try std.testing.expectEqual(@as(usize, 2), planner.panelCount());
    const request = switch (retry) {
        .retry => |line| line,
        else => return error.TestExpectedEqual,
    };
    const fresh = try expectAttempt(try planner.begin(&rng, request));
    try std.testing.expect(!fresh.replace_last);
    try std.testing.expect(fresh.panel.seed != committed_seed);
    _ = try accept(&planner, gpa, fresh);
    try std.testing.expectEqual(@as(usize, 3), planner.panelCount());
}

test "force-fit continuation recursively retains identity mode and split cookie" {
    const gpa = std.testing.allocator;
    var rng = MsvcrtRand{};
    var planner = try Planner.init(gpa, &rng);
    defer planner.deinit(gpa);

    const first = try expectAttempt(try planner.begin(&rng, .{
        .speaker_id = 42,
        .words = "a very long utterance",
        .modes = bm_think,
        .cookie = 10,
    }));
    const next = try planner.finish(gpa, first, .{
        .fit = true,
        .continuation = .{ .words = "...remainder", .cookie = 11 },
    });
    const continuation = switch (next) {
        .continuation => |line| line,
        else => return error.TestExpectedEqual,
    };
    try std.testing.expectEqual(@as(u32, 42), continuation.speaker_id);
    try std.testing.expectEqual(bm_think, continuation.modes);
    try std.testing.expectEqual(@as(usize, 11), continuation.cookie);
    try std.testing.expectEqualStrings("...remainder", continuation.words);

    // Recursive AddLine sees the same avatar in the just-committed panel.
    const second = try expectAttempt(try planner.begin(&rng, continuation));
    try std.testing.expect(!second.replace_last);
}

test "action equality and Brk implement exact source break behavior" {
    const gpa = std.testing.allocator;
    var rng = MsvcrtRand{};
    var planner = try Planner.init(gpa, &rng);
    defer planner.deinit(gpa);

    const first = try expectAttempt(try planner.begin(&rng, .{ .speaker_id = 1, .words = "one" }));
    _ = try accept(&planner, gpa, first);

    const combined = try expectAttempt(try planner.begin(&rng, .{
        .speaker_id = 2,
        .words = "whispered action",
        .modes = bm_action | bm_whisper,
    }));
    try std.testing.expect(combined.replace_last);

    const action = try expectAttempt(try planner.begin(&rng, .{
        .speaker_id = 2,
        .words = "action",
        .modes = bm_action,
    }));
    try std.testing.expect(!action.replace_last);

    const broken = try planner.begin(&rng, .{ .speaker_id = 9, .words = "<Brk>" });
    try std.testing.expect(broken == .forced_break);
    try std.testing.expect(planner.new_panel);
}

test "Chr is a reaction with no balloon and no repeated-speaker break" {
    const gpa = std.testing.allocator;
    var rng = MsvcrtRand{};
    var planner = try Planner.init(gpa, &rng);
    defer planner.deinit(gpa);

    const first = try expectAttempt(try planner.begin(&rng, .{ .speaker_id = 7, .words = "hello" }));
    _ = try accept(&planner, gpa, first);

    const reaction = try expectAttempt(try planner.begin(&rng, .{ .speaker_id = 7, .words = "<Chr>" }));
    try std.testing.expectEqual(AttemptKind.reaction, reaction.kind);
    try std.testing.expect(reaction.replace_last);
    try std.testing.expectEqual(@as(usize, 1), reaction.panel.element_count);
    _ = try accept(&planner, gpa, reaction);
    try std.testing.expectEqual(@as(usize, 2), planner.panelCount());
}

test "five reaction bodies force a fresh panel and reactions cannot continue" {
    const gpa = std.testing.allocator;
    var rng = MsvcrtRand{};
    var planner = try Planner.init(gpa, &rng);
    defer planner.deinit(gpa);

    const first = try expectAttempt(try planner.begin(&rng, .{ .speaker_id = 1, .words = "one" }));
    _ = try accept(&planner, gpa, first);
    try (try planner.lastPanel()).setBodies(&.{ 1, 2, 3, 4, 5 });

    const reaction = try expectAttempt(try planner.begin(&rng, .{ .speaker_id = 6, .words = "<Chr>" }));
    try std.testing.expect(!reaction.replace_last);
    try std.testing.expectError(error.UnexpectedContinuation, planner.finish(gpa, reaction, .{
        .fit = true,
        .continuation = .{ .words = "impossible" },
    }));
}
