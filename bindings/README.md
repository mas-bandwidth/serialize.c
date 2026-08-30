# bindings

Working, tested examples calling serialize.c from Zig and from Odin.

These are examples, not ports, and the distinction is deliberate: there is no
native Zig or Odin port of serialize, because both languages import C as a
first-class act and a header-only C89 library is exactly the C they import
best. C interop **is** the Zig and Odin story.

Both examples do the same thing: write a ranged int, a bool and a float,
flush, read them back and check the values — then hand the reader a truncated
buffer and require the read to fail. The refusal half is not decoration: this
library reads untrusted data, and a binding that only demonstrates the happy
path demonstrates the wrong thing.

The toolchains are pinned in [tending/PINS.md](../tending/PINS.md) and
unpacked under `dist/` (gitignored): fetch by url, verify the sha256, unpack.

## Zig

`@cImport` consumes serialize.h directly — static inline functions and all,
translate-c compiles them into Zig. There is no binding layer because none is
needed; [build.zig](zig/build.zig) adds the include path and compiles the
stub serialize.c alongside, which is the pattern to copy for any C library
that carries real translation units.

```
cd bindings/zig
../../dist/zig-aarch64-macos-0.16.0/zig build run
```

## Odin

Odin binds C by declaring the signatures it calls: the `foreign` block in
[example.odin](odin/example.odin) is the entire binding, with the streams as
opaque storage whose sizes are asserted against the C truth at startup. The
library is header-only, so [shim.c](odin/shim.c) first compiles it as plain
external functions — both inline macros are `#ifndef`-guarded for exactly
this consumer — into the object the `foreign import` links:

```
cd bindings/odin
cc -O2 -c shim.c -o shim.o
../../dist/odin-macos-arm64-nightly+2026-08-06/odin run .
```
