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
   measure stream
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

int serialize_measure_bits( serialize_measure_stream_t * stream, int bits )
{
    stream->bits_written += bits;
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

int serialize_measure_align( serialize_measure_stream_t * stream )
{
    int remainder = stream->bits_written % 8;
    if ( remainder != 0 )
    {
        stream->bits_written += 8 - remainder;
    }
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
    serialize_measure_int( stream, 0, buffer_size - 1 );
    serialize_measure_bytes( stream, length );
    return 1;
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

int serialize_write_compressed_float( serialize_write_stream_t * stream, float value, float min, float max, float res )
{
    float delta;
    float values;
    serialize_uint32_t max_integer_value;
    int bits;
    float normalized;
    serialize_uint32_t integer_value;

    if ( stream->error )
    {
        return 0;
    }

    delta = max - min;
    values = delta / res;
    if ( values < 1.0f )
    {
        values = 1.0f;
    }
    if ( values > 4294967040.0f )
    {
        values = 4294967040.0f;
    }
    max_integer_value = (serialize_uint32_t) ceil( (double) values );
    bits = serialize_bits_required( 0, (serialize_int32_t) max_integer_value );

    normalized = ( value - min ) / delta;
    if ( normalized < 0.0f )
    {
        normalized = 0.0f;
    }
    if ( normalized > 1.0f )
    {
        normalized = 1.0f;
    }

    integer_value = (serialize_uint32_t) floor( (double) normalized * (double) max_integer_value + 0.5 );

    return serialize_write_bits( stream, integer_value, bits );
}

int serialize_read_compressed_float( serialize_read_stream_t * stream, float * value, float min, float max, float res )
{
    float delta;
    float values;
    serialize_uint32_t max_integer_value;
    int bits;
    serialize_uint32_t integer_value = 0;

    if ( stream->error )
    {
        return 0;
    }

    delta = max - min;
    values = delta / res;
    if ( values < 1.0f )
    {
        values = 1.0f;
    }
    if ( values > 4294967040.0f )
    {
        values = 4294967040.0f;
    }
    max_integer_value = (serialize_uint32_t) ceil( (double) values );
    bits = serialize_bits_required( 0, (serialize_int32_t) max_integer_value );

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
