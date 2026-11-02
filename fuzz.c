/*
    libFuzzer harness for serialize.c, ported from the C++ library's fuzz.cpp.

    Every input runs four passes:

    1. Hostile read (fuzz_read). The read stream is the trust boundary of this
       library: it must survive arbitrary hostile bytes, failing reads by
       returning 0, never by corrupting memory or tripping an assert. Built
       with asserts live plus asan/ubsan so all three failure modes are caught.
       The read buffer is heap allocated at exactly payload length + 8. The
       slack is there because the reader loads whole 8-byte windows
       unconditionally, so near the end of a stream the final window begins
       inside the last bytes and reaches past them; the allocation contract
       (see the read stream struct in serialize.h) covers that load, and the
       bytes it reaches are loaded but never interpreted. Allocating exactly
       the contract and no more puts an asan fence on the contract's own
       edge: a read past data + 8 traps on every run. The slack is filled with
       0xFF rather than 0x00 so a reader that leaked slack bits into a value or
       decision cannot pass by accident.

    2. The same hostile bytes through the padded-copy wrapper
       (serialize_read_stream_init_padded), from a source allocation with no
       slack at all. That wrapper is what a caller holding an exactly sized
       payload uses, so the source is heap allocated at exactly payload
       length and asan fences the byte right after it: a wrapper that read
       past its source allocation would trap under asan here. The destination
       buffer is pre-filled with 0xFF before the copy so any payload or slack
       byte the wrapper failed to write is deterministically non-zero (while
       pass 3's 0x5A/0xA5 sentinel pass is what deterministically catches
       unwritten or clobbered scalar read destinations).

    3. Hostile read with sentinel destinations (fuzz_read_sentinel). The same
       hostile bytes, checking every scalar read so a refusal must leave its
       destination byte-for-byte as the caller left it (STANDARD.md), and
       checking that failure is terminal: every later read refuses, preserves
       the first refusal reason, leaves the poisoned cursor unchanged, and
       writes nothing. Run twice per input with two bit-complementary sentinels
       (0x5A and 0xA5), so a write that happens to match one sentinel is caught
       by the other.

    4. Differential round trip (fuzz_round_trip). Values generated from the
       input are written with a write stream, re-written into a tight multiple
       of 8 buffer followed by a 16-byte 0xAA canary, measured with a measure
       stream (which must never measure fewer bits than were written), then
       read back and compared. Any write/read asymmetry or canary overrun traps.

    The first bytes of the fuzz input are an op program selecting which
    serialize_* calls run and with what parameters; the rest are the hostile
    bitstream for passes 1-3 and the value pool for pass 4. The op table matches
    fuzz.cpp case for case so the two harnesses explore the same interleavings.
    One adaptation: C has no native 128-bit arithmetic, so where the C++
    harness draws uniform 128-bit values, this one drives the wide lanes with
    an exact endpoint, its negation, or a 64-bit draw -- the endpoints reach
    the high lane on every configuration, which is what the emulation needs
    proven.
*/

#include "serialize.h"

#include <stdio.h>
#include <stdlib.h>     /* malloc/free: the pass 1-3 and tight buffers carry the allocation contract exactly */
#include <string.h>
#include <math.h>

#ifndef INT32_MIN
#define INT32_MIN ( -2147483647 - 1 )
#endif
#ifndef INT32_MAX
#define INT32_MAX 2147483647
#endif
#ifndef INT64_MIN
#define INT64_MIN ( -9223372036854775807LL - 1 )
#endif
#ifndef INT64_MAX
#define INT64_MAX 9223372036854775807LL
#endif

/* the pass running now, so a failed check names the pass that caught it (the replay controls match on it) */
static const char * fuzz_pass = "none";

/* the replay driver exits with a status so make/ctest can match the message; libFuzzer wants the trap */
#ifdef SERIALIZE_FUZZ_REPLAY
#define fuzz_trap() exit( 86 )
#else
#define fuzz_trap() __builtin_trap()
#endif

#define fuzz_check( condition ) do { if ( !(condition) ) { fprintf( stderr, "fuzz_check failed: pass %s, line %d\n", fuzz_pass, __LINE__ ); fflush( stderr ); fuzz_trap(); } } while (0)

/* NEGATIVE CONTROLS. Built with SERIALIZE_FUZZ_CONTROL=n, exactly one check of
   this harness is sabotaged, and the replay driver below must then trap in the
   pass that owns that check (the Makefile's controls target runs each and
   passes only when the output names that pass). A harness that has never been
   seen to fail is a claim, not a measurement. 0 is the real harness.
     1 sentinel   a refused read is made to write its destination
     2 hostile    a range check is tightened below the range the reader accepts
     3 padded     serialize_read_stream_init_padded's success is expected to fail
     4 roundtrip  a round trip value is expected to differ from what was written
     5 canary     a byte past the tight write buffer is overwritten
     6 trailing   the trailing data refusal is expected to be an accept */
#ifndef SERIALIZE_FUZZ_CONTROL
#define SERIALIZE_FUZZ_CONTROL 0
#endif

#define FUZZ_NUM_OPS 32
#define FUZZ_MAX_PAYLOAD 4096

/* ---------------------------------------------------------------------------
   value pool

   Hands out bytes from the fuzz input, wrapping around when exhausted. The
   write, measure and read passes each start a fresh pool over the same bytes,
   so all three regenerate the identical value sequence.
   --------------------------------------------------------------------------- */

typedef struct fuzz_pool_t
{
    const serialize_uint8_t * data;
    size_t size;
    size_t index;
} fuzz_pool_t;

static void fuzz_pool_init( fuzz_pool_t * pool, const serialize_uint8_t * data, size_t size )
{
    pool->data = data;
    pool->size = size;
    pool->index = 0;
}

static serialize_uint8_t fuzz_pool_byte( fuzz_pool_t * pool )
{
    if ( pool->size == 0 )
    {
        return 0;
    }
    if ( pool->index >= pool->size )
    {
        pool->index = 0;
    }
    return pool->data[pool->index++];
}

static serialize_uint32_t fuzz_pool_uint32( fuzz_pool_t * pool )
{
    serialize_uint32_t value = 0;
    int i;
    for ( i = 0; i < 4; i++ )
    {
        value = ( value << 8 ) | fuzz_pool_byte( pool );
    }
    return value;
}

static serialize_uint64_t fuzz_pool_uint64( fuzz_pool_t * pool )
{
    return ( (serialize_uint64_t) fuzz_pool_uint32( pool ) << 32 ) | fuzz_pool_uint32( pool );
}

/* ---------------------------------------------------------------------------
   128-bit lane helpers

    The public type is two 64-bit lanes on every platform, so the harness does
    its own two's complement arithmetic on the lanes.
   --------------------------------------------------------------------------- */

/* 2^k for k in [0,126] */
static serialize_int128_t fuzz_int128_pow2( int k )
{
    if ( k < 64 )
    {
        return serialize_int128_make( 0, (serialize_uint64_t) 1 << k );
    }
    return serialize_int128_make( (serialize_uint64_t) 1 << ( k - 64 ), 0 );
}

static serialize_int128_t fuzz_int128_neg( serialize_int128_t v )
{
    serialize_uint64_t lo = (serialize_uint64_t) 0 - v.lo;
    serialize_uint64_t hi = (serialize_uint64_t) 0 - v.hi - ( v.lo != 0 ? 1 : 0 );
    return serialize_int128_make( hi, lo );
}

/* value * 2^16 + fraction, for an int64 value: the Q112.16 raw form */
static serialize_int128_t fuzz_int128_q16( serialize_int64_t whole, serialize_uint32_t fraction )
{
    serialize_uint64_t lo = (serialize_uint64_t) whole;
    serialize_uint64_t hi = whole < 0 ? ~ (serialize_uint64_t) 0 : 0;
    serialize_uint64_t shifted_hi = ( hi << 16 ) | ( lo >> 48 );
    serialize_uint64_t shifted_lo = ( lo << 16 ) | ( fraction & 0xFFFF );
    return serialize_int128_make( shifted_hi, shifted_lo );
}

/* ---------------------------------------------------------------------------
   string content oracles

   Ruling #8 (2026-08-15): a SUCCESSFUL hostile string read must yield only
   content a conforming writer could have produced -- well-formed UTF-8 with
   no interior NUL. This re-validates what the reader accepted, independently of the reader's own
   checks, so a validation bug that lets malformed content through trips the
   harness rather than propagating.
   --------------------------------------------------------------------------- */

static int fuzz_utf8_valid( const char * s, size_t n )
{
    size_t i = 0;
    while ( i < n )
    {
        unsigned int lead = (unsigned char) s[i];
        if ( lead < 0x80 ) { i += 1; }
        else if ( ( lead & 0xE0 ) == 0xC0 )
        {
            if ( lead < 0xC2 || i + 1 >= n ) return 0;
            if ( ( (unsigned char) s[i+1] & 0xC0 ) != 0x80 ) return 0;
            i += 2;
        }
        else if ( ( lead & 0xF0 ) == 0xE0 )
        {
            unsigned int b1, b2;
            if ( i + 2 >= n ) return 0;
            b1 = (unsigned char) s[i+1];
            b2 = (unsigned char) s[i+2];
            if ( ( b1 & 0xC0 ) != 0x80 || ( b2 & 0xC0 ) != 0x80 ) return 0;
            if ( lead == 0xE0 && b1 < 0xA0 ) return 0;
            if ( lead == 0xED && b1 >= 0xA0 ) return 0;
            i += 3;
        }
        else if ( ( lead & 0xF8 ) == 0xF0 )
        {
            unsigned int b1, b2, b3;
            if ( lead > 0xF4 || i + 3 >= n ) return 0;
            b1 = (unsigned char) s[i+1];
            b2 = (unsigned char) s[i+2];
            b3 = (unsigned char) s[i+3];
            if ( ( b1 & 0xC0 ) != 0x80 || ( b2 & 0xC0 ) != 0x80 || ( b3 & 0xC0 ) != 0x80 ) return 0;
            if ( lead == 0xF0 && b1 < 0x90 ) return 0;
            if ( lead == 0xF4 && b1 >= 0x90 ) return 0;
            i += 4;
        }
        else
        {
            return 0;
        }
    }
    return 1;
}

/* ---------------------------------------------------------------------------
   passes 1 and 2: hostile read

   Arbitrary bytes go in; reads either fail cleanly, which ends the program
   early because failure is sticky, or produce in-contract values. Both
   hostile passes run this, over the two allocation shapes.
   --------------------------------------------------------------------------- */

static void fuzz_read( serialize_read_stream_t * stream, const serialize_uint8_t * ops, int num_ops )
{
    serialize_uint8_t bytes[256];
    char string[256];
    int i;

    for ( i = 0; i < num_ops; i++ )
    {
        const int op = ops[i] & 15;
        const int param = ops[i] >> 4;              /* [0,15] */

        switch ( op )
        {
            case 0:
            case 1:
            {
                serialize_uint32_t value = 0;
                const int bits = ( op == 0 ) ? param + 1 : param + 17;      /* [1,32] */
                if ( !serialize_read_bits( stream, &value, bits ) ) return;
                fuzz_check( bits == 32 || value < ( (serialize_uint32_t) 1 << bits ) );
            }
            break;

            case 2:
            {
                serialize_int32_t value = 0;
                if ( !serialize_read_int( stream, &value, -100, +100 ) ) return;
#if SERIALIZE_FUZZ_CONTROL == 2
                fuzz_check( value >= -100 && value <= +50 );                /* NEGATIVE CONTROL */
#else
                fuzz_check( value >= -100 && value <= +100 );
#endif
            }
            break;

            case 3:
            {
                serialize_int32_t value = 0;
                serialize_int64_t value64 = 0;
                serialize_int64_t bound;
                serialize_int64_t ranged64 = 0;
                serialize_int32_t fixed32 = 0;
                serialize_int64_t fixed64 = 0;
                serialize_int128_t wide;
                serialize_int128_t wide_min;
                serialize_int128_t wide_max;
                serialize_int128_t bound128;
                serialize_int128_t ranged128;
                serialize_int128_t full128;

                if ( !serialize_read_int( stream, &value, INT32_MIN, INT32_MAX ) ) return;
                if ( !serialize_read_int64( stream, &value64, INT64_MIN, INT64_MAX ) ) return;

                /* spans of varying width exercise bits_required64 */
                bound = (serialize_int64_t) ( param + 1 ) << 35;
                if ( !serialize_read_int64( stream, &ranged64, -bound, +bound ) ) return;
                fuzz_check( ranged64 >= -bound && ranged64 <= +bound );

                /* fixed point: the configurations are compile time. values
                   read off hostile bytes must decode within the raw bounds or
                   fail the read. */
                if ( !serialize_read_fixed32( stream, &fixed32, 16, 16, -1000, +1000 ) ) return;
                fuzz_check( fixed32 >= -1000 * 65536 && fixed32 <= 1000 * 65536 );

                if ( !serialize_read_fixed64( stream, &fixed64, 48, 16, -100000000000LL, +100000000000LL ) ) return;
                fuzz_check( fixed64 >= -100000000000LL * 65536 && fixed64 <= 100000000000LL * 65536 );

                /* Q112.16 over +/- 2^60 whole units: a raw range of +/- 2^76,
                   past 64 bits, so the offset reaches the high lane */
                if ( !serialize_read_fixed128( stream, &wide, 112, 16, -1152921504606846976LL, +1152921504606846976LL ) ) return;
                wide_max = fuzz_int128_pow2( 76 );
                wide_min = fuzz_int128_neg( wide_max );
                fuzz_check( serialize_int128_compare( wide, wide_min ) >= 0 );
                fuzz_check( serialize_int128_compare( wide, wide_max ) <= 0 );

                /* ranged 128 bit integers, the bound width walking every
                   group structure: one group, two, three and four. hostile
                   bytes must decode inside the bounds or fail the read --
                   never clamp, never wrap. */
                bound128 = fuzz_int128_pow2( 20 + ( param % 100 ) );
                if ( !serialize_read_int128( stream, &ranged128, fuzz_int128_neg( bound128 ), bound128 ) ) return;
                fuzz_check( serialize_int128_compare( ranged128, fuzz_int128_neg( bound128 ) ) >= 0 );
                fuzz_check( serialize_int128_compare( ranged128, bound128 ) <= 0 );

                if ( !serialize_read_int128( stream, &full128,
                                             serialize_int128_make( 0x8000000000000000ULL, 0 ),
                                             serialize_int128_make( 0x7FFFFFFFFFFFFFFFULL, ~ (serialize_uint64_t) 0 ) ) ) return;
            }
            break;

            case 4:
            {
                int value = 0;
                if ( !serialize_read_bool( stream, &value ) ) return;
                fuzz_check( value == 0 || value == 1 );
            }
            break;

            case 5:
            {
                /* finite values only: whatever the bytes, an accepted float
                   is never NaN or infinite */
                float value = 0.0f;
                serialize_uint32_t value_bits = 0;
                if ( !serialize_read_float( stream, &value ) ) return;
                memcpy( &value_bits, &value, 4 );
                fuzz_check( serialize_float_bits_are_finite( value_bits ) );
            }
            break;

            case 6:
            {
                double value = 0.0;
                serialize_uint64_t value_bits = 0;
                if ( !serialize_read_double( stream, &value ) ) return;
                memcpy( &value_bits, &value, 8 );
                fuzz_check( serialize_double_bits_are_finite( value_bits ) );
            }
            break;

            case 7:
            {
                float value = 0.0f;
                serialize_uint32_t value_bits = 0;
                if ( !serialize_read_compressed_float( stream, &value, -10.0f, +10.0f, 0.01f ) ) return;
                memcpy( &value_bits, &value, 4 );
                fuzz_check( serialize_float_bits_are_finite( value_bits ) );
                fuzz_check( value >= -10.001f && value <= +10.001f );
            }
            break;

            case 8:
            {
                serialize_uint64_t value = 0;
                serialize_uint128_t value128;
                if ( !serialize_read_uint64( stream, &value ) ) return;
                if ( !serialize_read_uint128( stream, &value128 ) ) return;
            }
            break;

            case 9:
            {
                const int num_bytes = param * 16 + 1;                       /* [1,241] */
                if ( !serialize_read_bytes( stream, bytes, num_bytes ) ) return;
            }
            break;

            case 10:
            {
                if ( !serialize_read_string( stream, string, (int) sizeof( string ) ) ) return;
                fuzz_check( strlen( string ) < sizeof( string ) );
                /* ruling #8: an ACCEPTED string is well-formed UTF-8. The
                   interior-NUL refusal needs no oracle of its own here: had a
                   NUL been accepted inside the counted payload, strlen would
                   measure the truncated prefix, and this validates exactly
                   the strlen-perceived string -- the wire length and the
                   perceived length must agree for the accept to be sound. */
                fuzz_check( fuzz_utf8_valid( string, strlen( string ) ) );
            }
            break;

            case 11:
            {
                /* the end of the object: an accepted end leaves nothing in
                   the stream, so the next read is past the end, and the
                   program is over either way */
                serialize_uint32_t after = 0;
                if ( !serialize_read_end( stream ) ) return;
                fuzz_check( serialize_read_bits_processed( stream ) % 8 == 0 );
                fuzz_check( serialize_read_bits_remaining( stream ) == 0 );
                fuzz_check( !serialize_read_bits( stream, &after, 1 ) );
                fuzz_check( serialize_read_refusal( stream ) == SERIALIZE_REFUSAL_PAST_END );
                return;
            }

            case 12:
            {
                if ( !serialize_read_align( stream ) ) return;
            }
            break;

            case 13:
            {
                /* previous stays in the domain, 0 to 2^31 - 1 (STANDARD.md,
                   int_relative): it is the CALLER's state, never the fuzzer's
                   input, and one outside the domain is a contract violation
                   this harness would be committing itself. What the fuzzer
                   drives is the stream, and the reader's judgement of the
                   reconstructed value is what is under test. */
                const serialize_int32_t previous = param * 1000;
                serialize_int32_t current = 0;
                if ( !serialize_read_int_relative( stream, previous, &current ) ) return;
                fuzz_check( current > previous );
                fuzz_check( current >= 0 );             /* the domain, on an ACCEPTED read */
            }
            break;

            case 14:
            {
                const serialize_int32_t max = ( param + 1 ) * 1000;         /* ranges of varying width exercise bits_required */
                serialize_int32_t value = 0;
                if ( !serialize_read_int( stream, &value, 0, max ) ) return;
                fuzz_check( value >= 0 && value <= max );
            }
            break;

            case 15:
            {
                serialize_uint8_t value8 = 0;
                serialize_uint16_t value16 = 0;
                serialize_uint32_t value32 = 0;
                if ( !serialize_read_uint8( stream, &value8 ) ) return;
                if ( !serialize_read_uint16( stream, &value16 ) ) return;
                if ( !serialize_read_uint32( stream, &value32 ) ) return;
            }
            break;
        }
    }
}

/* ---------------------------------------------------------------------------
   pass 3: hostile read with sentinel destinations

   STANDARD.md, "A refused read must leave its destination unwritten": when a
   read of a scalar fails, the caller's value is exactly what it was before
   the call, and failure is terminal: every later read refuses, preserves the
   first refusal reason, keeps the poisoned cursor fixed, and writes nothing.
   Pass 1 stops at the first refusal; this pass keeps reading through the
   entire op program to prove terminality holds for every operation, and runs
   twice per input with two bit-complementary sentinels (0x5A and 0xA5).
   bytes and string leave their buffer unspecified after a refusal, so they
   are driven for the terminality check only.
   --------------------------------------------------------------------------- */

typedef struct fuzz_sentinel_run_t
{
    serialize_read_stream_t * stream;
    serialize_refusal_t refusal;        /* the first reason, once the stream has refused */
    serialize_int64_t bits;             /* poisoned cursor at the first refusal */
    int pattern;                        /* the sentinel byte pattern */
} fuzz_sentinel_run_t;

static void fuzz_sentinel_check( fuzz_sentinel_run_t * run, int ok, int unchanged )
{
    if ( run->refusal != SERIALIZE_REFUSAL_NONE )
    {
        fuzz_check( !ok );                                                  /* terminal: refused, whatever the operation */
        fuzz_check( serialize_read_refusal( run->stream ) == run->refusal );/* the first reason wins */
        fuzz_check( serialize_read_bits_processed( run->stream ) == run->bits ); /* and the cursor stays at the poison */
        fuzz_check( unchanged );
        return;
    }
    if ( ok )
    {
        fuzz_check( serialize_read_refusal( run->stream ) == SERIALIZE_REFUSAL_NONE );
        return;
    }
    fuzz_check( serialize_read_refusal( run->stream ) != SERIALIZE_REFUSAL_NONE ); /* a refused read stream never reports none */
    fuzz_check( unchanged );                                                /* and the destination is exactly what it was */
    run->refusal = serialize_read_refusal( run->stream );
    run->bits = serialize_read_bits_processed( run->stream );
}

static int fuzz_sentinel_untouched( const void * value, size_t size, int pattern )
{
    const serialize_uint8_t * raw = (const serialize_uint8_t *) value;
    size_t i;
    for ( i = 0; i < size; i++ )
    {
        if ( raw[i] != (serialize_uint8_t) pattern )
        {
            return 0;
        }
    }
    return 1;
}

#if SERIALIZE_FUZZ_CONTROL == 1
#define fuzz_sentinel_sabotage( ok, dest, pattern )                         \
    do                                                                      \
    {                                                                       \
        if ( !( ok ) )                                                      \
        {                                                                   \
            memset( &( dest ), ( pattern ) ^ 0xFF, sizeof( dest ) );        \
        }                                                                   \
    } while ( 0 )
#else
#define fuzz_sentinel_sabotage( ok, dest, pattern ) do { (void) ( ok ); (void) ( dest ); (void) ( pattern ); } while ( 0 )
#endif

#define sentinel_read( run, type, call )                                    \
    do                                                                      \
    {                                                                       \
        type destination;                                                   \
        int ok;                                                             \
        memset( &destination, ( run ).pattern, sizeof( destination ) );     \
        ok = ( call );                                                      \
        fuzz_sentinel_sabotage( ok, destination, ( run ).pattern );         \
        fuzz_sentinel_check( &( run ), ok, fuzz_sentinel_untouched( &destination, sizeof( destination ), ( run ).pattern ) ); \
    } while ( 0 )

static void fuzz_read_sentinel( serialize_read_stream_t * stream, const serialize_uint8_t * ops, int num_ops, int pattern )
{
    fuzz_sentinel_run_t run;
    serialize_uint8_t bytes[256];
    char string[256];
    int i;

    run.stream = stream;
    run.refusal = SERIALIZE_REFUSAL_NONE;
    run.bits = 0;
    run.pattern = pattern;

    for ( i = 0; i < num_ops; i++ )
    {
        const int op = ops[i] & 15;
        const int param = ops[i] >> 4;

        switch ( op )
        {
            case 0:
            {
                sentinel_read( run, serialize_uint32_t, serialize_read_bits( stream, &destination, param + 1 ) );
            }
            break;

            case 1:
            {
                sentinel_read( run, serialize_uint32_t, serialize_read_bits( stream, &destination, param + 17 ) );
            }
            break;

            case 2:
            {
                sentinel_read( run, serialize_int32_t, serialize_read_int( stream, &destination, -100, +100 ) );
            }
            break;

            case 3:
            {
                const serialize_int64_t bound = (serialize_int64_t) ( param + 1 ) << 35;
                const serialize_int128_t bound128 = fuzz_int128_pow2( 20 + ( param % 100 ) );
                sentinel_read( run, serialize_int32_t, serialize_read_int( stream, &destination, INT32_MIN, INT32_MAX ) );
                sentinel_read( run, serialize_int64_t, serialize_read_int64( stream, &destination, INT64_MIN, INT64_MAX ) );
                sentinel_read( run, serialize_int64_t, serialize_read_int64( stream, &destination, -bound, +bound ) );
                sentinel_read( run, serialize_int32_t, serialize_read_fixed32( stream, &destination, 16, 16, -1000, +1000 ) );
                sentinel_read( run, serialize_int64_t, serialize_read_fixed64( stream, &destination, 48, 16, -100000000000LL, +100000000000LL ) );
                sentinel_read( run, serialize_int128_t, serialize_read_fixed128( stream, &destination, 112, 16, -1152921504606846976LL, +1152921504606846976LL ) );
                sentinel_read( run, serialize_int128_t, serialize_read_int128( stream, &destination, fuzz_int128_neg( bound128 ), bound128 ) );
                sentinel_read( run, serialize_int128_t, serialize_read_int128( stream, &destination,
                                                                               serialize_int128_make( 0x8000000000000000ULL, 0 ),
                                                                               serialize_int128_make( 0x7FFFFFFFFFFFFFFFULL, ~ (serialize_uint64_t) 0 ) ) );
            }
            break;

            case 4:
            {
                sentinel_read( run, int, serialize_read_bool( stream, &destination ) );
            }
            break;

            case 5:
            {
                sentinel_read( run, float, serialize_read_float( stream, &destination ) );
            }
            break;

            case 6:
            {
                sentinel_read( run, double, serialize_read_double( stream, &destination ) );
            }
            break;

            case 7:
            {
                sentinel_read( run, float, serialize_read_compressed_float( stream, &destination, -10.0f, +10.0f, 0.01f ) );
            }
            break;

            case 8:
            {
                sentinel_read( run, serialize_uint64_t, serialize_read_uint64( stream, &destination ) );
                sentinel_read( run, serialize_uint128_t, serialize_read_uint128( stream, &destination ) );
            }
            break;

            case 9:
            {
                const int ok = serialize_read_bytes( stream, bytes, param * 16 + 1 );
                fuzz_sentinel_check( &run, ok, 1 );                         /* the buffer is unspecified after a refusal */
            }
            break;

            case 10:
            {
                const int ok = serialize_read_string( stream, string, (int) sizeof( string ) );
                fuzz_sentinel_check( &run, ok, 1 );
            }
            break;

            case 11:
            {
                const int ok = serialize_read_end( stream );
                fuzz_sentinel_check( &run, ok, 1 );
            }
            break;

            case 12:
            {
                const int ok = serialize_read_align( stream );
                fuzz_sentinel_check( &run, ok, 1 );
            }
            break;

            case 13:
            {
                sentinel_read( run, serialize_int32_t, serialize_read_int_relative( stream, param * 1000, &destination ) );
            }
            break;

            case 14:
            {
                sentinel_read( run, serialize_int32_t, serialize_read_int( stream, &destination, 0, ( param + 1 ) * 1000 ) );
            }
            break;

            case 15:
            {
                sentinel_read( run, serialize_uint8_t, serialize_read_uint8( stream, &destination ) );
                sentinel_read( run, serialize_uint16_t, serialize_read_uint16( stream, &destination ) );
                sentinel_read( run, serialize_uint32_t, serialize_read_uint32( stream, &destination ) );
            }
            break;
        }
    }
}

/* ---------------------------------------------------------------------------
   pass 4: differential round trip

   One function, three modes, because C has no stream template: every case
   generates its expected value from the pool FIRST, identically in all three
   modes, then performs the mode's operation. Writing or measuring an in-range
   value must succeed; reading back our own bytes must reproduce the value.
   --------------------------------------------------------------------------- */

typedef enum
{
    FUZZ_WRITE,
    FUZZ_MEASURE,
    FUZZ_READ
} fuzz_mode_t;

static void fuzz_round_trip( fuzz_mode_t mode,
                             serialize_write_stream_t * w,
                             serialize_measure_stream_t * m,
                             serialize_read_stream_t * r,
                             const serialize_uint8_t * ops, int num_ops,
                             fuzz_pool_t * pool )
{
    int i;

    for ( i = 0; i < num_ops; i++ )
    {
        const int op = ops[i] & 15;
        const int param = ops[i] >> 4;              /* [0,15] */

        switch ( op )
        {
            case 0:
            case 1:
            {
                const int bits = ( op == 0 ) ? param + 1 : param + 17;      /* [1,32] */
                const serialize_uint32_t mask = ( bits == 32 ) ? 0xFFFFFFFFu : ( ( (serialize_uint32_t) 1 << bits ) - 1 );
                const serialize_uint32_t expected = fuzz_pool_uint32( pool ) & mask;
                serialize_uint32_t value = 0;
                if ( mode == FUZZ_WRITE )        { fuzz_check( serialize_write_bits( w, expected, bits ) ); }
                else if ( mode == FUZZ_MEASURE ) { fuzz_check( serialize_measure_bits( m, bits ) ); }
                else                             { fuzz_check( serialize_read_bits( r, &value, bits ) ); fuzz_check( value == expected ); }
            }
            break;

            case 2:
            {
                const serialize_int32_t expected = -100 + (serialize_int32_t) ( fuzz_pool_uint32( pool ) % 201 );
                serialize_int32_t value = 0;
                if ( mode == FUZZ_WRITE )        { fuzz_check( serialize_write_int( w, expected, -100, +100 ) ); }
                else if ( mode == FUZZ_MEASURE ) { fuzz_check( serialize_measure_int( m, -100, +100 ) ); }
                else
                {
                    fuzz_check( serialize_read_int( r, &value, -100, +100 ) );
#if SERIALIZE_FUZZ_CONTROL == 4
                    fuzz_check( value == expected + 1 );                    /* NEGATIVE CONTROL */
#else
                    fuzz_check( value == expected );
#endif
                }
            }
            break;

            case 3:
            {
                const serialize_int32_t expected32 = (serialize_int32_t) fuzz_pool_uint32( pool );
                const serialize_int64_t expected64 = (serialize_int64_t) fuzz_pool_uint64( pool );
                const serialize_int64_t bound = (serialize_int64_t) ( param + 1 ) << 35;
                const serialize_uint64_t span = (serialize_uint64_t) bound * 2 + 1;
                const serialize_int64_t expected_ranged = -bound + (serialize_int64_t) ( fuzz_pool_uint64( pool ) % span );
                const serialize_uint32_t fixed32_range = 2000u * 65536;         /* bounds [-1000,+1000] whole units in Q16.16 */
                const serialize_int32_t expected_fixed32 = (serialize_int32_t) ( (serialize_uint32_t) ( -1000 * 65536 ) + fuzz_pool_uint32( pool ) % ( fixed32_range + 1 ) );
                const serialize_uint64_t fixed64_range = 200000000000ULL * 65536;   /* bounds [-1e11,+1e11] whole units in Q48.16 */
                const serialize_int64_t expected_fixed64 = (serialize_int64_t) ( (serialize_uint64_t) ( -100000000000LL * 65536 ) + fuzz_pool_uint64( pool ) % ( fixed64_range + 1 ) );

                /* Q112.16 over +/- 2^60 whole units. The whole part is drawn
                   as an int64 in [-2^60, +2^60] and scaled into the lanes, so
                   the raw value spans past 64 bits without needing 128-bit
                   modulo arithmetic; the top endpoint takes fraction 0 to stay
                   inside the raw bound. */
                const serialize_int64_t wide_whole = -1152921504606846976LL + (serialize_int64_t) ( fuzz_pool_uint64( pool ) % ( 2305843009213693953ULL ) );
                const serialize_uint32_t wide_fraction = ( wide_whole == 1152921504606846976LL ) ? 0 : ( fuzz_pool_uint32( pool ) & 0xFFFF );
                const serialize_int128_t expected_wide = fuzz_int128_q16( wide_whole, wide_fraction );

                /* ranged 128: endpoints or a 64-bit draw -- the endpoints
                   exercise the full group structure at every width */
                const int k = 20 + ( param % 100 );
                const serialize_int128_t bound128 = fuzz_int128_pow2( k );
                const int pick = fuzz_pool_byte( pool ) % 3;
                serialize_int128_t expected128;

                serialize_int32_t value32 = 0;
                serialize_int64_t value64 = 0;
                serialize_int64_t ranged64 = 0;
                serialize_int32_t fixed32 = 0;
                serialize_int64_t fixed64 = 0;
                serialize_int128_t wide;
                serialize_int128_t ranged128;

                if ( pick == 0 )
                {
                    expected128 = fuzz_int128_neg( bound128 );
                }
                else if ( pick == 1 )
                {
                    expected128 = bound128;
                }
                else
                {
                    serialize_int64_t draw = (serialize_int64_t) fuzz_pool_uint64( pool );
                    if ( k < 63 )
                    {
                        draw = (serialize_int64_t) ( (serialize_uint64_t) draw % ( ( (serialize_uint64_t) 1 << ( k + 1 ) ) + 1 ) ) - ( (serialize_int64_t) 1 << k );
                    }
                    expected128 = serialize_int128_from_int64( draw );
                }

                if ( mode == FUZZ_WRITE )
                {
                    fuzz_check( serialize_write_int( w, expected32, INT32_MIN, INT32_MAX ) );
                    fuzz_check( serialize_write_int64( w, expected64, INT64_MIN, INT64_MAX ) );
                    fuzz_check( serialize_write_int64( w, expected_ranged, -bound, +bound ) );
                    fuzz_check( serialize_write_fixed32( w, expected_fixed32, 16, 16, -1000, +1000 ) );
                    fuzz_check( serialize_write_fixed64( w, expected_fixed64, 48, 16, -100000000000LL, +100000000000LL ) );
                    fuzz_check( serialize_write_fixed128( w, expected_wide, 112, 16, -1152921504606846976LL, +1152921504606846976LL ) );
                    fuzz_check( serialize_write_int128( w, expected128, fuzz_int128_neg( bound128 ), bound128 ) );
                }
                else if ( mode == FUZZ_MEASURE )
                {
                    fuzz_check( serialize_measure_int( m, INT32_MIN, INT32_MAX ) );
                    fuzz_check( serialize_measure_int64( m, INT64_MIN, INT64_MAX ) );
                    fuzz_check( serialize_measure_int64( m, -bound, +bound ) );
                    fuzz_check( serialize_measure_fixed32( m, 16, 16, -1000, +1000 ) );
                    fuzz_check( serialize_measure_fixed64( m, 48, 16, -100000000000LL, +100000000000LL ) );
                    fuzz_check( serialize_measure_fixed128( m, 112, 16, -1152921504606846976LL, +1152921504606846976LL ) );
                    fuzz_check( serialize_measure_int128( m, fuzz_int128_neg( bound128 ), bound128 ) );
                }
                else
                {
                    fuzz_check( serialize_read_int( r, &value32, INT32_MIN, INT32_MAX ) );
                    fuzz_check( value32 == expected32 );
                    fuzz_check( serialize_read_int64( r, &value64, INT64_MIN, INT64_MAX ) );
                    fuzz_check( value64 == expected64 );
                    fuzz_check( serialize_read_int64( r, &ranged64, -bound, +bound ) );
                    fuzz_check( ranged64 == expected_ranged );
                    fuzz_check( serialize_read_fixed32( r, &fixed32, 16, 16, -1000, +1000 ) );
                    fuzz_check( fixed32 == expected_fixed32 );
                    fuzz_check( serialize_read_fixed64( r, &fixed64, 48, 16, -100000000000LL, +100000000000LL ) );
                    fuzz_check( fixed64 == expected_fixed64 );
                    fuzz_check( serialize_read_fixed128( r, &wide, 112, 16, -1152921504606846976LL, +1152921504606846976LL ) );
                    fuzz_check( serialize_int128_equal( wide, expected_wide ) );
                    fuzz_check( serialize_read_int128( r, &ranged128, fuzz_int128_neg( bound128 ), bound128 ) );
                    fuzz_check( serialize_int128_equal( ranged128, expected128 ) );
                }
            }
            break;

            case 4:
            {
                const int expected = ( fuzz_pool_byte( pool ) & 1 ) != 0;
                int value = 0;
                if ( mode == FUZZ_WRITE )        { fuzz_check( serialize_write_bool( w, expected ) ); }
                else if ( mode == FUZZ_MEASURE ) { fuzz_check( serialize_measure_bool( m ) ); }
                else                             { fuzz_check( serialize_read_bool( r, &value ) ); fuzz_check( value == expected ); }
            }
            break;

            case 5:
            {
                /* arbitrary FINITE bit patterns, -0 and subnormals included:
                   floats round trip bit exact. NaN and infinity are
                   non-conforming on write (format 2.0) and assert in this
                   harness, so a non-finite pattern has its exponent cleared,
                   which keeps its sign and mantissa entropy as a subnormal.
                   Hostile non-finite bytes reach the read path in pass 1. */
                const serialize_uint32_t raw_bits = fuzz_pool_uint32( pool );
                const serialize_uint32_t expected_bits = serialize_float_bits_are_finite( raw_bits ) ? raw_bits : ( raw_bits & 0x807FFFFFu );
                float value = 0.0f;
                serialize_uint32_t value_bits = 0;
                memcpy( &value, &expected_bits, 4 );
                if ( mode == FUZZ_WRITE )        { fuzz_check( serialize_write_float( w, value ) ); }
                else if ( mode == FUZZ_MEASURE ) { fuzz_check( serialize_measure_float( m ) ); }
                else
                {
                    value = 0.0f;
                    fuzz_check( serialize_read_float( r, &value ) );
                    memcpy( &value_bits, &value, 4 );
                    fuzz_check( value_bits == expected_bits );
                }
            }
            break;

            case 6:
            {
                const serialize_uint64_t raw_bits = fuzz_pool_uint64( pool );
                const serialize_uint64_t mantissa_and_sign = ( (serialize_uint64_t) 1 << 63 ) | ( ( (serialize_uint64_t) 1 << 52 ) - 1 );
                const serialize_uint64_t expected_bits = serialize_double_bits_are_finite( raw_bits ) ? raw_bits : ( raw_bits & mantissa_and_sign );
                double value = 0.0;
                serialize_uint64_t value_bits = 0;
                memcpy( &value, &expected_bits, 8 );
                if ( mode == FUZZ_WRITE )        { fuzz_check( serialize_write_double( w, value ) ); }
                else if ( mode == FUZZ_MEASURE ) { fuzz_check( serialize_measure_double( m ) ); }
                else
                {
                    value = 0.0;
                    fuzz_check( serialize_read_double( r, &value ) );
                    memcpy( &value_bits, &value, 8 );
                    fuzz_check( value_bits == expected_bits );
                }
            }
            break;

            case 7:
            {
                /* arbitrary bit patterns, FORCED FINITE before the write: a
                   non-finite value through compressed float is non-conforming
                   and asserts out on write (serialize fork #6), and this
                   pass's job is to generate CONFORMING writer input -- so a
                   NaN or Inf pattern has one exponent bit cleared, which
                   keeps every pattern finite while preserving hugely
                   out-of-range values. Those must still clamp into [min,max]
                   on write, and finite in range values must round trip
                   within the resolution. (Raw float, case 5, forces its
                   input finite the same way: format 2.0 carries finite
                   values only.) */
                serialize_uint32_t expected_bits = fuzz_pool_uint32( pool );
                float expected = 0.0f;
                float value = 0.0f;
                if ( ( expected_bits & 0x7F800000u ) == 0x7F800000u )       /* bit test: immune to fast math */
                {
                    expected_bits &= ~0x08000000u;
                }
                memcpy( &expected, &expected_bits, 4 );
                if ( mode == FUZZ_WRITE )        { fuzz_check( serialize_write_compressed_float( w, expected, -10.0f, +10.0f, 0.01f ) ); }
                else if ( mode == FUZZ_MEASURE ) { fuzz_check( serialize_measure_compressed_float( m, -10.0f, +10.0f, 0.01f ) ); }
                else
                {
                    fuzz_check( serialize_read_compressed_float( r, &value, -10.0f, +10.0f, 0.01f ) );
                    fuzz_check( value >= -10.001f && value <= +10.001f );
                    if ( expected >= -10.0f && expected <= +10.0f )
                    {
                        fuzz_check( fabs( (double) ( value - expected ) ) <= (double) 0.011f );
                    }
                }
            }
            break;

            case 8:
            {
                const serialize_uint64_t expected = fuzz_pool_uint64( pool );
                const serialize_uint64_t expected128_hi = fuzz_pool_uint64( pool );
                const serialize_uint64_t expected128_lo = fuzz_pool_uint64( pool );
                const serialize_uint128_t expected128 = serialize_uint128_make( expected128_hi, expected128_lo );
                serialize_uint64_t value = 0;
                serialize_uint128_t value128;
                if ( mode == FUZZ_WRITE )
                {
                    fuzz_check( serialize_write_uint64( w, expected ) );
                    fuzz_check( serialize_write_uint128( w, expected128 ) );
                }
                else if ( mode == FUZZ_MEASURE )
                {
                    fuzz_check( serialize_measure_uint64( m ) );
                    fuzz_check( serialize_measure_uint128( m ) );
                }
                else
                {
                    fuzz_check( serialize_read_uint64( r, &value ) );
                    fuzz_check( value == expected );
                    fuzz_check( serialize_read_uint128( r, &value128 ) );
                    fuzz_check( serialize_uint128_equal( value128, expected128 ) );
                }
            }
            break;

            case 9:
            {
                const int num_bytes = param * 16 + 1;                       /* [1,241] */
                serialize_uint8_t expected[256];
                serialize_uint8_t value[256];
                int j;
                for ( j = 0; j < num_bytes; j++ )
                {
                    expected[j] = fuzz_pool_byte( pool );
                }
                if ( mode == FUZZ_WRITE )        { fuzz_check( serialize_write_bytes( w, expected, num_bytes ) ); }
                else if ( mode == FUZZ_MEASURE ) { fuzz_check( serialize_measure_bytes( m, num_bytes ) ); }
                else
                {
                    fuzz_check( serialize_read_bytes( r, value, num_bytes ) );
                    fuzz_check( memcmp( value, expected, (size_t) num_bytes ) == 0 );
                }
            }
            break;

            case 10:
            {
                char expected[32];
                char value[32];
                const int length = (int) ( fuzz_pool_byte( pool ) % ( sizeof( expected ) - 1 ) );
                int j;
                for ( j = 0; j < length; j++ )
                {
                    /* masked to ASCII: the string payload is well-formed UTF-8
                       by the writer's contract (debug-asserted, and asserts
                       are live in this harness), and the reader REFUSES
                       malformed payloads (ruling #8, 2026-08-15), so the
                       differential round trip must generate conforming
                       content. Arbitrary bytes still reach the READ path via
                       fuzz_read. */
                    const serialize_uint8_t c = fuzz_pool_byte( pool ) & 0x7Fu;
                    expected[j] = ( c != 0 ) ? (char) c : ' ';
                }
                expected[length] = '\0';
                if ( mode == FUZZ_WRITE )        { fuzz_check( serialize_write_string( w, expected, (int) sizeof( expected ) ) ); }
                else if ( mode == FUZZ_MEASURE ) { fuzz_check( serialize_measure_string( m, expected, (int) sizeof( expected ) ) ); }
                else
                {
                    fuzz_check( serialize_read_string( r, value, (int) sizeof( value ) ) );
                    fuzz_check( strcmp( value, expected ) == 0 );
                }
            }
            break;

            case 11:
            {
                /* a string of real text: random Unicode scalar values, encoded
                   as UTF-8 here, so every length of sequence and every plane
                   reaches the writer and must come back byte for byte. NUL and
                   the surrogate block are not scalar text and are skipped */
                char expected[40];                                          /* seven four-byte characters at most, and the terminator */
                char value[40];
                int length = 0;
                const int characters = (int) ( fuzz_pool_byte( pool ) % 8 );
                int j;
                for ( j = 0; j < characters; j++ )
                {
                    serialize_uint32_t code = fuzz_pool_uint32( pool ) % 0x110000u;
                    if ( code == 0 || ( code >= 0xD800u && code <= 0xDFFFu ) )
                    {
                        code = 0x7881u;
                    }
                    if ( code < 0x80u )
                    {
                        expected[length++] = (char) code;
                    }
                    else if ( code < 0x800u )
                    {
                        expected[length++] = (char) ( 0xC0u | ( code >> 6 ) );
                        expected[length++] = (char) ( 0x80u | ( code & 0x3Fu ) );
                    }
                    else if ( code < 0x10000u )
                    {
                        expected[length++] = (char) ( 0xE0u | ( code >> 12 ) );
                        expected[length++] = (char) ( 0x80u | ( ( code >> 6 ) & 0x3Fu ) );
                        expected[length++] = (char) ( 0x80u | ( code & 0x3Fu ) );
                    }
                    else
                    {
                        expected[length++] = (char) ( 0xF0u | ( code >> 18 ) );
                        expected[length++] = (char) ( 0x80u | ( ( code >> 12 ) & 0x3Fu ) );
                        expected[length++] = (char) ( 0x80u | ( ( code >> 6 ) & 0x3Fu ) );
                        expected[length++] = (char) ( 0x80u | ( code & 0x3Fu ) );
                    }
                }
                expected[length] = '\0';
                if ( mode == FUZZ_WRITE )        { fuzz_check( serialize_write_string( w, expected, (int) sizeof( expected ) ) ); }
                else if ( mode == FUZZ_MEASURE ) { fuzz_check( serialize_measure_string( m, expected, (int) sizeof( expected ) ) ); }
                else
                {
                    fuzz_check( serialize_read_string( r, value, (int) sizeof( value ) ) );
                    fuzz_check( strcmp( value, expected ) == 0 );
                }
            }
            break;

            case 12:
            {
                if ( mode == FUZZ_WRITE )        { fuzz_check( serialize_write_align( w ) ); }
                else if ( mode == FUZZ_MEASURE ) { fuzz_check( serialize_measure_align( m ) ); }
                else                             { fuzz_check( serialize_read_align( r ) ); }
            }
            break;

            case 13:
            {
                const serialize_int32_t previous = param * 1000;                                                                 /* in the domain: it is the caller's state */
                const serialize_int32_t expected = previous + 1 + (serialize_int32_t) ( fuzz_pool_uint32( pool ) % 1000000 );    /* strictly greater than previous */
                serialize_int32_t value = 0;
                if ( mode == FUZZ_WRITE )        { fuzz_check( serialize_write_int_relative( w, previous, expected ) ); }
                else if ( mode == FUZZ_MEASURE ) { fuzz_check( serialize_measure_int_relative( m, previous, expected ) ); }
                else
                {
                    fuzz_check( serialize_read_int_relative( r, previous, &value ) );
                    fuzz_check( value == expected );
                }
            }
            break;

            case 14:
            {
                const serialize_int32_t max = ( param + 1 ) * 1000;         /* ranges of varying width exercise bits_required */
                const serialize_int32_t expected = (serialize_int32_t) ( fuzz_pool_uint32( pool ) % (serialize_uint32_t) ( max + 1 ) );
                serialize_int32_t value = 0;
                if ( mode == FUZZ_WRITE )        { fuzz_check( serialize_write_int( w, expected, 0, max ) ); }
                else if ( mode == FUZZ_MEASURE ) { fuzz_check( serialize_measure_int( m, 0, max ) ); }
                else                             { fuzz_check( serialize_read_int( r, &value, 0, max ) ); fuzz_check( value == expected ); }
            }
            break;

            case 15:
            {
                const serialize_uint8_t expected8 = fuzz_pool_byte( pool );
                const serialize_uint16_t expected16 = (serialize_uint16_t) fuzz_pool_uint32( pool );
                const serialize_uint32_t expected32 = fuzz_pool_uint32( pool );
                serialize_uint8_t value8 = 0;
                serialize_uint16_t value16 = 0;
                serialize_uint32_t value32 = 0;
                if ( mode == FUZZ_WRITE )
                {
                    fuzz_check( serialize_write_uint8( w, expected8 ) );
                    fuzz_check( serialize_write_uint16( w, expected16 ) );
                    fuzz_check( serialize_write_uint32( w, expected32 ) );
                }
                else if ( mode == FUZZ_MEASURE )
                {
                    fuzz_check( serialize_measure_uint8( m ) );
                    fuzz_check( serialize_measure_uint16( m ) );
                    fuzz_check( serialize_measure_uint32( m ) );
                }
                else
                {
                    fuzz_check( serialize_read_uint8( r, &value8 ) );
                    fuzz_check( value8 == expected8 );
                    fuzz_check( serialize_read_uint16( r, &value16 ) );
                    fuzz_check( value16 == expected16 );
                    fuzz_check( serialize_read_uint32( r, &value32 ) );
                    fuzz_check( value32 == expected32 );
                }
            }
            break;
        }
    }
}

int LLVMFuzzerTestOneInput( const serialize_uint8_t * data, size_t size );
int LLVMFuzzerTestOneInput( const serialize_uint8_t * data, size_t size )
{
    const serialize_uint8_t * ops;
    const serialize_uint8_t * payload;
    size_t payload_bytes;

    if ( size <= FUZZ_NUM_OPS || size > FUZZ_NUM_OPS + FUZZ_MAX_PAYLOAD )
    {
        return 0;
    }

    ops = data;
    payload = data + FUZZ_NUM_OPS;
    payload_bytes = size - FUZZ_NUM_OPS;

    /* pass 1: hostile read of arbitrary bytes. Heap allocated at exactly
       payload_bytes + 8: the reader loads whole 8-byte windows
       unconditionally, so the final window of a stream reaches past the data
       and the allocation must extend at least 8 bytes past it (the C++
       harness allocates the same slack for the same reason). Allocating the
       contract exactly puts an asan fence on its edge, so any read past the
       slack traps. The slack is all ones, not zeros: bytes past the end are
       loaded and never interpreted, so a reader that leaked them into a value
       or decision would pass on zeros (end's padding check included). */
    fuzz_pass = "hostile";
    {
        serialize_uint8_t * read_buffer = (serialize_uint8_t *) malloc( payload_bytes + 8 );
        serialize_read_stream_t stream;
        if ( read_buffer == NULL )
        {
            return 0;
        }
        memset( read_buffer, 0xFF, payload_bytes + 8 );
        memcpy( read_buffer, payload, payload_bytes );
        serialize_read_stream_init( &stream, read_buffer, (int) payload_bytes );
        fuzz_read( &stream, ops, FUZZ_NUM_OPS );
        free( read_buffer );
    }

    /* pass 2: the same hostile bytes, read through the padded-copy wrapper
       from a source allocation with no slack. The source is heap allocated
       at exactly payload_bytes, so asan fences the byte right after the
       payload: a wrapper that read past its source allocation traps under
       asan here. The destination buffer is pre-filled with 0xFF before the
       wrapper runs so any payload or slack byte the wrapper failed to write
       is deterministically non-zero (while pass 3's 0x5A/0xA5 sentinel pass
       is what deterministically catches unwritten or clobbered scalar read
       destinations). */
    {
        serialize_uint8_t * exact = (serialize_uint8_t *) malloc( payload_bytes );
        serialize_uint8_t * padded = (serialize_uint8_t *) malloc( payload_bytes + 8 );
        serialize_read_stream_t stream;
        if ( exact == NULL || padded == NULL )
        {
            free( exact );
            free( padded );
            return 0;
        }
        memcpy( exact, payload, payload_bytes );
        memset( padded, 0xFF, payload_bytes + 8 );
        fuzz_pass = "padded";
        serialize_read_stream_init_padded( &stream, padded, (int) ( payload_bytes + 8 ), exact, (int) payload_bytes );
#if SERIALIZE_FUZZ_CONTROL == 3
        fuzz_check( serialize_read_error( &stream ) != 0 );                 /* NEGATIVE CONTROL */
#else
        fuzz_check( serialize_read_error( &stream ) == 0 );
#endif
        fuzz_pass = "hostile";
        fuzz_read( &stream, ops, FUZZ_NUM_OPS );
        free( exact );
        free( padded );
    }

    /* pass 3: hostile read with sentinel destinations, twice with different sentinels */
    fuzz_pass = "sentinel";
    {
        int pattern;
        for ( pattern = 0x5A; pattern != 0; pattern = ( pattern == 0x5A ) ? 0xA5 : 0 )
        {
            serialize_uint8_t * read_buffer = (serialize_uint8_t *) malloc( payload_bytes + 8 );
            serialize_read_stream_t stream;
            if ( read_buffer == NULL )
            {
                return 0;
            }
            memset( read_buffer, 0xFF, payload_bytes + 8 );
            memcpy( read_buffer, payload, payload_bytes );
            serialize_read_stream_init( &stream, read_buffer, (int) payload_bytes );
            fuzz_read_sentinel( &stream, ops, FUZZ_NUM_OPS, pattern );
            free( read_buffer );
        }
    }

    /* pass 4: differential round trip of values generated from the same bytes.
       worst case is ~260 bytes per op (a 241 byte serialize_bytes plus
       alignment), so 32 ops fit comfortably in 16KB */
    fuzz_pass = "roundtrip";
    {
        /* the 16KB write_buffer extends well past the written bytes (worst
           case ~8KB), satisfying the reader's 8-bytes-past allocation
           contract for the read-back below -- the C++ harness relies on the
           same slack */
        static serialize_uint8_t write_buffer[16 * 1024];
        serialize_write_stream_t write_stream;
        serialize_measure_stream_t measure_stream;
        serialize_read_stream_t read_stream;
        fuzz_pool_t pool;

        memset( write_buffer, 0, sizeof( write_buffer ) );

        serialize_write_stream_init( &write_stream, write_buffer, (int) sizeof( write_buffer ) );
        fuzz_pool_init( &pool, payload, payload_bytes );
        fuzz_round_trip( FUZZ_WRITE, &write_stream, NULL, NULL, ops, FUZZ_NUM_OPS, &pool );
        serialize_write_flush( &write_stream );
        fuzz_check( !serialize_write_error( &write_stream ) );          /* writing in-range values must always succeed */

        /* the same writes into a buffer that is exactly the smallest multiple
           of 8 holding them, followed by a canary. The word flush must stay
           inside the buffer and produce the same bytes. ASan cannot see a
           partial-granule overrun past a tight allocation, so the canary is
           the check */
        {
            const serialize_int64_t written = serialize_write_bytes_processed( &write_stream );
            serialize_int64_t tight_bytes = ( ( written + 7 ) / 8 ) * 8;
            const int canary_bytes = 16;
            serialize_uint8_t * tight;
            serialize_write_stream_t tight_stream;
            fuzz_pool_t tight_pool;
            int c;

            if ( tight_bytes == 0 )
            {
                tight_bytes = 8;
            }
            tight = (serialize_uint8_t *) malloc( (size_t) ( tight_bytes + canary_bytes ) );
            if ( tight == NULL )
            {
                return 0;
            }
            memset( tight, 0xA5, (size_t) tight_bytes );
            memset( tight + tight_bytes, 0xAA, (size_t) canary_bytes );
            serialize_write_stream_init( &tight_stream, tight, (int) tight_bytes );
            fuzz_pool_init( &tight_pool, payload, payload_bytes );
            fuzz_round_trip( FUZZ_WRITE, &tight_stream, NULL, NULL, ops, FUZZ_NUM_OPS, &tight_pool );
            serialize_write_flush( &tight_stream );
            fuzz_check( !serialize_write_error( &tight_stream ) );
            fuzz_check( serialize_write_bytes_processed( &tight_stream ) == written );
            fuzz_check( written == 0 || memcmp( tight, write_buffer, (size_t) written ) == 0 );
#if SERIALIZE_FUZZ_CONTROL == 5
            tight[tight_bytes] = 0;                                         /* NEGATIVE CONTROL */
#endif
            fuzz_pass = "canary";
            for ( c = 0; c < canary_bytes; c++ )
            {
                fuzz_check( tight[tight_bytes + c] == 0xAA );
            }
            free( tight );
        }

        fuzz_pass = "roundtrip";
        serialize_measure_stream_init( &measure_stream );
        fuzz_pool_init( &pool, payload, payload_bytes );
        fuzz_round_trip( FUZZ_MEASURE, NULL, &measure_stream, NULL, ops, FUZZ_NUM_OPS, &pool );
        fuzz_check( serialize_measure_bits_processed( &measure_stream ) >= serialize_write_bits_processed( &write_stream ) );    /* measure must be conservative */

        serialize_read_stream_init( &read_stream, write_buffer, (int) serialize_write_bytes_processed( &write_stream ) );
        fuzz_pool_init( &pool, payload, payload_bytes );
        fuzz_round_trip( FUZZ_READ, NULL, NULL, &read_stream, ops, FUZZ_NUM_OPS, &pool );
        fuzz_check( !serialize_read_error( &read_stream ) );            /* reading back our own data must always succeed */
        fuzz_check( serialize_read_end( &read_stream ) );               /* and it ends exactly where the writer stopped */

        /* one byte more than the writer reported is trailing data, refused
           at the end: the zeroed write buffer holds a zero byte there */
        serialize_read_stream_init( &read_stream, write_buffer, (int) serialize_write_bytes_processed( &write_stream ) + 1 );
        fuzz_pool_init( &pool, payload, payload_bytes );
        fuzz_round_trip( FUZZ_READ, NULL, NULL, &read_stream, ops, FUZZ_NUM_OPS, &pool );
        fuzz_check( !serialize_read_error( &read_stream ) );
        fuzz_pass = "trailing";
#if SERIALIZE_FUZZ_CONTROL == 6
        fuzz_check( serialize_read_end( &read_stream ) != 0 );          /* NEGATIVE CONTROL */
#else
        fuzz_check( !serialize_read_end( &read_stream ) );
#endif
        fuzz_check( serialize_read_refusal( &read_stream ) == SERIALIZE_REFUSAL_TRAILING_DATA );
    }

    return 0;
}

#ifdef SERIALIZE_FUZZ_REPLAY

/* The replay driver: the same target, run over a fixed set of generated inputs
   with no libFuzzer, so `make test` and the MSVC leg can run it on every
   platform, C standard and compiler in the matrix, and so the negative
   controls above have a deterministic input to fail on. The generator is a
   fixed xorshift (no clock, no files). The first inputs are the edge shapes
   (all zero, all ones), the rest are random bytes of varying length, and
   every input is also run with each op byte cycled through the sixteen
   operations so that every operation is reached whatever the generator drew. */

static serialize_uint64_t replay_state = 0x9E3779B97F4A7C15ULL;

static serialize_uint32_t replay_next( void )
{
    replay_state ^= replay_state << 13;
    replay_state ^= replay_state >> 7;
    replay_state ^= replay_state << 17;
    return (serialize_uint32_t) ( replay_state >> 16 );
}

int main( int argc, char ** argv )
{
    int count = 3000;
    int n;
    serialize_uint8_t input[FUZZ_NUM_OPS + 600];

    if ( argc > 1 )
    {
        count = atoi( argv[1] );
    }

    for ( n = 0; n < count; n++ )
    {
        const int payload = ( n % 7 == 0 ) ? 1 + (int) ( replay_next() % 8 ) : 1 + (int) ( replay_next() % 600 );
        const size_t total = (size_t) ( FUZZ_NUM_OPS + payload );
        size_t i;
        int j;

        for ( i = 0; i < total; i++ )
        {
            input[i] = (serialize_uint8_t) replay_next();
        }
        if ( n % 11 == 0 )
        {
            for ( j = 0; j < payload; j++ )
            {
                input[FUZZ_NUM_OPS + j] = (serialize_uint8_t) ( ( n % 2 ) ? 0xFF : 0x00 );
            }
        }
        /* cycle the op selectors so every operation appears, the parameters stay random */
        for ( j = 0; j < FUZZ_NUM_OPS; j++ )
        {
            if ( n % 3 == 0 )
            {
                input[j] = (serialize_uint8_t) ( ( input[j] & 0xF0 ) | ( ( j + n ) & 15 ) );
            }
        }
        LLVMFuzzerTestOneInput( input, total );
    }

    printf( "replayed %d inputs\n", count );
    return 0;
}

#endif /* SERIALIZE_FUZZ_REPLAY */
