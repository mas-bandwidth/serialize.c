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

    Every operation that can fail returns int: 1 on success, 0 on failure.
    Failure is sticky — once a stream fails, subsequent operations fail
    without touching the buffer, so you can check once at the end of a message
    rather than after every field.

    Reads fail on out-of-range values rather than clamping them. This library
    is used on packet paths that face the open internet, and a read is the
    place where untrusted data arrives.
*/

#ifndef SERIALIZE_H
#define SERIALIZE_H

/* ---------------------------------------------------------------------------
   version

   Kept in step with the tag by CI, the same contract the C++ library uses.
   --------------------------------------------------------------------------- */

#define SERIALIZE_VERSION_MAJOR 1
#define SERIALIZE_VERSION_MINOR 0
#define SERIALIZE_VERSION_PATCH 0
#define SERIALIZE_VERSION "1.0.0"

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
    SERIALIZE_INLINE — C89 has no inline. The library is compiled as a
    translation unit rather than a header, so this only affects the few
    helpers that benefit from it.
*/
#ifndef SERIALIZE_INLINE
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#define SERIALIZE_INLINE inline
#elif defined(_MSC_VER)
#define SERIALIZE_INLINE __inline
#elif defined(__GNUC__)
#define SERIALIZE_INLINE __inline__
#else
#define SERIALIZE_INLINE
#endif
#endif

#include <stddef.h>   /* wchar_t */

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
    int error;                  /* sticky: once set, every write is a no-op */
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
} serialize_read_stream_t;

/*
    A measure stream: counts the bits a message would occupy without producing
    any. The write and measure halves must perform the same operations for the
    count to be meaningful.
*/
typedef struct serialize_measure_stream_t
{
    int bits_written;
} serialize_measure_stream_t;

/* ---------------------------------------------------------------------------
   stream lifecycle
   --------------------------------------------------------------------------- */

void serialize_write_stream_init( serialize_write_stream_t * stream, void * buffer, int bytes );

/* Flushes the partial scratch word. Call once, after the final write. */
void serialize_write_flush( serialize_write_stream_t * stream );

int serialize_write_bits_processed( const serialize_write_stream_t * stream );
int serialize_write_bytes_processed( const serialize_write_stream_t * stream );
int serialize_write_bits_available( const serialize_write_stream_t * stream );
int serialize_write_error( const serialize_write_stream_t * stream );

void serialize_read_stream_init( serialize_read_stream_t * stream, const void * buffer, int bytes );

int serialize_read_bits_processed( const serialize_read_stream_t * stream );
int serialize_read_bytes_processed( const serialize_read_stream_t * stream );
int serialize_read_bits_remaining( const serialize_read_stream_t * stream );
int serialize_read_error( const serialize_read_stream_t * stream );

void serialize_measure_stream_init( serialize_measure_stream_t * stream );
int serialize_measure_bits_processed( const serialize_measure_stream_t * stream );
int serialize_measure_bytes_processed( const serialize_measure_stream_t * stream );

/* ---------------------------------------------------------------------------
   bit-level primitives
   --------------------------------------------------------------------------- */

/* bits must be in [1,32] and value must be less than 2^bits. */
int serialize_write_bits( serialize_write_stream_t * stream, serialize_uint32_t value, int bits );
int serialize_read_bits( serialize_read_stream_t * stream, serialize_uint32_t * value, int bits );

int serialize_write_bool( serialize_write_stream_t * stream, int value );
int serialize_read_bool( serialize_read_stream_t * stream, int * value );

/*
    Pads with zero bits to the next byte boundary, writing nothing if already
    aligned. The reader verifies the padding is zero and FAILS if it is not —
    malformed streams are detectable rather than silently accepted.
*/
int serialize_write_align( serialize_write_stream_t * stream );
int serialize_read_align( serialize_read_stream_t * stream );

int serialize_write_align_bits( const serialize_write_stream_t * stream );
int serialize_read_align_bits( const serialize_read_stream_t * stream );

/* ---------------------------------------------------------------------------
   integers
   --------------------------------------------------------------------------- */

/*
    The defining operation. The width comes entirely from the range, so
    [0,7] costs 3 bits and a degenerate min == max costs nothing at all.
    Both sides must pass identical bounds — the wire carries no description
    of itself.
*/
int serialize_write_int( serialize_write_stream_t * stream, serialize_int32_t value, serialize_int32_t min, serialize_int32_t max );
int serialize_read_int( serialize_read_stream_t * stream, serialize_int32_t * value, serialize_int32_t min, serialize_int32_t max );

int serialize_write_int64( serialize_write_stream_t * stream, serialize_int64_t value, serialize_int64_t min, serialize_int64_t max );
int serialize_read_int64( serialize_read_stream_t * stream, serialize_int64_t * value, serialize_int64_t min, serialize_int64_t max );

/*
    Fixed-width helpers. These are NOT ranged — serialize_write_uint64 always
    costs 64 bits, where serialize_write_int64 costs only what its bounds
    require. The names are similar and the encodings are not.
*/
int serialize_write_uint8( serialize_write_stream_t * stream, serialize_uint8_t value );
int serialize_read_uint8( serialize_read_stream_t * stream, serialize_uint8_t * value );
int serialize_write_uint16( serialize_write_stream_t * stream, serialize_uint16_t value );
int serialize_read_uint16( serialize_read_stream_t * stream, serialize_uint16_t * value );
int serialize_write_uint32( serialize_write_stream_t * stream, serialize_uint32_t value );
int serialize_read_uint32( serialize_read_stream_t * stream, serialize_uint32_t * value );
int serialize_write_uint64( serialize_write_stream_t * stream, serialize_uint64_t value );
int serialize_read_uint64( serialize_read_stream_t * stream, serialize_uint64_t * value );

/* An increasing sequence, encoded as a ladder of tier flags. A difference of
   1 — the common case for sequence numbers — costs a single bit. */
int serialize_write_int_relative( serialize_write_stream_t * stream, serialize_int32_t previous, serialize_int32_t current );
int serialize_read_int_relative( serialize_read_stream_t * stream, serialize_int32_t previous, serialize_int32_t * current );

/* ---------------------------------------------------------------------------
   floating point
   --------------------------------------------------------------------------- */

int serialize_write_float( serialize_write_stream_t * stream, float value );
int serialize_read_float( serialize_read_stream_t * stream, float * value );

int serialize_write_double( serialize_write_stream_t * stream, double value );
int serialize_read_double( serialize_read_stream_t * stream, double * value );

/* Lossy by construction: a round trip returns the nearest quantum. */
int serialize_write_compressed_float( serialize_write_stream_t * stream, float value, float min, float max, float res );
int serialize_read_compressed_float( serialize_read_stream_t * stream, float * value, float min, float max, float res );

/* ---------------------------------------------------------------------------
   bytes and strings
   --------------------------------------------------------------------------- */

/* Aligns first — that alignment is part of the format, not an optimization.
   count is not transmitted; both sides must already agree on it. */
int serialize_write_bytes( serialize_write_stream_t * stream, const serialize_uint8_t * data, int bytes );
int serialize_read_bytes( serialize_read_stream_t * stream, serialize_uint8_t * data, int bytes );

/* A null-terminated string. buffer_size sets the width of the length field,
   so both sides must agree on it; the terminator is not transmitted. */
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

   buffer_size counts WIDE CHARACTERS, not bytes. Each character rides as a
   32-bit group regardless of the local wchar_t width, so streams are
   compatible between 2-byte and 4-byte wchar_t platforms.

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

   They return int for symmetry with the write half, and return 0 for input a
   writer would refuse: a string or wstring longer than its buffer, an
   int_relative that does not increase, inverted bounds. Nothing is counted in
   that case, so a measure that returns 0 leaves the stream as it was.

   What a measure CANNOT refuse is a value out of its declared range, because
   it is never given the value -- only the bounds that set the width. The
   write is where that fails.
   --------------------------------------------------------------------------- */

int serialize_measure_bits( serialize_measure_stream_t * stream, int bits );
int serialize_measure_bool( serialize_measure_stream_t * stream );
int serialize_measure_align( serialize_measure_stream_t * stream );

int serialize_measure_int( serialize_measure_stream_t * stream, serialize_int32_t min, serialize_int32_t max );
int serialize_measure_int64( serialize_measure_stream_t * stream, serialize_int64_t min, serialize_int64_t max );

int serialize_measure_uint8( serialize_measure_stream_t * stream );
int serialize_measure_uint16( serialize_measure_stream_t * stream );
int serialize_measure_uint32( serialize_measure_stream_t * stream );
int serialize_measure_uint64( serialize_measure_stream_t * stream );

int serialize_measure_int_relative( serialize_measure_stream_t * stream, serialize_int32_t previous, serialize_int32_t current );

int serialize_measure_float( serialize_measure_stream_t * stream );
int serialize_measure_double( serialize_measure_stream_t * stream );
int serialize_measure_compressed_float( serialize_measure_stream_t * stream, float min, float max, float res );

int serialize_measure_bytes( serialize_measure_stream_t * stream, int bytes );
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

/* Bits needed for a value in [min,max]; 0 when min == max. */
int serialize_bits_required( serialize_int32_t min, serialize_int32_t max );
int serialize_bits_required64( serialize_int64_t min, serialize_int64_t max );

/* Bounded copy that always null-terminates. */
void serialize_copy_string( char * dest, const char * source, unsigned long dest_size );

#ifdef __cplusplus
}
#endif

#endif /* SERIALIZE_H */
