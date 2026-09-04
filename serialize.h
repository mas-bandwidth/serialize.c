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

    WHY THE WHOLE LIBRARY IS IN THIS HEADER

    The C++ library is header only, so every field a caller serializes inlines
    into the caller and the bit width — which comes from bounds that are almost
    always compile-time constants — folds to a literal. A C library compiled as
    a separate translation unit gets none of that: every field costs a call,
    and serialize_bits_required( min, max ) becomes a runtime computation of a
    number the compiler already knew.

    So everything is defined here (the ruling, 2026-08-17: "everything in C
    should be inlined!!!"), and serialize.c is an empty compatibility stub —
    include this header and you have the whole library, whether or not your
    build still compiles the stub. The per-field spines DEMAND inlining as
    SERIALIZE_ALWAYS_INLINE, which says why where it is defined. The bulk and
    branchy bodies — strings, the byte-block write, int_relative, the 128-bit
    lanes — are ordinary SERIALIZE_INLINE header functions, the
    residency the C++ reference gives its own string and block internals:
    their cost is dominated by the work rather than the call, but as TU
    functions they could never inline at ANY threshold, and every short
    string paid a boundary call the C++ template did not. This is a
    size/speed trade made once, for everybody, with no LTO flag and no define
    to opt into: measured on Apple silicon, hoisting the per-field surface
    took ranged-int reads from 22.8 to 186 million packets a second, which is
    the C++ library's number.

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
    Failure does not stop the cursor: a refused read still advances bits_read
    by the bits it asked for (see serialize_read_bits for why), so
    bits_processed on a FAILED stream is meaningless — it counts reads
    ATTEMPTED, not data decoded. Check serialize_read_error; only a stream
    that has not failed has a cursor worth reading.
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
    behavior, yours.

    The measure stream carries the same contracts the same way — debug
    asserts, zero release checking — and so do the caller-owned parameters
    of a READ: bounds the wrong way round on a ranged read are your bug,
    asserted, never a read failure. What a read checks for real, in every
    build mode, is the data. A read validates the network; it does not
    validate you.
*/

#ifndef SERIALIZE_H
#define SERIALIZE_H

/* ---------------------------------------------------------------------------
   version

   Kept in step with the tag by CI, the same contract the C++ library uses.
   --------------------------------------------------------------------------- */

#define SERIALIZE_VERSION_MAJOR 1
#define SERIALIZE_VERSION_MINOR 8
#define SERIALIZE_VERSION_PATCH 0
#define SERIALIZE_VERSION "1.8.0"

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
    SERIALIZE_INLINE — how the library's functions are spelled.

    C89 has no inline keyword, so the floor is plain static: a private copy per
    translation unit, which the compiler inlines anyway at any optimization
    level and which needs SERIALIZE_UNUSED so an unused one does not warn.
    Everything above C89 spells it properly. static either way, so a caller
    that includes only this header links, and two translation units that both
    include it get their own copies and never collide.
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
    read side that price is the buffer-end test, which via the poisoned
    limit is the sticky-failure test as well — the one release-build branch
    the read path carries, same as the C++ ReadStream's WouldReadPastEnd.
    The write side no longer carries any runtime check (issue #52:
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
    bodies (the byte-block write, strings, int_relative), whose cost is the
    work rather than the call — header-resident as SERIALIZE_INLINE since
    the 2026-08-17 hoist, so a call site carrying literal bounds may inline
    and fold them, but nothing forces it. The 128-bit lane cluster was in
    that exempt list until gcc x86-64 showed why it cannot be: a flattened
    real-world spine exhausts gcc's --param large-function-growth budget
    and every remaining plain-inline lane call is refused regardless of
    size. The lanes are instructions in the C++ reference (native
    __int128), so they demand too — receipts at the cluster's own comment.

    The fixed point family is in the demand set END TO END — wrappers, cores,
    the measure half, and the raw-bound helper that exists only to serve
    them. The cores joined first, when the align-up read path (#21) thinned
    the read body enough that the fixed core's price fell to one call's
    width of the threshold (clang priced it 260 against 250 and stranded it
    out of the fixed32/64/128 wrappers — schema's inline gate caught the
    strand on probearray read). The wrappers joined when the real-world
    bench (space-game packets, 2026-08-17) showed why hinting was not
    enough: the schema-generated spine passes the bounds as literals at
    every fixed call site, but the parameters are runtime arguments, so
    clang priced the wrappers 145-250 against the cold-callsite threshold
    of 45 and left every fixed field of the real packet outlined — and the
    C real_packet rows ran far behind the C++ reference, whose templates
    take the same bounds as template parameters and fold the span
    arithmetic to literals. Demanding the whole chain — spine literals into
    wrapper, wrapper into core, raw bound folded on the way — is the C
    spelling of that same fold.

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
    SERIALIZE_WORD_COPY — how the packer's 8-byte word moves are spelled.

    These moves were plain memcpy, and on Darwin that spelling is not what
    the optimizer prices: _FORTIFY_SOURCE is on by default there, and its
    macro capture rewrites memcpy into __builtin___memcpy_chk before the
    compiler proper ever sees the call. The checked form survives into
    LLVM's mid-pipeline as an opaque call, and the loop-unroll cost model
    prices it as one — the 16-wide bitpacker write group came out ~24% over
    the full-unroll budget and lost its unroll, measured C at 180% of C++
    where the airport era had exact parity, and the same opacity sits in
    every generated-C write spine. The C++ leg is never fortified (Darwin's
    fortify capture applies only to C), so the two legs were not being
    priced on the same IR: defeating the capture IS check-model parity.

    __builtin_memcpy is the same operation, byte for byte — only the
    mid-pipeline IR form changes, from opaque call back to the intrinsic
    the unroll pricer folds. Guarded to clang/GCC, which both spell the
    builtin; MSVC never fortifies, so plain memcpy is already the intrinsic
    there.

    Do not "simplify" this back to memcpy: that spelling re-arms the
    fortify capture and re-loses the unroll. Receipts: the 2026-08-16
    post-law investigation (r-candidate.txt and the per-SHA remark
    ledgers).
*/
#if defined(__clang__) || defined(__GNUC__)
#define SERIALIZE_WORD_COPY( dst, src ) __builtin_memcpy( ( dst ), ( src ), 8 )
#else
#define SERIALIZE_WORD_COPY( dst, src ) memcpy( ( dst ), ( src ), 8 )
#endif

/*
    SERIALIZE_BULK_COPY — the same fortify story, for the variable-length
    payload moves: the serialize_read_bytes payload copy and the
    serialize_write_bytes body. Darwin's capture rewrites these memcpy calls
    into __builtin___memcpy_chk too, and the checked form rides the
    mid-pipeline as an opaque call — priced as a call by the inliner, it
    pushed the string body past the inline threshold in generated callers.
    Late simplification folds the check away (the destination's object size
    is unknowable here, so the check never checked anything), but the pricing
    happened before the fold. Same guard as above: clang and GCC spell the
    builtin, MSVC never fortifies.
*/
#if defined(__clang__) || defined(__GNUC__)
#define SERIALIZE_BULK_COPY( dst, src, bytes ) __builtin_memcpy( ( dst ), ( src ), ( bytes ) )
#else
#define SERIALIZE_BULK_COPY( dst, src, bytes ) memcpy( ( dst ), ( src ), ( bytes ) )
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
#include <math.h>     /* ceil, floor — compressed float quantization */

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

    IMPORTANT: the allocation backing the buffer must extend AT LEAST 8 BYTES
    PAST THE END of the data, because the reader loads a 64-bit window from
    the current byte position, and near the end of the stream that window
    begins inside the final bytes — exactly the allocation contract the C++
    BitReader documents. The bytes past the end are loaded but never
    interpreted: poison there changes no decoded value and no refusal, and
    test/roundtrip.c proves it. This is the family's accepted best practice
    (STANDARD.md, Implementation Law — the buffer contract): machinery that
    avoids the slack obligation at the cost of per-operation work in the hot
    path is a slower correct option, and is refused.

    The data pointer itself does not need to be aligned: each window is
    loaded with memcpy, which packet payloads require because they typically
    start at an unaligned offset once the transport header is stripped.

    A payload that arrived in an exactly sized allocation does not meet this
    contract. Read it through serialize_read_stream_init_padded, which copies
    it into a destination you supply and zeroes the slack.

    The buffer must not change while the stream is reading it.
*/
typedef struct serialize_read_stream_t
{
    const serialize_uint8_t * data;

    /*
        The cursor fields are serialize_int64_t — the C++ BitReader's
        int64_t m_bitsRead, matched deliberately rather than cosmetically.
        bits_read advances UNCONDITIONALLY: a refused read still advances it
        by the bits it asked for (see serialize_read_bits for why), so on a
        stream that has already failed, an adversary spamming reads keeps
        the cursor climbing — an int32 cursor could be driven to signed
        overflow, which is undefined behavior. 64 bits puts that beyond
        reach.
    */
    serialize_int64_t num_bits;
    serialize_int64_t bits_read;
    int error;                  /* sticky: once set, every read fails */

    /*
        num_bits, and -1 once the stream has failed.

        Every read already tests that it fits, so poisoning the limit is what
        makes failure sticky WITHOUT a second test per field: one comparison
        answers both questions. And it is the ONLY thing that makes failure
        sticky — the failure path does not restore the cursor (see
        serialize_read_bits), so an unpoisoned limit would accept the next
        read. Set it through serialize_read_fail and never by hand — a
        stream whose error flag is set and whose limit is not would do
        exactly that.
    */
    serialize_int64_t bits_limit;
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
   the buffer — though each still advances the cursor, so the processed
   counters on a failed stream are meaningless (see ERRORS at the top of this
   file). On a WRITE stream the flag is reported by serialize_write_error
   but the release write path never consults it — writes are trusted and
   cannot fail (see ERRORS) — so do not keep writing to a stream you have
   failed: that is caller error, and the poisoned bit limit makes the next
   write assert in a debug build. */
SERIALIZE_INLINE int serialize_write_fail( serialize_write_stream_t * stream );
SERIALIZE_INLINE int serialize_read_fail( serialize_read_stream_t * stream );

SERIALIZE_INLINE void serialize_read_stream_init( serialize_read_stream_t * stream, const void * buffer, int bytes );

/* Reads a payload whose allocation carries no slack.

   serialize_read_stream_init requires the allocation behind the buffer to
   extend at least 8 bytes past the data (see the read stream struct). A
   packet received into an exactly sized allocation does not satisfy that
   contract, and this is the supported way to read one. It copies the
   payload into a destination you supply, zeroes the 8 slack bytes past the
   copy, and initializes the stream over the copy.

   destination must hold at least bytes + 8. The library allocates nothing,
   so the destination is yours: an array of your largest packet size plus 8
   is the usual choice, and one destination serves every packet. A
   destination that is too small is caller error, asserted in a debug build
   and checked by nothing in a release build, like every other size contract
   here. Passing destination_bytes is what gives that assert something to
   test against.

   The slack bytes are zeroed for hygiene only. They are loaded and never
   interpreted, so their contents change no decoded value and no refusal,
   which test/roundtrip.c proves.

   The cost is one memcpy of the payload. Reading in place through
   serialize_read_stream_init remains the fast path, and is what to use
   whenever the receive buffer is yours to size. */
SERIALIZE_INLINE void serialize_read_stream_init_padded( serialize_read_stream_t * stream, void * destination, int destination_bytes, const void * data, int bytes );

/* serialize_int64_t, not int, and the C++ reader's accessors return int64_t
   for the same reason: the cursor these report is 64-bit — see the read
   stream struct. On a FAILED stream all three are meaningless: the cursor
   advances on refused reads too, so they reflect reads attempted, not data
   decoded (see ERRORS at the top of this file). */
SERIALIZE_INLINE serialize_int64_t serialize_read_bits_processed( const serialize_read_stream_t * stream );
SERIALIZE_INLINE serialize_int64_t serialize_read_bytes_processed( const serialize_read_stream_t * stream );
SERIALIZE_INLINE serialize_int64_t serialize_read_bits_remaining( const serialize_read_stream_t * stream );
SERIALIZE_INLINE int serialize_read_error( const serialize_read_stream_t * stream );

SERIALIZE_INLINE void serialize_measure_stream_init( serialize_measure_stream_t * stream );
SERIALIZE_INLINE int serialize_measure_bits_processed( const serialize_measure_stream_t * stream );
SERIALIZE_INLINE int serialize_measure_bytes_processed( const serialize_measure_stream_t * stream );

/* ---------------------------------------------------------------------------
   bit-level primitives
   --------------------------------------------------------------------------- */

/* bits must be in [1,32] and value must be less than 2^bits. Both are caller
   error, so both are asserted rather than checked, exactly as the C++
   BitWriter asserts them — and nothing else: no mask, no fallback. So is
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
SERIALIZE_INLINE int serialize_write_int_relative( serialize_write_stream_t * stream, serialize_int32_t previous, serialize_int32_t current );
SERIALIZE_INLINE int serialize_read_int_relative( serialize_read_stream_t * stream, serialize_int32_t previous, serialize_int32_t * current );

/* ---------------------------------------------------------------------------
   floating point
   --------------------------------------------------------------------------- */

SERIALIZE_ALWAYS_INLINE int serialize_write_float( serialize_write_stream_t * stream, float value );
SERIALIZE_ALWAYS_INLINE int serialize_read_float( serialize_read_stream_t * SERIALIZE_RESTRICT stream, float * SERIALIZE_RESTRICT value );

SERIALIZE_ALWAYS_INLINE int serialize_write_double( serialize_write_stream_t * stream, double value );
SERIALIZE_ALWAYS_INLINE int serialize_read_double( serialize_read_stream_t * SERIALIZE_RESTRICT stream, double * SERIALIZE_RESTRICT value );

/* Lossy by construction: a round trip returns the nearest quantum. */
SERIALIZE_ALWAYS_INLINE int serialize_write_compressed_float( serialize_write_stream_t * stream, float value, float min, float max, float res );
SERIALIZE_ALWAYS_INLINE int serialize_read_compressed_float( serialize_read_stream_t * stream, float * value, float min, float max, float res );

/* The same wire from constants derived ONCE instead of per call, for the
   generated path (mas-bandwidth/schema#82, the ruling verbatim: "Runtime
   side-entry point could be something that supports schema." / "But in
   addition to, not instead of, the current function."). The constants depend
   only on the declaration, never on the value, so a schema compiler derives
   them at code generation time — with exactly the arithmetic of
   serialize_compressed_float_params — and passes them as literals, skipping
   the per-field divide, clamp, ceil and bits_required. The entry points
   above derive with that same function and forward to these, so the two
   spellings are wire identical by construction; test/precomputed.c holds
   the composition, and a frozen copy of the pre-split bodies, to byte and
   bit identity. Constants that are not what the derivation produces are
   caller error, debug-asserted like every other writer contract. */
SERIALIZE_ALWAYS_INLINE void serialize_compressed_float_params( float min, float max, float res, serialize_uint32_t * max_integer_value, int * bits, float * delta );
SERIALIZE_ALWAYS_INLINE int serialize_write_compressed_float_precomputed( serialize_write_stream_t * stream, float value, serialize_uint32_t max_integer_value, int bits, float delta, float min );
SERIALIZE_ALWAYS_INLINE int serialize_read_compressed_float_precomputed( serialize_read_stream_t * stream, float * value, serialize_uint32_t max_integer_value, int bits, float delta, float min );

/* ---------------------------------------------------------------------------
   bytes and strings
   --------------------------------------------------------------------------- */

/* Aligns first — that alignment is part of the format, not an optimization.
   count is not transmitted; both sides must already agree on it.

   The two halves make different demands. The read half after its align is a
   single memcpy, and for the small blocks packet code actually reads per
   field the call was the cost — so it rides the read spine's demand. The
   write half runs the head/body/tail packing machinery, whose cost is the
   work: header-resident like everything else, never forced. */
SERIALIZE_INLINE int serialize_write_bytes( serialize_write_stream_t * stream, const serialize_uint8_t * data, int bytes );
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
SERIALIZE_INLINE int serialize_write_string( serialize_write_stream_t * stream, const char * string, int buffer_size );
SERIALIZE_INLINE int serialize_read_string( serialize_read_stream_t * stream, char * string, int buffer_size );

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

SERIALIZE_ALWAYS_INLINE serialize_uint128_t serialize_uint128_make( serialize_uint64_t hi, serialize_uint64_t lo );
SERIALIZE_ALWAYS_INLINE serialize_int128_t serialize_int128_make( serialize_uint64_t hi, serialize_uint64_t lo );

/* Sign-extends a 64-bit value into the 128-bit domain. */
SERIALIZE_ALWAYS_INLINE serialize_int128_t serialize_int128_from_int64( serialize_int64_t value );

SERIALIZE_ALWAYS_INLINE int serialize_uint128_equal( serialize_uint128_t a, serialize_uint128_t b );
SERIALIZE_ALWAYS_INLINE int serialize_int128_equal( serialize_int128_t a, serialize_int128_t b );

/* -1, 0 or 1. The signed form compares in the signed domain. */
SERIALIZE_ALWAYS_INLINE int serialize_int128_compare( serialize_int128_t a, serialize_int128_t b );

/* Always 128 bits: the low half first, then the high half. NOT ranged. */
SERIALIZE_INLINE int serialize_write_uint128( serialize_write_stream_t * stream, serialize_uint128_t value );
SERIALIZE_INLINE int serialize_read_uint128( serialize_read_stream_t * stream, serialize_uint128_t * value );

/* Ranged, and the only ranged 128-bit operation. Where the range fits 64 bits
   the bytes are identical to serialize_write_int64 over the same bounds, so a
   field may widen from 64 to 128 without moving the wire. */
SERIALIZE_INLINE int serialize_write_int128( serialize_write_stream_t * stream, serialize_int128_t value, serialize_int128_t min, serialize_int128_t max );
SERIALIZE_INLINE int serialize_read_int128( serialize_read_stream_t * stream, serialize_int128_t * value, serialize_int128_t min, serialize_int128_t max );

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

SERIALIZE_ALWAYS_INLINE int serialize_write_fixed32( serialize_write_stream_t * stream, serialize_int32_t value, int integer_bits, int fraction_bits, serialize_int32_t min, serialize_int32_t max );
SERIALIZE_ALWAYS_INLINE int serialize_read_fixed32( serialize_read_stream_t * stream, serialize_int32_t * value, int integer_bits, int fraction_bits, serialize_int32_t min, serialize_int32_t max );

SERIALIZE_ALWAYS_INLINE int serialize_write_fixed64( serialize_write_stream_t * stream, serialize_int64_t value, int integer_bits, int fraction_bits, serialize_int64_t min, serialize_int64_t max );
SERIALIZE_ALWAYS_INLINE int serialize_read_fixed64( serialize_read_stream_t * stream, serialize_int64_t * value, int integer_bits, int fraction_bits, serialize_int64_t min, serialize_int64_t max );

SERIALIZE_ALWAYS_INLINE int serialize_write_fixed128( serialize_write_stream_t * stream, serialize_int128_t value, int integer_bits, int fraction_bits, serialize_int64_t min, serialize_int64_t max );
SERIALIZE_ALWAYS_INLINE int serialize_read_fixed128( serialize_read_stream_t * stream, serialize_int128_t * value, int integer_bits, int fraction_bits, serialize_int64_t min, serialize_int64_t max );

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

SERIALIZE_INLINE int serialize_write_wstring( serialize_write_stream_t * stream, const wchar_t * string, int buffer_size );
SERIALIZE_INLINE int serialize_read_wstring( serialize_read_stream_t * stream, wchar_t * string, int buffer_size );

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

   They return int for symmetry with the write half, and in a release build
   they always return 1: measure carries the writer's contracts — a string or
   wstring longer than its declared buffer, an int_relative that does not
   increase, inverted bounds — as serialize_assert, exactly like the write
   half (issue #52 — see ERRORS). The C++ MeasureStream makes the same split:
   its contracts are debug asserts and its release build is pure bit
   arithmetic with zero checks. Feeding a measure input no conforming writer
   could produce is caller error, caught in a debug build; a release build
   counts it unchecked, exactly as the release writer would write it.

   What a measure cannot even assert is a value out of its declared range,
   because it is never given the value -- only the bounds that set the width.
   The write is where that is asserted.
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

SERIALIZE_INLINE int serialize_measure_int_relative( serialize_measure_stream_t * stream, serialize_int32_t previous, serialize_int32_t current );

SERIALIZE_INLINE int serialize_measure_float( serialize_measure_stream_t * stream );
SERIALIZE_INLINE int serialize_measure_double( serialize_measure_stream_t * stream );
SERIALIZE_ALWAYS_INLINE int serialize_measure_compressed_float( serialize_measure_stream_t * stream, float min, float max, float res );

/* The precomputed companion takes bits alone, not the four constants the
   write and read halves take: the measure doctrine above — the arguments
   that determine the WIDTH and nothing else — and for a precomputed
   compressed float the width IS bits. */
SERIALIZE_ALWAYS_INLINE int serialize_measure_compressed_float_precomputed( serialize_measure_stream_t * stream, int bits );

SERIALIZE_INLINE int serialize_measure_bytes( serialize_measure_stream_t * stream, int bytes );
SERIALIZE_INLINE int serialize_measure_string( serialize_measure_stream_t * stream, const char * string, int buffer_size );
SERIALIZE_INLINE int serialize_measure_wstring( serialize_measure_stream_t * stream, const wchar_t * string, int buffer_size );

SERIALIZE_INLINE int serialize_measure_uint128( serialize_measure_stream_t * stream );
SERIALIZE_INLINE int serialize_measure_int128( serialize_measure_stream_t * stream, serialize_int128_t min, serialize_int128_t max );

SERIALIZE_ALWAYS_INLINE int serialize_measure_fixed32( serialize_measure_stream_t * stream, int integer_bits, int fraction_bits, serialize_int32_t min, serialize_int32_t max );
SERIALIZE_ALWAYS_INLINE int serialize_measure_fixed64( serialize_measure_stream_t * stream, int integer_bits, int fraction_bits, serialize_int64_t min, serialize_int64_t max );
SERIALIZE_ALWAYS_INLINE int serialize_measure_fixed128( serialize_measure_stream_t * stream, int integer_bits, int fraction_bits, serialize_int64_t min, serialize_int64_t max );

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
SERIALIZE_INLINE void serialize_copy_string( char * dest, const char * source, unsigned long dest_size );

/* ===========================================================================
   the implementation

   Everything below is the implementation of every operation declared above —
   the whole library. The per-field surface has to inline into the caller for
   the bit width to fold to a literal, and the bulk bodies live here so that
   a call site carrying literal bounds can inline and fold them too. See WHY
   THE WHOLE LIBRARY IS IN THIS HEADER at the top of this file.

   Nothing is defined in serialize.c. A caller including only this header
   gets the whole library and links.
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

    /* a value wider than its declared bits is caller error, asserted exactly
       where the C++ BitWriter asserts it — and, like the C++ BitWriter, not
       masked: the release write path trusts the value it is given (issue
       #52). This port used to mask here "so the damage stays inside the
       field", which was an invented release-build defence the C++ writer
       does not perform; the cross-language contract is that the CALLER is
       responsible for well-formed writes. */
    serialize_assert( bits == 32 || value <= (serialize_uint32_t) ( ( 1UL << bits ) - 1 ) );

    stream->scratch |= ( (serialize_uint64_t) value ) << stream->scratch_bits;

    new_scratch_bits = stream->scratch_bits + bits;

    if ( new_scratch_bits >= 64 )
    {
        serialize_uint64_t word = serialize_host_to_wire64( stream->scratch );
        SERIALIZE_WORD_COPY( stream->data + (size_t) stream->word_index * 8, &word );
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
        SERIALIZE_WORD_COPY( stream->data + (size_t) stream->word_index * 8, &word );
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
    /* caller error, asserted exactly where the C++ BitReader asserts it.
       The allocation contract — at least 8 bytes past the end of the data,
       see the read stream struct — is the caller's, like every allocation,
       and is checked by nothing here: there is nothing to test it against.
       serialize_read_stream_init_padded takes the destination size and so
       does assert it. */
    serialize_assert( buffer );

    stream->data = (const serialize_uint8_t *) buffer;
    stream->num_bits = (serialize_int64_t) bytes * 8;
    stream->bits_read = 0;
    stream->error = 0;
    stream->bits_limit = (serialize_int64_t) bytes * 8;
}

/* The padded copy for an exactly sized payload: see the declaration above
   for when to reach for it. */
SERIALIZE_INLINE void serialize_read_stream_init_padded( serialize_read_stream_t * stream, void * destination, int destination_bytes, const void * data, int bytes )
{
    serialize_uint64_t slack = 0;

    serialize_assert( destination );
    serialize_assert( bytes >= 0 );

    /* spelled as a subtraction rather than destination_bytes >= bytes + 8 so
       a huge bytes cannot overflow the addition before the assert sees it */
    serialize_assert( destination_bytes >= 8 );
    serialize_assert( destination_bytes - 8 >= bytes );

    if ( bytes > 0 )
    {
        serialize_assert( data );
        SERIALIZE_BULK_COPY( destination, data, (size_t) bytes );
    }

    /* exactly the 8 bytes the reader's final window can reach. They are
       loaded and never interpreted, so zeroing them is hygiene rather than
       correctness, and one word store is the whole cost. */
    SERIALIZE_WORD_COPY( (serialize_uint8_t *) destination + bytes, &slack );

    serialize_read_stream_init( stream, destination, bytes );
}

SERIALIZE_ALWAYS_INLINE int serialize_read_bits( serialize_read_stream_t * SERIALIZE_RESTRICT stream, serialize_uint32_t * SERIALIZE_RESTRICT value, int bits )
{
    serialize_uint64_t window;
    serialize_int64_t begin;

    /* caller error, asserted exactly where the C++ BitReader asserts it */
    serialize_assert( bits > 0 );
    serialize_assert( bits <= 32 );

    /*
        The cursor advances UNCONDITIONALLY, before the limit test, and the
        failure path does not restore it — sticky failure rides entirely on
        the poisoned limit, which already refuses every later read.

        This makes bits_read an AFFINE function of the reads attempted: with
        the advance conditional on the check, every read's bit position was
        control-dependent on every earlier read's limit test, so across an
        unrolled group of reads the compiler had to thread the cursor
        serially — recompute position, then test, then advance, sixteen
        times in a chain. With the cursor affine, every position inside a
        group is a compile-time constant off the group base, the checks
        fold to comparisons against precomputed bounds, and the window
        loads schedule independently — the C++ BitReader's shape. With the
        advance conditional, this read path measured at nearly twice the
        C++ reader's cost, and the serial cursor thread was the mechanism.

        The price is that a failed stream's cursor keeps counting attempts
        (see ERRORS), which is why the cursor is serialize_int64_t — see
        the read stream struct.
    */
    begin = stream->bits_read;
    stream->bits_read = begin + bits;

    /* the network's error, not the caller's: this is C++'s WouldReadPastEnd,
       which is a real check there too — and, via the poisoned limit, the
       sticky flag as well. See serialize_write_bits. */
    if ( begin + bits > stream->bits_limit )
    {
        return serialize_read_fail( stream );
    }

    /* loads up to 7 bytes past the last data byte: the allocation contract
       covers this (see the read stream struct). One unconditional load and a
       shift by the bit remainder — the C++ BitReader's read path, load for
       load, with no per-read branch beyond the buffer-end test above. */
    SERIALIZE_WORD_COPY( &window, stream->data + ( begin >> 3 ) );
    window = serialize_wire_to_host64( window );

    *value = ( (serialize_uint32_t) ( window >> ( (int) ( begin & 7 ) ) ) )
           & (serialize_uint32_t) ( ( ( (serialize_uint64_t) 1 ) << bits ) - 1 );

    return 1;
}

/* Meaningless on a FAILED stream — the cursor counts reads attempted, not
   data decoded. See ERRORS at the top of this file, and the prototypes for
   why these return serialize_int64_t. */
SERIALIZE_INLINE serialize_int64_t serialize_read_bits_processed( const serialize_read_stream_t * stream )
{
    return stream->bits_read;
}

SERIALIZE_INLINE serialize_int64_t serialize_read_bytes_processed( const serialize_read_stream_t * stream )
{
    return ( stream->bits_read + 7 ) / 8;
}

SERIALIZE_INLINE serialize_int64_t serialize_read_bits_remaining( const serialize_read_stream_t * stream )
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
    return (int) ( ( 8 - ( stream->bits_read % 8 ) ) % 8 );
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
    int remainder = (int) ( stream->bits_read % 8 );
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

   On the read spine because after the align it is a single memcpy, and the
   blocks packet code actually reads per field — a hash, a MAC, a session id —
   are small enough that the call was the cost. The write half is with the
   other bulk machinery below.
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

    /* byte aligned by the align above, so this is a straight copy — through
       SERIALIZE_BULK_COPY, because a plain memcpy spelling here re-arms the
       fortify capture the word moves already dodge */
    serialize_assert( ( stream->bits_read % 8 ) == 0 );
    SERIALIZE_BULK_COPY( data, stream->data + ( stream->bits_read >> 3 ), (size_t) bytes );
    stream->bits_read += (serialize_int64_t) bytes * 8;

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

/* ---------------------------------------------------------------------------
   compressed float — header-resident, and the inlining is DEMANDED.

   Hoisted from the translation unit (2026-08-16): the C++ reference's
   compressed float is a header template, so at a call site carrying literal
   min/max/res the whole body inlines and the width computation constant-folds
   away — schema's inline gate shows cpp probearray read at full, zero calls
   out of line. As a TU function this port's version could never inline at
   any threshold: every generated call site paid a boundary call AND
   recomputed the width at runtime. Same rough algorithm, demonstrated in
   C++; this is the C spelling of it. The demand rather than a hint for the
   same reason as the per-field spine above: fallible-chain block-frequency
   decay prices these call sites cold.
   --------------------------------------------------------------------------- */

/*
    Finite, spelled so the C89 floor holds: isfinite is C99. finite - finite
    is zero; Inf - Inf and NaN - NaN are NaN, and NaN compares unequal to
    everything, so the subtraction answers for every input. The Makefile's
    -ffp-contract=off and the absence of any fast-math flag are what keep a
    compiler from folding it. Referenced only from serialize_assert, so it
    compiles to nothing under NDEBUG.
*/
SERIALIZE_INLINE int serialize_float_is_finite( float value )
{
    return value - value == 0.0f;
}

/*
    The width of a compressed float, and the quantization ceiling that goes
    with it. In one place because three callers need it -- write, read and
    measure -- and a formula copied three times is a formula that drifts twice.

    In the demand set with its callers: spelled SERIALIZE_INLINE, gcc x86-64
    stranded exactly this call (x48) out of a flattened real-world spine once
    the --param large-function-growth budget was spent — see the 128-bit lane
    cluster's comment for the full receipts — and every stranded call
    recomputed at runtime a width that folds to a literal whenever min, max
    and res are literals, as the schema-generated call sites always pass them.
*/
SERIALIZE_ALWAYS_INLINE int serialize_compressed_float_bits( float min, float max, float res, serialize_uint32_t * max_integer_value )
{
    float delta = max - min;
    float values = delta / res;
    serialize_assert( min < max && res > 0.0f );
    /* a declaration whose span or step count does not compute finite is
       non-conforming (serialize fork #6, the ruling verbatim: "it's
       non-conforming") — caller error at the site the parameters are
       computed, debug-asserted like every other declaration contract. The
       clamps below keep the release build deterministic regardless. */
    serialize_assert( serialize_float_is_finite( delta ) );
    serialize_assert( serialize_float_is_finite( values ) );
    /* clamp with the !>= form so the uint32 conversion below is defined even
       for pathological delta / res -- NaN fails every ordered comparison, so
       the plain < form lets it through and the conversion of NaN to unsigned
       is undefined in C. The !>= form catches it. Match serialize.h's
       serialize_compressed_float_internal exactly. */
    if ( !( values >= 1.0f ) )
    {
        values = 1.0f;
    }
    else if ( values > 4294967040.0f )
    {
        values = 4294967040.0f;
    }
    *max_integer_value = (serialize_uint32_t) ceil( (double) values );
    /* the ceiling can reach 4294967040, above INT32_MAX: the helper takes the
       unsigned domain, so the value passes through without narrowing into a
       signed parameter (implementation-defined at the C89 floor) */
    return serialize_bits_required( 0, *max_integer_value );
}

/*
    The full constant set for a compressed float declaration, derived once:
    the step count and wire width serialize_compressed_float_bits computes,
    plus the float32 range width the quantization divides by. This is exactly
    the derivation every compressed float call above performs on the way to
    the wire, exposed so it can be paid once instead — the constants depend
    only on the declaration, never on the value, so a schema compiler runs
    this same derivation at code generation time and passes the results to
    the *_compressed_float_precomputed entry points at every generated call
    site (mas-bandwidth/schema#82, the ruling verbatim: "Runtime side-entry
    point could be something that supports schema." / "But in addition to,
    not instead of, the current function."). The legacy entry points derive
    with exactly this function and forward to exactly those entry points, so
    the two spellings are wire identical by construction; test/precomputed.c
    holds the composition, plus a frozen copy of the pre-split bodies, to
    byte and bit identity across the declaration corpus.

    delta is max - min COMPUTED IN FLOAT32: the quantization arithmetic is
    pinned to float32 (STANDARD.md), so the wire depends on this exact value,
    not on the real-number difference. A declaration whose delta or step
    count does not compute finite in float32 is non-conforming and asserts
    in the helper above, like every other declaration contract.
*/
SERIALIZE_ALWAYS_INLINE void serialize_compressed_float_params( float min, float max, float res, serialize_uint32_t * max_integer_value, int * bits, float * delta )
{
    *delta = max - min;
    *bits = serialize_compressed_float_bits( min, max, res, max_integer_value );
}

/*
    The compressed float write from precomputed constants — the audited home
    of the write-side quantization arithmetic. serialize_write_compressed_float
    derives its constants per call and forwards here; generated code passes
    constants a schema compiler derived at generation time with the same
    arithmetic as serialize_compressed_float_params, as literals, so the
    per-field derivation is never paid at runtime. The C++ reference's
    audited home is serialize_compressed_float_precomputed_internal; this is
    the write half of the C spelling of it.

    The constants must be exactly what serialize_compressed_float_params
    derives for a conforming declaration — anything else is caller error,
    debug-asserted per the writes-trusted doctrine and checked nowhere in a
    release build. A wire width that disagrees with the step count would
    occupy a width no other conforming implementation of the declaration
    expects, which is why bits is asserted against the step count rather
    than merely against [1,32].
*/
SERIALIZE_ALWAYS_INLINE int serialize_write_compressed_float_precomputed( serialize_write_stream_t * stream, float value, serialize_uint32_t max_integer_value, int bits, float delta, float min )
{
    float normalized;
    float scaled;
    serialize_uint32_t integer_value;

    serialize_assert( max_integer_value >= 1 );
    serialize_assert( bits == serialize_bits_required( 0, max_integer_value ) );
    serialize_assert( delta > 0.0f );
    serialize_assert( serialize_float_is_finite( delta ) );

    /* a non-finite value is non-conforming (serialize fork #6, the ruling
       verbatim: "attempting to send NaN or INF or anything else through
       compressed float is non-conforming and should assert out on write
       too") — asserted at intake, per the writes-trusted doctrine */
    serialize_assert( serialize_float_is_finite( value ) );

    /* clamp with the !>= / !<= form so a non-finite value that survives into
       a release build (the assert above compiles out) is forced into range
       instead of reaching the uint32 conversion below -- NaN writes as min,
       deterministically. Match the C++ serialize.h's
       serialize_compressed_float_precomputed_internal exactly. */
    normalized = ( value - min ) / delta;
    if ( !( normalized >= 0.0f ) )
    {
        normalized = 0.0f;
    }
    else if ( !( normalized <= 1.0f ) )
    {
        normalized = 1.0f;
    }

    /* The arithmetic is float32, and the two roundings are REQUIRED. Widening
       to double here looks harmless and is not: it changes the wire. Over
       [0,10] at resolution 0.01, value 0.005 quantizes to 1 in float32 and 0
       in double, and 0.025 / 0.105 / 9.995 diverge the same way. Only values
       that land exactly on a quantum agree, which is why a golden built from
       such values stays green while the wire is wrong. The product is stored
       through a local before the add so the intermediate rounds to float32 --
       a compiler is otherwise free to contract the multiply and add into a
       single FMA and round ONCE, which diverges again. Match the C++
       serialize.h's serialize_compressed_float_precomputed_internal exactly. */
    scaled = normalized * (float) max_integer_value;
    integer_value = (serialize_uint32_t) floor( (double) ( scaled + 0.5f ) );

    /* STANDARD.md: the integer clamp is normative (2026-08-23, schema#109).
       Once max_integer_value >= 2^23 the float32 ulp at the top of the range
       reaches 1, so the rounded sum can exceed max_integer_value itself: the
       writer emits a code its own reader rejects, or one bit wider than the
       field. Clamping after the floor closes both; no byte changes for any
       declaration outside [2^23, 2^24). Match the C++ serialize.h's
       serialize_compressed_float_precomputed_internal exactly. */
    if ( integer_value > max_integer_value )
    {
        integer_value = max_integer_value;
    }

    return serialize_write_bits( stream, integer_value, bits );
}

/*
    The read half of the audited home. What a read checks for real, in every
    build mode, is the data: an integer above max_integer_value smuggled into
    the bit headroom is refused, the same refusal the derive-per-call read
    makes. The constants are the CALLER's, asserted like every other
    caller-owned read parameter and never checked in release.
*/
SERIALIZE_ALWAYS_INLINE int serialize_read_compressed_float_precomputed( serialize_read_stream_t * stream, float * value, serialize_uint32_t max_integer_value, int bits, float delta, float min )
{
    serialize_uint32_t integer_value = 0;
    float normalized;
    float scaled;

    serialize_assert( max_integer_value >= 1 );
    serialize_assert( bits == serialize_bits_required( 0, max_integer_value ) );
    serialize_assert( delta > 0.0f );
    serialize_assert( serialize_float_is_finite( delta ) );

    if ( stream->error )
    {
        return 0;
    }

    if ( !serialize_read_bits( stream, &integer_value, bits ) )
    {
        return 0;
    }

    if ( integer_value > max_integer_value )
    {
        return serialize_read_fail( stream );
    }

    /* The reconstruction is float32 like the writer's quantization, and the
       same contraction hazard applies: written as one expression, a compiler
       permitted to contract (clang's default is -ffp-contract=on) may fuse
       the multiply and the add into a single FMA and round once instead of
       twice, and two hosts then reconstruct different floats from the same
       bytes. The wire does not change -- the decoded VALUE does, which is a
       cross-platform divergence of its own. Store the product through a
       local, exactly as the writer does, and see the Makefile's
       -ffp-contract=off for the compilers that fuse across statements. */
    normalized = (float) integer_value / (float) max_integer_value;
    scaled = normalized * delta;
    *value = scaled + min;

    return 1;
}

SERIALIZE_ALWAYS_INLINE int serialize_measure_compressed_float_precomputed( serialize_measure_stream_t * stream, int bits )
{
    /* bits alone: the measure doctrine takes the arguments that determine
       the width and nothing else, and here the width IS bits. All the
       measure can hold the caller to without the step count is the range a
       conforming derivation can produce. */
    serialize_assert( bits >= 1 );
    serialize_assert( bits <= 32 );
    stream->bits_written += bits;
    return 1;
}

/*
    The derive-per-call entry points, since the mas-bandwidth/schema#82 split:
    each body IS its pre-split function, split at the line that issue names —
    everything that depends only on the declaration lives in
    serialize_compressed_float_params, everything that touches the value or
    the wire lives in the precomputed entry point above, statement for
    statement. test/precomputed.c holds this composition to byte and bit
    identity against a frozen verbatim copy of the original unsplit bodies.
*/
SERIALIZE_ALWAYS_INLINE int serialize_write_compressed_float( serialize_write_stream_t * stream, float value, float min, float max, float res )
{
    serialize_uint32_t max_integer_value;
    int bits;
    float delta;
    serialize_compressed_float_params( min, max, res, &max_integer_value, &bits, &delta );
    return serialize_write_compressed_float_precomputed( stream, value, max_integer_value, bits, delta, min );
}

SERIALIZE_ALWAYS_INLINE int serialize_read_compressed_float( serialize_read_stream_t * stream, float * value, float min, float max, float res )
{
    serialize_uint32_t max_integer_value;
    int bits;
    float delta;
    serialize_compressed_float_params( min, max, res, &max_integer_value, &bits, &delta );
    return serialize_read_compressed_float_precomputed( stream, value, max_integer_value, bits, delta, min );
}

SERIALIZE_ALWAYS_INLINE int serialize_measure_compressed_float( serialize_measure_stream_t * stream, float min, float max, float res )
{
    serialize_uint32_t max_integer_value;
    int bits;
    float delta;
    serialize_compressed_float_params( min, max, res, &max_integer_value, &bits, &delta );
    return serialize_measure_compressed_float_precomputed( stream, bits );
}

/* ===========================================================================
   the bulk bodies — header-resident, hoisted from the translation unit
   2026-08-17 (the ruling, verbatim: "everything in C should be inlined!!!").

   These were the last functions living in serialize.c, and every call to one
   was a boundary call: the C++ reference's equivalents — the string and
   wstring internals, WriteBytes, int_relative, the 128-bit and fixed point
   paths — are header templates and inline members that inline at the call
   site, so every short-string read or write in C paid a call C++ did not,
   and the length-field width was recomputed at runtime where a
   literal-bearing call site folds it to a constant (schema's bench rows
   before the hoist: chat write 131% of C++, chat read 120%, testdata read
   125%, and ~74% of batch read's 113% was string/block bits).

   SERIALIZE_INLINE, not the demand: the reference's own string internals are
   plain header functions, and these bodies' cost is the work rather than the
   call — header residency is the win, and the inliner keeps its judgment.
   The one exception is the fixed point cores, which carried the demand
   before the hoist (#22) and keep it. Bodies verbatim from the translation
   unit; the wire is untouched.
   =========================================================================== */

/* ---------------------------------------------------------------------------
   int_relative
   --------------------------------------------------------------------------- */

SERIALIZE_INLINE int serialize_write_int_relative( serialize_write_stream_t * stream, serialize_int32_t previous, serialize_int32_t current )
{
    serialize_uint32_t difference;

    /* STRICTLY increasing is the writer's contract, debug-asserted exactly
       where the C++ writer asserts it (issue #52); the reader enforces it
       for real, because that is where untrusted data arrives */
    serialize_assert( current > previous );

    /* subtract in the unsigned domain: current - previous overflows signed
       arithmetic when the gap is wider than 2^31 */
    difference = (serialize_uint32_t) current - (serialize_uint32_t) previous;

    if ( !serialize_write_bool( stream, difference == 1 ) ) return 0;
    if ( difference == 1 ) return 1;

    if ( !serialize_write_bool( stream, difference <= 6 ) ) return 0;
    if ( difference <= 6 )
    {
        return serialize_write_int( stream, (serialize_int32_t) difference, 2, 6 );
    }

    if ( !serialize_write_bool( stream, difference <= 23 ) ) return 0;
    if ( difference <= 23 )
    {
        return serialize_write_int( stream, (serialize_int32_t) difference, 7, 23 );
    }

    if ( !serialize_write_bool( stream, difference <= 280 ) ) return 0;
    if ( difference <= 280 )
    {
        return serialize_write_int( stream, (serialize_int32_t) difference, 24, 280 );
    }

    if ( !serialize_write_bool( stream, difference <= 4377 ) ) return 0;
    if ( difference <= 4377 )
    {
        return serialize_write_int( stream, (serialize_int32_t) difference, 281, 4377 );
    }

    if ( !serialize_write_bool( stream, difference <= 69914 ) ) return 0;
    if ( difference <= 69914 )
    {
        return serialize_write_int( stream, (serialize_int32_t) difference, 4378, 69914 );
    }

    /*
        The fallback writes CURRENT, not the difference. Every tier above
        encodes the difference, so this looks like an inconsistency and is
        not: at this width there is nothing to gain from the subtraction, and
        sending the absolute value lets the reader validate current > previous
        directly. STANDARD.md said only "32 raw bits" without saying of what,
        which is how this port got it wrong first time round.
    */
    return serialize_write_bits( stream, (serialize_uint32_t) current, 32 );
}

SERIALIZE_INLINE int serialize_read_int_relative( serialize_read_stream_t * stream, serialize_int32_t previous, serialize_int32_t * current )
{
    int flag = 0;
    serialize_int32_t difference = 0;

    if ( stream->error )
    {
        return 0;
    }

    if ( !serialize_read_bool( stream, &flag ) ) return 0;
    if ( flag )
    {
        *current = (serialize_int32_t) ( (serialize_uint32_t) previous + 1 );
        return 1;
    }

    if ( !serialize_read_bool( stream, &flag ) ) return 0;
    if ( flag )
    {
        if ( !serialize_read_int( stream, &difference, 2, 6 ) ) return 0;
        *current = (serialize_int32_t) ( (serialize_uint32_t) previous + (serialize_uint32_t) difference );
        return 1;
    }

    if ( !serialize_read_bool( stream, &flag ) ) return 0;
    if ( flag )
    {
        if ( !serialize_read_int( stream, &difference, 7, 23 ) ) return 0;
        *current = (serialize_int32_t) ( (serialize_uint32_t) previous + (serialize_uint32_t) difference );
        return 1;
    }

    if ( !serialize_read_bool( stream, &flag ) ) return 0;
    if ( flag )
    {
        if ( !serialize_read_int( stream, &difference, 24, 280 ) ) return 0;
        *current = (serialize_int32_t) ( (serialize_uint32_t) previous + (serialize_uint32_t) difference );
        return 1;
    }

    if ( !serialize_read_bool( stream, &flag ) ) return 0;
    if ( flag )
    {
        if ( !serialize_read_int( stream, &difference, 281, 4377 ) ) return 0;
        *current = (serialize_int32_t) ( (serialize_uint32_t) previous + (serialize_uint32_t) difference );
        return 1;
    }

    if ( !serialize_read_bool( stream, &flag ) ) return 0;
    if ( flag )
    {
        if ( !serialize_read_int( stream, &difference, 4378, 69914 ) ) return 0;
        *current = (serialize_int32_t) ( (serialize_uint32_t) previous + (serialize_uint32_t) difference );
        return 1;
    }

    {
        serialize_uint32_t raw = 0;
        if ( !serialize_read_bits( stream, &raw, 32 ) ) return 0;
        *current = (serialize_int32_t) raw;
        /* the absolute form carries no ordering guarantee of its own, so the
           reader enforces it */
        if ( *current <= previous )
        {
            return serialize_read_fail( stream );
        }
    }

    return 1;
}

/* ---------------------------------------------------------------------------
   bytes and strings
   --------------------------------------------------------------------------- */

/*
    The bulk write path, mirroring the C++ WriteBytes. The read half rides the
    read spine above — after its align it is one memcpy, and the small blocks
    packet code reads per field made the call the cost.

    A block of bytes is byte aligned by construction — serialize_write_align
    runs first, and that alignment is part of the format — so the whole payload
    is copied straight into the buffer instead of pushed through the packer
    eight bits at a time. The bytes are identical either way and on either
    endianness: a byte packed through the scratch word lands at its own offset
    in the buffer, because the scratch is byte swapped on a big-endian host
    precisely so that it does — which is also why the partial words at the
    block's edges can move between scratch and buffer as single stores and
    loads. The golden vectors pin that, and CI runs them on s390x.
*/

SERIALIZE_INLINE int serialize_write_bytes( serialize_write_stream_t * stream, const serialize_uint8_t * data, int bytes )
{
    int tail_bits;

    serialize_assert( bytes >= 0 );

    if ( !serialize_write_align( stream ) )
    {
        return 0;
    }

    /* capacity is the writer's contract, asserted exactly where the C++
       WriteBytes asserts it (issue #52). Divided rather than multiplied so
       the comparison cannot overflow int on a block larger than 256MB, and
       against bits_limit so a stream failed by serialize_write_fail is
       caught too. */
    serialize_assert( bytes <= ( stream->bits_limit - stream->bits_written ) / 8 );

    /* byte aligned by the align above, and mid-stream the scratch tracks the
       cursor: scratch_bits == bits_written % 64 */
    serialize_assert( ( stream->bits_written % 8 ) == 0 );
    serialize_assert( stream->scratch_bits == stream->bits_written % 64 );

    /*
        The head: one word store, not a byte loop. The partial scratch word
        goes to the buffer as a whole 8-byte store — its low scratch_bits are
        the bytes already written, its high bits are zero, and the payload
        copy below overwrites exactly those zero bytes. The old shape pushed
        the head AND the tail through the packer a byte at a time, which is
        the loop that priced this function out of generated callers; the C++
        WriteBytes has the same byte loops and pays the same way, so this is
        not a place where mirroring the reference shape was the fast form —
        both sides of the airport comparison were wrong here, and the wire
        bytes are identical either way.
    */
    if ( stream->scratch_bits != 0 )
    {
        serialize_uint64_t word = serialize_host_to_wire64( stream->scratch );
        SERIALIZE_WORD_COPY( stream->data + (size_t) stream->word_index * 8, &word );
    }

    /* the body: the whole payload, straight in at the byte cursor */
    SERIALIZE_BULK_COPY( stream->data + (size_t) ( stream->bits_written >> 3 ), data, (size_t) bytes );

    stream->bits_written += bytes * 8;
    stream->word_index = stream->bits_written / 64;

    /*
        The tail: reload the trailing partial word into the scratch, so later
        writes pack into it exactly as if its bytes had gone through the
        packer. The load reads the word the final flush is already obliged to
        store (tail_bits != 0 keeps scratch_bits != 0, so flush WILL store
        it), so it touches no memory the stream does not already own; the
        bits above the tail are whatever the buffer held and the mask
        discards them.
    */
    tail_bits = stream->bits_written % 64;
    if ( tail_bits != 0 )
    {
        serialize_uint64_t word;
        SERIALIZE_WORD_COPY( &word, stream->data + (size_t) stream->word_index * 8 );
        stream->scratch = serialize_wire_to_host64( word ) & ( ( ( (serialize_uint64_t) 1 ) << tail_bits ) - 1 );
    }
    else
    {
        stream->scratch = 0;
    }
    stream->scratch_bits = tail_bits;

    return 1;
}

/*
    The string payload is well-formed UTF-8 (STANDARD.md, adopted 2026-08-15).
    On the write side that is the writer's obligation, debug-asserted per the
    writes-trusted doctrine. On the READ side it is a mandatory refusal in
    every build mode (ruling #8, adopted 2026-08-15): the reader faces
    untrusted bytes, and passing malformed UTF-8 through hands every consumer
    downstream the job this boundary exists to do.

    This is the FULL well-formedness definition, not just continuation-byte
    shape: overlong encodings, surrogate code points and values above U+10FFFF
    are all refused. The overlongs matter most — an overlong encoding is the
    classic bypass of any filter that runs after the decode, which is exactly
    where a reader that "just checks the bit pattern" leaves its callers.
*/
SERIALIZE_INLINE int serialize_string_is_valid_utf8( const char * string, int length )
{
    int i = 0;
    while ( i < length )
    {
        unsigned int lead = (unsigned char) string[i];
        if ( lead < 0x80 )
        {
            i += 1;
        }
        else if ( ( lead & 0xE0 ) == 0xC0 )
        {
            if ( lead < 0xC2 ) return 0;                                    /* overlong */
            if ( i + 1 >= length ) return 0;
            if ( ( (unsigned char) string[i+1] & 0xC0 ) != 0x80 ) return 0;
            i += 2;
        }
        else if ( ( lead & 0xF0 ) == 0xE0 )
        {
            unsigned int b1, b2;
            if ( i + 2 >= length ) return 0;
            b1 = (unsigned char) string[i+1];
            b2 = (unsigned char) string[i+2];
            if ( ( b1 & 0xC0 ) != 0x80 || ( b2 & 0xC0 ) != 0x80 ) return 0;
            if ( lead == 0xE0 && b1 < 0xA0 ) return 0;                      /* overlong */
            if ( lead == 0xED && b1 >= 0xA0 ) return 0;                     /* surrogate code point */
            i += 3;
        }
        else if ( ( lead & 0xF8 ) == 0xF0 )
        {
            unsigned int b1, b2, b3;
            if ( lead > 0xF4 ) return 0;                                    /* above U+10FFFF */
            if ( i + 3 >= length ) return 0;
            b1 = (unsigned char) string[i+1];
            b2 = (unsigned char) string[i+2];
            b3 = (unsigned char) string[i+3];
            if ( ( b1 & 0xC0 ) != 0x80 || ( b2 & 0xC0 ) != 0x80 || ( b3 & 0xC0 ) != 0x80 ) return 0;
            if ( lead == 0xF0 && b1 < 0x90 ) return 0;                      /* overlong */
            if ( lead == 0xF4 && b1 >= 0x90 ) return 0;                     /* above U+10FFFF */
            i += 4;
        }
        else
        {
            return 0;                                                       /* continuation or invalid lead byte */
        }
    }
    return 1;
}

SERIALIZE_INLINE int serialize_write_string( serialize_write_stream_t * stream, const char * string, int buffer_size )
{
    int length;

    length = (int) strlen( string );

    /* fitting the declared buffer is the writer's contract, asserted exactly
       where the C++ serialize_string_internal asserts it (issue #52) */
    serialize_assert( length < buffer_size );

    /* the writer's contract, debug only. See serialize_string_is_valid_utf8. */
    serialize_assert( serialize_string_is_valid_utf8( string, length ) );

    if ( !serialize_write_int( stream, length, 0, buffer_size - 1 ) )
    {
        return 0;
    }

    if ( length > 0 )
    {
        return serialize_write_bytes( stream, (const serialize_uint8_t *) string, length );
    }

    /*
        A zero-length string still aligns. serialize_write_bytes aligns before
        writing, so the zero-byte case must align too or the reader and writer
        disagree about where the next field starts.
    */
    return serialize_write_align( stream );
}

SERIALIZE_INLINE int serialize_read_string( serialize_read_stream_t * stream, char * string, int buffer_size )
{
    serialize_int32_t length = 0;

    if ( stream->error )
    {
        return 0;
    }

    if ( !serialize_read_int( stream, &length, 0, buffer_size - 1 ) )
    {
        return 0;
    }

    if ( length > 0 )
    {
        int i;

        if ( !serialize_read_bytes( stream, (serialize_uint8_t *) string, length ) )
        {
            return 0;
        }

        /*
            Content refusals (ruling #8, adopted 2026-08-15). The reader
            REFUSES what a conforming writer cannot produce, in every build
            mode — the write side trusts its caller, the read side trusts
            nobody.

            The interior NUL first, and separately from UTF-8, because NUL is
            well-formed UTF-8 (U+0000) and the validator below cannot see it.
            A conforming writer derives length from strlen, so a NUL inside
            the counted payload is impossible from conformance — and accepting
            one mints a string whose wire length and whose strlen-perceived
            length disagree, the two-lengths smuggling primitive.
        */
        for ( i = 0; i < length; i++ )
        {
            if ( string[i] == '\0' )
            {
                return serialize_read_fail( stream );
            }
        }

        if ( !serialize_string_is_valid_utf8( string, length ) )
        {
            return serialize_read_fail( stream );
        }
    }
    else
    {
        if ( !serialize_read_align( stream ) )
        {
            return 0;
        }
    }

    string[length] = '\0';

    return 1;
}

/* ---------------------------------------------------------------------------
   helpers
   --------------------------------------------------------------------------- */

SERIALIZE_INLINE void serialize_copy_string( char * dest, const char * source, unsigned long dest_size )
{
    unsigned long i;

    if ( dest_size == 0 )
    {
        return;
    }

    for ( i = 0; i < dest_size - 1 && source[i] != '\0'; i++ )
    {
        dest[i] = source[i];
    }

    dest[i] = '\0';
}

/* ---------------------------------------------------------------------------
   128-bit integers, emulated as two 64-bit lanes

   Requiring __int128 would exclude MSVC and every C89 toolchain, and buys
   nothing: STANDARD.md defines the 128-bit operations in terms of 32-bit
   groups from least significant upward, which two lanes reproduce exactly.

   The whole lane cluster is in the demand set. The C++ reference gets these
   operations as native unsigned __int128 arithmetic where the compiler has
   it (its serialize.h documents native as "the fastest representation"), so
   its flattened spine carries them as instructions, never as calls. Spelled
   SERIALIZE_INLINE here, gcc x86-64 disagreed at exactly one place: a
   flattened real-world spine (97 fields) exhausts gcc's
   --param large-function-growth budget, after which every remaining plain
   inline candidate is refused regardless of size — opt-info showed 2,346
   refusals on that spine, stranding ~320 lane-helper calls per message
   (u128_sub x80, u128_bit_length and compressed_float_bits x48 each,
   u128_group32 and int128_from_int64 x32 each, set_group32/compare/add x16
   each — down to uint128_make, which is two stores). Each stranded call
   also un-folds the literal-bounds arithmetic behind it. always_inline is
   exempt from the growth budget (proven on the same binary by #27/#28,
   whose functions vanished from the stranded list), so the demand is the C
   spelling of the reference's native-__int128 shape. clang was already
   inlining all of these; for it the demand is a no-op.
   --------------------------------------------------------------------------- */

SERIALIZE_ALWAYS_INLINE serialize_uint128_t serialize_uint128_make( serialize_uint64_t hi, serialize_uint64_t lo )
{
    serialize_uint128_t r;
    r.hi = hi;
    r.lo = lo;
    return r;
}

SERIALIZE_ALWAYS_INLINE serialize_int128_t serialize_int128_make( serialize_uint64_t hi, serialize_uint64_t lo )
{
    serialize_int128_t r;
    r.hi = hi;
    r.lo = lo;
    return r;
}

SERIALIZE_ALWAYS_INLINE serialize_int128_t serialize_int128_from_int64( serialize_int64_t value )
{
    serialize_int128_t r;
    r.lo = (serialize_uint64_t) value;
    /* sign extend: an arithmetic shift of a negative value is
       implementation-defined in C, so the sign is tested instead */
    r.hi = ( value < 0 ) ? ~(serialize_uint64_t) 0 : (serialize_uint64_t) 0;
    return r;
}

SERIALIZE_ALWAYS_INLINE int serialize_uint128_equal( serialize_uint128_t a, serialize_uint128_t b )
{
    return a.lo == b.lo && a.hi == b.hi;
}

SERIALIZE_ALWAYS_INLINE int serialize_int128_equal( serialize_int128_t a, serialize_int128_t b )
{
    return a.lo == b.lo && a.hi == b.hi;
}

/* a - b in the unsigned 128-bit domain, wrapping like the hardware would */
SERIALIZE_ALWAYS_INLINE serialize_uint128_t serialize_u128_sub( serialize_uint128_t a, serialize_uint128_t b )
{
    serialize_uint128_t r;
    r.lo = a.lo - b.lo;
    r.hi = a.hi - b.hi - ( a.lo < b.lo ? 1u : 0u );
    return r;
}

SERIALIZE_ALWAYS_INLINE int serialize_u128_compare( serialize_uint128_t a, serialize_uint128_t b )
{
    if ( a.hi != b.hi )
    {
        return a.hi < b.hi ? -1 : 1;
    }
    if ( a.lo != b.lo )
    {
        return a.lo < b.lo ? -1 : 1;
    }
    return 0;
}

SERIALIZE_ALWAYS_INLINE int serialize_int128_compare( serialize_int128_t a, serialize_int128_t b )
{
    /* flip the sign bit and compare unsigned: the standard trick, and it
       avoids any signed overflow */
    serialize_uint128_t ua, ub;
    ua.lo = a.lo;
    ua.hi = a.hi ^ ( (serialize_uint64_t) 1 << 63 );
    ub.lo = b.lo;
    ub.hi = b.hi ^ ( (serialize_uint64_t) 1 << 63 );
    return serialize_u128_compare( ua, ub );
}

SERIALIZE_ALWAYS_INLINE int serialize_u128_bit_length( serialize_uint128_t v )
{
    if ( v.hi != 0 )
    {
        return 64 + serialize_bit_length64( v.hi );
    }
    return serialize_bit_length64( v.lo );
}

/* the 32-bit group at index i, counting from least significant */
SERIALIZE_ALWAYS_INLINE serialize_uint32_t serialize_u128_group32( serialize_uint128_t v, int i )
{
    switch ( i )
    {
        case 0: return (serialize_uint32_t) ( v.lo & 0xFFFFFFFFu );
        case 1: return (serialize_uint32_t) ( v.lo >> 32 );
        case 2: return (serialize_uint32_t) ( v.hi & 0xFFFFFFFFu );
        default: return (serialize_uint32_t) ( v.hi >> 32 );
    }
}

SERIALIZE_ALWAYS_INLINE void serialize_u128_set_group32( serialize_uint128_t * v, int i, serialize_uint32_t g )
{
    switch ( i )
    {
        case 0: v->lo |= (serialize_uint64_t) g; break;
        case 1: v->lo |= ( (serialize_uint64_t) g ) << 32; break;
        case 2: v->hi |= (serialize_uint64_t) g; break;
        default: v->hi |= ( (serialize_uint64_t) g ) << 32; break;
    }
}

SERIALIZE_ALWAYS_INLINE serialize_uint128_t serialize_u128_add( serialize_uint128_t a, serialize_uint128_t b )
{
    serialize_uint128_t r;
    r.lo = a.lo + b.lo;
    r.hi = a.hi + b.hi + ( r.lo < a.lo ? 1u : 0u );
    return r;
}

/*
    Writes `bits` bits of an unsigned 128-bit value as 32-bit groups from least
    significant upward: bits <= 32 is a single group, otherwise full groups
    from the bottom with the final group carrying the remainder. This is the
    same splitting rule serialize_bits uses for wide values, and the one the
    128-bit and fixed point paths share.

    In the demand set with the fixed point family (#27): every schema fixed
    call site hands the cores a literal bit count, so this loop folds to one
    or two serialize_write_bits calls at a folded width — but spelled
    SERIALIZE_INLINE the loop priced over the cold-callsite threshold and
    clang stranded exactly this call out of the folded spine (8 `bl
    serialize_write_u128_bits` in the real packet's write spine, one per
    fixed field, each re-deriving the fold at runtime). The wrappers' demand
    is worthless if the chain breaks one level down; this is the last link.
*/
SERIALIZE_ALWAYS_INLINE int serialize_write_u128_bits( serialize_write_stream_t * stream, serialize_uint128_t value, int bits )
{
    int written = 0;
    int group = 0;

    while ( written < bits )
    {
        int chunk = bits - written;
        if ( chunk > 32 )
        {
            chunk = 32;
        }
        if ( !serialize_write_bits( stream, serialize_u128_group32( value, group ), chunk ) )
        {
            return 0;
        }
        written += chunk;
        group++;
    }

    return 1;
}

SERIALIZE_ALWAYS_INLINE int serialize_read_u128_bits( serialize_read_stream_t * stream, serialize_uint128_t * value, int bits )
{
    int read = 0;
    int group = 0;

    value->lo = 0;
    value->hi = 0;

    while ( read < bits )
    {
        serialize_uint32_t g = 0;
        int chunk = bits - read;
        if ( chunk > 32 )
        {
            chunk = 32;
        }
        if ( !serialize_read_bits( stream, &g, chunk ) )
        {
            return 0;
        }
        serialize_u128_set_group32( value, group, g );
        read += chunk;
        group++;
    }

    return 1;
}

SERIALIZE_INLINE int serialize_write_uint128( serialize_write_stream_t * stream, serialize_uint128_t value )
{
    if ( !serialize_write_uint64( stream, value.lo ) )
    {
        return 0;
    }
    return serialize_write_uint64( stream, value.hi );
}

SERIALIZE_INLINE int serialize_read_uint128( serialize_read_stream_t * stream, serialize_uint128_t * value )
{
    if ( !serialize_read_uint64( stream, &value->lo ) )
    {
        return 0;
    }
    return serialize_read_uint64( stream, &value->hi );
}

SERIALIZE_INLINE int serialize_write_int128( serialize_write_stream_t * stream, serialize_int128_t value, serialize_int128_t min, serialize_int128_t max )
{
    serialize_uint128_t uvalue, umin, umax, span, offset;
    int bits;

    /* bounds and range are the writer's contract, asserted exactly as the
       C++ SerializeInteger128 asserts them (issue #52) */
    serialize_assert( serialize_int128_compare( min, max ) <= 0 );
    serialize_assert( serialize_int128_compare( value, min ) >= 0 );
    serialize_assert( serialize_int128_compare( value, max ) <= 0 );

    /* converted to the unsigned domain first, so a range wider than 2^127 is
       exact rather than overflowing */
    uvalue = serialize_uint128_make( value.hi, value.lo );
    umin = serialize_uint128_make( min.hi, min.lo );
    umax = serialize_uint128_make( max.hi, max.lo );

    span = serialize_u128_sub( umax, umin );
    bits = serialize_u128_bit_length( span );
    if ( bits == 0 )
    {
        return 1;
    }

    offset = serialize_u128_sub( uvalue, umin );

    return serialize_write_u128_bits( stream, offset, bits );
}

SERIALIZE_INLINE int serialize_read_int128( serialize_read_stream_t * stream, serialize_int128_t * value, serialize_int128_t min, serialize_int128_t max )
{
    serialize_uint128_t umin, umax, span, offset, result;
    int bits;

    if ( stream->error )
    {
        return 0;
    }

    /* bounds are the caller's, not the network's: asserted exactly as the
       C++ ReadStream::SerializeInteger128 asserts them, never checked in a
       release build. What IS checked for real, below, is the decoded offset
       against the span — that is where untrusted data arrives. */
    serialize_assert( serialize_int128_compare( min, max ) <= 0 );

    umin = serialize_uint128_make( min.hi, min.lo );
    umax = serialize_uint128_make( max.hi, max.lo );
    span = serialize_u128_sub( umax, umin );
    bits = serialize_u128_bit_length( span );

    if ( bits == 0 )
    {
        *value = min;
        return 1;
    }

    if ( !serialize_read_u128_bits( stream, &offset, bits ) )
    {
        return 0;
    }

    /* reject, never clamp */
    if ( serialize_u128_compare( offset, span ) > 0 )
    {
        return serialize_read_fail( stream );
    }

    result = serialize_u128_add( umin, offset );
    value->lo = result.lo;
    value->hi = result.hi;

    return 1;
}

/* ---------------------------------------------------------------------------
   fixed point
   --------------------------------------------------------------------------- */

/*
    The shared core: an offset encoding over the RAW (scaled) bounds. All three
    widths funnel here, so there is one place the format lives.
*/
SERIALIZE_ALWAYS_INLINE int serialize_write_fixed_core( serialize_write_stream_t * stream, serialize_uint128_t raw_value,
                                       serialize_uint128_t raw_min, serialize_uint128_t raw_max )
{
    serialize_uint128_t span = serialize_u128_sub( raw_max, raw_min );
    int bits = serialize_u128_bit_length( span );
    serialize_uint128_t offset;

    if ( bits == 0 )
    {
        /* degenerate range: the value IS the range, nothing to send
           (STANDARD.md: min == max costs zero bits, on every storage width).
           The write-side check is a debug assert, per the writes-trusted
           doctrine. */
        serialize_assert( raw_value.lo == raw_min.lo && raw_value.hi == raw_min.hi );
        return 1;
    }

    offset = serialize_u128_sub( raw_value, raw_min );

    /* the value must be within [min,max] whole units — the writer's contract,
       asserted exactly as the C++ fixed point writer asserts it (issue #52) */
    serialize_assert( serialize_u128_compare( offset, span ) <= 0 );

    return serialize_write_u128_bits( stream, offset, bits );
}

SERIALIZE_ALWAYS_INLINE int serialize_read_fixed_core( serialize_read_stream_t * stream, serialize_uint128_t * raw_value,
                                      serialize_uint128_t raw_min, serialize_uint128_t raw_max )
{
    serialize_uint128_t span = serialize_u128_sub( raw_max, raw_min );
    int bits = serialize_u128_bit_length( span );
    serialize_uint128_t offset;

    if ( bits == 0 )
    {
        *raw_value = raw_min;
        return 1;
    }

    if ( !serialize_read_u128_bits( stream, &offset, bits ) )
    {
        return 0;
    }

    if ( serialize_u128_compare( offset, span ) > 0 )
    {
        return serialize_read_fail( stream );
    }

    *raw_value = serialize_u128_add( raw_min, offset );

    return 1;
}

/* min and max are WHOLE units; the raw bound is min << fraction_bits.
   In the demand set with the rest of the fixed family: fraction_bits is a
   literal at every schema call site, so this whole body folds to a constant
   — unless a cold-callsite inliner strands it, which is exactly what the
   demand forbids. */
SERIALIZE_ALWAYS_INLINE serialize_uint128_t serialize_raw_bound( serialize_int64_t whole, int fraction_bits )
{
    serialize_int128_t wide = serialize_int128_from_int64( whole );
    serialize_uint128_t u = serialize_uint128_make( wide.hi, wide.lo );
    int i;
    for ( i = 0; i < fraction_bits; i++ )
    {
        u.hi = ( u.hi << 1 ) | ( u.lo >> 63 );
        u.lo = u.lo << 1;
    }
    return u;
}

SERIALIZE_ALWAYS_INLINE int serialize_write_fixed32( serialize_write_stream_t * stream, serialize_int32_t value, int integer_bits, int fraction_bits, serialize_int32_t min, serialize_int32_t max )
{
    serialize_int128_t wide;
    (void) integer_bits;
    wide = serialize_int128_from_int64( (serialize_int64_t) value );
    return serialize_write_fixed_core( stream, serialize_uint128_make( wide.hi, wide.lo ),
                                       serialize_raw_bound( (serialize_int64_t) min, fraction_bits ),
                                       serialize_raw_bound( (serialize_int64_t) max, fraction_bits ) );
}

SERIALIZE_ALWAYS_INLINE int serialize_read_fixed32( serialize_read_stream_t * stream, serialize_int32_t * value, int integer_bits, int fraction_bits, serialize_int32_t min, serialize_int32_t max )
{
    serialize_uint128_t raw;
    (void) integer_bits;
    if ( stream->error ) return 0;
    if ( !serialize_read_fixed_core( stream, &raw,
                                     serialize_raw_bound( (serialize_int64_t) min, fraction_bits ),
                                     serialize_raw_bound( (serialize_int64_t) max, fraction_bits ) ) )
    {
        return 0;
    }
    *value = (serialize_int32_t) (serialize_uint32_t) ( raw.lo & 0xFFFFFFFFu );
    return 1;
}

SERIALIZE_ALWAYS_INLINE int serialize_write_fixed64( serialize_write_stream_t * stream, serialize_int64_t value, int integer_bits, int fraction_bits, serialize_int64_t min, serialize_int64_t max )
{
    serialize_int128_t wide;
    (void) integer_bits;
    wide = serialize_int128_from_int64( value );
    return serialize_write_fixed_core( stream, serialize_uint128_make( wide.hi, wide.lo ),
                                       serialize_raw_bound( min, fraction_bits ),
                                       serialize_raw_bound( max, fraction_bits ) );
}

SERIALIZE_ALWAYS_INLINE int serialize_read_fixed64( serialize_read_stream_t * stream, serialize_int64_t * value, int integer_bits, int fraction_bits, serialize_int64_t min, serialize_int64_t max )
{
    serialize_uint128_t raw;
    (void) integer_bits;
    if ( stream->error ) return 0;
    if ( !serialize_read_fixed_core( stream, &raw,
                                     serialize_raw_bound( min, fraction_bits ),
                                     serialize_raw_bound( max, fraction_bits ) ) )
    {
        return 0;
    }
    *value = (serialize_int64_t) raw.lo;
    return 1;
}

SERIALIZE_ALWAYS_INLINE int serialize_write_fixed128( serialize_write_stream_t * stream, serialize_int128_t value, int integer_bits, int fraction_bits, serialize_int64_t min, serialize_int64_t max )
{
    (void) integer_bits;
    return serialize_write_fixed_core( stream, serialize_uint128_make( value.hi, value.lo ),
                                       serialize_raw_bound( min, fraction_bits ),
                                       serialize_raw_bound( max, fraction_bits ) );
}

SERIALIZE_ALWAYS_INLINE int serialize_read_fixed128( serialize_read_stream_t * stream, serialize_int128_t * value, int integer_bits, int fraction_bits, serialize_int64_t min, serialize_int64_t max )
{
    serialize_uint128_t raw;
    (void) integer_bits;
    if ( stream->error ) return 0;
    if ( !serialize_read_fixed_core( stream, &raw,
                                     serialize_raw_bound( min, fraction_bits ),
                                     serialize_raw_bound( max, fraction_bits ) ) )
    {
        return 0;
    }
    value->lo = raw.lo;
    value->hi = raw.hi;
    return 1;
}

/* ---------------------------------------------------------------------------
   wide strings
   --------------------------------------------------------------------------- */

/*
    Each 32-bit group carries one UTF-16 CODE UNIT, not one code point
    (STANDARD.md, adopted 2026-08-15), so 2-byte and 4-byte wchar_t platforms
    produce identical bytes: the 4-byte platform converts at the boundary,
    splitting an astral code point into its surrogate pair on write and
    recombining the pair on read. This counts the units a string transmits —
    which on a 4-byte platform is more than its character count when astral
    text is present, and is the count the length field carries.
*/
SERIALIZE_INLINE int serialize_wstring_unit_count( const wchar_t * string )
{
    int units = 0;
    int i;
    for ( i = 0; string[i] != 0; i++ )
    {
        serialize_uint32_t c = (serialize_uint32_t) string[i];
        units += ( c >= 0x10000u && c <= 0x10FFFFu ) ? 2 : 1;
    }
    return units;
}

/*
    The wstring payload is well-formed UTF-16 BY CONTRACT (STANDARD.md): an
    unpaired surrogate is a writer contract violation, debug-asserted per the
    writes-trusted doctrine. On a 4-byte wchar_t platform the string is UTF-32,
    where a surrogate CODE POINT or a value above U+10FFFF is the malformation;
    on a 2-byte platform it is UTF-16, where the pairing itself is checked.
    Referenced only from serialize_assert; compiles to nothing under NDEBUG.
    This validates the WRITER's wchar_t input. The wire itself is no longer
    trusted: serialize_read_wstring refuses malformed unit sequences inline,
    in every build mode (ruling #8, adopted 2026-08-15).
*/
SERIALIZE_INLINE int serialize_wstring_is_valid_utf16( const wchar_t * string )
{
    /* through a local rather than tested inline, so MSVC's /W4 does not flag
       the constant conditional */
    const int wide_wchar = sizeof( wchar_t ) >= 4 ? 1 : 0;
    int i = 0;
    while ( string[i] != 0 )
    {
        serialize_uint32_t c = (serialize_uint32_t) string[i];
        if ( wide_wchar )
        {
            if ( c >= 0xD800u && c <= 0xDFFFu ) return 0;   /* a surrogate is not a code point */
            if ( c > 0x10FFFFu ) return 0;                  /* above Unicode */
            i += 1;
        }
        else
        {
            if ( c >= 0xD800u && c <= 0xDBFFu )
            {
                serialize_uint32_t next = (serialize_uint32_t) string[i+1];
                if ( next < 0xDC00u || next > 0xDFFFu ) return 0;   /* high surrogate without its pair */
                i += 2;
            }
            else if ( c >= 0xDC00u && c <= 0xDFFFu )
            {
                return 0;                                   /* low surrogate with no high before it */
            }
            else
            {
                i += 1;
            }
        }
    }
    return 1;
}

SERIALIZE_INLINE int serialize_write_wstring( serialize_write_stream_t * stream, const wchar_t * string, int buffer_size )
{
    int units;
    int i;

    /* the writer's contract, debug only. See serialize_wstring_is_valid_utf16. */
    serialize_assert( serialize_wstring_is_valid_utf16( string ) );

    units = serialize_wstring_unit_count( string );

    /* fitting the declared buffer is the writer's contract, debug-asserted
       (issue #52), exactly as the narrow string path asserts it */
    serialize_assert( units < buffer_size );

    if ( !serialize_write_int( stream, units, 0, buffer_size - 1 ) )
    {
        return 0;
    }

    /* NO align here -- deliberately unlike the narrow path. See the wide
       strings prototype block above. */
    for ( i = 0; string[i] != 0; i++ )
    {
        serialize_uint32_t c = (serialize_uint32_t) string[i];
        if ( c >= 0x10000u && c <= 0x10FFFFu )
        {
            /* an astral code point in a 4-byte wchar_t: split into its
               surrogate pair at the boundary, so the bytes are the ones a
               2-byte wchar_t platform produces */
            serialize_uint32_t v = c - 0x10000u;
            if ( !serialize_write_bits( stream, 0xD800u + ( v >> 10 ), 32 ) )
            {
                return 0;
            }
            if ( !serialize_write_bits( stream, 0xDC00u + ( v & 0x3FFu ), 32 ) )
            {
                return 0;
            }
        }
        else
        {
            if ( !serialize_write_bits( stream, c, 32 ) )
            {
                return 0;
            }
        }
    }

    return 1;
}

SERIALIZE_INLINE int serialize_read_wstring( serialize_read_stream_t * stream, wchar_t * string, int buffer_size )
{
    const int wide_wchar = sizeof( wchar_t ) >= 4 ? 1 : 0;      /* a local, so /W4 does not flag the constant conditional */
    serialize_int32_t units = 0;
    serialize_uint32_t pending = 0;     /* a high surrogate awaiting its pair */
    int have_pending = 0;
    int out = 0;
    int i;

    if ( stream->error )
    {
        return 0;
    }

    if ( !serialize_read_int( stream, &units, 0, buffer_size - 1 ) )
    {
        return 0;
    }

    for ( i = 0; i < units; i++ )
    {
        serialize_uint32_t c = 0;
        if ( !serialize_read_bits( stream, &c, 32 ) )
        {
            return 0;
        }

        /*
            Content refusals (ruling #8, adopted 2026-08-15). The payload is
            well-formed UTF-16 with no interior NUL, and the reader REFUSES
            anything else in every build mode, on BOTH wchar_t widths — the
            4-byte platform no longer passes malformed sequences through just
            because a wide wchar_t happens to hold them.

            A zero unit first: the writer derives the unit count from the
            terminator, so a NUL inside the counted payload is impossible from
            conformance — and accepting one mints a string whose wire length
            and whose wcslen-perceived length disagree, the two-lengths
            smuggling primitive. Then the unit range: each 32-bit group
            carries one UTF-16 CODE UNIT, so a group above 0xFFFF is
            malformed on every platform — this subsumes the old 2-byte-only
            "wchar_t cannot hold it" refusal. The surrogate pairing is
            checked below.
        */
        if ( c == 0 )
        {
            return serialize_read_fail( stream );
        }
        if ( c > 0xFFFFu )
        {
            return serialize_read_fail( stream );
        }

        if ( have_pending )
        {
            if ( c < 0xDC00u || c > 0xDFFFu )
            {
                return serialize_read_fail( stream );   /* high surrogate without its low */
            }
            if ( wide_wchar )
            {
                /* recombine at the boundary: the pair becomes one code
                   point, the inverse of the split the writer performed */
                string[out++] = (wchar_t) ( 0x10000u + ( ( pending - 0xD800u ) << 10 ) + ( c - 0xDC00u ) );
            }
            else
            {
                /* a 2-byte wchar_t IS a UTF-16 code unit: the pair is
                   stored as the two units it arrived as */
                string[out++] = (wchar_t) pending;
                string[out++] = (wchar_t) c;
            }
            have_pending = 0;
            continue;
        }

        if ( c >= 0xD800u && c <= 0xDBFFu )
        {
            pending = c;
            have_pending = 1;
            continue;
        }

        if ( c >= 0xDC00u && c <= 0xDFFFu )
        {
            return serialize_read_fail( stream );       /* low surrogate with no high before it */
        }

        string[out++] = (wchar_t) c;
    }

    if ( have_pending )
    {
        return serialize_read_fail( stream );           /* the payload ends inside a pair */
    }

    string[out] = 0;

    return 1;
}

/* ---------------------------------------------------------------------------
   measure stream operations — the bulk mirrors

   One per bulk write operation, bounding the bits that write would emit
   without emitting any. They are here, at the bottom, because they mirror
   the whole surface and reuse the same helpers the fixed point and 128-bit
   paths are built from -- the widths are computed by the same code that
   computes them on the write path, not by a second copy that could drift
   from it.

   The bound is CONSERVATIVE, never below: align charges its worst case of 7
   bits wherever it appears, because the padding is bit position dependent
   and a measure does not know where its message will land (see
   serialize_measure_align above -- the fork ruling, and the C++
   MeasureStream's model). Everything position-independent counts exactly.

   test/roundtrip.c checks every one of these against the writer, field by
   field: never below the bits actually emitted -- a measure that under-counts
   is worse than no measure at all, because it sizes a buffer that then
   overflows -- and above them by exactly the align worst case and nothing
   else.
   --------------------------------------------------------------------------- */

/*
    The one operation whose width cannot be worked around with measure_bits:
    the tier is chosen by the difference, so the caller would have to
    reimplement the ladder to know the count. This walks the same ladder
    serialize_write_int_relative does, in the same order, and asks
    serialize_bits_required for each tier's width rather than restating it as
    a literal that could disagree with the writer.
*/
SERIALIZE_INLINE int serialize_measure_int_relative( serialize_measure_stream_t * stream, serialize_int32_t previous, serialize_int32_t current )
{
    serialize_uint32_t difference;

    /* STRICTLY increasing is the caller's contract on measure exactly as it
       is on write, debug-asserted (issue #52). The C++ measure path rides
       the same assert — MeasureStream is a writing stream — and its release
       build checks nothing. */
    serialize_assert( current > previous );

    difference = (serialize_uint32_t) current - (serialize_uint32_t) previous;

    stream->bits_written += 1;
    if ( difference == 1 )
    {
        return 1;
    }

    stream->bits_written += 1;
    if ( difference <= 6 )
    {
        stream->bits_written += serialize_bits_required( 2, 6 );
        return 1;
    }

    stream->bits_written += 1;
    if ( difference <= 23 )
    {
        stream->bits_written += serialize_bits_required( 7, 23 );
        return 1;
    }

    stream->bits_written += 1;
    if ( difference <= 280 )
    {
        stream->bits_written += serialize_bits_required( 24, 280 );
        return 1;
    }

    stream->bits_written += 1;
    if ( difference <= 4377 )
    {
        stream->bits_written += serialize_bits_required( 281, 4377 );
        return 1;
    }

    stream->bits_written += 1;
    if ( difference <= 69914 )
    {
        stream->bits_written += serialize_bits_required( 4378, 69914 );
        return 1;
    }

    stream->bits_written += 32;

    return 1;
}

SERIALIZE_INLINE int serialize_measure_string( serialize_measure_stream_t * stream, const char * string, int buffer_size )
{
    int length = (int) strlen( string );

    /* fitting the declared buffer is the caller's contract on measure exactly
       as it is on write, debug-asserted (issue #52) and never checked in a
       release build — the C++ measure path rides the same assert */
    serialize_assert( length < buffer_size );

    serialize_measure_int( stream, 0, buffer_size - 1 );
    serialize_measure_bytes( stream, length );

    return 1;
}

/* NO align, matching serialize_write_wstring. See the wide strings prototype
   block above. Counts UTF-16 CODE UNITS through the same helper the writer
   uses, so an astral character on a 4-byte wchar_t platform measures as the
   two groups it transmits. */
SERIALIZE_INLINE int serialize_measure_wstring( serialize_measure_stream_t * stream, const wchar_t * string, int buffer_size )
{
    int units = serialize_wstring_unit_count( string );

    /* the caller's contract, debug-asserted exactly as the narrow path
       asserts it (issue #52) */
    serialize_assert( units < buffer_size );

    stream->bits_written += serialize_bits_required( 0, (serialize_uint32_t) ( buffer_size - 1 ) );
    stream->bits_written += units * 32;

    return 1;
}

SERIALIZE_INLINE int serialize_measure_uint128( serialize_measure_stream_t * stream )
{
    stream->bits_written += 128;
    return 1;
}

SERIALIZE_INLINE int serialize_measure_int128( serialize_measure_stream_t * stream, serialize_int128_t min, serialize_int128_t max )
{
    serialize_uint128_t umin, umax, span;

    /* bounds are the caller's contract on measure exactly as they are on
       write, debug-asserted (issue #52) — the C++ MeasureStream asserts
       min <= max the same way and its release build checks nothing */
    serialize_assert( serialize_int128_compare( min, max ) <= 0 );

    umin = serialize_uint128_make( min.hi, min.lo );
    umax = serialize_uint128_make( max.hi, max.lo );
    span = serialize_u128_sub( umax, umin );

    stream->bits_written += serialize_u128_bit_length( span );

    return 1;
}

/* the shared core, mirroring serialize_write_fixed_core: the width comes from
   the span of the RAW (scaled) bounds, and nothing else */
SERIALIZE_ALWAYS_INLINE int serialize_measure_fixed_core( serialize_measure_stream_t * stream, int fraction_bits, serialize_int64_t min, serialize_int64_t max )
{
    serialize_uint128_t raw_min = serialize_raw_bound( min, fraction_bits );
    serialize_uint128_t raw_max = serialize_raw_bound( max, fraction_bits );
    serialize_uint128_t span = serialize_u128_sub( raw_max, raw_min );

    stream->bits_written += serialize_u128_bit_length( span );

    return 1;
}

SERIALIZE_ALWAYS_INLINE int serialize_measure_fixed32( serialize_measure_stream_t * stream, int integer_bits, int fraction_bits, serialize_int32_t min, serialize_int32_t max )
{
    (void) integer_bits;
    return serialize_measure_fixed_core( stream, fraction_bits, (serialize_int64_t) min, (serialize_int64_t) max );
}

SERIALIZE_ALWAYS_INLINE int serialize_measure_fixed64( serialize_measure_stream_t * stream, int integer_bits, int fraction_bits, serialize_int64_t min, serialize_int64_t max )
{
    (void) integer_bits;
    return serialize_measure_fixed_core( stream, fraction_bits, min, max );
}

SERIALIZE_ALWAYS_INLINE int serialize_measure_fixed128( serialize_measure_stream_t * stream, int integer_bits, int fraction_bits, serialize_int64_t min, serialize_int64_t max )
{
    (void) integer_bits;
    return serialize_measure_fixed_core( stream, fraction_bits, min, max );
}

#ifdef __cplusplus
}
#endif

#endif /* SERIALIZE_H */
