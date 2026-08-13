# serialize.c

A bitpacking serialization library for **C**. Wire compatible with the
[C++](https://github.com/mas-bandwidth/serialize),
[C#](https://github.com/mas-bandwidth/serialize.cs),
[Go](https://github.com/mas-bandwidth/serialize.go) and
[Rust](https://github.com/mas-bandwidth/serialize.rs) ports — the same values
produce the same bytes in all five, so a stream written by one reads in any
other.

```c
#include "serialize.h"

serialize_write_stream_t stream;
serialize_uint8_t buffer[1024];

serialize_write_stream_init( &stream, buffer, sizeof( buffer ) );

serialize_write_int( &stream, health, 0, 1000 );   /* 10 bits, not 32 */
serialize_write_bool( &stream, at_rest );          /* 1 bit */
serialize_write_float( &stream, position_x );

serialize_write_flush( &stream );

int bytes = serialize_write_bytes_processed( &stream );
```

## What is different about this port

The C++ library expresses reading and writing **once**, as a single function
templated over a write stream, a read stream or a measure stream. C has no
templates, and faking them with macros produces a library that is hard to
read, hard to step through, and hard to get useful errors out of.

So this port does the honest thing: **separate `serialize_write_*` and
`serialize_read_*` functions**. You write the two halves yourself.

That is more typing and one more thing to keep in sync — and keeping them in
sync is exactly the drift that
[schema](https://github.com/mas-bandwidth/schema) removes, by generating both
halves from one declaration.

## Historical and modern C

The floor is **C89**, and that is checked rather than claimed:

```
make test-all-standards
```

builds and runs the suite under `c89`, `c99`, `c11` and `c17`.

In practice that means comments, declarations at the top of their block, no
`bool`, and no assumption that `stdint.h` exists — there are fallback typedefs
for a toolchain without it. 64-bit integers are required; C89 has no
`long long`, but every C89 compiler that matters shipped one as an extension,
and `serialize_int64_t` is the single hook if yours spells it differently.

Nothing about the standard you build against changes the bytes.

## Errors

Every operation returns `int`: 1 on success, 0 on failure. **Failure is
sticky** — once a stream fails, subsequent operations fail without touching
the buffer, so you check once at the end of a message rather than after every
field.

**Reads reject, they do not clamp.** A value outside its declared range, a
malformed alignment pad, or a read past the end of the buffer all fail the
read. This library is used on packet paths facing the open internet, and a
read is where untrusted data arrives.

## Building

There is no build system to adopt: two files, `serialize.h` and `serialize.c`.
Drop them in your project and compile `serialize.c`. It needs `-lm` for
`ceil` and `floor` on the compressed-float path, and nothing else.

The Makefile here is for developing the library itself:

```
make test                  # round trip, rejection, and measure-stream tests
make diff                  # byte-compare against the C++ library
make test-all-standards    # c89, c99, c11, c17
```

`make diff` expects the C++ library as a sibling checkout; override with
`SERIALIZE_CPP=/path/to/serialize`.

## Buffer contract

The **writer** commits whole 8-byte words, so the final commit may touch bytes
past the meaningful length. Give it a buffer that is a multiple of 8 bytes and
ask for the meaningful length with `serialize_write_bytes_processed` after
flushing.

The **reader** loads through an 8-byte window near the end of the buffer, and
assembles that window byte by byte rather than over-reading — unlike the C++
library, which loads unconditionally and documents the slack as the caller's
responsibility. You can hand this reader a buffer of exactly the meaningful
length.

## What is not here yet

- `serialize_int128` / `serialize_uint128`, and the `serialize_fixed` Q-format
  path. The wire for these is specified in [STANDARD.md](STANDARD.md) and the
  other four ports implement them; this one does not, so a schema using
  128-bit or fixed point fields cannot target C yet.
- `serialize_wstring`. Narrow strings are supported.
- The C++ library's aligned fast path for `serialize_bytes` — this port writes
  byte at a time through the packer. Same bytes, less speed.

## Licence

BSD 3-Clause, matching the rest of the family. See [LICENSE](LICENSE).
