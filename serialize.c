/*
    serialize.c 1.0 — a bitpacking serializer for C

    See serialize.h for the API and STANDARD.md for the wire format.

    The bit packer accumulates into a 64-bit scratch word, least significant
    bit first, and commits whole 8-byte words. That is the whole trick, and
    everything else in this file is a consumer of write_bits / read_bits.

    WHAT IS IN THIS FILE AND WHAT IS NOT

    The per-field operations — bits, bool, align, ranged int and int64, the
    fixed-width helpers, float, double, the byte-block read, the stream
    lifecycle and the measure operations that mirror them — are in
    serialize.h, so they inline into the caller and their bit widths fold to
    literals; the read spine demands that inlining as SERIALIZE_ALWAYS_INLINE
    where the write side merely asks. The header says why.

    What is here is everything whose cost is what it does rather than the call
    to get there: strings, the byte-block write, int_relative, the 128-bit
    lanes and the fixed point path built on them. Each is a consumer of the
    header's write_bits / read_bits, and gets them inlined the same way a
    caller does. Compressed float moved to the header 2026-08-16 — at call
    sites carrying literal parameters its width computation folds away, the
    shape the C++ template demonstrates — see the header's section comment.
*/

#include "serialize.h"

#include <string.h>
#include <math.h>

/* ---------------------------------------------------------------------------
   int_relative
   --------------------------------------------------------------------------- */

int serialize_write_int_relative( serialize_write_stream_t * stream, serialize_int32_t previous, serialize_int32_t current )
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
            return serialize_read_fail( stream );
        }
    }

    return 1;
}

/* ---------------------------------------------------------------------------
   compressed float — lives in serialize.h with the per-field spine, hoisted
   2026-08-16 so generated call sites carrying literal min/max/res inline and
   constant-fold the width computation, the shape the C++ header template
   demonstrates. See the header's section comment for the receipts.
   --------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
   bytes and strings
   --------------------------------------------------------------------------- */

/*
    The bulk write path, mirroring the C++ WriteBytes. The read half lives in
    serialize.h with the per-field surface — after its align it is one memcpy,
    and the small blocks packet code reads per field made the call the cost.

    A block of bytes is byte aligned by construction — serialize_write_align
    runs first, and that alignment is part of the format — so most of it can be
    copied straight into the buffer instead of pushed through the packer eight
    bits at a time. The bytes are identical either way and on either endianness:
    a byte written through the packer lands at its own offset in the buffer,
    because the scratch word is byte swapped on a big-endian host precisely so
    that it does. The golden vectors pin that, and CI runs them on s390x.
*/

int serialize_write_bytes( serialize_write_stream_t * stream, const serialize_uint8_t * data, int bytes )
{
    int head_bytes;
    int num_words;
    int i;

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

    /* the head: whole bytes through the packer until the scratch word is
       empty and the buffer position is a whole word */
    head_bytes = ( 8 - ( stream->bits_written % 64 ) / 8 ) % 8;
    if ( head_bytes > bytes )
    {
        head_bytes = bytes;
    }
    for ( i = 0; i < head_bytes; i++ )
    {
        if ( !serialize_write_bits( stream, (serialize_uint32_t) data[i], 8 ) )
        {
            return 0;
        }
    }
    if ( head_bytes == bytes )
    {
        return 1;
    }

    serialize_assert( ( stream->bits_written % 64 ) == 0 );
    serialize_assert( stream->scratch_bits == 0 );

    /* the body: whole words, straight in */
    num_words = ( bytes - head_bytes ) / 8;
    if ( num_words > 0 )
    {
        memcpy( stream->data + (size_t) stream->word_index * 8, data + head_bytes, (size_t) num_words * 8 );
        stream->bits_written += num_words * 64;
        stream->word_index += num_words;
    }

    /* and the tail, back through the packer */
    for ( i = head_bytes + num_words * 8; i < bytes; i++ )
    {
        if ( !serialize_write_bits( stream, (serialize_uint32_t) data[i], 8 ) )
        {
            return 0;
        }
    }

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
static int serialize_string_is_valid_utf8( const char * string, int length )
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

int serialize_write_string( serialize_write_stream_t * stream, const char * string, int buffer_size )
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

int serialize_read_int128( serialize_read_stream_t * stream, serialize_int128_t * value, serialize_int128_t min, serialize_int128_t max )
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

/*
    Each 32-bit group carries one UTF-16 CODE UNIT, not one code point
    (STANDARD.md, adopted 2026-08-15), so 2-byte and 4-byte wchar_t platforms
    produce identical bytes: the 4-byte platform converts at the boundary,
    splitting an astral code point into its surrogate pair on write and
    recombining the pair on read. This counts the units a string transmits —
    which on a 4-byte platform is more than its character count when astral
    text is present, and is the count the length field carries.
*/
static int serialize_wstring_unit_count( const wchar_t * string )
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
static SERIALIZE_UNUSED int serialize_wstring_is_valid_utf16( const wchar_t * string )
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

int serialize_write_wstring( serialize_write_stream_t * stream, const wchar_t * string, int buffer_size )
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

    /* NO align here -- deliberately unlike the narrow path. See the header. */
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

int serialize_read_wstring( serialize_read_stream_t * stream, wchar_t * string, int buffer_size )
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
   measure stream operations

   One per write operation, bounding the bits that write would emit without
   emitting any. They are here, at the bottom, because they mirror the whole
   surface and reuse the same static helpers the fixed point and 128-bit paths
   are built from -- the widths are computed by the same code that computes
   them on the write path, not by a second copy that could drift from it.

   The bound is CONSERVATIVE, never below: align charges its worst case of 7
   bits wherever it appears, because the padding is bit position dependent
   and a measure does not know where its message will land (see
   serialize_measure_align in serialize.h -- the fork ruling, and the C++
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
int serialize_measure_int_relative( serialize_measure_stream_t * stream, serialize_int32_t previous, serialize_int32_t current )
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

int serialize_measure_string( serialize_measure_stream_t * stream, const char * string, int buffer_size )
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

/* NO align, matching serialize_write_wstring. See the header. Counts UTF-16
   CODE UNITS through the same helper the writer uses, so an astral character
   on a 4-byte wchar_t platform measures as the two groups it transmits. */
int serialize_measure_wstring( serialize_measure_stream_t * stream, const wchar_t * string, int buffer_size )
{
    int units = serialize_wstring_unit_count( string );

    /* the caller's contract, debug-asserted exactly as the narrow path
       asserts it (issue #52) */
    serialize_assert( units < buffer_size );

    stream->bits_written += serialize_bits_required( 0, (serialize_uint32_t) ( buffer_size - 1 ) );
    stream->bits_written += units * 32;

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
