const std = @import("std");

// comicchat: a modern, cross-platform, *pure Zig* reimplementation of
// Microsoft Comic Chat 2.5. No C interop, no SDL. Windowing/rendering is
// hand-rolled per OS (added later); the core below is platform-independent.
pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // The reusable library: protocol codec, asset decoders, IRC, comic layout.
    const mod = b.addModule("comicchat", .{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
    });

    // The CLI / app entry point.
    const exe = b.addExecutable(.{
        .name = "comicchat",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "comicchat", .module = mod },
            },
        }),
    });
    b.installArtifact(exe);

    // `zig build run`
    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());
    if (b.args) |args| run_cmd.addArgs(args);
    const run_step = b.step("run", "Run the app");
    run_step.dependOn(&run_cmd.step);

    // `zig build test` — runs all inline tests in the library module.
    const mod_tests = b.addTest(.{ .root_module = mod });
    const run_mod_tests = b.addRunArtifact(mod_tests);
    const test_step = b.step("test", "Run unit tests");
    test_step.dependOn(&run_mod_tests.step);
}
