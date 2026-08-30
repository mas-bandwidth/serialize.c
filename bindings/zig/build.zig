const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const exe = b.addExecutable(.{
        .name = "example",
        .root_module = b.createModule(.{
            .root_source_file = b.path("main.zig"),
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        }),
    });

    // serialize.c is header-only: the include path is the whole dependency.
    // The stub serialize.c is compiled too, so a build that links it keeps
    // working unchanged -- and this line is the pattern to copy for any C
    // library that does carry code.
    exe.root_module.addIncludePath(b.path("../.."));
    exe.root_module.addCSourceFile(.{ .file = b.path("../../serialize.c") });

    b.installArtifact(exe);

    const run_step = b.step("run", "Run the example");
    const run_cmd = b.addRunArtifact(exe);
    run_step.dependOn(&run_cmd.step);
}
