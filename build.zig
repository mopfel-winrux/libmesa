const std = @import("std");

pub fn build(b: *std.Build) !void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const t = target.result;

    const copts: []const []const u8 =
        b.option([]const []const u8, "copt", "") orelse &.{};

    //
    //  C flags, following vere's urbit_flags
    //

    var flags = std.array_list.Managed([]const u8).init(b.allocator);
    defer flags.deinit();

    try flags.appendSlice(&.{
        "-std=gnu23",
        "-Wall",
        "-Werror",
        "-Wimplicit-fallthrough",
        "-Wno-deprecated-non-prototype",
        "-Wno-gnu",
        "-Wno-unused-variable",
        "-Wno-unused-function",
        "-Wno-unused-but-set-variable",
        "-fno-omit-frame-pointer",
        "-fms-extensions",
        "-DU3_OS_ENDIAN_little=1",
        "-DU3_OS_PROF=1",
    });

    switch (t.os.tag) {
        .macos => try flags.append("-DU3_OS_osx=1"),
        .linux => try flags.append("-DU3_OS_linux=1"),
        .windows => try flags.append("-DU3_OS_windows=1"),
        else => return error.UnsupportedOs,
    }

    try flags.appendSlice(copts);

    //
    //  urcrypt subset: blake3, chacha (monocypher), ed25519
    //

    const urcrypt = b.dependency("urcrypt", .{});
    const crypt = libcrypt(b, target, optimize, urcrypt);

    //
    //  sqlite (the default store)
    //

    const sqlite = b.dependency("sqlite", .{});
    const sql = b.addLibrary(.{
        .name = "mesa-sqlite",
        .linkage = .static,
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
        }),
    });
    sql.linkLibC();
    sql.addCSourceFiles(.{
        .root = sqlite.path("."),
        .files = &.{"sqlite3.c"},
        .flags = &.{
            "-O2",
            "-fno-sanitize=all",
            "-DSQLITE_DQS=0",
            "-DSQLITE_DEFAULT_MEMSTATUS=0",
            "-DSQLITE_OMIT_LOAD_EXTENSION",
            "-DSQLITE_OMIT_DEPRECATED",
            "-DSQLITE_LIKE_DOESNT_MATCH_BLOBS",
        },
    });
    sql.installHeader(sqlite.path("sqlite3.h"), "sqlite3.h");

    //
    //  libmesa
    //

    const lib = b.addLibrary(.{
        .name = "mesa",
        .linkage = .static,
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
        }),
    });

    lib.linkLibC();
    lib.linkLibrary(crypt);
    lib.linkLibrary(sql);
    lib.addIncludePath(sqlite.path("."));

    //  Debug builds are compiled with UBSan; bundle its runtime so the
    //  archive links from non-zig toolchains
    lib.bundle_ubsan_rt = true;

    addIncludes(b, lib, urcrypt);

    lib.addCSourceFiles(.{
        .root = b.path("vendor"),
        .files = &.{"c3/defs.c"},
        .flags = flags.items,
    });

    lib.addCSourceFiles(.{
        .root = b.path("vendor/murmur3"),
        .files = &.{"murmur3.c"},
        .flags = &.{ "-O3", "-fno-sanitize=all" },
    });

    lib.addCSourceFiles(.{
        .root = b.path("src"),
        .files = &.{
            "util.c",
            "ship.c",
            "noun.c",
            "crypto.c",
            "sha.c",
            "ident.c",
            "json.c",
            "azimuth.c",
            "knot.c",
            "store_mem.c",
            "store_sqlite.c",
            "gall.c",
            "eng/eng_util.c",
            "eng/eng_space.c",
            "eng/eng_serve.c",
            "eng/eng_peer.c",
            "eng/eng_fetch.c",
            "eng/eng_flow.c",
            "eng/engine.c",
            "wire/bitset.c",
            "wire/lss.c",
            "wire/pact.c",
        },
        .flags = flags.items,
    });

    lib.installHeadersDirectory(b.path("include/mesa"), "mesa", .{});

    b.installArtifact(lib);
    b.installArtifact(crypt);
    b.installArtifact(sql);

    //
    //  tests
    //

    addE2e(b, target, optimize, lib, crypt, sql, urcrypt, flags.items);

    //
    //  libmesa-host: libuv event loop, galaxy DNS, azimuth over HTTPS
    //

    const no_host = b.option(bool, "no-host", "Don't build libmesa-host (libuv, curl, OpenSSL)") orelse false;
    if (!no_host) {
        const no_lto = target.result.os.tag == .macos;
        if (b.lazyDependency("libuv", .{ .target = target, .optimize = optimize, .no_lto = no_lto })) |uv| {
            if (b.lazyDependency("curl", .{ .target = target, .optimize = optimize, .no_lto = no_lto })) |curl| {
                const host = b.addLibrary(.{
                    .name = "mesa-host",
                    .linkage = .static,
                    .root_module = b.createModule(.{
                        .target = target,
                        .optimize = optimize,
                    }),
                });
                host.linkLibC();
                host.linkLibrary(lib);
                host.linkLibrary(uv.artifact("libuv"));
                host.linkLibrary(curl.artifact("curl"));
                host.bundle_ubsan_rt = true;
                addIncludes(b, host, urcrypt);
                host.addCSourceFiles(.{
                    .root = b.path("src"),
                    .files = &.{"host/host.c"},
                    .flags = flags.items,
                });
                b.installArtifact(host);
                b.installArtifact(uv.artifact("libuv"));
                b.installArtifact(curl.artifact("curl"));

                //  mesa-ping: poke a ship from the command line
                //
                const tool = b.addExecutable(.{
                    .name = "mesa-ping",
                    .root_module = b.createModule(.{
                        .target = target,
                        .optimize = optimize,
                    }),
                });
                tool.linkLibC();
                tool.linkLibrary(host);
                tool.linkLibrary(uv.artifact("libuv"));
                tool.linkLibrary(curl.artifact("curl"));
                tool.linkLibrary(lib);
                tool.linkLibrary(crypt);
                tool.linkLibrary(sql);
                addIncludes(b, tool, urcrypt);
                tool.addCSourceFiles(.{ .files = &.{"tools/mesa-ping.c"}, .flags = flags.items });
                b.installArtifact(tool);
            }
        }
    }

    const test_step = b.step("test", "Run unit tests");

    const tests = [_]struct { name: []const u8, file: []const u8, defs: []const []const u8 }{
        .{ .name = "pact-test", .file = "test/pact_test.c", .defs = &.{} },
        .{ .name = "ship-test", .file = "test/ship_test.c", .defs = &.{} },
        .{ .name = "lss-test", .file = "test/lss_test.c", .defs = &.{} },
        .{ .name = "bitset-test", .file = "test/bitset_test.c", .defs = &.{} },
        .{ .name = "noun-test", .file = "test/noun_test.c", .defs = &.{} },
        .{ .name = "crypto-test", .file = "test/crypto_test.c", .defs = &.{} },
        .{ .name = "ident-test", .file = "test/ident_test.c", .defs = &.{} },
        .{ .name = "azimuth-test", .file = "test/azimuth_test.c", .defs = &.{} },
        .{ .name = "engine-test", .file = "test/engine_test.c", .defs = &.{} },
        .{ .name = "store-test", .file = "test/store_test.c", .defs = &.{} },
        .{ .name = "gall-test", .file = "test/gall_test.c", .defs = &.{} },
    };

    for (tests) |tst| {
        const exe = b.addExecutable(.{
            .name = tst.name,
            .root_module = b.createModule(.{
                .target = target,
                .optimize = optimize,
            }),
        });

        exe.linkLibC();
        exe.linkLibrary(lib);
        exe.linkLibrary(crypt);
        exe.linkLibrary(sql);
        exe.addIncludePath(sqlite.path("."));

        addIncludes(b, exe, urcrypt);

        exe.addCSourceFiles(.{
            .files = &.{tst.file},
            .flags = flags.items,
        });

        const run = b.addRunArtifact(exe);
        run.has_side_effects = true;
        test_step.dependOn(&run.step);

        const build_only = b.step(tst.name, b.fmt("Build and run {s}", .{tst.name}));
        build_only.dependOn(&run.step);
    }
}

fn addE2e(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    lib: *std.Build.Step.Compile,
    crypt: *std.Build.Step.Compile,
    sql: *std.Build.Step.Compile,
    urcrypt: *std.Build.Dependency,
    flags: []const []const u8,
) void {
    const exe = b.addExecutable(.{
        .name = "mesa-e2e",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
        }),
    });
    exe.linkLibC();
    exe.linkLibrary(lib);
    exe.linkLibrary(crypt);
    exe.linkLibrary(sql);
    addIncludes(b, exe, urcrypt);
    exe.addCSourceFiles(.{ .files = &.{"test/e2e/e2e.c"}, .flags = flags });

    const step = b.step("e2e", "Build the interop test against a fake ~zod");
    step.dependOn(&b.addInstallArtifact(exe, .{}).step);
}

fn addIncludes(
    b: *std.Build,
    c: *std.Build.Step.Compile,
    urcrypt: *std.Build.Dependency,
) void {
    c.addIncludePath(b.path("include"));
    c.addIncludePath(b.path("src"));
    c.addIncludePath(b.path("src/wire"));
    c.addIncludePath(b.path("src/eng"));
    c.addIncludePath(b.path("vendor"));
    c.addIncludePath(b.path("vendor/murmur3"));
    c.addIncludePath(b.path("vendor/verstable"));
    c.addIncludePath(urcrypt.path("urcrypt"));
    c.addIncludePath(urcrypt.path("blake3"));
    c.addIncludePath(urcrypt.path("ed25519/src"));
}

fn libcrypt(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    dep: *std.Build.Dependency,
) *std.Build.Step.Compile {
    const t = target.result;

    const lib = b.addLibrary(.{
        .name = "mesa-urcrypt",
        .linkage = .static,
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
        }),
    });

    lib.linkLibC();

    lib.addIncludePath(dep.path("urcrypt"));
    lib.addIncludePath(dep.path("blake3"));
    lib.addIncludePath(dep.path("ed25519/src"));
    lib.addIncludePath(dep.path("ge-additions"));
    lib.addIncludePath(dep.path("monocypher"));

    const cflags: []const []const u8 = &.{
        "-O2",
        "-fno-omit-frame-pointer",
        "-fno-sanitize=all",
        "-Wno-unused-result",
    };

    //  urcrypt wrappers
    //
    lib.addCSourceFiles(.{
        .root = dep.path("urcrypt"),
        .files = &.{
            "blake3.c",
            "chacha.c",
            "ed25519.c",
            "ge_additions.c",
            "util.c",
        },
        .flags = cflags,
    });

    //  blake3
    //
    lib.addCSourceFiles(.{
        .root = dep.path("blake3"),
        .files = &.{
            "blake3.c",
            "blake3_dispatch.c",
            "blake3_portable.c",
        },
        .flags = cflags,
    });

    if (t.cpu.arch == .x86_64 and t.os.tag != .windows) {
        lib.addCSourceFiles(.{
            .root = dep.path("blake3"),
            .files = &.{
                "blake3_sse2_x86-64_unix.S",
                "blake3_sse41_x86-64_unix.S",
                "blake3_avx2_x86-64_unix.S",
                "blake3_avx512_x86-64_unix.S",
            },
            .flags = cflags,
        });
    } else if (t.cpu.arch == .aarch64) {
        lib.addCSourceFiles(.{
            .root = dep.path("blake3"),
            .files = &.{"blake3_neon.c"},
            .flags = &.{ "-O2", "-DBLAKE3_USE_NEON=1" },
        });
    } else {
        lib.root_module.addCMacro("BLAKE3_NO_SSE2", "1");
        lib.root_module.addCMacro("BLAKE3_NO_SSE41", "1");
        lib.root_module.addCMacro("BLAKE3_NO_AVX2", "1");
        lib.root_module.addCMacro("BLAKE3_NO_AVX512", "1");
    }

    //  ed25519 (orlp) + ge-additions
    //
    lib.addCSourceFiles(.{
        .root = dep.path("ed25519/src"),
        .files = &.{
            "add_scalar.c",
            "fe.c",
            "ge.c",
            "keypair.c",
            "key_exchange.c",
            "sc.c",
            "seed.c",
            "sha512.c",
            "sign.c",
            "verify.c",
        },
        .flags = cflags,
    });

    lib.addCSourceFiles(.{
        .root = dep.path("ge-additions"),
        .files = &.{"ge-additions.c"},
        .flags = cflags,
    });

    //  monocypher (chacha)
    //
    lib.addCSourceFiles(.{
        .root = dep.path("monocypher"),
        .files = &.{"monocypher.c"},
        .flags = cflags,
    });

    lib.installHeader(dep.path("urcrypt/urcrypt.h"), "urcrypt.h");

    return lib;
}
