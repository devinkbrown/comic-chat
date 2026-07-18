const std = @import("std");

// comicchat: a modern, cross-platform Zig port of Microsoft Comic Chat 2.5.
// Platform backends present one shared core; official mbedTLS provides the
// portable verified TLS transport.
pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const mbedtls_dep = b.dependency("mbedtls", .{});
    const mbedtls = addMbedTls(b, mbedtls_dep, target, optimize);

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
    exe.root_module.linkLibrary(mbedtls);
    addTlsSystemLibraries(exe, target);
    b.installArtifact(exe);

    // `zig build run`
    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());
    run_cmd.addPassthruArgs();
    const run_step = b.step("run", "Run the app");
    run_step.dependOn(&run_cmd.step);

    // `zig build test` — runs all inline tests in the library module.
    const mod_tests = b.addTest(.{ .root_module = mod });
    mod_tests.root_module.linkLibrary(mbedtls);
    addTlsSystemLibraries(mod_tests, target);
    const run_mod_tests = b.addRunArtifact(mod_tests);
    const test_step = b.step("test", "Run unit tests");
    test_step.dependOn(&run_mod_tests.step);
}

const mbedtls_sources = [_][]const u8{
    "library/aes.c",
    "library/aesni.c",
    "library/aesce.c",
    "library/aria.c",
    "library/asn1parse.c",
    "library/asn1write.c",
    "library/base64.c",
    "library/bignum.c",
    "library/bignum_core.c",
    "library/bignum_mod.c",
    "library/bignum_mod_raw.c",
    "library/block_cipher.c",
    "library/camellia.c",
    "library/ccm.c",
    "library/chacha20.c",
    "library/chachapoly.c",
    "library/cipher.c",
    "library/cipher_wrap.c",
    "library/constant_time.c",
    "library/cmac.c",
    "library/ctr_drbg.c",
    "library/des.c",
    "library/dhm.c",
    "library/ecdh.c",
    "library/ecdsa.c",
    "library/ecjpake.c",
    "library/ecp.c",
    "library/ecp_curves.c",
    "library/ecp_curves_new.c",
    "library/entropy.c",
    "library/entropy_poll.c",
    "library/error.c",
    "library/gcm.c",
    "library/hkdf.c",
    "library/hmac_drbg.c",
    "library/lmots.c",
    "library/lms.c",
    "library/md.c",
    "library/md5.c",
    "library/memory_buffer_alloc.c",
    "library/nist_kw.c",
    "library/oid.c",
    "library/padlock.c",
    "library/pem.c",
    "library/pk.c",
    "library/pk_ecc.c",
    "library/pk_wrap.c",
    "library/pkcs12.c",
    "library/pkcs5.c",
    "library/pkparse.c",
    "library/pkwrite.c",
    "library/platform.c",
    "library/platform_util.c",
    "library/poly1305.c",
    "library/psa_crypto.c",
    "library/psa_crypto_aead.c",
    "library/psa_crypto_cipher.c",
    "library/psa_crypto_client.c",
    "library/psa_crypto_driver_wrappers_no_static.c",
    "library/psa_crypto_ecp.c",
    "library/psa_crypto_ffdh.c",
    "library/psa_crypto_hash.c",
    "library/psa_crypto_mac.c",
    "library/psa_crypto_pake.c",
    "library/psa_crypto_rsa.c",
    "library/psa_crypto_random.c",
    "library/psa_crypto_se.c",
    "library/psa_crypto_slot_management.c",
    "library/psa_crypto_storage.c",
    "library/psa_its_file.c",
    "library/psa_util.c",
    "library/ripemd160.c",
    "library/rsa.c",
    "library/rsa_alt_helpers.c",
    "library/sha1.c",
    "library/sha256.c",
    "library/sha512.c",
    "library/sha3.c",
    "library/threading.c",
    "library/timing.c",
    "library/version.c",
    "library/version_features.c",
    "library/pkcs7.c",
    "library/x509.c",
    "library/x509_create.c",
    "library/x509_crl.c",
    "library/x509_crt.c",
    "library/x509_csr.c",
    "library/x509write.c",
    "library/x509write_crt.c",
    "library/x509write_csr.c",
    "library/debug.c",
    "library/mps_reader.c",
    "library/mps_trace.c",
    "library/net_sockets.c",
    "library/ssl_cache.c",
    "library/ssl_ciphersuites.c",
    "library/ssl_client.c",
    "library/ssl_cookie.c",
    "library/ssl_debug_helpers_generated.c",
    "library/ssl_msg.c",
    "library/ssl_ticket.c",
    "library/ssl_tls.c",
    "library/ssl_tls12_client.c",
    "library/ssl_tls12_server.c",
    "library/ssl_tls13_keys.c",
    "library/ssl_tls13_server.c",
    "library/ssl_tls13_client.c",
    "library/ssl_tls13_generic.c",
};

fn addMbedTls(
    b: *std.Build,
    dependency: *std.Build.Dependency,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
) *std.Build.Step.Compile {
    const library = b.addLibrary(.{
        .name = "comicchat-mbedtls",
        .linkage = .static,
        .root_module = b.createModule(.{ .target = target, .optimize = optimize }),
    });
    library.root_module.addIncludePath(dependency.path("include"));
    library.root_module.addIncludePath(b.path("src/net"));
    library.root_module.addCSourceFiles(.{
        .root = dependency.path(""),
        .files = &mbedtls_sources,
        .flags = &.{"-std=c99"},
    });
    library.root_module.addCSourceFile(.{ .file = b.path("src/net/mbedtls_shim.c"), .flags = &.{"-std=c99"} });
    library.root_module.link_libc = true;
    return library;
}

fn addTlsSystemLibraries(compile: *std.Build.Step.Compile, target: std.Build.ResolvedTarget) void {
    if (target.result.os.tag == .windows) {
        compile.root_module.linkSystemLibrary("ws2_32", .{});
        compile.root_module.linkSystemLibrary("bcrypt", .{});
        compile.root_module.linkSystemLibrary("crypt32", .{});
    }
}
