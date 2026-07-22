const std = @import("std");

// comicchat: a modern, cross-platform Zig port of Microsoft Comic Chat 2.5.
// Platform backends present one shared core; Onyx provides verified TLS.
pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const strip = b.option(bool, "strip", "Strip symbols and build-path debug metadata") orelse false;
    const onyx_build_info = b.addOptions();
    onyx_build_info.addOption([]const u8, "git_commit", "06bb350");
    onyx_build_info.addOption([]const u8, "version", "0.5.7");
    const onyx_tls = b.addModule("onyx_tls", .{
        .root_source_file = b.path("onyx_tls_root.zig"),
        .target = target,
        .optimize = optimize,
        .imports = &.{.{ .name = "build_info", .module = onyx_build_info.createModule() }},
    });

    const source_ui_assets = b.createModule(.{
        .root_source_file = b.path("source_ui_assets.zig"),
        .target = target,
    });

    // The reusable library: protocol codec, asset decoders, IRC, comic layout.
    const mod = b.addModule("comicchat", .{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .imports = &.{
            .{ .name = "source_ui_assets", .module = source_ui_assets },
            .{ .name = "onyx_tls", .module = onyx_tls },
        },
    });
    // The CLI / app entry point.
    const exe = b.addExecutable(.{
        .name = "reinked",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = target,
            .optimize = optimize,
            .strip = strip,
            .imports = &.{
                .{ .name = "comicchat", .module = mod },
            },
        }),
    });
    b.installArtifact(exe);

    // `zig build run`
    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());
    run_cmd.addPassthruArgs();
    const run_step = b.step("run", "Run the app");
    run_step.dependOn(&run_cmd.step);

    // `zig build test` — runs all inline tests in the library module.
    const mod_tests = b.addTest(.{ .root_module = mod });
    const run_mod_tests = b.addRunArtifact(mod_tests);
    const main_tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{.{ .name = "comicchat", .module = mod }},
        }),
    });
    const run_main_tests = b.addRunArtifact(main_tests);
    const test_step = b.step("test", "Run unit tests");
    test_step.dependOn(&run_mod_tests.step);
    test_step.dependOn(&run_main_tests.step);
}
