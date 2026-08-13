# serialize.c

[![CI](https://github.com/mas-bandwidth/serialize.c/actions/workflows/ci.yml/badge.svg)](https://github.com/mas-bandwidth/serialize.c/actions/workflows/ci.yml)
[![License: BSD-3-Clause](https://img.shields.io/badge/license-BSD--3--Clause-blue.svg)](LICENSE)

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
make golden                # the pinned wire vector
make diff                  # byte-compare against the C++ library
make test-all-standards    # c89, c99, c11, c17
```

CI additionally runs the golden wire test on a **big-endian** machine
(s390x under qemu). That job earns its place: a round trip test cannot catch
an endianness bug, because a packer writing the scratch word in the wrong byte
order would be read back in the same wrong order and every round trip would
pass while the bytes stayed incompatible with the other four languages. Only a
pinned golden on a big-endian machine proves the byte order.

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

## The full surface

Everything the wire standard defines is here, so a schema using any feature of
the language can target C:

- bits, bool, align, and the fixed-width helpers
- ranged `int`, `int64` and `int128`
- `uint128`, always 128 bits
- **fixed point** (`serialize_write_fixed32` / `64` / `128`) — the Q-format
  path, exact by construction, which is what makes lockstep simulation and
  deterministic replay possible
- float, double, compressed float
- bytes, string, wide string
- `int_relative`, all six tiers
- the measure stream

128-bit integers are **emulated as two 64-bit lanes** rather than requiring
`__int128`, because C89 has no such type and neither does MSVC. The wire is
identical either way — STANDARD.md defines these in terms of 32-bit groups
from least significant upward, which two lanes reproduce exactly.

One deliberate omission: the C++ library's aligned fast path for
`serialize_bytes`. This port writes byte at a time through the packer. Same
bytes, less speed.

## Licence

BSD 3-Clause, matching the rest of the family. See [LICENSE](LICENSE).
