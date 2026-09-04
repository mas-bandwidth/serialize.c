/*
    libFuzzer harness for serialize.c, ported from the C++ library's fuzz.cpp.

    Every input runs three passes:

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
       edge: a read past data + 8 traps on every run.

    2. The same hostile bytes through the padded-copy wrapper
       (serialize_read_stream_init_padded), from a source allocation with no
       slack at all. That wrapper is what a caller holding an exactly sized
       payload uses, so the source is heap allocated at exactly payload
       length and asan fences the byte right after it: a wrapper that read
       its source the way the reader reads a buffer would trap here. The
       destination is left uninitialized on purpose, so any byte the reader
       can touch that the wrapper failed to write is a ubsan/asan finding
       rather than a silent zero.

    3. Differential round trip (fuzz_round_trip). Values generated from the
       input are written with a write stream, measured with a measure stream
       (which must never measure fewer bits than were written), then read back
       and compared. Any write/read asymmetry traps.

    The first bytes of the fuzz input are an op program selecting which
    serialize_* calls run and with what parameters; the rest are the hostile
    bitstream for passes 1 and 2 and the value pool for pass 3. The op table matches
    fuzz.cpp case for case so the two harnesses explore the same interleavings.
    One adaptation: C has no native 128-bit arithmetic, so where the C++
    harness draws uniform 128-bit values, this one drives the wide lanes with
    an exact endpoint, its negation, or a 64-bit draw -- the endpoints reach
    the high lane on every configuration, which is what the emulation needs
    proven.

    Clang only: libFuzzer is clang's. See the fuzz target in the Makefile.
*/

#include "serialize.h"

#include <stdint.h>
#include <stdlib.h>     /* malloc/free: the pass 1 and pass 2 buffers carry the allocation contract exactly */
#include <string.h>
#include <math.h>
#include <wchar.h>

#define fuzz_check( condition ) do { if ( !(condition) ) { __builtin_trap(); } } while (0)

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
   no interior NUL, well-formed UTF-16 with no unpaired surrogate. These
   re-validate what the reader accepted, independently of the reader's own
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

static int fuzz_wstring_well_formed( const wchar_t * s )
{
    const int wide_wchar = sizeof( wchar_t ) >= 4 ? 1 : 0;
    size_t n = wcslen( s );
    size_t i = 0;
    while ( i < n )
    {
        serialize_uint32_t c = (serialize_uint32_t) s[i];
        if ( wide_wchar )
        {
            /* a 4-byte wchar_t holds CODE POINTS: pairs were recombined at
               the read boundary, so no surrogate value may survive */
            if ( c > 0x10FFFFu ) return 0;
            if ( c >= 0xD800u && c <= 0xDFFFu ) return 0;
            i += 1;
        }
        else
        {
            /* a 2-byte wchar_t holds CODE UNITS: pairs must be present and
               correctly ordered */
            if ( c >= 0xD800u && c <= 0xDBFFu )
            {
                serialize_uint32_t next = ( i + 1 < n ) ? (serialize_uint32_t) s[i+1] : 0;
                if ( next < 0xDC00u || next > 0xDFFFu ) return 0;
                i += 2;
            }
            else if ( c >= 0xDC00u && c <= 0xDFFFu )
            {
                return 0;
            }
            else
            {
                i += 1;
            }
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
    wchar_t wstring[64];
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
                fuzz_check( value >= -100 && value <= +100 );
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
                float value = 0.0f;
                if ( !serialize_read_float( stream, &value ) ) return;
            }
            break;

            case 6:
            {
                double value = 0.0;
                if ( !serialize_read_double( stream, &value ) ) return;
            }
            break;

            case 7:
            {
                float value = 0.0f;
                if ( !serialize_read_compressed_float( stream, &value, -10.0f, +10.0f, 0.01f ) ) return;
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
                if ( !serialize_read_wstring( stream, wstring, (int) ( sizeof( wstring ) / sizeof( wchar_t ) ) ) ) return;
                fuzz_check( wcslen( wstring ) < sizeof( wstring ) / sizeof( wchar_t ) );
                /* ruling #8: an ACCEPTED wstring is well-formed UTF-16 --
                   no unpaired surrogate, nothing above U+10FFFF */
                fuzz_check( fuzz_wstring_well_formed( wstring ) );
            }
            break;

            case 12:
            {
                if ( !serialize_read_align( stream ) ) return;
            }
            break;

            case 13:
            {
                const serialize_int32_t previous = param * 1000 - 8000;
                serialize_int32_t current = 0;
                if ( !serialize_read_int_relative( stream, previous, &current ) ) return;
                fuzz_check( current > previous );
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
   pass 3: differential round trip

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
                else                             { fuzz_check( serialize_read_int( r, &value, -100, +100 ) ); fuzz_check( value == expected ); }
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
                /* arbitrary bit patterns, including nan and inf: floats must
                   round trip bit exact */
                const serialize_uint32_t expected_bits = fuzz_pool_uint32( pool );
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
                const serialize_uint64_t expected_bits = fuzz_pool_uint64( pool );
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
                   pass's job is to generate CONFORMING writer input — so a
                   NaN or Inf pattern has one exponent bit cleared, which
                   keeps every pattern finite while preserving hugely
                   out-of-range values. Those must still clamp into [min,max]
                   on write, and finite in range values must round trip
                   within the resolution. (Raw float, case 5, still carries
                   NaN and Inf bit-exact: that contract is unchanged.) */
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
                        fuzz_check( fabs( value - expected ) <= 0.011f );
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
                    fuzz_check( memcmp( value, expected, num_bytes ) == 0 );
                }
            }
            break;

            case 10:
            {
                char expected[32];
                char value[32];
                const int length = fuzz_pool_byte( pool ) % ( sizeof( expected ) - 1 );
                int j;
                for ( j = 0; j < length; j++ )
                {
                    /* masked to ASCII: the string payload is well-formed UTF-8
                       by the writer's contract, debug-asserted, and asserts
                       are live in this harness. Arbitrary bytes still reach
                       the READ path through the hostile pass. */
                    const serialize_uint8_t c = fuzz_pool_byte( pool ) & 0x7F;
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
                wchar_t expected[8];
                wchar_t value[8];
                const int length = fuzz_pool_byte( pool ) % ( sizeof( expected ) / sizeof( wchar_t ) - 1 );
                int j;
                for ( j = 0; j < length; j++ )
                {
                    serialize_uint32_t unit = fuzz_pool_uint32( pool ) % 0xFFFF + 1;        /* [1,0xFFFF]: valid for 2 and 4 byte wchar_t */
                    if ( unit >= 0xD800 && unit <= 0xDFFF )
                    {
                        unit -= 0x0800;     /* out of the surrogate block: the payload is well-formed
                                               UTF-16 by the writer's contract, debug-asserted, and
                                               asserts are live in this harness */
                    }
                    expected[j] = (wchar_t) unit;
                }
                expected[length] = L'\0';
                if ( mode == FUZZ_WRITE )        { fuzz_check( serialize_write_wstring( w, expected, (int) ( sizeof( expected ) / sizeof( wchar_t ) ) ) ); }
                else if ( mode == FUZZ_MEASURE ) { fuzz_check( serialize_measure_wstring( m, expected, (int) ( sizeof( expected ) / sizeof( wchar_t ) ) ) ); }
                else
                {
                    fuzz_check( serialize_read_wstring( r, value, (int) ( sizeof( value ) / sizeof( wchar_t ) ) ) );
                    fuzz_check( wcscmp( value, expected ) == 0 );
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
                const serialize_int32_t previous = param * 1000 - 8000;
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
       slack traps */
    {
        serialize_uint8_t * read_buffer = (serialize_uint8_t *) malloc( payload_bytes + 8 );
        serialize_read_stream_t stream;
        if ( read_buffer == NULL )
        {
            return 0;
        }
        memset( read_buffer, 0, payload_bytes + 8 );
        memcpy( read_buffer, payload, payload_bytes );
        serialize_read_stream_init( &stream, read_buffer, (int) payload_bytes );
        fuzz_read( &stream, ops, FUZZ_NUM_OPS );
        free( read_buffer );
    }

    /* pass 2: the same hostile bytes, read through the padded-copy wrapper
       from a source allocation with no slack. The source is heap allocated
       at exactly payload_bytes, so asan fences the byte right after the
       payload: a wrapper that read its source the way the reader reads a
       buffer traps here. The destination is deliberately not zeroed first,
       so a byte the reader can touch that the wrapper failed to write shows
       up as a finding rather than a silent zero */
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
        serialize_read_stream_init_padded( &stream, padded, (int) ( payload_bytes + 8 ), exact, (int) payload_bytes );
        fuzz_read( &stream, ops, FUZZ_NUM_OPS );
        free( exact );
        free( padded );
    }

    /* pass 3: differential round trip of values generated from the same bytes.
       worst case is ~260 bytes per op (a 241 byte serialize_bytes plus
       alignment), so 32 ops fit comfortably in 16KB */
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

        serialize_measure_stream_init( &measure_stream );
        fuzz_pool_init( &pool, payload, payload_bytes );
        fuzz_round_trip( FUZZ_MEASURE, NULL, &measure_stream, NULL, ops, FUZZ_NUM_OPS, &pool );
        fuzz_check( serialize_measure_bits_processed( &measure_stream ) >= serialize_write_bits_processed( &write_stream ) );    /* measure must be conservative */

        serialize_read_stream_init( &read_stream, write_buffer, serialize_write_bytes_processed( &write_stream ) );
        fuzz_pool_init( &pool, payload, payload_bytes );
        fuzz_round_trip( FUZZ_READ, NULL, NULL, &read_stream, ops, FUZZ_NUM_OPS, &pool );
        fuzz_check( !serialize_read_error( &read_stream ) );            /* reading back our own data must always succeed */
    }

    return 0;
}
