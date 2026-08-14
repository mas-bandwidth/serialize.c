/*
    serialize.c 1.0 — a bitpacking serializer for C

    See serialize.h for the API and STANDARD.md for the wire format.

    The bit packer accumulates into a 64-bit scratch word, least significant
    bit first, and commits whole 8-byte words. That is the whole trick, and
    everything else in this file is a consumer of write_bits / read_bits.
*/

#include "serialize.h"

#include <string.h>
#include <math.h>

/* ---------------------------------------------------------------------------
   endianness

   The wire is little-endian. On a little-endian host the scratch word is
   copied out as-is; on a big-endian host it is byte-swapped, so the bytes
   match everywhere.
   --------------------------------------------------------------------------- */

static int serialize_host_is_big_endian( void )
{
    /* Computed rather than #ifdef'd: the preprocessor spellings for this vary
       by compiler and platform, and every one of them is a chance to be wrong
       on a platform nobody tested. A compiler folds this to a constant. */
    union { serialize_uint32_t i; unsigned char c[4]; } probe;
    probe.i = 0x01020304u;
    return probe.c[0] == 0x01;
}

static serialize_uint64_t serialize_bswap64( serialize_uint64_t value )
{
    /* Built by shifting rather than from 0x...ULL masks: a C89 compiler has no
       64-bit literal suffix it is happy about, and this needs none. */
    serialize_uint64_t result = 0;
    int i;
    for ( i = 0; i < 8; i++ )
    {
        result = ( result << 8 ) | ( ( value >> ( i * 8 ) ) & 0xFF );
    }
    return result;
}

static serialize_uint64_t serialize_host_to_wire64( serialize_uint64_t value )
{
    return serialize_host_is_big_endian() ? serialize_bswap64( value ) : value;
}

static serialize_uint64_t serialize_wire_to_host64( serialize_uint64_t value )
{
    return serialize_host_is_big_endian() ? serialize_bswap64( value ) : value;
}

/* ---------------------------------------------------------------------------
   bits required
   --------------------------------------------------------------------------- */

static int serialize_bit_length32( serialize_uint32_t value )
{
    int bits = 0;
    while ( value )
    {
        bits++;
        value >>= 1;
    }
    return bits;
}

static int serialize_bit_length64( serialize_uint64_t value )
{
    int bits = 0;
    while ( value )
    {
        bits++;
        value >>= 1;
    }
    return bits;
}

int serialize_bits_required( serialize_int32_t min, serialize_int32_t max )
{
    if ( min == max )
    {
        return 0;
    }
    /* the span is computed in the unsigned domain so a range spanning the
       whole int32 space does not overflow */
    return serialize_bit_length32( (serialize_uint32_t) max - (serialize_uint32_t) min );
}

int serialize_bits_required64( serialize_int64_t min, serialize_int64_t max )
{
    if ( min == max )
    {
        return 0;
    }
    return serialize_bit_length64( (serialize_uint64_t) max - (serialize_uint64_t) min );
}

/* ---------------------------------------------------------------------------
   write stream
   --------------------------------------------------------------------------- */

void serialize_write_stream_init( serialize_write_stream_t * stream, void * buffer, int bytes )
{
    stream->data = (serialize_uint8_t *) buffer;
    stream->num_bits = bytes * 8;
    stream->bits_written = 0;
    stream->word_index = 0;
    stream->scratch = 0;
    stream->scratch_bits = 0;
    stream->error = 0;
}

int serialize_write_bits( serialize_write_stream_t * stream, serialize_uint32_t value, int bits )
{
    int new_scratch_bits;

    if ( stream->error )
    {
        return 0;
    }

    if ( bits <= 0 || bits > 32 )
    {
        stream->error = 1;
        return 0;
    }

    if ( stream->bits_written + bits > stream->num_bits )
    {
        stream->error = 1;
        return 0;
    }

    /* mask rather than trust: a caller passing a value wider than its declared
       bits would otherwise corrupt the following field instead of failing */
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

void serialize_write_flush( serialize_write_stream_t * stream )
{
    if ( stream->error )
    {
        return;
    }

    if ( stream->scratch_bits != 0 )
    {
        serialize_uint64_t word = serialize_host_to_wire64( stream->scratch );
        memcpy( stream->data + (size_t) stream->word_index * 8, &word, sizeof( word ) );
        stream->scratch = 0;
        stream->scratch_bits = 0;
        stream->word_index++;
    }
}

int serialize_write_bits_processed( const serialize_write_stream_t * stream )
{
    return stream->bits_written;
}

int serialize_write_bytes_processed( const serialize_write_stream_t * stream )
{
    return ( stream->bits_written + 7 ) / 8;
}

int serialize_write_bits_available( const serialize_write_stream_t * stream )
{
    return stream->num_bits - stream->bits_written;
}

int serialize_write_error( const serialize_write_stream_t * stream )
{
    return stream->error;
}

/* ---------------------------------------------------------------------------
   read stream
   --------------------------------------------------------------------------- */

void serialize_read_stream_init( serialize_read_stream_t * stream, const void * buffer, int bytes )
{
    stream->data = (const serialize_uint8_t *) buffer;
    stream->num_bits = bytes * 8;
    stream->bits_read = 0;
    stream->error = 0;
}

int serialize_read_bits( serialize_read_stream_t * stream, serialize_uint32_t * value, int bits )
{
    serialize_uint64_t window;
    int byte_index;
    int available;

    if ( stream->error )
    {
        return 0;
    }

    if ( bits <= 0 || bits > 32 )
    {
        stream->error = 1;
        return 0;
    }

    if ( stream->bits_read + bits > stream->num_bits )
    {
        stream->error = 1;
        return 0;
    }

    /*
        The C++ reader loads a full 8-byte window unconditionally and relies on
        the caller having allocated the slack. This port cannot make that
        assumption of every caller, so a window near the end of the buffer is
        assembled byte by byte from what is actually there. Same bits, no read
        past the end.
    */
    byte_index = stream->bits_read >> 3;
    available = ( stream->num_bits + 7 ) / 8 - byte_index;

    if ( available >= 8 )
    {
        memcpy( &window, stream->data + byte_index, sizeof( window ) );
        window = serialize_wire_to_host64( window );
    }
    else
    {
        serialize_uint8_t bytes[8];
        int i;
        memset( bytes, 0, sizeof( bytes ) );
        for ( i = 0; i < available; i++ )
        {
            bytes[i] = stream->data[byte_index + i];
        }
        memcpy( &window, bytes, sizeof( window ) );
        window = serialize_wire_to_host64( window );
    }

    *value = (serialize_uint32_t) ( window >> ( stream->bits_read & 7 ) );
    if ( bits < 32 )
    {
        *value &= (serialize_uint32_t) ( ( 1UL << bits ) - 1 );
    }

    stream->bits_read += bits;

    return 1;
}

int serialize_read_bits_processed( const serialize_read_stream_t * stream )
{
    return stream->bits_read;
}

int serialize_read_bytes_processed( const serialize_read_stream_t * stream )
{
    return ( stream->bits_read + 7 ) / 8;
}

int serialize_read_bits_remaining( const serialize_read_stream_t * stream )
{
    return stream->num_bits - stream->bits_read;
}

int serialize_read_error( const serialize_read_stream_t * stream )
{
    return stream->error;
}

/* ---------------------------------------------------------------------------
   measure stream lifecycle

   The measure OPERATIONS are at the bottom of this file rather than here: they
   mirror the whole surface, including the fixed point and 128-bit paths, and
   they reuse the same static helpers those are built from.
   --------------------------------------------------------------------------- */

void serialize_measure_stream_init( serialize_measure_stream_t * stream )
{
    stream->bits_written = 0;
}

int serialize_measure_bits_processed( const serialize_measure_stream_t * stream )
{
    return stream->bits_written;
}

int serialize_measure_bytes_processed( const serialize_measure_stream_t * stream )
{
    return ( stream->bits_written + 7 ) / 8;
}

/* ---------------------------------------------------------------------------
   bool
   --------------------------------------------------------------------------- */

int serialize_write_bool( serialize_write_stream_t * stream, int value )
{
    return serialize_write_bits( stream, value ? 1u : 0u, 1 );
}

int serialize_read_bool( serialize_read_stream_t * stream, int * value )
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

int serialize_write_align_bits( const serialize_write_stream_t * stream )
{
    return ( 8 - ( stream->bits_written % 8 ) ) % 8;
}

int serialize_read_align_bits( const serialize_read_stream_t * stream )
{
    return ( 8 - ( stream->bits_read % 8 ) ) % 8;
}

int serialize_write_align( serialize_write_stream_t * stream )
{
    int remainder = stream->bits_written % 8;
    if ( remainder != 0 )
    {
        return serialize_write_bits( stream, 0, 8 - remainder );
    }
    return 1;
}

int serialize_read_align( serialize_read_stream_t * stream )
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
            stream->error = 1;
            return 0;
        }
    }
    return 1;
}

/* ---------------------------------------------------------------------------
   ranged integers
   --------------------------------------------------------------------------- */

int serialize_write_int( serialize_write_stream_t * stream, serialize_int32_t value, serialize_int32_t min, serialize_int32_t max )
{
    int bits;
    serialize_uint32_t offset;

    if ( stream->error )
    {
        return 0;
    }

    if ( min > max || value < min || value > max )
    {
        stream->error = 1;
        return 0;
    }

    bits = serialize_bits_required( min, max );
    if ( bits == 0 )
    {
        return 1;               /* degenerate range: the value is the range */
    }

    offset = (serialize_uint32_t) value - (serialize_uint32_t) min;

    return serialize_write_bits( stream, offset, bits );
}

int serialize_read_int( serialize_read_stream_t * stream, serialize_int32_t * value, serialize_int32_t min, serialize_int32_t max )
{
    int bits;
    serialize_uint32_t offset = 0;
    serialize_int32_t decoded;

    if ( stream->error )
    {
        return 0;
    }

    if ( min > max )
    {
        stream->error = 1;
        return 0;
    }

    bits = serialize_bits_required( min, max );
    if ( bits == 0 )
    {
        *value = min;
        return 1;
    }

    if ( !serialize_read_bits( stream, &offset, bits ) )
    {
        return 0;
    }

    decoded = (serialize_int32_t) ( (serialize_uint32_t) min + offset );

    /* reject, never clamp — this is where untrusted data arrives */
    if ( decoded < min || decoded > max )
    {
        stream->error = 1;
        return 0;
    }

    *value = decoded;

    return 1;
}

int serialize_write_int64( serialize_write_stream_t * stream, serialize_int64_t value, serialize_int64_t min, serialize_int64_t max )
{
    int bits;
    serialize_uint64_t offset;

    if ( stream->error )
    {
        return 0;
    }

    if ( min > max || value < min || value > max )
    {
        stream->error = 1;
        return 0;
    }

    bits = serialize_bits_required64( min, max );
    if ( bits == 0 )
    {
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

int serialize_read_int64( serialize_read_stream_t * stream, serialize_int64_t * value, serialize_int64_t min, serialize_int64_t max )
{
    int bits;
    serialize_uint64_t offset;
    serialize_uint32_t lo = 0;
    serialize_uint32_t hi = 0;

    if ( stream->error )
    {
        return 0;
    }

    if ( min > max )
    {
        stream->error = 1;
        return 0;
    }

    bits = serialize_bits_required64( min, max );
    if ( bits == 0 )
    {
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
        stream->error = 1;
        return 0;
    }

    *value = (serialize_int64_t) ( (serialize_uint64_t) min + offset );

    return 1;
}

/* ---------------------------------------------------------------------------
   fixed-width integers — NOT ranged
   --------------------------------------------------------------------------- */

int serialize_write_uint8( serialize_write_stream_t * stream, serialize_uint8_t value )
{
    return serialize_write_bits( stream, (serialize_uint32_t) value, 8 );
}

int serialize_read_uint8( serialize_read_stream_t * stream, serialize_uint8_t * value )
{
    serialize_uint32_t raw = 0;
    if ( !serialize_read_bits( stream, &raw, 8 ) )
    {
        return 0;
    }
    *value = (serialize_uint8_t) raw;
    return 1;
}

int serialize_write_uint16( serialize_write_stream_t * stream, serialize_uint16_t value )
{
    return serialize_write_bits( stream, (serialize_uint32_t) value, 16 );
}

int serialize_read_uint16( serialize_read_stream_t * stream, serialize_uint16_t * value )
{
    serialize_uint32_t raw = 0;
    if ( !serialize_read_bits( stream, &raw, 16 ) )
    {
        return 0;
    }
    *value = (serialize_uint16_t) raw;
    return 1;
}

int serialize_write_uint32( serialize_write_stream_t * stream, serialize_uint32_t value )
{
    return serialize_write_bits( stream, value, 32 );
}

int serialize_read_uint32( serialize_read_stream_t * stream, serialize_uint32_t * value )
{
    return serialize_read_bits( stream, value, 32 );
}

int serialize_write_uint64( serialize_write_stream_t * stream, serialize_uint64_t value )
{
    if ( !serialize_write_bits( stream, (serialize_uint32_t) ( value & 0xFFFFFFFFu ), 32 ) )
    {
        return 0;
    }
    return serialize_write_bits( stream, (serialize_uint32_t) ( value >> 32 ), 32 );
}

int serialize_read_uint64( serialize_read_stream_t * stream, serialize_uint64_t * value )
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
   int_relative
   --------------------------------------------------------------------------- */

int serialize_write_int_relative( serialize_write_stream_t * stream, serialize_int32_t previous, serialize_int32_t current )
{
    serialize_uint32_t difference;

    if ( stream->error )
    {
        return 0;
    }

    if ( current <= previous )
    {
        stream->error = 1;
        return 0;
    }

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

int serialize_read_int_relative( serialize_read_stream_t * stream, serialize_int32_t previous, serialize_int32_t * current )
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
            stream->error = 1;
            return 0;
        }
    }

    return 1;
}

/* ---------------------------------------------------------------------------
   floating point
   --------------------------------------------------------------------------- */

int serialize_write_float( serialize_write_stream_t * stream, float value )
{
    serialize_uint32_t bits;
    /* through memcpy rather than a union or a cast: the only spelling that is
       not a strict-aliasing violation in every C standard */
    memcpy( &bits, &value, 4 );
    return serialize_write_bits( stream, bits, 32 );
}

int serialize_read_float( serialize_read_stream_t * stream, float * value )
{
    serialize_uint32_t bits = 0;
    if ( !serialize_read_bits( stream, &bits, 32 ) )
    {
        return 0;
    }
    memcpy( value, &bits, 4 );
    return 1;
}

int serialize_write_double( serialize_write_stream_t * stream, double value )
{
    serialize_uint64_t bits;
    memcpy( &bits, &value, 8 );
    return serialize_write_uint64( stream, bits );
}

int serialize_read_double( serialize_read_stream_t * stream, double * value )
{
    serialize_uint64_t bits = 0;
    if ( !serialize_read_uint64( stream, &bits ) )
    {
        return 0;
    }
    memcpy( value, &bits, 8 );
    return 1;
}

/*
    The width of a compressed float, and the quantization ceiling that goes
    with it. In one place because three callers need it -- write, read and
    measure -- and a formula copied three times is a formula that drifts twice.
*/
static int serialize_compressed_float_bits( float min, float max, float res, serialize_uint32_t * max_integer_value )
{
    float delta = max - min;
    float values = delta / res;
    if ( values < 1.0f )
    {
        values = 1.0f;
    }
    if ( values > 4294967040.0f )
    {
        values = 4294967040.0f;
    }
    *max_integer_value = (serialize_uint32_t) ceil( (double) values );
    return serialize_bits_required( 0, (serialize_int32_t) *max_integer_value );
}

int serialize_write_compressed_float( serialize_write_stream_t * stream, float value, float min, float max, float res )
{
    float delta;
    serialize_uint32_t max_integer_value;
    int bits;
    float normalized;
    float scaled;
    serialize_uint32_t integer_value;

    if ( stream->error )
    {
        return 0;
    }

    delta = max - min;
    bits = serialize_compressed_float_bits( min, max, res, &max_integer_value );

    normalized = ( value - min ) / delta;
    if ( normalized < 0.0f )
    {
        normalized = 0.0f;
    }
    if ( normalized > 1.0f )
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
       single FMA and round ONCE, which diverges again. Match serialize.h's
       serialize_compressed_float_internal exactly. */
    scaled = normalized * (float) max_integer_value;
    integer_value = (serialize_uint32_t) floor( (double) ( scaled + 0.5f ) );

    return serialize_write_bits( stream, integer_value, bits );
}

int serialize_read_compressed_float( serialize_read_stream_t * stream, float * value, float min, float max, float res )
{
    float delta;
    serialize_uint32_t max_integer_value;
    int bits;
    serialize_uint32_t integer_value = 0;

    if ( stream->error )
    {
        return 0;
    }

    delta = max - min;
    bits = serialize_compressed_float_bits( min, max, res, &max_integer_value );

    if ( !serialize_read_bits( stream, &integer_value, bits ) )
    {
        return 0;
    }

    if ( integer_value > max_integer_value )
    {
        stream->error = 1;
        return 0;
    }

    *value = ( (float) integer_value / (float) max_integer_value ) * delta + min;

    return 1;
}

/* ---------------------------------------------------------------------------
   bytes and strings
   --------------------------------------------------------------------------- */

int serialize_write_bytes( serialize_write_stream_t * stream, const serialize_uint8_t * data, int bytes )
{
    int i;

    if ( !serialize_write_align( stream ) )
    {
        return 0;
    }

    /*
        Byte at a time through write_bits. The C++ library has a fast path that
        memcpys whole words once aligned; this is the simple form, and it
        produces identical bytes. Speed here is a later concern than being
        obviously correct.
    */
    for ( i = 0; i < bytes; i++ )
    {
        if ( !serialize_write_bits( stream, (serialize_uint32_t) data[i], 8 ) )
        {
            return 0;
        }
    }

    return 1;
}

int serialize_read_bytes( serialize_read_stream_t * stream, serialize_uint8_t * data, int bytes )
{
    int i;

    if ( !serialize_read_align( stream ) )
    {
        return 0;
    }

    for ( i = 0; i < bytes; i++ )
    {
        serialize_uint32_t raw = 0;
        if ( !serialize_read_bits( stream, &raw, 8 ) )
        {
            return 0;
        }
        data[i] = (serialize_uint8_t) raw;
    }

    return 1;
}

int serialize_write_string( serialize_write_stream_t * stream, const char * string, int buffer_size )
{
    int length;

    if ( stream->error )
    {
        return 0;
    }

    length = (int) strlen( string );
    if ( length >= buffer_size )
    {
        stream->error = 1;
        return 0;
    }

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

int serialize_read_string( serialize_read_stream_t * stream, char * string, int buffer_size )
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
        if ( !serialize_read_bytes( stream, (serialize_uint8_t *) string, length ) )
        {
            return 0;
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

void serialize_copy_string( char * dest, const char * source, unsigned long dest_size )
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
   --------------------------------------------------------------------------- */

serialize_uint128_t serialize_uint128_make( serialize_uint64_t hi, serialize_uint64_t lo )
{
    serialize_uint128_t r;
    r.hi = hi;
    r.lo = lo;
    return r;
}

serialize_int128_t serialize_int128_make( serialize_uint64_t hi, serialize_uint64_t lo )
{
    serialize_int128_t r;
    r.hi = hi;
    r.lo = lo;
    return r;
}

serialize_int128_t serialize_int128_from_int64( serialize_int64_t value )
{
    serialize_int128_t r;
    r.lo = (serialize_uint64_t) value;
    /* sign extend: an arithmetic shift of a negative value is
       implementation-defined in C, so the sign is tested instead */
    r.hi = ( value < 0 ) ? ~(serialize_uint64_t) 0 : (serialize_uint64_t) 0;
    return r;
}

int serialize_uint128_equal( serialize_uint128_t a, serialize_uint128_t b )
{
    return a.lo == b.lo && a.hi == b.hi;
}

int serialize_int128_equal( serialize_int128_t a, serialize_int128_t b )
{
    return a.lo == b.lo && a.hi == b.hi;
}

/* a - b in the unsigned 128-bit domain, wrapping like the hardware would */
static serialize_uint128_t serialize_u128_sub( serialize_uint128_t a, serialize_uint128_t b )
{
    serialize_uint128_t r;
    r.lo = a.lo - b.lo;
    r.hi = a.hi - b.hi - ( a.lo < b.lo ? 1u : 0u );
    return r;
}

static int serialize_u128_compare( serialize_uint128_t a, serialize_uint128_t b )
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

int serialize_int128_compare( serialize_int128_t a, serialize_int128_t b )
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

static int serialize_u128_bit_length( serialize_uint128_t v )
{
    if ( v.hi != 0 )
    {
        return 64 + serialize_bit_length64( v.hi );
    }
    return serialize_bit_length64( v.lo );
}

/* the 32-bit group at index i, counting from least significant */
static serialize_uint32_t serialize_u128_group32( serialize_uint128_t v, int i )
{
    switch ( i )
    {
        case 0: return (serialize_uint32_t) ( v.lo & 0xFFFFFFFFu );
        case 1: return (serialize_uint32_t) ( v.lo >> 32 );
        case 2: return (serialize_uint32_t) ( v.hi & 0xFFFFFFFFu );
        default: return (serialize_uint32_t) ( v.hi >> 32 );
    }
}

static void serialize_u128_set_group32( serialize_uint128_t * v, int i, serialize_uint32_t g )
{
    switch ( i )
    {
        case 0: v->lo |= (serialize_uint64_t) g; break;
        case 1: v->lo |= ( (serialize_uint64_t) g ) << 32; break;
        case 2: v->hi |= (serialize_uint64_t) g; break;
        default: v->hi |= ( (serialize_uint64_t) g ) << 32; break;
    }
}

static serialize_uint128_t serialize_u128_add( serialize_uint128_t a, serialize_uint128_t b )
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
*/
static int serialize_write_u128_bits( serialize_write_stream_t * stream, serialize_uint128_t value, int bits )
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

static int serialize_read_u128_bits( serialize_read_stream_t * stream, serialize_uint128_t * value, int bits )
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

int serialize_write_uint128( serialize_write_stream_t * stream, serialize_uint128_t value )
{
    if ( !serialize_write_uint64( stream, value.lo ) )
    {
        return 0;
    }
    return serialize_write_uint64( stream, value.hi );
}

int serialize_read_uint128( serialize_read_stream_t * stream, serialize_uint128_t * value )
{
    if ( !serialize_read_uint64( stream, &value->lo ) )
    {
        return 0;
    }
    return serialize_read_uint64( stream, &value->hi );
}

int serialize_write_int128( serialize_write_stream_t * stream, serialize_int128_t value, serialize_int128_t min, serialize_int128_t max )
{
    serialize_uint128_t uvalue, umin, umax, span, offset;
    int bits;

    if ( stream->error )
    {
        return 0;
    }

    if ( serialize_int128_compare( min, max ) > 0 ||
         serialize_int128_compare( value, min ) < 0 ||
         serialize_int128_compare( value, max ) > 0 )
    {
        stream->error = 1;
        return 0;
    }

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

int serialize_read_int128( serialize_read_stream_t * stream, serialize_int128_t * value, serialize_int128_t min, serialize_int128_t max )
{
    serialize_uint128_t umin, umax, span, offset, result;
    int bits;

    if ( stream->error )
    {
        return 0;
    }

    if ( serialize_int128_compare( min, max ) > 0 )
    {
        stream->error = 1;
        return 0;
    }

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
        stream->error = 1;
        return 0;
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
static int serialize_write_fixed_core( serialize_write_stream_t * stream, serialize_uint128_t raw_value,
                                       serialize_uint128_t raw_min, serialize_uint128_t raw_max )
{
    serialize_uint128_t span = serialize_u128_sub( raw_max, raw_min );
    int bits = serialize_u128_bit_length( span );
    serialize_uint128_t offset;

    if ( bits == 0 )
    {
        return 1;
    }

    offset = serialize_u128_sub( raw_value, raw_min );

    if ( serialize_u128_compare( offset, span ) > 0 )
    {
        stream->error = 1;
        return 0;
    }

    return serialize_write_u128_bits( stream, offset, bits );
}

static int serialize_read_fixed_core( serialize_read_stream_t * stream, serialize_uint128_t * raw_value,
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
        stream->error = 1;
        return 0;
    }

    *raw_value = serialize_u128_add( raw_min, offset );

    return 1;
}

/* min and max are WHOLE units; the raw bound is min << fraction_bits */
static serialize_uint128_t serialize_raw_bound( serialize_int64_t whole, int fraction_bits )
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

int serialize_write_fixed32( serialize_write_stream_t * stream, serialize_int32_t value, int integer_bits, int fraction_bits, serialize_int32_t min, serialize_int32_t max )
{
    serialize_int128_t wide;
    (void) integer_bits;
    if ( stream->error ) return 0;
    wide = serialize_int128_from_int64( (serialize_int64_t) value );
    return serialize_write_fixed_core( stream, serialize_uint128_make( wide.hi, wide.lo ),
                                       serialize_raw_bound( (serialize_int64_t) min, fraction_bits ),
                                       serialize_raw_bound( (serialize_int64_t) max, fraction_bits ) );
}

int serialize_read_fixed32( serialize_read_stream_t * stream, serialize_int32_t * value, int integer_bits, int fraction_bits, serialize_int32_t min, serialize_int32_t max )
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

int serialize_write_fixed64( serialize_write_stream_t * stream, serialize_int64_t value, int integer_bits, int fraction_bits, serialize_int64_t min, serialize_int64_t max )
{
    serialize_int128_t wide;
    (void) integer_bits;
    if ( stream->error ) return 0;
    wide = serialize_int128_from_int64( value );
    return serialize_write_fixed_core( stream, serialize_uint128_make( wide.hi, wide.lo ),
                                       serialize_raw_bound( min, fraction_bits ),
                                       serialize_raw_bound( max, fraction_bits ) );
}

int serialize_read_fixed64( serialize_read_stream_t * stream, serialize_int64_t * value, int integer_bits, int fraction_bits, serialize_int64_t min, serialize_int64_t max )
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

int serialize_write_fixed128( serialize_write_stream_t * stream, serialize_int128_t value, int integer_bits, int fraction_bits, serialize_int64_t min, serialize_int64_t max )
{
    (void) integer_bits;
    if ( stream->error ) return 0;
    return serialize_write_fixed_core( stream, serialize_uint128_make( value.hi, value.lo ),
                                       serialize_raw_bound( min, fraction_bits ),
                                       serialize_raw_bound( max, fraction_bits ) );
}

int serialize_read_fixed128( serialize_read_stream_t * stream, serialize_int128_t * value, int integer_bits, int fraction_bits, serialize_int64_t min, serialize_int64_t max )
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

int serialize_write_wstring( serialize_write_stream_t * stream, const wchar_t * string, int buffer_size )
{
    int length = 0;
    int i;

    if ( stream->error )
    {
        return 0;
    }

    while ( string[length] != 0 )
    {
        length++;
    }

    if ( length >= buffer_size )
    {
        stream->error = 1;
        return 0;
    }

    if ( !serialize_write_int( stream, length, 0, buffer_size - 1 ) )
    {
        return 0;
    }

    /* NO align here -- deliberately unlike the narrow path. See the header. */
    for ( i = 0; i < length; i++ )
    {
        if ( !serialize_write_bits( stream, (serialize_uint32_t) string[i], 32 ) )
        {
            return 0;
        }
    }

    return 1;
}

int serialize_read_wstring( serialize_read_stream_t * stream, wchar_t * string, int buffer_size )
{
    serialize_int32_t length = 0;
    int i;

    if ( stream->error )
    {
        return 0;
    }

    if ( !serialize_read_int( stream, &length, 0, buffer_size - 1 ) )
    {
        return 0;
    }

    for ( i = 0; i < length; i++ )
    {
        serialize_uint32_t c = 0;
        if ( !serialize_read_bits( stream, &c, 32 ) )
        {
            return 0;
        }
        /*
            Characters ride as 32 bits regardless of the local wchar_t width.
            Where wchar_t cannot hold what arrived, FAIL rather than truncate:
            a silently mangled code point is worse than a refused packet.
        */
        if ( sizeof( wchar_t ) < 4 && c > 0xFFFFu )
        {
            stream->error = 1;
            return 0;
        }
        string[i] = (wchar_t) c;
    }

    string[length] = 0;

    return 1;
}

/* ---------------------------------------------------------------------------
   measure stream operations

   One per write operation, counting the bits that write would emit without
   emitting any. They are here, at the bottom, because they mirror the whole
   surface and reuse the same static helpers the fixed point and 128-bit paths
   are built from -- the widths are computed by the same code that computes
   them on the write path, not by a second copy that could drift from it.

   test/roundtrip.c checks every one of these against the writer, field by
   field: a measure that does not equal the bits actually emitted is worse
   than no measure at all, because it sizes a buffer that then overflows.
   --------------------------------------------------------------------------- */

int serialize_measure_bits( serialize_measure_stream_t * stream, int bits )
{
    stream->bits_written += bits;
    return 1;
}

int serialize_measure_bool( serialize_measure_stream_t * stream )
{
    stream->bits_written += 1;
    return 1;
}

int serialize_measure_align( serialize_measure_stream_t * stream )
{
    int remainder = stream->bits_written % 8;
    if ( remainder != 0 )
    {
        stream->bits_written += 8 - remainder;
    }
    return 1;
}

int serialize_measure_int( serialize_measure_stream_t * stream, serialize_int32_t min, serialize_int32_t max )
{
    stream->bits_written += serialize_bits_required( min, max );
    return 1;
}

int serialize_measure_int64( serialize_measure_stream_t * stream, serialize_int64_t min, serialize_int64_t max )
{
    stream->bits_written += serialize_bits_required64( min, max );
    return 1;
}

int serialize_measure_uint8( serialize_measure_stream_t * stream )
{
    stream->bits_written += 8;
    return 1;
}

int serialize_measure_uint16( serialize_measure_stream_t * stream )
{
    stream->bits_written += 16;
    return 1;
}

int serialize_measure_uint32( serialize_measure_stream_t * stream )
{
    stream->bits_written += 32;
    return 1;
}

int serialize_measure_uint64( serialize_measure_stream_t * stream )
{
    stream->bits_written += 64;
    return 1;
}

/*
    The one operation whose width cannot be worked around with measure_bits:
    the tier is chosen by the difference, so the caller would have to
    reimplement the ladder to know the count. This walks the same ladder
    serialize_write_int_relative does, in the same order, and asks
    serialize_bits_required for each tier's width rather than restating it as
    a literal that could disagree with the writer.
*/
int serialize_measure_int_relative( serialize_measure_stream_t * stream, serialize_int32_t previous, serialize_int32_t current )
{
    serialize_uint32_t difference;

    if ( current <= previous )
    {
        return 0;
    }

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

int serialize_measure_float( serialize_measure_stream_t * stream )
{
    stream->bits_written += 32;
    return 1;
}

int serialize_measure_double( serialize_measure_stream_t * stream )
{
    stream->bits_written += 64;
    return 1;
}

int serialize_measure_compressed_float( serialize_measure_stream_t * stream, float min, float max, float res )
{
    serialize_uint32_t max_integer_value;
    stream->bits_written += serialize_compressed_float_bits( min, max, res, &max_integer_value );
    return 1;
}

int serialize_measure_bytes( serialize_measure_stream_t * stream, int bytes )
{
    serialize_measure_align( stream );
    stream->bits_written += bytes * 8;
    return 1;
}

int serialize_measure_string( serialize_measure_stream_t * stream, const char * string, int buffer_size )
{
    int length = (int) strlen( string );

    /* the writer refuses a string that does not fit, so a count for one would
       be a number no write could ever produce */
    if ( length >= buffer_size )
    {
        return 0;
    }

    serialize_measure_int( stream, 0, buffer_size - 1 );
    serialize_measure_bytes( stream, length );

    return 1;
}

/* NO align, matching serialize_write_wstring. See the header. */
int serialize_measure_wstring( serialize_measure_stream_t * stream, const wchar_t * string, int buffer_size )
{
    int length = 0;

    while ( string[length] != 0 )
    {
        length++;
    }

    if ( length >= buffer_size )
    {
        return 0;
    }

    stream->bits_written += serialize_bits_required( 0, buffer_size - 1 );
    stream->bits_written += length * 32;

    return 1;
}

int serialize_measure_uint128( serialize_measure_stream_t * stream )
{
    stream->bits_written += 128;
    return 1;
}

int serialize_measure_int128( serialize_measure_stream_t * stream, serialize_int128_t min, serialize_int128_t max )
{
    serialize_uint128_t umin, umax, span;

    if ( serialize_int128_compare( min, max ) > 0 )
    {
        return 0;
    }

    umin = serialize_uint128_make( min.hi, min.lo );
    umax = serialize_uint128_make( max.hi, max.lo );
    span = serialize_u128_sub( umax, umin );

    stream->bits_written += serialize_u128_bit_length( span );

    return 1;
}

/* the shared core, mirroring serialize_write_fixed_core: the width comes from
   the span of the RAW (scaled) bounds, and nothing else */
static int serialize_measure_fixed_core( serialize_measure_stream_t * stream, int fraction_bits, serialize_int64_t min, serialize_int64_t max )
{
    serialize_uint128_t raw_min = serialize_raw_bound( min, fraction_bits );
    serialize_uint128_t raw_max = serialize_raw_bound( max, fraction_bits );
    serialize_uint128_t span = serialize_u128_sub( raw_max, raw_min );

    stream->bits_written += serialize_u128_bit_length( span );

    return 1;
}

int serialize_measure_fixed32( serialize_measure_stream_t * stream, int integer_bits, int fraction_bits, serialize_int32_t min, serialize_int32_t max )
{
    (void) integer_bits;
    return serialize_measure_fixed_core( stream, fraction_bits, (serialize_int64_t) min, (serialize_int64_t) max );
}

int serialize_measure_fixed64( serialize_measure_stream_t * stream, int integer_bits, int fraction_bits, serialize_int64_t min, serialize_int64_t max )
{
    (void) integer_bits;
    return serialize_measure_fixed_core( stream, fraction_bits, min, max );
}

int serialize_measure_fixed128( serialize_measure_stream_t * stream, int integer_bits, int fraction_bits, serialize_int64_t min, serialize_int64_t max )
{
    (void) integer_bits;
    return serialize_measure_fixed_core( stream, fraction_bits, min, max );
}
