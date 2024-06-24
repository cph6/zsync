const std = @import("std");

// Although this function looks imperative, note that its job is to
// declaratively construct a build graph that will be executed by an external
// runner.
pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const config = b.addConfigHeader(
        .{
            .style = .blank,
            .include_path = "config.h",
        },
        .{
            .HAVE_FSEEKO = 1,
            .HAVE_GETADDRINFO = 1,
            .HAVE_INTTYPES_H = 1,
            .HAVE_LIBSOCKET = 1,
            .HAVE_MEMCPY = 1,
            .HAVE_MEMORY_H = 1,
            .HAVE_MKSTEMP = 1,
            .HAVE_PREAD = 1,
            .HAVE_PWRITE = 1,
            .HAVE_STDINT_H = 1,
            .HAVE_STDLIB_H = 1,
            .HAVE_STRINGS_H = 1,
            .HAVE_STRING_H = 1,
            .HAVE_SYS_STAT_H = 1,
            .HAVE_SYS_TYPES_H = 1,
            .HAVE_UNISTD_H = 1,
            .H_ERRNO_DECLARED = 1,
            .PACKAGE = "zsync",
            .PACKAGE_BUGREPORT = "http://zsync.moria.org.uk/",
            .PACKAGE_NAME = "zsync",
            .PACKAGE_STRING = "zsync 0.6.2",
            .PACKAGE_TARNAME = "zsync",
            .PACKAGE_URL = "",
            .PACKAGE_VERSION = "0.6.2",
            .SIZEOF_OFF_T = 8,
            .SIZEOF_SIZE_T = 8,
            .STDC_HEADERS = 1,
            .VERSION = "0.6.2",
            ._XOPEN_SOURCE = 600,
            ._BSD_SOURCE = 1,
        },
    );

    const librcksum = b.addStaticLibrary(.{
        .name = "librcksum",
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });

    librcksum.addCSourceFiles(.{
        .files = &[_][]const u8{
            "librcksum/state.c",
            "librcksum/hash.c",
            "librcksum/range.c",
            "librcksum/rsum.c",
            "librcksum/md4.c",
        },
        .flags = &.{
            "-std=c11",
            "-DHAVE_CONFIG_H",
        },
    });

    librcksum.addIncludePath(b.path("./"));
    librcksum.addConfigHeader(config);

    const zlib = b.addStaticLibrary(.{
        .name = "zlib",
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });

    zlib.addCSourceFiles(.{
        .files = &[_][]const u8{
            "zlib/inflate.c",
            "zlib/zutil.c",
            "zlib/crc32.c",
            "zlib/adler32.c",
            "zlib/inftrees.c",
            "zlib/deflate.c",
            "zlib/trees.c",
            "zlib/compress.c",
        },
        .flags = &.{
            "-std=c11",
        },
    });

    const libzsync = b.addStaticLibrary(.{
        .name = "libzsync",
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });

    libzsync.addCSourceFiles(.{
        .files = &[_][]const u8{
            "libzsync/zsync.c",
            "libzsync/zmap.c",
            "libzsync/sha1.c",
        },
        .flags = &.{
            "-std=c11",
            "-DHAVE_CONFIG_H",
        },
    });

    libzsync.addIncludePath(b.path("./"));
    libzsync.addConfigHeader(config);

    libzsync.linkLibrary(zlib);
    libzsync.linkLibrary(librcksum);

    const zzsync_util = b.addStaticLibrary(.{
        .name = "zzsync_util",
        .target = target,
        .optimize = optimize,
        .root_source_file = b.path("src/util.zig"),
        .link_libc = true,
    });

    const zzsync = b.addExecutable(.{
        .name = "zsync",
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });

    zzsync.addCSourceFiles(.{
        .files = &[_][]const u8{
            "client.c",
            "progress.c",
            "url.c",
        },
        .flags = &.{
            "-std=c11",
            "-DHAVE_CONFIG_H",
            "-fno-sanitize=undefined",
        },
    });
    zzsync.addIncludePath(b.path("src/zig_headers/"));

    zzsync.addConfigHeader(config);
    zzsync.linkLibrary(libzsync);
    zzsync.linkLibrary(zzsync_util);

    const zsyncmake = b.addExecutable(.{
        .name = "zsyncmake",
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });

    zsyncmake.addCSourceFiles(.{
        .files = &[_][]const u8{
            "make.c",
            "makegz.c",
            "progress.c",
        },
        .flags = &.{
            "-std=c11",
            "-DHAVE_CONFIG_H",
        },
    });
    zsyncmake.addIncludePath(b.path("./"));
    zsyncmake.addIncludePath(b.path("stc/zig_headers"));

    zsyncmake.addConfigHeader(config);

    zsyncmake.linkLibrary(zlib);
    zsyncmake.linkLibrary(libzsync);
    zsyncmake.linkLibrary(zzsync_util);

    b.installArtifact(zzsync);
    b.installArtifact(zsyncmake);

    // This *creates* a Run step in the build graph, to be executed when another
    // step is evaluated that depends on it. The next line below will establish
    // such a dependency.
    const run_cmd = b.addRunArtifact(zzsync);

    // By making the run step depend on the install step, it will be run from the
    // installation directory rather than directly from within the cache directory.
    // This is not necessary, however, if the application depends on other installed
    // files, this ensures they will be present and in the expected location.
    run_cmd.step.dependOn(b.getInstallStep());

    // This allows the user to pass arguments to the application in the build
    // command itself, like this: `zig build run -- arg1 arg2 etc`
    if (b.args) |args| {
        run_cmd.addArgs(args);
    }

    // This creates a build step. It will be visible in the `zig build --help` menu,
    // and can be selected like this: `zig build run`
    // This will evaluate the `run` step rather than the default, which is "install".
    const run_step = b.step("run", "Run the app");
    run_step.dependOn(&run_cmd.step);
}
