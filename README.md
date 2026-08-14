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

**Your mistakes are not stream errors.** Bounds the wrong way round, a value
outside its declared range on write, a bit count outside `[1,32]` — those are
`serialize_assert`, which fires in a debug build and compiles out under
`NDEBUG`. That is the same division the C++ library makes. If you need to fail
a stream yourself, call `serialize_read_fail` or `serialize_write_fail`; do
not set the `error` field by hand, because failure is carried by a poisoned
bit limit as well as by that flag.

## Building

There is no build system to adopt: two files, `serialize.h` and `serialize.c`.
Drop them in your project and compile `serialize.c`. It needs `-lm` for
`ceil` and `floor` on the compressed-float path, and nothing else.

The Makefile here is for developing the library itself:

```
make test                  # round trip, rejection, and measure-stream tests
make golden                # the pinned wire vectors, core and wide
make wstest                # STANDARD.md's worked wstring example
make diff                  # byte-compare against the C++ library
make test-all-standards    # c89, c99, c11, c17
```

CI additionally runs the golden wire test on a **big-endian** machine
(s390x under qemu). That job earns its place: a round trip test cannot catch
an endianness bug, because a packer writing the scratch word in the wrong byte
order would be read back in the same wrong order and every round trip would
pass while the bytes stayed incompatible with the other four languages. Only a
pinned golden on a big-endian machine proves the byte order.

The golden pins **two** sequences and the big-endian job runs both. The second
covers `uint128`, ranged `int128`, `fixed32`/`64`/`128` and `wstring` — the
hand-rolled two-lane 64-bit emulation, which is both the likeliest place for a
byte order bug and the part a round trip proves least about. Two of its cases
span more than 64 bits deliberately, so the offset reaches the high lane;
everything narrower fits in the low one and would leave half the emulation
unproven.

The pinned bytes are not merely self-consistent. They live in `test/vectors.h`,
and the C++ twins behind `make diff` check their own output against the *same*
constants — so the vector is agreed by two independent implementations, and a
drift between the golden's sequence and the differential's fails loudly instead
of quietly turning the pin into a string that agrees only with itself.

`make diff` expects the C++ library as a sibling checkout; override with
`SERIALIZE_CPP=/path/to/serialize`.

## Buffer contract

The **writer** commits whole 8-byte words, so the final commit may touch bytes
past the meaningful length. Give it a buffer that is a multiple of 8 bytes and
ask for the meaningful length with `serialize_write_bytes_processed` after
flushing.

The **reader** loads through an 8-byte window, and the last such window — the
one that would reach past the end — is assembled once when the stream is
initialized, from the bytes that are there. Unlike the C++ library, which
loads unconditionally and documents the slack as the caller's responsibility,
you can hand this reader a buffer of exactly the meaningful length. The buffer
must not change while the stream is reading it.

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
- the measure stream, with **one operation per write operation** — including
  `int_relative` and `wstring`, whose widths depend on the value and so cannot
  be worked around with `serialize_measure_bits` by a caller unwilling to
  reimplement the tier ladder. Every one is tested against the bits the
  corresponding write actually emits.

128-bit integers are **emulated as two 64-bit lanes** rather than requiring
`__int128`, because C89 has no such type and neither does MSVC. The wire is
identical either way — STANDARD.md defines these in terms of 32-bit groups
from least significant upward, which two lanes reproduce exactly.

## Speed

The per-field operations — bits, bool, align, ranged `int` and `int64`, the
fixed-width helpers, float, double, the byte-block read, the stream lifecycle
and the measure operations — are defined in `serialize.h` rather than
`serialize.c`, so they inline into your code and their bit widths fold to
literals the way the C++ macros do. You get that by compiling normally: no
LTO flag, no define.

Both halves of that surface go one step further: they **demand** inlining
(`SERIALIZE_ALWAYS_INLINE`) instead of hinting at it. A serialize path is a
chain of fallible operations, the compiler's static branch heuristics treat
each success/failure split as even odds, and a few fields into a message every
remaining callsite is judged cold and refused at a threshold these functions
do not fit — the later fields of a message quietly fall out of line. The read
spine took the demand first (the stranded read rows went from 0.6–0.7x of the
C++ library to 0.8–0.9x); the write spine follows because every C write is
fallible too — the capacity check below prices it a handful of instructions
past the same cold threshold — measured as the stream write leg going from
2755 to 3088 MB/s, with the stranded float, uint64 and mixed-field writes
rescued the same way.

Measured against the C++ library at the same `-O2` on an Apple M2, `make
bench-all` (C++ column: the `serialize` checkout with its write-spine
inlining, [serialize PR #45](https://github.com/mas-bandwidth/serialize/pull/45)):

| | C | C++ |
|---|---|---|
| int packet read | 244 M/s | 188 M/s |
| int packet write | 122 M/s | 174 M/s |
| mixed packet read | 250 M/s | 193 M/s |
| stream read | 9263 MB/s | 5273 MB/s |
| stream write | 3088 MB/s | 3989 MB/s |
| bitpacker read | 2099 MB/s | 2706 MB/s |
| bitpacker write | 2120 MB/s | 2157 MB/s |

The raw bitpacker read and the packet writes are the places this port is
meaningfully behind, and both are behind on purpose. The C++ reader loads its
64-bit window unconditionally and **requires the caller's allocation to extend
8 bytes past the data**; this one reads no byte you did not give it, and pays
one predictable branch per read for that. Removing the branch — measured —
takes bitpacker read to 2497 MB/s. On the write side the capacity check makes
every field a possible early exit, so the compiler must keep the stream's
scratch word, bit counts and index correct in memory at each field boundary —
the C++ writer, whose release build checks nothing, batches that bookkeeping
across whole packets and keeps its scratch in registers. The check costs more
in lost coalescing than in executed compares, and it is the same trade as the
reader's: never touch memory you were not given, even at a price.

Caller error is `serialize_assert` and compiles out under `NDEBUG`, matching
the C++ library operation for operation: bounds the wrong way round, a value
outside its declared range on write, a bit count outside `[1,32]`. What stays
in a release build is everything that judges the network — range checks on
read, malformed align padding, reads past the end — plus a capacity check on
write that the C++ library only asserts, because running off the end of your
buffer is worse than being slower.

## Licence

BSD 3-Clause, matching the rest of the family. See [LICENSE](LICENSE).
