# serialize.c

[![CI](https://github.com/mas-bandwidth/serialize.c/actions/workflows/ci.yml/badge.svg)](https://github.com/mas-bandwidth/serialize.c/actions/workflows/ci.yml)
[![License: BSD-3-Clause](https://img.shields.io/badge/license-BSD--3--Clause-blue.svg)](LICENSE)

If this library helps you, please support it: **[Become a supporter](https://www.patreon.com/MasBandwidth/membership)**

A bitpacking serialization library for **C**. Wire compatible with the
[C++](https://github.com/mas-bandwidth/serialize),
[C#](https://github.com/mas-bandwidth/serialize.cs),
[Dart](https://github.com/mas-bandwidth/serialize.dart),
[Elixir](https://github.com/mas-bandwidth/serialize.elixir),
[Go](https://github.com/mas-bandwidth/serialize.go),
[Java](https://github.com/mas-bandwidth/serialize.java),
[JavaScript](https://github.com/mas-bandwidth/serialize.js) and
[Rust](https://github.com/mas-bandwidth/serialize.rs) ports — the same values
produce the same bytes in all nine, so a stream written by one reads in any
other. The wire is specified by [STANDARD.md](STANDARD.md), vendored here
verbatim; this release implements **format version 1.1**.

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

Every operation returns `int` for a uniform surface, and the two halves mean
different things by it.

**Reads can fail, and read failure is sticky.** A read returns 1 on success,
0 on failure, and once a read stream fails, subsequent reads fail without
touching the buffer — so you check once at the end of a message rather than
after every field. **Reads reject, they do not clamp.** A value outside its
declared range, a malformed alignment pad, or a read past the end of the
buffer all fail the read. This library is used on packet paths facing the
open internet, and a read is where untrusted data arrives.

**A refused read leaves your value unwritten.** When a read of a scalar
fails, the destination you passed holds exactly what it held before the
call — nothing partial, nothing zero-filled, nothing the stream never
carried. Two things that rule does not reach, deliberately: a read into a
buffer you own — `serialize_read_bytes`, `serialize_read_string`,
`serialize_read_wstring` — leaves that buffer's **contents unspecified**
after a refusal, because a copy path is not restructured for it; and a
sequence of reads over a struct or an array may leave earlier members
written, because each primitive read carries the rule alone.

**Failure is terminal, and the stream enforces it, not your discipline.**
Nothing after a failing operation has a defined position, so nothing after it
is interpretable. The first failed read poisons the stream, every later read
fails without consuming bits or writing a destination, and the failure
persists until you point the stream at a new buffer with
`serialize_read_stream_init` (or `serialize_read_stream_init_padded`) or
discard it. This is the standard's **latch**, implemented as a poisoned bit
limit: every read already tests the limit, so terminality costs the read path
nothing.

**Writes are trusted and always return 1 in a release build.** Under `NDEBUG`
the write path performs no validation at all — no per-field checks and no
capacity check — exactly matching the C++ library
([serialize#52](https://github.com/mas-bandwidth/serialize/issues/52), the
ruling: "C should match C++ and have no checks on write at all (except
assert). ... C and C++ should be equivalent."). Everything that can go wrong
on write is caller error: bounds the wrong way round, a value outside its
declared range, a bit count outside `[1,32]`, a non-finite compressed float,
a buffer too small for the message. Those are `serialize_assert`, which fires
in a debug build — `test/assertdeath.c` proves each one does — and compiles
out under `NDEBUG`. Size the buffer with a measure stream if you are not
certain the message fits: writing past the end of your buffer in a release
build is undefined behavior, yours.

That trust is the whole caller-facing surface, not just the write half. The
**measure stream carries the same contracts the same way**: a string longer
than its declared buffer, an `int_relative` that does not increase, inverted
128-bit bounds are debug asserts, and a release-build measure is pure bit
arithmetic with zero checks — like the C++ `MeasureStream`, whose release
build checks nothing either. The parameters you pass a **read** are yours
too: inverted bounds on a ranged read are caller error, asserted in debug
and never checked in release, exactly as the C++ `ReadStream` asserts them.
What a reader checks for real — in every build mode — is the **data**: a
value outside its declared range, a read past the end of the buffer, nonzero
align padding, and malformed string/wstring content are refused, the same
refusals the C++ reader keeps in its release build. The model is exact
parity with C++, operation by operation: zero release-build validation of
the caller, full release-build refusal of the wire.

If you need to mark a stream failed yourself, call `serialize_read_fail` or
`serialize_write_fail`; do not set the `error` field by hand, because failure
is carried by a poisoned bit limit as well as by that flag. On a read stream
that failure is sticky as above. On a write stream the flag is reported by
`serialize_write_error`, and continuing to write after failing is caller
error, caught by the debug assert.

## Building

There is no build system to adopt: the library is header-only. Include
`serialize.h` and you have all of it. It needs `-lm` for `ceil` and `floor`
on the compressed-float path, and nothing else.

`serialize.c` still ships as an empty compatibility stub, so a build that
compiles and links it keeps working unchanged — but linking it is optional,
the way a single-header library works.

The Makefile here is for developing the library itself:

```
make test                  # round trip, rejection, measure-stream,
                           # writer-contract (assert) and conformance-corpus
                           # tests
make golden                # the pinned wire vectors, core and wide
make wstest                # STANDARD.md's worked wstring example
make diff                  # byte-compare against the C++ library
make test-all-standards    # c89, c99, c11, c17
```

### The conformance corpus

`conformance/` is a **verbatim vendored copy** of the shared corpus in
[mas-bandwidth/serialize](https://github.com/mas-bandwidth/serialize), the way
`STANDARD.md` is, and CI holds both copies to upstream. It is the family's one
conformance instrument: every implementation runs the same vectors, one file
per operation, each stating the bytes, the decoded value or a refusal, and the
bits a conforming reader consumes. `make test` runs every vector in it through
this library's reader — accepted vectors must yield the stated value and
consume the stated bits, refused vectors must be refused with the destination
left unwritten. `test/conformance.c` **scans** the directory rather than
listing its files, so a vector file the corpus gains runs as soon as it is
vendored, and an operation with no runner is a failure rather than a silent
skip. Nothing here regenerates its own expectations: a suite that checks a
port against itself proves only that the port agrees with itself.

CI additionally runs the golden wire test on a **big-endian** machine
(s390x under qemu). That job earns its place: a round trip test cannot catch
an endianness bug, because a packer writing the scratch word in the wrong byte
order would be read back in the same wrong order and every round trip would
pass while the bytes stayed incompatible with the other eight languages. Only a
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
past the meaningful length. Give it a buffer that is a multiple of 8 bytes —
and big enough: the writer checks capacity only as a debug assert, never in a
release build (see Errors). Ask for the meaningful length with
`serialize_write_bytes_processed` after flushing.

The **reader** loads whole 8-byte windows at byte granularity,
unconditionally — the same read path as the C++ library, load for load. The
allocation backing the buffer must therefore extend **at least 8 bytes past
the end of the data**: near the end of the stream the final window begins
inside the last bytes and reaches past them. The bytes past the end are
loaded but **never interpreted** — poison there changes no decoded value and
no refusal, and the test suite proves it. Any allocation at least 8 bytes
longer than the data satisfies the contract; the in-tree tests
spell it `[N + 8]`. The data pointer itself needs no alignment — windows are
loaded with `memcpy`, because packet payloads typically start at an unaligned
offset once the transport header is stripped.

This is the family's accepted best practice, and law (STANDARD.md,
"Implementation Law" — the buffer contract): reading whole words through the
end of the buffer, with the allocation extended so the final word load is
legal, is how a conforming implementation reads. Machinery that avoids the
slack obligation at the cost of per-operation work in the hot path is a
slower correct option, and is refused.

The buffer must not change while the stream is reading it. Handing the reader
an allocation without the slack is caller error with no runtime check at any
build setting — like every allocation, it is yours.

### Reading a packet you did not allocate

A payload that arrives in an exactly sized allocation does not meet the
contract. `serialize_read_stream_init_padded` is the supported way to read
one. It copies the payload into a destination you supply, zeroes the 8 slack
bytes past the copy, and initializes the stream over the copy:

```c
serialize_uint8_t scratch[MAX_PACKET_BYTES + 8];   /* + 8: the slack */
serialize_read_stream_t stream;

serialize_read_stream_init_padded( &stream, scratch, sizeof( scratch ),
                                   packet, packet_bytes );

serialize_read_int( &stream, &health, 0, 1000 );
serialize_read_bool( &stream, &at_rest );
serialize_read_float( &stream, &position_x );

if ( serialize_read_error( &stream ) )
{
    /* refuse the packet */
}
```

The library allocates nothing, so the destination is yours. One buffer of
your largest packet size plus 8 serves every packet. A destination smaller
than `bytes + 8` is caller error, asserted in a debug build and checked by
nothing in a release build, like every other size contract here.

The cost is one `memcpy` of the payload. Reading in place through
`serialize_read_stream_init` stays the fast path, and is what you want
whenever the receive buffer is yours to size.

## The full surface

Everything the wire standard defines is here, so a schema using any feature of
the language can target C:

- bits, bool, align, and the fixed-width helpers
- ranged `int`, `int64` and `int128`, where `min <= max` is the legal
  relation at every width: a **degenerate range** where `min == max` is a
  field, not a misuse, and costs zero bits — the writer emits nothing, the
  reader takes the value from `min` and consumes nothing, and a measure adds
  nothing. (`compressed_float` is not a ranged operation and does require
  `min < max`: a zero span has no quantization to define.)
- `uint128`, always 128 bits
- **fixed point** (`serialize_write_fixed32` / `64` / `128`) — the Q-format
  path, exact by construction, which is what makes lockstep simulation and
  deterministic replay possible
- float, double, compressed float
- bytes, string, wide string
- `int_relative`, all six tiers, over the domain `0` to `2^31 - 1`. Both
  `previous` and `current` live in it. `previous` is your own state and never
  arrives off the wire, so one outside the domain is caller error, asserted;
  `current` is reconstructed in a width that cannot wrap and then refused
  unless it lands in the domain and above `previous` — in **every** tier,
  including the absolute one, whose 32 raw bits are read unsigned. There is no
  wrapping sequence number here and no truncation to a signed type: a stream
  that would reconstruct past the top of the domain is refused, not wrapped.
- the measure stream, with **one operation per write operation** — including
  `int_relative` and `wstring`, whose widths depend on the value and so cannot
  be worked around with `serialize_measure_bits` by a caller unwilling to
  reimplement the tier ladder. The count is a **conservative upper bound**,
  never an under-count: align (and everything that aligns) charges its worst
  case of 7 bits, because padding is bit position dependent and a message is
  measured once but written at arbitrary positions — the same charge the C++
  `MeasureStream` makes. Every operation is tested against the bits the
  corresponding write actually emits: never below, and above by exactly the
  align worst case.

128-bit integers are **emulated as two 64-bit lanes** rather than requiring
`__int128`, because C89 has no such type and neither does MSVC. The wire is
identical either way — STANDARD.md defines these in terms of 32-bit groups
from least significant upward, which two lanes reproduce exactly.

## Speed

The whole library is defined in `serialize.h`, so every operation inlines
into your code and its bit width folds to a literal the way the C++ macros
do. You get that by compiling normally: no LTO flag, no define. The bulk
bodies — strings, the byte-block write, int_relative, 128-bit, fixed point —
are ordinary inline header functions whose cost is the work rather than the
call, but header residency is what lets a call site carrying literal bounds
fold their width computations too; as separate-TU functions every short
string paid a boundary call the C++ template did not.

The per-field spines go one step further: they **demand** inlining
(`SERIALIZE_ALWAYS_INLINE`) instead of hinting at it. A serialize path is a
chain of fallible operations, the compiler's static branch heuristics treat
each success/failure split as even odds, and a few fields into a message every
remaining callsite is judged cold and refused at a threshold these functions
do not fit — the later fields of a message quietly fall out of line. The read
spine took the demand first, closing most of the gap to the C++ library on
the stranded read rows; the write spine took it while it still carried a
per-field capacity check, and keeps it now that the check is gone, for the
same reason the C++ library demands its own write spine: an out-of-line field
costs the call and un-folds the bit width.

Benchmarking for the serialize family lives in [mas-bandwidth/schema](https://github.com/mas-bandwidth/schema)'s data-driven bench, which measures the generated codecs across every language on one corpus.

Caller error is `serialize_assert` and compiles out under `NDEBUG`, matching
the C++ library operation for operation: bounds the wrong way round, a value
outside its declared range on write, a bit count outside `[1,32]`, a write
past the end of the buffer. What stays in a release build is everything that
judges the network — range checks on read, malformed align padding, reads
past the end, malformed string and wstring content — and nothing on write,
measure, or the caller-owned parameters of a read at all.

## Zig and Odin

There is no native Zig or Odin port, deliberately: both languages import C as
a first-class act, and a header-only C89 library is exactly the C they import
best. [bindings/](bindings) holds working, tested examples of each — Zig
through `@cImport`, Odin through a `foreign` block — round trip and truncated
read refusal both.

## Contributing

Contributions are accepted under the repository's
[BSD 3-Clause license](LICENSE).

## Licence

BSD 3-Clause, matching the rest of the family. See [LICENSE](LICENSE).
