/*
    serialize.c 1.0 — a bitpacking serializer for C

    Wire compatible with the C++, C#, Go and Rust ports: the same schema
    produces byte-identical output in all five. The format is specified in
    STANDARD.md, which is kept identical across the family.

    WHAT IS DIFFERENT ABOUT THIS PORT

    The C++ library expresses reading and writing ONCE, as a single function
    templated over a write stream, a read stream or a measure stream. C has no
    templates, and faking them with macros would produce a library that is
    hard to read, hard to step through in a debugger, and hard to get useful
    errors out of.

    So this port does the honest thing: separate serialize_write_* and
    serialize_read_* functions. You write the two halves yourself, which is
    more typing and one more thing to keep in sync — and that is exactly the
    drift the schema compiler exists to remove, by generating both halves from
    one declaration.

    WHY THE HOT PATH IS IN THIS HEADER

    The C++ library is header only, so every field a caller serializes inlines
    into the caller and the bit width — which comes from bounds that are almost
    always compile-time constants — folds to a literal. A C library compiled as
    a separate translation unit gets none of that: every field costs a call,
    and serialize_bits_required( min, max ) becomes a runtime computation of a
    number the compiler already knew.

    So the operations a packet path performs per FIELD are defined here — the
    read and write spines as SERIALIZE_ALWAYS_INLINE, which says why where it
    is defined — and the ones performed per MESSAGE or not at all in a tight
    loop stay in serialize.c. This is a size/speed trade made once, for
    everybody, with no LTO flag and no define to opt into: measured on Apple
    silicon, moving them took ranged-int reads from 22.8 to 186 million packets
    a second, which is the C++ library's number.

    What is here is the whole per-field surface: bits, bool, align, ranged int
    and int64, the fixed-width helpers, float, double, the byte-block read,
    the stream lifecycle, and the measure operations that mirror them. What is
    not is everything whose cost is dominated by what it does rather than by
    the call: strings, the byte-block write, 128-bit, fixed point, compressed
    float, int_relative.

    HISTORICAL AND MODERN C

    The floor is C89. That means: comments are, declarations sit at the top of
    their block, there is no bool, and nothing assumes stdint.h. Where a
    modern toolchain is available the library uses it (stdint.h, inline,
    static assertions); where it is not, it falls back without changing the
    wire. See the configuration block below.

    64-bit integers are required. C89 has no long long, but every C89 compiler
    that matters shipped one as an extension; serialize_int64_t is the hook if
    yours spells it differently.

    ERRORS

    Every operation returns int for a uniform surface, and the two halves mean
    different things by it — the same division the C++ library makes.

    READS can fail: 1 on success, 0 on failure. Failure is sticky — once a
    read stream fails, subsequent reads fail without touching the buffer, so
    you can check once at the end of a message rather than after every field.
    Reads fail on out-of-range values rather than clamping them. This library
    is used on packet paths that face the open internet, and a read is the
    place where untrusted data arrives.

    WRITES are trusted and always return 1 in a release build: under NDEBUG
    the write path performs no validation at all — no per-field checks and no
    capacity check — exactly matching the C++ writer (serialize issue #52,
    the ruling: "C should match C++ and have no checks on write at all
    (except assert). ... C and C++ should be equivalent."). Everything that
    can go wrong on write is caller error — bounds the wrong way round, a
    value outside its declared range, a bit count outside [1,32], a buffer
    too small for the message — and caller error is serialize_assert, which
    fires in a debug build and compiles to nothing under NDEBUG. Size the
    buffer with a measure stream if you are not certain the message fits:
    writing past the end of your buffer in a release build is undefined
    behavior, yours. A read validates the network; it does not validate you.
*/

#ifndef SERIALIZE_H
#define SERIALIZE_H

/* ---------------------------------------------------------------------------
   version

   Kept in step with the tag by CI, the same contract the C++ library uses.
   --------------------------------------------------------------------------- */

#define SERIALIZE_VERSION_MAJOR 1
#define SERIALIZE_VERSION_MINOR 2
#define SERIALIZE_VERSION_PATCH 0
#define SERIALIZE_VERSION "1.2.0"

/* ---------------------------------------------------------------------------
   configuration
   --------------------------------------------------------------------------- */

/*
    SERIALIZE_HAS_STDINT — stdint.h is C99. Define it to 0 to use the fallback
    typedefs below, which is what a genuine C89 toolchain needs.
*/
#ifndef SERIALIZE_HAS_STDINT
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#define SERIALIZE_HAS_STDINT 1
#elif defined(_MSC_VER) && _MSC_VER >= 1600
#define SERIALIZE_HAS_STDINT 1
#else
#define SERIALIZE_HAS_STDINT 0
#endif
#endif

#if SERIALIZE_HAS_STDINT
#include <stdint.h>
typedef uint8_t serialize_uint8_t;
typedef uint16_t serialize_uint16_t;
typedef uint32_t serialize_uint32_t;
typedef uint64_t serialize_uint64_t;
typedef int8_t serialize_int8_t;
typedef int16_t serialize_int16_t;
typedef int32_t serialize_int32_t;
typedef int64_t serialize_int64_t;
#else
/*
    C89 fallback. These are the widths the wire format needs; if your platform
    spells any of them differently, this is the block to edit — nothing below
    depends on anything but these names.
*/
typedef unsigned char serialize_uint8_t;
typedef unsigned short serialize_uint16_t;
typedef unsigned int serialize_uint32_t;
typedef signed char serialize_int8_t;
typedef short serialize_int16_t;
typedef int serialize_int32_t;
#if defined(_MSC_VER)
typedef unsigned __int64 serialize_uint64_t;
typedef __int64 serialize_int64_t;
#else
typedef unsigned long long serialize_uint64_t;
typedef long long serialize_int64_t;
#endif
#endif

/*
    SERIALIZE_UNUSED — marks a definition the translation unit may not use.
    The hot path is defined in this header, and no consumer calls all of it.
*/
#ifndef SERIALIZE_UNUSED
#if defined(__GNUC__) || defined(__clang__)
#define SERIALIZE_UNUSED __attribute__((unused))
#else
#define SERIALIZE_UNUSED
#endif
#endif

/*
    SERIALIZE_INLINE — how the hot path is spelled.

    C89 has no inline keyword, so the floor is plain static: a private copy per
    translation unit, which the compiler inlines anyway at any optimization
    level and which needs SERIALIZE_UNUSED so an unused one does not warn.
    Everything above C89 spells it properly. static either way, so a caller
    that includes only this header links, and nothing here collides with
    serialize.c.
*/
#ifndef SERIALIZE_INLINE
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#define SERIALIZE_INLINE static inline
#elif defined(__GNUC__)
#define SERIALIZE_INLINE static __inline__
#elif defined(_MSC_VER)
#define SERIALIZE_INLINE static __inline
#else
#define SERIALIZE_INLINE static SERIALIZE_UNUSED
#endif
#endif

/*
    SERIALIZE_ALWAYS_INLINE — how the per-field spine is spelled, both halves.

    A serialize path is a chain of fallible operations, the compiler's static
    branch heuristics treat each success/failure split as roughly even odds,
    and block frequency decays geometrically down the chain — so a few fields
    into a message, every remaining callsite is judged cold and held to the
    cold-callsite inline threshold, which these functions do not fit. On the
    read side that price is the bounds-safe window and the sticky-failure
    test. The write side no longer carries any runtime check (issue #52:
    writes are trusted, capacity asserted in debug only, matching the C++
    writer), but the demand stays, for the same reason the C++ library
    demands its own write spine: an out-of-line field costs the call and
    un-folds the bit width. Measured on Apple silicon (Apple clang 21, schema
    bench, O2 and O3), while the write spine still carried its capacity
    check: SERIALIZE_INLINE alone stranded the later read fields of a message
    out of line (affected read rows 0.57–0.71x of the C++ library), and once
    the C++ library demanded its own write spine the same refusals showed on
    the write side — serialize_write_bits held at cost 105 against threshold
    45, write_bool at 110, write_uint64 at 245 — with the C write rows at
    0.59–0.71x. So both per-field spines demand inlining instead of hinting
    at it. What deliberately does NOT make the demand: the bulk and branchy
    bodies (byte blocks, strings, int_relative, compressed float, fixed
    point), whose cost is the work rather than the call.

    Branch-weight hints are not the fix and are not used: __builtin_expect
    on the error edges was measured in this library (bits read −6.5%, mixed
    write −8%) and in the C++ sibling, where the cold blocks it created
    invited Apple clang's machine outliner to shred the hot bodies into
    bl-called fragments (bits write −25%). Do not re-add hints unmeasured.

    C89 floor: where no always-inline spelling exists this falls back to
    SERIALIZE_INLINE, and from there to plain static.
*/
#ifndef SERIALIZE_ALWAYS_INLINE
#if defined(__GNUC__) || defined(__clang__)
#define SERIALIZE_ALWAYS_INLINE SERIALIZE_INLINE __attribute__((always_inline))
#elif defined(_MSC_VER)
#define SERIALIZE_ALWAYS_INLINE static __forceinline
#else
#define SERIALIZE_ALWAYS_INLINE SERIALIZE_INLINE
#endif
#endif

/*
    SERIALIZE_RESTRICT — the promise that a stream does not overlap the value
    being read or written. Without it a store through the caller's pointer
    could, as far as the compiler knows, have changed the stream's own bit
    position, and every field reloads the whole stream from memory. The C++
    library makes the same promise on its bit writer, for the same reason.
*/
#ifndef SERIALIZE_RESTRICT
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#define SERIALIZE_RESTRICT restrict
#elif defined(__GNUC__)
#define SERIALIZE_RESTRICT __restrict__
#elif defined(_MSC_VER)
#define SERIALIZE_RESTRICT __restrict
#else
#define SERIALIZE_RESTRICT
#endif
#endif

/*
    serialize_assert — caller error, not stream error.

    Spelled and overridable exactly as the C++ library spells it, and for the
    same purpose: bounds the wrong way round or a value outside its declared
    range on write are bugs in the calling code, caught in a debug build and
    costing nothing in a release one. Data arriving from the network is never
    checked this way. See ERRORS at the top of this file.
*/
#ifndef serialize_assert
#include <assert.h>
#define serialize_assert assert
#endif

#include <stddef.h>   /* wchar_t */
#include <string.h>   /* memcpy — the packer moves whole words through it */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
   streams
   --------------------------------------------------------------------------- */

/*
    A write stream. Initialize it over a buffer, perform writes, then flush.

    The buffer must be a multiple of 4 bytes and 4-byte aligned: the packer
    accumulates into a 64-bit scratch word and commits whole 8-byte words, so
    the last commit may touch bytes past the meaningful length. Ask for the
    length with serialize_write_bytes_processed after flushing.
*/
typedef struct serialize_write_stream_t
{
    serialize_uint8_t * data;
    int num_bits;
    int bits_written;
    int word_index;
    serialize_uint64_t scratch;
    int scratch_bits;
    int error;                  /* set only by serialize_write_fail; the write
                                   path itself cannot fail — see ERRORS */

    /*
        num_bits again, and -1 once serialize_write_fail has been called.

        The release write path never reads it: capacity is the writer's
        contract, asserted in debug only (issue #52 — see ERRORS). The debug
        assert tests against THIS field rather than num_bits so that writing
        to a stream you already failed is caught too — the poisoned limit
        fails the assert for good. The read stream keeps the real, release-
        build version of this mechanism; see serialize_read_stream_t.
    */
    int bits_limit;
} serialize_write_stream_t;

/*
    A read stream over a buffer produced by a write stream.

    The reader loads through an 8-byte window, so it may read up to 7 bytes
    past the last meaningful byte. The buffer must therefore have 8 readable
    bytes from the start of the final word — the same allocation contract the
    C++ library documents. Rounding the buffer up to a multiple of 8 satisfies
    it.
*/
typedef struct serialize_read_stream_t
{
    const serialize_uint8_t * data;
    int num_bits;
    int bits_read;
    int error;                  /* sticky: once set, every read fails */

    /*
        num_bits, and -1 once the stream has failed.

        Every read already tests that it fits, so poisoning the limit is what
        makes failure sticky WITHOUT a second test per field: one comparison
        answers both questions. Set it through serialize_read_fail and never
        by hand — a stream whose error flag is set and whose limit is not
        would accept the next read.
    */
    int bits_limit;

    /*
        The last window of the buffer, assembled once at init.

        The reader loads a 64-bit window at the current byte. Within the first
        bytes - 8 of the buffer that window is a single load; past
        that it would run off the end, so the final window is built up front
        from the bytes that are there and every read in the last 8 bytes uses
        it, shifted. tail_base is the byte index that window starts at, and is
        0 for a buffer shorter than 8 bytes — which is then entirely in tail.

        This is what lets the read path stay two straight-line instructions per
        arm and require nothing of the caller's allocation. The C++ reader
        instead loads unconditionally and requires 8 bytes of slack past the
        data; this port asks nothing, and the price is one predictable branch.

        The buffer must not change while the stream is reading it.
    */
    serialize_uint64_t tail;    /* host order, ready to shift */
    int tail_base;
} serialize_read_stream_t;

/*
    A measure stream: bounds the bits a message would occupy without producing
    any. The write and measure halves must perform the same operations for the
    count to be meaningful.

    The count is a CONSERVATIVE upper bound, not an exact size — align (and
    everything that aligns: bytes, string) charges its worst case of 7 bits,
    because a message is measured once and then written at arbitrary bit
    positions, where the padding differs. The C++ MeasureStream makes the
    same charge, and the fork ruling makes it the family model: "measure must
    be large enough to serialize the message but doesn't need to be exact";
    "the exact is not possible, since align is going to be different in 1st
    and 2nd times serialize is called. it is bit position dependent."
    A message with no alignment in it measures exactly.
*/
typedef struct serialize_measure_stream_t
{
    int bits_written;
} serialize_measure_stream_t;

/* ---------------------------------------------------------------------------
   stream lifecycle
   --------------------------------------------------------------------------- */

SERIALIZE_INLINE void serialize_write_stream_init( serialize_write_stream_t * stream, void * buffer, int bytes );

/* Flushes the partial scratch word. Call once, after the final write. */
SERIALIZE_INLINE void serialize_write_flush( serialize_write_stream_t * stream );

SERIALIZE_INLINE int serialize_write_bits_processed( const serialize_write_stream_t * stream );
SERIALIZE_INLINE int serialize_write_bytes_processed( const serialize_write_stream_t * stream );
SERIALIZE_INLINE int serialize_write_bits_available( const serialize_write_stream_t * stream );
SERIALIZE_INLINE int serialize_write_error( const serialize_write_stream_t * stream );

/* Fails a stream, and always returns 0 so a caller can `return` it directly.
   This is the ONLY supported way to fail a stream: setting the error flag by
   hand leaves the bit limit unpoisoned — see bits_limit in the stream structs.

   On a READ stream failure is sticky: every later read fails without touching
   the buffer. On a WRITE stream the flag is reported by serialize_write_error
   but the release write path never consults it — writes are trusted and
   cannot fail (see ERRORS) — so do not keep writing to a stream you have
   failed: that is caller error, and the poisoned bit limit makes the next
   write assert in a debug build. */
SERIALIZE_INLINE int serialize_write_fail( serialize_write_stream_t * stream );
SERIALIZE_INLINE int serialize_read_fail( serialize_read_stream_t * stream );

SERIALIZE_INLINE void serialize_read_stream_init( serialize_read_stream_t * stream, const void * buffer, int bytes );

SERIALIZE_INLINE int serialize_read_bits_processed( const serialize_read_stream_t * stream );
SERIALIZE_INLINE int serialize_read_bytes_processed( const serialize_read_stream_t * stream );
SERIALIZE_INLINE int serialize_read_bits_remaining( const serialize_read_stream_t * stream );
SERIALIZE_INLINE int serialize_read_error( const serialize_read_stream_t * stream );

SERIALIZE_INLINE void serialize_measure_stream_init( serialize_measure_stream_t * stream );
SERIALIZE_INLINE int serialize_measure_bits_processed( const serialize_measure_stream_t * stream );
SERIALIZE_INLINE int serialize_measure_bytes_processed( const serialize_measure_stream_t * stream );

/* ---------------------------------------------------------------------------
   bit-level primitives
   --------------------------------------------------------------------------- */

/* bits must be in [1,32] and value must be less than 2^bits. Both are caller
   error, so both are asserted rather than checked; a value wider than its
   declared bits is masked, so it damages its own field and no other. So is
   capacity, on the write side: the write path performs no release-build
   checks at all (issue #52 — see ERRORS). */
SERIALIZE_ALWAYS_INLINE int serialize_write_bits( serialize_write_stream_t * SERIALIZE_RESTRICT stream, serialize_uint32_t value, int bits );
SERIALIZE_ALWAYS_INLINE int serialize_read_bits( serialize_read_stream_t * SERIALIZE_RESTRICT stream, serialize_uint32_t * SERIALIZE_RESTRICT value, int bits );

SERIALIZE_ALWAYS_INLINE int serialize_write_bool( serialize_write_stream_t * stream, int value );
SERIALIZE_ALWAYS_INLINE int serialize_read_bool( serialize_read_stream_t * SERIALIZE_RESTRICT stream, int * SERIALIZE_RESTRICT value );

/*
    Pads with zero bits to the next byte boundary, writing nothing if already
    aligned. The reader verifies the padding is zero and FAILS if it is not —
    malformed streams are detectable rather than silently accepted.
*/
SERIALIZE_ALWAYS_INLINE int serialize_write_align( serialize_write_stream_t * stream );
SERIALIZE_ALWAYS_INLINE int serialize_read_align( serialize_read_stream_t * stream );

SERIALIZE_INLINE int serialize_write_align_bits( const serialize_write_stream_t * stream );
SERIALIZE_INLINE int serialize_read_align_bits( const serialize_read_stream_t * stream );

/* ---------------------------------------------------------------------------
   integers
   --------------------------------------------------------------------------- */

/*
    The defining operation. The width comes entirely from the range, so
    [0,7] costs 3 bits and a degenerate min == max costs nothing at all.
    Both sides must pass identical bounds — the wire carries no description
    of itself.

    min <= max is asserted, not checked: bounds are the caller's, not the
    network's. On READ the decoded value is range checked for real, always,
    because that is where untrusted data arrives.
*/
SERIALIZE_ALWAYS_INLINE int serialize_write_int( serialize_write_stream_t * stream, serialize_int32_t value, serialize_int32_t min, serialize_int32_t max );
SERIALIZE_ALWAYS_INLINE int serialize_read_int( serialize_read_stream_t * SERIALIZE_RESTRICT stream, serialize_int32_t * SERIALIZE_RESTRICT value, serialize_int32_t min, serialize_int32_t max );

SERIALIZE_ALWAYS_INLINE int serialize_write_int64( serialize_write_stream_t * stream, serialize_int64_t value, serialize_int64_t min, serialize_int64_t max );
SERIALIZE_ALWAYS_INLINE int serialize_read_int64( serialize_read_stream_t * SERIALIZE_RESTRICT stream, serialize_int64_t * SERIALIZE_RESTRICT value, serialize_int64_t min, serialize_int64_t max );

/*
    Fixed-width helpers. These are NOT ranged — serialize_write_uint64 always
    costs 64 bits, where serialize_write_int64 costs only what its bounds
    require. The names are similar and the encodings are not.
*/
SERIALIZE_ALWAYS_INLINE int serialize_write_uint8( serialize_write_stream_t * stream, serialize_uint8_t value );
SERIALIZE_ALWAYS_INLINE int serialize_read_uint8( serialize_read_stream_t * SERIALIZE_RESTRICT stream, serialize_uint8_t * SERIALIZE_RESTRICT value );
SERIALIZE_ALWAYS_INLINE int serialize_write_uint16( serialize_write_stream_t * stream, serialize_uint16_t value );
SERIALIZE_ALWAYS_INLINE int serialize_read_uint16( serialize_read_stream_t * SERIALIZE_RESTRICT stream, serialize_uint16_t * SERIALIZE_RESTRICT value );
SERIALIZE_ALWAYS_INLINE int serialize_write_uint32( serialize_write_stream_t * stream, serialize_uint32_t value );
SERIALIZE_ALWAYS_INLINE int serialize_read_uint32( serialize_read_stream_t * SERIALIZE_RESTRICT stream, serialize_uint32_t * SERIALIZE_RESTRICT value );
SERIALIZE_ALWAYS_INLINE int serialize_write_uint64( serialize_write_stream_t * stream, serialize_uint64_t value );
SERIALIZE_ALWAYS_INLINE int serialize_read_uint64( serialize_read_stream_t * SERIALIZE_RESTRICT stream, serialize_uint64_t * SERIALIZE_RESTRICT value );

/* An increasing sequence, encoded as a ladder of tier flags. A difference of
   1 — the common case for sequence numbers — costs a single bit.

   STRICTLY increasing, and no wrap semantics exist (STANDARD.md, the ruling
   verbatim: "no wrapping sequence numbers"): a caller with a wrapping counter
   unwraps it before serializing, and the reader fails a current that does not
   exceed previous. */
int serialize_write_int_relative( serialize_write_stream_t * stream, serialize_int32_t previous, serialize_int32_t current );
int serialize_read_int_relative( serialize_read_stream_t * stream, serialize_int32_t previous, serialize_int32_t * current );

/* ---------------------------------------------------------------------------
   floating point
   --------------------------------------------------------------------------- */

SERIALIZE_ALWAYS_INLINE int serialize_write_float( serialize_write_stream_t * stream, float value );
SERIALIZE_ALWAYS_INLINE int serialize_read_float( serialize_read_stream_t * SERIALIZE_RESTRICT stream, float * SERIALIZE_RESTRICT value );

SERIALIZE_ALWAYS_INLINE int serialize_write_double( serialize_write_stream_t * stream, double value );
SERIALIZE_ALWAYS_INLINE int serialize_read_double( serialize_read_stream_t * SERIALIZE_RESTRICT stream, double * SERIALIZE_RESTRICT value );

/* Lossy by construction: a round trip returns the nearest quantum. */
int serialize_write_compressed_float( serialize_write_stream_t * stream, float value, float min, float max, float res );
int serialize_read_compressed_float( serialize_read_stream_t * stream, float * value, float min, float max, float res );

/* ---------------------------------------------------------------------------
   bytes and strings
   --------------------------------------------------------------------------- */

/* Aligns first — that alignment is part of the format, not an optimization.
   count is not transmitted; both sides must already agree on it.

   The two halves live in different places. The write half runs the
   head/body/tail packing machinery and stays in serialize.c; the read half
   after its align is a single memcpy, and for the small blocks packet code
   actually reads per field the call was the cost — so it lives in this
   header, on the read spine with the rest of the per-field surface. */
int serialize_write_bytes( serialize_write_stream_t * stream, const serialize_uint8_t * data, int bytes );
SERIALIZE_ALWAYS_INLINE int serialize_read_bytes( serialize_read_stream_t * SERIALIZE_RESTRICT stream, serialize_uint8_t * SERIALIZE_RESTRICT data, int bytes );

/* A null-terminated string. buffer_size sets the width of the length field,
   so both sides must agree on it; the terminator is not transmitted.

   The payload is well-formed UTF-8 with no interior NUL (STANDARD.md). On
   the write side that is the writer's obligation, debug-asserted only. On
   the READ side it is enforced in every build mode (ruling #8, adopted
   2026-08-15): the reader refuses invalid UTF-8 — overlongs, surrogate code
   points, anything above U+10FFFF, truncated sequences — and refuses a NUL
   inside the counted payload, whose wire length and strlen-perceived length
   would otherwise disagree. Refusal is the read-failure convention: return
   0, stream error set, sticky. Genuinely arbitrary payloads belong in
   serialize_write_bytes, which remains exactly that. */
int serialize_write_string( serialize_write_stream_t * stream, const char * string, int buffer_size );
int serialize_read_string( serialize_read_stream_t * stream, char * string, int buffer_size );

/* ---------------------------------------------------------------------------
   128-bit integers

   Emulated as two 64-bit lanes rather than requiring __int128, because C89
   has no such type and neither does MSVC. The wire is identical either way:
   STANDARD.md defines these operations in terms of 32-bit groups from least
   significant upward, which two 64-bit lanes reproduce exactly.

   int128 is the same two lanes read as two's complement.
   --------------------------------------------------------------------------- */

typedef struct serialize_uint128_t
{
    serialize_uint64_t lo;
    serialize_uint64_t hi;
} serialize_uint128_t;

typedef struct serialize_int128_t
{
    serialize_uint64_t lo;
    serialize_uint64_t hi;
} serialize_int128_t;

serialize_uint128_t serialize_uint128_make( serialize_uint64_t hi, serialize_uint64_t lo );
serialize_int128_t serialize_int128_make( serialize_uint64_t hi, serialize_uint64_t lo );

/* Sign-extends a 64-bit value into the 128-bit domain. */
serialize_int128_t serialize_int128_from_int64( serialize_int64_t value );

int serialize_uint128_equal( serialize_uint128_t a, serialize_uint128_t b );
int serialize_int128_equal( serialize_int128_t a, serialize_int128_t b );

/* -1, 0 or 1. The signed form compares in the signed domain. */
int serialize_int128_compare( serialize_int128_t a, serialize_int128_t b );

/* Always 128 bits: the low half first, then the high half. NOT ranged. */
int serialize_write_uint128( serialize_write_stream_t * stream, serialize_uint128_t value );
int serialize_read_uint128( serialize_read_stream_t * stream, serialize_uint128_t * value );

/* Ranged, and the only ranged 128-bit operation. Where the range fits 64 bits
   the bytes are identical to serialize_write_int64 over the same bounds, so a
   field may widen from 64 to 128 without moving the wire. */
int serialize_write_int128( serialize_write_stream_t * stream, serialize_int128_t value, serialize_int128_t min, serialize_int128_t max );
int serialize_read_int128( serialize_read_stream_t * stream, serialize_int128_t * value, serialize_int128_t min, serialize_int128_t max );

/* ---------------------------------------------------------------------------
   fixed point (Q format)

   The value is an integer scaled by 2^fraction_bits; min and max are bounds in
   WHOLE REAL UNITS. The encoding is an offset over the raw (scaled) bounds, so
   with fraction_bits = 0 the operation IS a ranged integer.

   Exact by construction: unlike compressed_float there is no quantization
   step, so the same raw value produces the same bytes and reads back
   bit-for-bit identical on every platform. That is what makes it usable for
   lockstep simulation and deterministic replay.

   A degenerate range where min == max is legal and costs ZERO BITS on every
   storage width (STANDARD.md): nothing is written, and the reader recovers
   the value from the range alone -- the raw value min << fraction_bits.

   Three widths because C has no overloading; pick the one matching your
   storage. integer_bits + fraction_bits must equal the storage width.
   --------------------------------------------------------------------------- */

int serialize_write_fixed32( serialize_write_stream_t * stream, serialize_int32_t value, int integer_bits, int fraction_bits, serialize_int32_t min, serialize_int32_t max );
int serialize_read_fixed32( serialize_read_stream_t * stream, serialize_int32_t * value, int integer_bits, int fraction_bits, serialize_int32_t min, serialize_int32_t max );

int serialize_write_fixed64( serialize_write_stream_t * stream, serialize_int64_t value, int integer_bits, int fraction_bits, serialize_int64_t min, serialize_int64_t max );
int serialize_read_fixed64( serialize_read_stream_t * stream, serialize_int64_t * value, int integer_bits, int fraction_bits, serialize_int64_t min, serialize_int64_t max );

int serialize_write_fixed128( serialize_write_stream_t * stream, serialize_int128_t value, int integer_bits, int fraction_bits, serialize_int64_t min, serialize_int64_t max );
int serialize_read_fixed128( serialize_read_stream_t * stream, serialize_int128_t * value, int integer_bits, int fraction_bits, serialize_int64_t min, serialize_int64_t max );

/* ---------------------------------------------------------------------------
   wide strings

   buffer_size counts WIDE CHARACTERS, not bytes. Each 32-bit group carries
   one UTF-16 CODE UNIT -- not one code point -- and the payload is
   well-formed UTF-16 with no interior NUL (STANDARD.md, adopted 2026-08-15):
   surrogate pairs are valid, an unpaired surrogate is a writer contract
   violation, debug-asserted. On the READ side well-formedness is enforced in
   every build mode (ruling #8, adopted 2026-08-15): the reader refuses an
   unpaired or misordered surrogate, a zero unit inside the counted payload,
   and any group above 0xFFFF -- which is not a UTF-16 code unit on ANY
   platform, subsuming the old 2-byte-only "wchar_t cannot hold it" refusal.
   Refusal is the read-failure convention: return 0, stream error set,
   sticky. 2-byte and 4-byte wchar_t platforms produce IDENTICAL bytes: the
   4-byte platform converts at the boundary, splitting an astral code point
   into its surrogate pair on write and recombining on read.

   NO ALIGNMENT is performed anywhere in this operation -- the one place the
   wide path deliberately differs from the narrow one, which aligns via
   serialize_bytes. An implementation that mirrors the narrow path here
   produces the wrong bytes.
   --------------------------------------------------------------------------- */

int serialize_write_wstring( serialize_write_stream_t * stream, const wchar_t * string, int buffer_size );
int serialize_read_wstring( serialize_read_stream_t * stream, wchar_t * string, int buffer_size );

/* ---------------------------------------------------------------------------
   measure stream

   One measure operation for every write operation, so a schema that can
   generate a write half can always generate a measure half. This block sits
   last because it mirrors everything above it.

   The measure functions take the arguments that determine the WIDTH and
   nothing else: a ranged integer costs the same whatever the value, so
   serialize_measure_int takes only the bounds. Where the width does depend on
   the value -- int_relative, string, wstring -- the value is taken too, and
   there is no way to get the count right without it.

   They return int for symmetry with the write half, and return 0 for input
   the writer's contract forbids: a string or wstring longer than its buffer,
   an int_relative that does not increase, inverted bounds. The writer itself
   debug-asserts these (issue #52 — see ERRORS); the measure stream keeps the
   real refusal because a measure is how a buffer gets sized, and a count for
   a message no conforming writer produces is the one way a measure can do
   damage. Nothing is counted in that case, so a measure that returns 0
   leaves the stream as it was.

   What a measure CANNOT refuse is a value out of its declared range, because
   it is never given the value -- only the bounds that set the width. The
   write is where that is asserted.
   --------------------------------------------------------------------------- */

SERIALIZE_INLINE int serialize_measure_bits( serialize_measure_stream_t * stream, int bits );
SERIALIZE_INLINE int serialize_measure_bool( serialize_measure_stream_t * stream );
SERIALIZE_INLINE int serialize_measure_align( serialize_measure_stream_t * stream );

SERIALIZE_INLINE int serialize_measure_int( serialize_measure_stream_t * stream, serialize_int32_t min, serialize_int32_t max );
SERIALIZE_INLINE int serialize_measure_int64( serialize_measure_stream_t * stream, serialize_int64_t min, serialize_int64_t max );

SERIALIZE_INLINE int serialize_measure_uint8( serialize_measure_stream_t * stream );
SERIALIZE_INLINE int serialize_measure_uint16( serialize_measure_stream_t * stream );
SERIALIZE_INLINE int serialize_measure_uint32( serialize_measure_stream_t * stream );
SERIALIZE_INLINE int serialize_measure_uint64( serialize_measure_stream_t * stream );

int serialize_measure_int_relative( serialize_measure_stream_t * stream, serialize_int32_t previous, serialize_int32_t current );

SERIALIZE_INLINE int serialize_measure_float( serialize_measure_stream_t * stream );
SERIALIZE_INLINE int serialize_measure_double( serialize_measure_stream_t * stream );
int serialize_measure_compressed_float( serialize_measure_stream_t * stream, float min, float max, float res );

SERIALIZE_INLINE int serialize_measure_bytes( serialize_measure_stream_t * stream, int bytes );
int serialize_measure_string( serialize_measure_stream_t * stream, const char * string, int buffer_size );
int serialize_measure_wstring( serialize_measure_stream_t * stream, const wchar_t * string, int buffer_size );

int serialize_measure_uint128( serialize_measure_stream_t * stream );
int serialize_measure_int128( serialize_measure_stream_t * stream, serialize_int128_t min, serialize_int128_t max );

int serialize_measure_fixed32( serialize_measure_stream_t * stream, int integer_bits, int fraction_bits, serialize_int32_t min, serialize_int32_t max );
int serialize_measure_fixed64( serialize_measure_stream_t * stream, int integer_bits, int fraction_bits, serialize_int64_t min, serialize_int64_t max );
int serialize_measure_fixed128( serialize_measure_stream_t * stream, int integer_bits, int fraction_bits, serialize_int64_t min, serialize_int64_t max );

/* ---------------------------------------------------------------------------
   helpers
   --------------------------------------------------------------------------- */

/* Bits needed for a value in [min,max]; 0 when min == max.

   The parameters live in the UNSIGNED domain, matching the C++ library's
   bits_required( uint32_t, uint32_t ): the span is max - min modulo the
   type's width, so signed bounds converted in give exactly the widths they
   always did, and an unsigned bound above INT32_MAX -- the compressed float
   ceiling reaches 4294967040 -- needs no narrowing through a signed
   parameter, which is implementation-defined at this library's C89 floor.

   Inline because it is the width of every ranged field, and bounds are almost
   always constants: called from this header it folds to a literal at the call
   site, which is what the C++ macro gets for free and what an out-of-line C
   call was paying for per field. */
SERIALIZE_INLINE int serialize_bits_required( serialize_uint32_t min, serialize_uint32_t max );
SERIALIZE_INLINE int serialize_bits_required64( serialize_uint64_t min, serialize_uint64_t max );

/* Bounded copy that always null-terminates. */
void serialize_copy_string( char * dest, const char * source, unsigned long dest_size );

/* ===========================================================================
   the hot path

   Everything below is the implementation of the operations declared above as
   SERIALIZE_INLINE and SERIALIZE_ALWAYS_INLINE: the ones a packet path
   performs per field, which have to inline into the caller for the bit width
   to fold to a literal. See WHY THE HOT PATH IS IN THIS HEADER at the top of
   this file.

   Nothing below is also defined in serialize.c. A caller including only this
   header gets the whole per-field surface and links.
   =========================================================================== */

/* ---------------------------------------------------------------------------
   endianness

   The wire is little-endian. On a little-endian host the scratch word is
   copied out as-is; on a big-endian host it is byte-swapped, so the bytes
   match everywhere.
   --------------------------------------------------------------------------- */

SERIALIZE_INLINE int serialize_host_is_big_endian( void )
{
    /* Computed rather than #ifdef'd: the preprocessor spellings for this vary
       by compiler and platform, and every one of them is a chance to be wrong
       on a platform nobody tested. A compiler folds this to a constant. */
    union { serialize_uint32_t i; unsigned char c[4]; } probe;
    probe.i = 0x01020304u;
    return probe.c[0] == 0x01;
}

SERIALIZE_INLINE serialize_uint64_t serialize_bswap64( serialize_uint64_t value )
{
#if defined(__GNUC__)
    /* not spelled with an (unsigned long long) cast: naming the type warns
       under -std=c89 -pedantic, and the builtin takes the value as it is */
    return __builtin_bswap64( value );
#else
    /* Built by shifting rather than from 0x...ULL masks: a C89 compiler has no
       64-bit literal suffix it is happy about, and this needs none. */
    serialize_uint64_t result = 0;
    int i;
    for ( i = 0; i < 8; i++ )
    {
        result = ( result << 8 ) | ( ( value >> ( i * 8 ) ) & 0xFF );
    }
    return result;
#endif
}

SERIALIZE_INLINE serialize_uint64_t serialize_host_to_wire64( serialize_uint64_t value )
{
    return serialize_host_is_big_endian() ? serialize_bswap64( value ) : value;
}

SERIALIZE_INLINE serialize_uint64_t serialize_wire_to_host64( serialize_uint64_t value )
{
    return serialize_host_is_big_endian() ? serialize_bswap64( value ) : value;
}

/* ---------------------------------------------------------------------------
   bits required
   --------------------------------------------------------------------------- */

/*
    Bit length, and 0 for 0.

    The count leading zeros builtin is UNDEFINED at zero — on arm64 it happens
    to give the right answer and on x86 it does not — and zero reaches here:
    serialize_u128_bit_length asks for the length of a span, and a degenerate
    128-bit range has a span of zero. So the zero is tested rather than
    assumed. Where the bounds are constants, as they nearly always are, the
    whole thing folds to a literal and none of this survives.
*/

SERIALIZE_INLINE int serialize_bit_length32( serialize_uint32_t value )
{
#if defined(__GNUC__)
    return value ? 32 - __builtin_clz( (unsigned int) value ) : 0;
#else
    int bits = 0;
    while ( value )
    {
        bits++;
        value >>= 1;
    }
    return bits;
#endif
}

SERIALIZE_INLINE int serialize_bit_length64( serialize_uint64_t value )
{
#if defined(__GNUC__)
    return value ? 64 - __builtin_clzll( value ) : 0;
#else
    int bits = 0;
    while ( value )
    {
        bits++;
        value >>= 1;
    }
    return bits;
#endif
}

SERIALIZE_INLINE int serialize_bits_required( serialize_uint32_t min, serialize_uint32_t max )
{
    if ( min == max )
    {
        return 0;
    }
    /* the parameters are already the unsigned domain, so a range spanning the
       whole int32 space cannot overflow the subtraction */
    return serialize_bit_length32( max - min );
}

SERIALIZE_INLINE int serialize_bits_required64( serialize_uint64_t min, serialize_uint64_t max )
{
    if ( min == max )
    {
        return 0;
    }
    return serialize_bit_length64( max - min );
}

/* ---------------------------------------------------------------------------
   write stream
   --------------------------------------------------------------------------- */

SERIALIZE_INLINE void serialize_write_stream_init( serialize_write_stream_t * stream, void * buffer, int bytes )
{
    stream->data = (serialize_uint8_t *) buffer;
    stream->num_bits = bytes * 8;
    stream->bits_written = 0;
    stream->word_index = 0;
    stream->scratch = 0;
    stream->scratch_bits = 0;
    stream->error = 0;
    stream->bits_limit = bytes * 8;
}

SERIALIZE_INLINE int serialize_write_fail( serialize_write_stream_t * stream )
{
    stream->error = 1;
    stream->bits_limit = -1;    /* the bounds test every write already makes now fails for good */
    return 0;
}

SERIALIZE_INLINE int serialize_read_fail( serialize_read_stream_t * stream )
{
    stream->error = 1;
    stream->bits_limit = -1;
    return 0;
}

SERIALIZE_ALWAYS_INLINE int serialize_write_bits( serialize_write_stream_t * SERIALIZE_RESTRICT stream, serialize_uint32_t value, int bits )
{
    int new_scratch_bits;

    /* caller error, asserted exactly where the C++ BitWriter asserts it */
    serialize_assert( bits > 0 );
    serialize_assert( bits <= 32 );

    /*
        Capacity is the writer's contract, not a runtime check (issue #52, the
        ruling: "C should match C++ and have no checks on write at all (except
        assert). ... C and C++ should be equivalent."). The C++ BitWriter
        asserts exactly this and its release build writes unchecked; this port
        used to keep the test as a real branch, and that branch was the
        measured write residual against C++ — it made every write fallible, so
        the compiler kept the scratch word and bit counts correct in memory at
        every field boundary instead of coalescing them in registers across a
        message. Asserted against bits_limit rather than num_bits so a stream
        failed by serialize_write_fail is also caught in a debug build: the
        poisoned limit fails this assert for good.
    */
    serialize_assert( stream->bits_written + bits <= stream->bits_limit );

    /* mask rather than trust: the assert above catches a value wider than its
       declared bits in a debug build, and the mask keeps a release build
       deterministic — the damage stays inside the field it belongs to */
    serialize_assert( bits == 32 || value <= (serialize_uint32_t) ( ( 1UL << bits ) - 1 ) );
    if ( bits < 32 )
    {
        value &= (serialize_uint32_t) ( ( 1UL << bits ) - 1 );
    }

    stream->scratch |= ( (serialize_uint64_t) value ) << stream->scratch_bits;

    new_scratch_bits = stream->scratch_bits + bits;

    if ( new_scratch_bits >= 64 )
    {
        serialize_uint64_t word = serialize_host_to_wire64( stream->scratch );
        memcpy( stream->data + (size_t) stream->word_index * 8, &word, sizeof( word ) );
        stream->word_index++;
        /* recover the bits that spilled past 64. new_scratch_bits >= 64 with
           bits <= 32 means the shift is in [1,32], never the undefined 64 */
        stream->scratch = ( (serialize_uint64_t) value ) >> ( 64 - stream->scratch_bits );
        stream->scratch_bits = new_scratch_bits - 64;
    }
    else
    {
        stream->scratch_bits = new_scratch_bits;
    }

    stream->bits_written += bits;

    return 1;
}

SERIALIZE_INLINE void serialize_write_flush( serialize_write_stream_t * stream )
{
    if ( stream->scratch_bits != 0 )
    {
        serialize_uint64_t word = serialize_host_to_wire64( stream->scratch );
        memcpy( stream->data + (size_t) stream->word_index * 8, &word, sizeof( word ) );
        stream->scratch = 0;
        stream->scratch_bits = 0;
        stream->word_index++;
    }
}

SERIALIZE_INLINE int serialize_write_bits_processed( const serialize_write_stream_t * stream )
{
    return stream->bits_written;
}

SERIALIZE_INLINE int serialize_write_bytes_processed( const serialize_write_stream_t * stream )
{
    return ( stream->bits_written + 7 ) / 8;
}

SERIALIZE_INLINE int serialize_write_bits_available( const serialize_write_stream_t * stream )
{
    return stream->num_bits - stream->bits_written;
}

SERIALIZE_INLINE int serialize_write_error( const serialize_write_stream_t * stream )
{
    return stream->error;
}

/* ---------------------------------------------------------------------------
   read stream
   --------------------------------------------------------------------------- */

SERIALIZE_INLINE void serialize_read_stream_init( serialize_read_stream_t * stream, const void * buffer, int bytes )
{
    const serialize_uint8_t * data = (const serialize_uint8_t *) buffer;

    stream->data = data;
    stream->num_bits = bytes * 8;
    stream->bits_read = 0;
    stream->error = 0;
    stream->bits_limit = bytes * 8;

    /* the final window, built once. See the struct. */
    if ( bytes >= 8 )
    {
        serialize_uint64_t word;
        stream->tail_base = bytes - 8;
        memcpy( &word, data + bytes - 8, sizeof( word ) );
        stream->tail = serialize_wire_to_host64( word );
    }
    else
    {
        /* Spelled out rather than looped: a loop here is a loop the caller's
           per-message code has to carry, and there are at most seven bytes.
           Shifting from the low byte up IS the wire order, so this needs no
           byte swap on either host. */
        serialize_uint64_t word = 0;
        stream->tail_base = 0;
        if ( bytes > 0 ) word |= ( (serialize_uint64_t) data[0] );
        if ( bytes > 1 ) word |= ( (serialize_uint64_t) data[1] ) << 8;
        if ( bytes > 2 ) word |= ( (serialize_uint64_t) data[2] ) << 16;
        if ( bytes > 3 ) word |= ( (serialize_uint64_t) data[3] ) << 24;
        if ( bytes > 4 ) word |= ( (serialize_uint64_t) data[4] ) << 32;
        if ( bytes > 5 ) word |= ( (serialize_uint64_t) data[5] ) << 40;
        if ( bytes > 6 ) word |= ( (serialize_uint64_t) data[6] ) << 48;
        stream->tail = word;
    }
}

SERIALIZE_ALWAYS_INLINE int serialize_read_bits( serialize_read_stream_t * SERIALIZE_RESTRICT stream, serialize_uint32_t * SERIALIZE_RESTRICT value, int bits )
{
    serialize_uint64_t window;
    int byte_index;
    int shift;

    /* caller error, asserted exactly where the C++ BitReader asserts it */
    serialize_assert( bits > 0 );
    serialize_assert( bits <= 32 );

    /* the network's error, not the caller's: this is C++'s WouldReadPastEnd,
       which is a real check there too — and, via the poisoned limit, the
       sticky flag as well. See serialize_write_bits. */
    if ( stream->bits_read + bits > stream->bits_limit )
    {
        return serialize_read_fail( stream );
    }

    /*
        The window. Inside the buffer it is one load; in the last 8 bytes it is
        the word init already assembled, shifted to where this read starts. Two
        instructions either way, and neither runs off the end of the buffer —
        which is what this port promises and the C++ reader does not. See the
        read stream struct.
    */
    byte_index = stream->bits_read >> 3;

    if ( byte_index < stream->tail_base )
    {
        memcpy( &window, stream->data + byte_index, sizeof( window ) );
        window = serialize_wire_to_host64( window );
        shift = stream->bits_read & 7;
    }
    else
    {
        window = stream->tail;
        shift = stream->bits_read - stream->tail_base * 8;
    }

    *value = (serialize_uint32_t) ( window >> shift );
    if ( bits < 32 )
    {
        *value &= (serialize_uint32_t) ( ( 1UL << bits ) - 1 );
    }

    stream->bits_read += bits;

    return 1;
}

SERIALIZE_INLINE int serialize_read_bits_processed( const serialize_read_stream_t * stream )
{
    return stream->bits_read;
}

SERIALIZE_INLINE int serialize_read_bytes_processed( const serialize_read_stream_t * stream )
{
    return ( stream->bits_read + 7 ) / 8;
}

SERIALIZE_INLINE int serialize_read_bits_remaining( const serialize_read_stream_t * stream )
{
    return stream->num_bits - stream->bits_read;
}

SERIALIZE_INLINE int serialize_read_error( const serialize_read_stream_t * stream )
{
    return stream->error;
}

/* ---------------------------------------------------------------------------
   measure stream lifecycle
   --------------------------------------------------------------------------- */

SERIALIZE_INLINE void serialize_measure_stream_init( serialize_measure_stream_t * stream )
{
    stream->bits_written = 0;
}

SERIALIZE_INLINE int serialize_measure_bits_processed( const serialize_measure_stream_t * stream )
{
    return stream->bits_written;
}

SERIALIZE_INLINE int serialize_measure_bytes_processed( const serialize_measure_stream_t * stream )
{
    return ( stream->bits_written + 7 ) / 8;
}

/* ---------------------------------------------------------------------------
   bool
   --------------------------------------------------------------------------- */

SERIALIZE_ALWAYS_INLINE int serialize_write_bool( serialize_write_stream_t * stream, int value )
{
    return serialize_write_bits( stream, value ? 1u : 0u, 1 );
}

SERIALIZE_ALWAYS_INLINE int serialize_read_bool( serialize_read_stream_t * SERIALIZE_RESTRICT stream, int * SERIALIZE_RESTRICT value )
{
    serialize_uint32_t raw = 0;
    if ( !serialize_read_bits( stream, &raw, 1 ) )
    {
        return 0;
    }
    *value = (int) raw;
    return 1;
}

/* ---------------------------------------------------------------------------
   align
   --------------------------------------------------------------------------- */

SERIALIZE_INLINE int serialize_write_align_bits( const serialize_write_stream_t * stream )
{
    return ( 8 - ( stream->bits_written % 8 ) ) % 8;
}

SERIALIZE_INLINE int serialize_read_align_bits( const serialize_read_stream_t * stream )
{
    return ( 8 - ( stream->bits_read % 8 ) ) % 8;
}

SERIALIZE_ALWAYS_INLINE int serialize_write_align( serialize_write_stream_t * stream )
{
    int remainder = stream->bits_written % 8;
    if ( remainder != 0 )
    {
        return serialize_write_bits( stream, 0, 8 - remainder );
    }
    return 1;
}

SERIALIZE_ALWAYS_INLINE int serialize_read_align( serialize_read_stream_t * stream )
{
    int remainder = stream->bits_read % 8;
    if ( remainder != 0 )
    {
        serialize_uint32_t padding = 0;
        if ( !serialize_read_bits( stream, &padding, 8 - remainder ) )
        {
            return 0;
        }
        /* the padding must be zero. A stream where it is not was not produced
           by a conforming writer, and accepting it silently would hide the
           desynchronization until some later field decoded as nonsense. */
        if ( padding != 0 )
        {
            return serialize_read_fail( stream );
        }
        return 1;
    }
    /* nothing to read, so nothing tested the limit. See serialize_write_align. */
    return !stream->error;
}

/* ---------------------------------------------------------------------------
   ranged integers

   min > max, and a value outside [min,max] on WRITE, are caller error and are
   asserted rather than checked — the same split the C++ serialize_int macro
   and SerializeInteger make. What survives into a release build is the READ
   side range check, because that one is about the network.
   --------------------------------------------------------------------------- */

SERIALIZE_ALWAYS_INLINE int serialize_write_int( serialize_write_stream_t * stream, serialize_int32_t value, serialize_int32_t min, serialize_int32_t max )
{
    int bits;
    serialize_uint32_t offset;

    serialize_assert( min <= max );
    serialize_assert( value >= min );
    serialize_assert( value <= max );

    bits = serialize_bits_required( (serialize_uint32_t) min, (serialize_uint32_t) max );
    if ( bits == 0 )
    {
        /* degenerate range: the value IS the range, and nothing is written */
        return 1;
    }

    offset = (serialize_uint32_t) value - (serialize_uint32_t) min;

    return serialize_write_bits( stream, offset, bits );
}

SERIALIZE_ALWAYS_INLINE int serialize_read_int( serialize_read_stream_t * SERIALIZE_RESTRICT stream, serialize_int32_t * SERIALIZE_RESTRICT value, serialize_int32_t min, serialize_int32_t max )
{
    int bits;
    serialize_uint32_t offset = 0;

    serialize_assert( min <= max );

    bits = serialize_bits_required( (serialize_uint32_t) min, (serialize_uint32_t) max );
    if ( bits == 0 )
    {
        if ( stream->error )
        {
            return 0;
        }
        *value = min;
        return 1;
    }

    if ( !serialize_read_bits( stream, &offset, bits ) )
    {
        return 0;
    }

    /* reject, never clamp — this is where untrusted data arrives. Compared in
       the unsigned domain against the span, as the C++ SerializeInteger does:
       min + offset can overflow signed arithmetic on a wide range. */
    if ( offset > (serialize_uint32_t) max - (serialize_uint32_t) min )
    {
        return serialize_read_fail( stream );
    }

    *value = (serialize_int32_t) ( (serialize_uint32_t) min + offset );

    return 1;
}

SERIALIZE_ALWAYS_INLINE int serialize_write_int64( serialize_write_stream_t * stream, serialize_int64_t value, serialize_int64_t min, serialize_int64_t max )
{
    int bits;
    serialize_uint64_t offset;

    serialize_assert( min <= max );
    serialize_assert( value >= min );
    serialize_assert( value <= max );

    bits = serialize_bits_required64( (serialize_uint64_t) min, (serialize_uint64_t) max );
    if ( bits == 0 )
    {
        /* degenerate range: the value IS the range, and nothing is written */
        return 1;
    }

    offset = (serialize_uint64_t) value - (serialize_uint64_t) min;

    if ( bits <= 32 )
    {
        return serialize_write_bits( stream, (serialize_uint32_t) offset, bits );
    }

    /* low 32 first, then the remainder — the same split serialize_bits uses */
    if ( !serialize_write_bits( stream, (serialize_uint32_t) ( offset & 0xFFFFFFFFu ), 32 ) )
    {
        return 0;
    }
    return serialize_write_bits( stream, (serialize_uint32_t) ( offset >> 32 ), bits - 32 );
}

SERIALIZE_ALWAYS_INLINE int serialize_read_int64( serialize_read_stream_t * SERIALIZE_RESTRICT stream, serialize_int64_t * SERIALIZE_RESTRICT value, serialize_int64_t min, serialize_int64_t max )
{
    int bits;
    serialize_uint64_t offset;
    serialize_uint32_t lo = 0;
    serialize_uint32_t hi = 0;

    serialize_assert( min <= max );

    bits = serialize_bits_required64( (serialize_uint64_t) min, (serialize_uint64_t) max );
    if ( bits == 0 )
    {
        if ( stream->error )
        {
            return 0;
        }
        *value = min;
        return 1;
    }

    if ( bits <= 32 )
    {
        if ( !serialize_read_bits( stream, &lo, bits ) )
        {
            return 0;
        }
        offset = (serialize_uint64_t) lo;
    }
    else
    {
        if ( !serialize_read_bits( stream, &lo, 32 ) )
        {
            return 0;
        }
        if ( !serialize_read_bits( stream, &hi, bits - 32 ) )
        {
            return 0;
        }
        offset = (serialize_uint64_t) lo | ( ( (serialize_uint64_t) hi ) << 32 );
    }

    /* compare in the unsigned domain: max - min can exceed int64 range */
    if ( offset > (serialize_uint64_t) max - (serialize_uint64_t) min )
    {
        return serialize_read_fail( stream );
    }

    *value = (serialize_int64_t) ( (serialize_uint64_t) min + offset );

    return 1;
}

/* ---------------------------------------------------------------------------
   fixed-width integers — NOT ranged
   --------------------------------------------------------------------------- */

SERIALIZE_ALWAYS_INLINE int serialize_write_uint8( serialize_write_stream_t * stream, serialize_uint8_t value )
{
    return serialize_write_bits( stream, (serialize_uint32_t) value, 8 );
}

SERIALIZE_ALWAYS_INLINE int serialize_read_uint8( serialize_read_stream_t * SERIALIZE_RESTRICT stream, serialize_uint8_t * SERIALIZE_RESTRICT value )
{
    serialize_uint32_t raw = 0;
    if ( !serialize_read_bits( stream, &raw, 8 ) )
    {
        return 0;
    }
    *value = (serialize_uint8_t) raw;
    return 1;
}

SERIALIZE_ALWAYS_INLINE int serialize_write_uint16( serialize_write_stream_t * stream, serialize_uint16_t value )
{
    return serialize_write_bits( stream, (serialize_uint32_t) value, 16 );
}

SERIALIZE_ALWAYS_INLINE int serialize_read_uint16( serialize_read_stream_t * SERIALIZE_RESTRICT stream, serialize_uint16_t * SERIALIZE_RESTRICT value )
{
    serialize_uint32_t raw = 0;
    if ( !serialize_read_bits( stream, &raw, 16 ) )
    {
        return 0;
    }
    *value = (serialize_uint16_t) raw;
    return 1;
}

SERIALIZE_ALWAYS_INLINE int serialize_write_uint32( serialize_write_stream_t * stream, serialize_uint32_t value )
{
    return serialize_write_bits( stream, value, 32 );
}

SERIALIZE_ALWAYS_INLINE int serialize_read_uint32( serialize_read_stream_t * SERIALIZE_RESTRICT stream, serialize_uint32_t * SERIALIZE_RESTRICT value )
{
    return serialize_read_bits( stream, value, 32 );
}

SERIALIZE_ALWAYS_INLINE int serialize_write_uint64( serialize_write_stream_t * stream, serialize_uint64_t value )
{
    if ( !serialize_write_bits( stream, (serialize_uint32_t) ( value & 0xFFFFFFFFu ), 32 ) )
    {
        return 0;
    }
    return serialize_write_bits( stream, (serialize_uint32_t) ( value >> 32 ), 32 );
}

SERIALIZE_ALWAYS_INLINE int serialize_read_uint64( serialize_read_stream_t * SERIALIZE_RESTRICT stream, serialize_uint64_t * SERIALIZE_RESTRICT value )
{
    serialize_uint32_t lo = 0;
    serialize_uint32_t hi = 0;
    if ( !serialize_read_bits( stream, &lo, 32 ) )
    {
        return 0;
    }
    if ( !serialize_read_bits( stream, &hi, 32 ) )
    {
        return 0;
    }
    *value = (serialize_uint64_t) lo | ( ( (serialize_uint64_t) hi ) << 32 );
    return 1;
}

/* ---------------------------------------------------------------------------
   floating point
   --------------------------------------------------------------------------- */

SERIALIZE_ALWAYS_INLINE int serialize_write_float( serialize_write_stream_t * stream, float value )
{
    serialize_uint32_t bits;
    /* through memcpy rather than a union or a cast: the only spelling that is
       not a strict-aliasing violation in every C standard */
    memcpy( &bits, &value, 4 );
    return serialize_write_bits( stream, bits, 32 );
}

SERIALIZE_ALWAYS_INLINE int serialize_read_float( serialize_read_stream_t * SERIALIZE_RESTRICT stream, float * SERIALIZE_RESTRICT value )
{
    serialize_uint32_t bits = 0;
    if ( !serialize_read_bits( stream, &bits, 32 ) )
    {
        return 0;
    }
    memcpy( value, &bits, 4 );
    return 1;
}

SERIALIZE_ALWAYS_INLINE int serialize_write_double( serialize_write_stream_t * stream, double value )
{
    serialize_uint64_t bits;
    memcpy( &bits, &value, 8 );
    return serialize_write_uint64( stream, bits );
}

SERIALIZE_ALWAYS_INLINE int serialize_read_double( serialize_read_stream_t * SERIALIZE_RESTRICT stream, double * SERIALIZE_RESTRICT value )
{
    serialize_uint64_t bits = 0;
    if ( !serialize_read_uint64( stream, &bits ) )
    {
        return 0;
    }
    memcpy( value, &bits, 8 );
    return 1;
}

/* ---------------------------------------------------------------------------
   bytes — the read half

   The write half is in serialize.c with the other bulk machinery; this one is
   here because after the align it is a single memcpy, and the blocks packet
   code actually reads per field — a hash, a MAC, a session id — are small
   enough that the call across the translation unit boundary was the cost.
   --------------------------------------------------------------------------- */

SERIALIZE_ALWAYS_INLINE int serialize_read_bytes( serialize_read_stream_t * SERIALIZE_RESTRICT stream, serialize_uint8_t * SERIALIZE_RESTRICT data, int bytes )
{
    if ( bytes < 0 )
    {
        return serialize_read_fail( stream );
    }

    if ( !serialize_read_align( stream ) )
    {
        return 0;
    }

    if ( bytes > ( stream->bits_limit - stream->bits_read ) / 8 )
    {
        return serialize_read_fail( stream );
    }

    /* byte aligned by the align above, so this is a straight copy */
    serialize_assert( ( stream->bits_read % 8 ) == 0 );
    memcpy( data, stream->data + ( stream->bits_read >> 3 ), (size_t) bytes );
    stream->bits_read += bytes * 8;

    return 1;
}

/* ---------------------------------------------------------------------------
   measure operations

   The mirror of the write operations above, and inline for the same reason:
   a measure is a handful of additions, and every one of them was a call.
   --------------------------------------------------------------------------- */

SERIALIZE_INLINE int serialize_measure_bits( serialize_measure_stream_t * stream, int bits )
{
    stream->bits_written += bits;
    return 1;
}

SERIALIZE_INLINE int serialize_measure_bool( serialize_measure_stream_t * stream )
{
    stream->bits_written += 1;
    return 1;
}

SERIALIZE_INLINE int serialize_measure_align( serialize_measure_stream_t * stream )
{
    /*
        The worst case, unconditionally — never the exact padding. A measure
        is taken once and the message is then written at arbitrary bit
        positions, where the padding differs: an exact-from-zero count
        under-counts at unaligned starts ({byte, align, byte} measures 16
        bits from zero but costs 23 written from bit 1), and a fits-check
        trusting it overflows the very buffer it sized. The C++ MeasureStream
        returns worst case 7 from GetAlignBits for exactly this reason, and
        the fork ruling makes that the family model: "so if some
        implementations of serialize measure in other languages are exact,
        they probably should not be. make them conservative bounds like in
        C++, and the standard should specify this is what is expected."
    */
    stream->bits_written += 7;
    return 1;
}

SERIALIZE_INLINE int serialize_measure_int( serialize_measure_stream_t * stream, serialize_int32_t min, serialize_int32_t max )
{
    stream->bits_written += serialize_bits_required( (serialize_uint32_t) min, (serialize_uint32_t) max );
    return 1;
}

SERIALIZE_INLINE int serialize_measure_int64( serialize_measure_stream_t * stream, serialize_int64_t min, serialize_int64_t max )
{
    stream->bits_written += serialize_bits_required64( (serialize_uint64_t) min, (serialize_uint64_t) max );
    return 1;
}

SERIALIZE_INLINE int serialize_measure_uint8( serialize_measure_stream_t * stream )
{
    stream->bits_written += 8;
    return 1;
}

SERIALIZE_INLINE int serialize_measure_uint16( serialize_measure_stream_t * stream )
{
    stream->bits_written += 16;
    return 1;
}

SERIALIZE_INLINE int serialize_measure_uint32( serialize_measure_stream_t * stream )
{
    stream->bits_written += 32;
    return 1;
}

SERIALIZE_INLINE int serialize_measure_uint64( serialize_measure_stream_t * stream )
{
    stream->bits_written += 64;
    return 1;
}

SERIALIZE_INLINE int serialize_measure_float( serialize_measure_stream_t * stream )
{
    stream->bits_written += 32;
    return 1;
}

SERIALIZE_INLINE int serialize_measure_double( serialize_measure_stream_t * stream )
{
    stream->bits_written += 64;
    return 1;
}

SERIALIZE_INLINE int serialize_measure_bytes( serialize_measure_stream_t * stream, int bytes )
{
    serialize_measure_align( stream );
    stream->bits_written += bytes * 8;
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* SERIALIZE_H */
