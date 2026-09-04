/*
    The shared conformance corpus, run through this library's reader, writer
    and measure.

    conformance/ is a VERBATIM VENDORED COPY of the corpus in
    mas-bandwidth/serialize, the same way STANDARD.md is, and CI's
    `conformance corpus matches upstream` job holds the copy to it. The corpus
    is the family's one conformance instrument: every implementation runs the
    same vectors, so one wrong reading of the standard cannot travel to nine
    ports under green results the way it does when each port generates its own
    expectations and then checks them against itself.

    Every vector in every file is run. The directory is SCANNED rather than
    listed here, so a file upstream adds arrives with the sync and runs on the
    next build with no edit to this file, and an EMPTY directory fails the
    run. An operation this runner cannot drive is a FAILURE rather than a
    silent skip, and so is a parameter it does not understand and a fixed
    point declaration it cannot carry.

    What each vector asserts, from STANDARD.md's "The vector format" and its
    runner contract:

      accepted   the read succeeds, every step decodes the stated value, and
                 serialize_read_bits_processed equals the stated `consumed`
      refused    the read fails, the caller's SCALAR destination holds exactly
                 what it held before the call -- the non-mutation rule under
                 Reader Obligations, checked here against a sentinel the
                 caller planted -- and the stream is left TERMINAL

    A caller-owned BUFFER -- bytes, string and wstring -- is left UNSPECIFIED
    after a refusal by the standard, so nothing here checks one.

    `writer = canonical` additionally pins the bytes a conforming writer emits:
    the decoded values are re-emitted through the write stream and the WHOLE
    emission is compared byte for byte, flush included, which is where the
    trailing-bits obligation bites.

    `measure_at_least` states a FLOOR the measure stream must reach, never an
    equality, because a measure is a bound and not the packet size.

    TERMINALITY, checked by behavior rather than by an accessor. After a
    refusal every later step of a sequence must refuse too, however many
    readable bits remain, and a further read the vector does not name must fail
    and write nothing to its own destination. This port's cursor is the third
    thing the reference runner checks and it does not port: bits_read here
    advances UNCONDITIONALLY, before the limit test, so that the read path
    carries no serial cursor dependency (see serialize_read_bits), and on a
    FAILED stream it counts reads ATTEMPTED rather than data decoded -- which
    the header states in ERRORS. What makes failure terminal here is the
    poisoned bits_limit, and its effect is exactly the two things checked
    below: the later read refuses, and it decodes nothing.

    THE BUFFER CONTRACT. This library's reader loads 64-bit windows at byte
    granularity and requires at least 8 bytes of allocation past the data. Every
    stream here carries that slack, and the slack is filled with a NON-ZERO
    pattern, so a decode that depends on memory past the end cannot pass by
    reading zeros. That is why the streams are built here and handed to
    serialize_read_stream_init rather than to the padded wrapper, which zeroes
    the slack it adds: the wrapper is the convenience entry point for an
    exactly sized payload, and zeroed slack is the weaker instrument.
*/

/* MSVC's CRT deprecates sprintf in favour of sprintf_s, which no other
   toolchain has. Every use here writes a bounded string into a fixed
   destination -- failure messages from fields capped by MAX_TEXT, and vector
   file paths -- so the portable spelling stays and the warning stands down.
   Must precede every CRT header. */
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../serialize.h"
#include "verbose.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

#define MAX_PARAMS 8
#define MAX_STEPS 48                /* the golden message is 28 operations long */
#define MAX_BYTES 256
#define MAX_SLACK 8
#define SLACK_FILL 0xA5
#define MAX_TEXT 128
#define MAX_LINE 512

/* ---------------------------------------------------------------------------
   the vector record
   --------------------------------------------------------------------------- */

typedef struct
{
    const char * file;
    char operation[MAX_TEXT];
    char name[MAX_TEXT];
    char param_name[MAX_PARAMS][MAX_TEXT];
    char param_value[MAX_PARAMS][MAX_TEXT];
    int param_count;
    char step_text[MAX_STEPS][MAX_TEXT];
    int step_count;
    serialize_uint8_t bytes[MAX_BYTES];
    int byte_count;
    int refused;
    char expect[MAX_LINE];
    serialize_int64_t consumed;
    int have_consumed;
    serialize_int64_t measure_at_least;
    int have_measure;
    int writer_canonical;
} vector_t;

static int failures = 0;
static int vectors_run = 0;
static int writer_checks = 0;
static int measure_checks = 0;

static void fail( const vector_t * v, const char * what )
{
    printf( "FAILED %s: %s [%s]\n", v->name, what, v->file );
    failures++;
}

/* ---------------------------------------------------------------------------
   128-bit numbers, in and out

   The corpus states values as signed decimal or as 0x hexadecimal, and a
   parser must accept 128 bits of them. This library's 128-bit type is two
   64-bit lanes rather than a native __int128 (see serialize.h), so the
   conversion is done by hand in both directions.
   --------------------------------------------------------------------------- */

static serialize_uint128_t u128_shl1( serialize_uint128_t v )
{
    serialize_uint128_t r;
    r.hi = ( v.hi << 1 ) | ( v.lo >> 63 );
    r.lo = v.lo << 1;
    return r;
}

static serialize_uint128_t u128_neg( serialize_uint128_t v )
{
    serialize_uint128_t n;
    n.hi = ~v.hi;
    n.lo = ~v.lo;
    return serialize_u128_add( n, serialize_uint128_make( 0, 1 ) );
}

static int hex_digit( char c )
{
    if ( c >= '0' && c <= '9' ) return c - '0';
    if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
    if ( c >= 'A' && c <= 'F' ) return c - 'A' + 10;
    return -1;
}

/*
    Signed decimal or 0x hexadecimal, accumulated in the UNSIGNED domain so the
    corpus's extremes -- the full signed minimum and the unsigned maximum as a
    decimal -- land where they should by wrapping rather than by overflowing a
    signed type. The two's complement reading happens once, at the end.
*/
static int parse_number( const char * text, serialize_int128_t * out )
{
    serialize_uint128_t acc = { 0, 0 };
    int negative = 0;
    int digits = 0;

    if ( *text == '-' )
    {
        negative = 1;
        text++;
    }
    else if ( *text == '+' )
    {
        text++;
    }

    if ( text[0] == '0' && ( text[1] == 'x' || text[1] == 'X' ) )
    {
        text += 2;
        for ( ; *text != '\0'; text++ )
        {
            int digit = hex_digit( *text );
            if ( digit < 0 )
            {
                return 0;
            }
            acc = u128_shl1( u128_shl1( u128_shl1( u128_shl1( acc ) ) ) );
            acc = serialize_u128_add( acc, serialize_uint128_make( 0, (serialize_uint64_t) digit ) );
            digits++;
        }
    }
    else
    {
        for ( ; *text != '\0'; text++ )
        {
            serialize_uint128_t x2, x8;
            if ( *text < '0' || *text > '9' )
            {
                return 0;
            }
            x2 = u128_shl1( acc );
            x8 = u128_shl1( u128_shl1( x2 ) );
            acc = serialize_u128_add( serialize_u128_add( x8, x2 ),
                                      serialize_uint128_make( 0, (serialize_uint64_t) ( *text - '0' ) ) );
            digits++;
        }
    }

    if ( digits == 0 )
    {
        return 0;
    }

    if ( negative )
    {
        acc = u128_neg( acc );
    }

    *out = serialize_int128_make( acc.hi, acc.lo );

    return 1;
}

/* writes 0x and 32 hexadecimal digits into a caller buffer of at least 35 */
static void render_hex128( serialize_uint128_t v, char * out )
{
    static const char * digits = "0123456789ABCDEF";
    int i;
    out[0] = '0';
    out[1] = 'x';
    for ( i = 0; i < 4; i++ )
    {
        serialize_uint32_t group = serialize_u128_group32( v, 3 - i );
        int n;
        for ( n = 0; n < 8; n++ )
        {
            out[2 + i * 8 + n] = digits[( group >> ( 28 - n * 4 ) ) & 0xF];
        }
    }
    out[34] = '\0';
}

/* ---------------------------------------------------------------------------
   the step machine

   One step drives both the single operation files and the sequence, object and
   message files: a single operation vector is a one or two step sequence built
   from the record's own parameters, so there is exactly one execution path and
   the sequence files cannot drift away from the operation files.
   --------------------------------------------------------------------------- */

typedef enum
{
    STEP_BITS,
    STEP_BOOL,
    STEP_UINT128,
    STEP_ALIGN,
    STEP_INT,
    STEP_INT64,
    STEP_INT128,
    STEP_INT_RELATIVE,
    STEP_FLOAT,
    STEP_DOUBLE,
    STEP_COMPRESSED_FLOAT,
    STEP_BYTES,
    STEP_STRING,
    STEP_WSTRING,
    STEP_FIXED,
    STEP_OBJECT                     /* wraps the next `width` steps in a nested object */
} step_kind_t;

typedef struct
{
    step_kind_t kind;
    serialize_int64_t width;        /* bits, count, buffer_size, or the span an object wraps */
    serialize_int128_t min;
    serialize_int128_t max;
    float fmin;
    float fmax;
    float fres;
    int integer_bits;
    int fraction_bits;
    int fixed_storage;              /* 32, 64 or 128 */
    serialize_int32_t previous;

    /* the destination, in and out: the reader decodes into it and the writer
       and the measure legs re-serialize exactly what the reader decoded */
    serialize_uint128_t pattern;    /* bits, uint128, float, double, compressed_float */
    serialize_int128_t number;      /* int, int64, int128, int_relative, fixed */
    int boolean;
    serialize_uint8_t buffer[MAX_BYTES + 1];
    wchar_t wbuffer[MAX_BYTES + 1];
} step_t;

static step_t * failed_step = NULL;

static int step_value_is_a_pattern( step_kind_t kind )
{
    return kind == STEP_BITS || kind == STEP_UINT128 || kind == STEP_FLOAT
        || kind == STEP_DOUBLE || kind == STEP_COMPRESSED_FLOAT;
}

static int step_value_is_a_number( step_kind_t kind )
{
    return kind == STEP_INT || kind == STEP_INT64 || kind == STEP_INT128
        || kind == STEP_INT_RELATIVE || kind == STEP_FIXED;
}

/* a nested object owns the steps that follow it, so a walk sees one step per
   object */
static int step_span( const step_t * steps, int index )
{
    if ( steps[index].kind == STEP_OBJECT )
    {
        return 1 + (int) steps[index].width;
    }
    return 1;
}

static serialize_uint64_t pattern_lo( const step_t * step )
{
    return step->pattern.lo;
}

static void set_pattern_lo( step_t * step, serialize_uint64_t value )
{
    step->pattern.lo = value;
    step->pattern.hi = 0;
}

/* ---------------------------------------------------------------------------
   the read leg
   --------------------------------------------------------------------------- */

static int steps_read( serialize_read_stream_t * r, step_t * steps, int count, int * stopped_at );

/*
    A nested object's own serialize function. STANDARD.md, "object": it is
    composition and not an encoding -- no framing, no length prefix, no
    alignment inserted around it -- which in C is exactly a call that runs the
    nested steps against the same stream.
*/
static int object_read( serialize_read_stream_t * r, step_t * steps, int count )
{
    return steps_read( r, steps, count, NULL );
}

static int step_read( serialize_read_stream_t * r, step_t * step )
{
    switch ( step->kind )
    {
        case STEP_BITS:
        {
            serialize_uint64_t value = pattern_lo( step );
            if ( !serialize_read_bits64( r, &value, (int) step->width ) )
            {
                return 0;
            }
            set_pattern_lo( step, value );
            return 1;
        }

        case STEP_BOOL:
            return serialize_read_bool( r, &step->boolean );

        case STEP_UINT128:
            return serialize_read_uint128( r, &step->pattern );

        case STEP_ALIGN:
            return serialize_read_align( r );

        case STEP_INT:
        {
            serialize_int32_t value = (serialize_int32_t) step->number.lo;
            if ( !serialize_read_int( r, &value, (serialize_int32_t) step->min.lo, (serialize_int32_t) step->max.lo ) )
            {
                return 0;
            }
            step->number = serialize_int128_from_int64( (serialize_int64_t) value );
            return 1;
        }

        case STEP_INT64:
        {
            serialize_int64_t value = (serialize_int64_t) step->number.lo;
            if ( !serialize_read_int64( r, &value, (serialize_int64_t) step->min.lo, (serialize_int64_t) step->max.lo ) )
            {
                return 0;
            }
            step->number = serialize_int128_from_int64( value );
            return 1;
        }

        case STEP_INT128:
            return serialize_read_int128( r, &step->number, step->min, step->max );

        case STEP_INT_RELATIVE:
        {
            serialize_int32_t value = (serialize_int32_t) step->number.lo;
            if ( !serialize_read_int_relative( r, step->previous, &value ) )
            {
                return 0;
            }
            step->number = serialize_int128_from_int64( (serialize_int64_t) value );
            return 1;
        }

        case STEP_FLOAT:
        {
            serialize_uint32_t bits = (serialize_uint32_t) pattern_lo( step );
            float value;
            memcpy( &value, &bits, 4 );
            if ( !serialize_read_float( r, &value ) )
            {
                return 0;
            }
            memcpy( &bits, &value, 4 );
            set_pattern_lo( step, (serialize_uint64_t) bits );
            return 1;
        }

        case STEP_DOUBLE:
        {
            serialize_uint64_t bits = pattern_lo( step );
            double value;
            memcpy( &value, &bits, 8 );
            if ( !serialize_read_double( r, &value ) )
            {
                return 0;
            }
            memcpy( &bits, &value, 8 );
            set_pattern_lo( step, bits );
            return 1;
        }

        case STEP_COMPRESSED_FLOAT:
        {
            serialize_uint32_t bits = (serialize_uint32_t) pattern_lo( step );
            float value;
            memcpy( &value, &bits, 4 );
            if ( !serialize_read_compressed_float( r, &value, step->fmin, step->fmax, step->fres ) )
            {
                return 0;
            }
            memcpy( &bits, &value, 4 );
            set_pattern_lo( step, (serialize_uint64_t) bits );
            return 1;
        }

        case STEP_BYTES:
            return serialize_read_bytes( r, step->buffer, (int) step->width );

        case STEP_STRING:
            return serialize_read_string( r, (char *) step->buffer, (int) step->width );

        case STEP_WSTRING:
            return serialize_read_wstring( r, step->wbuffer, (int) step->width );

        case STEP_FIXED:
        {
            if ( step->fixed_storage == 32 )
            {
                serialize_int32_t value = (serialize_int32_t) step->number.lo;
                if ( !serialize_read_fixed32( r, &value, step->integer_bits, step->fraction_bits,
                                              (serialize_int32_t) step->min.lo, (serialize_int32_t) step->max.lo ) )
                {
                    return 0;
                }
                step->number = serialize_int128_from_int64( (serialize_int64_t) value );
                return 1;
            }
            if ( step->fixed_storage == 64 )
            {
                serialize_int64_t value = (serialize_int64_t) step->number.lo;
                if ( !serialize_read_fixed64( r, &value, step->integer_bits, step->fraction_bits,
                                              (serialize_int64_t) step->min.lo, (serialize_int64_t) step->max.lo ) )
                {
                    return 0;
                }
                step->number = serialize_int128_from_int64( value );
                return 1;
            }
            return serialize_read_fixed128( r, &step->number, step->integer_bits, step->fraction_bits,
                                            (serialize_int64_t) step->min.lo, (serialize_int64_t) step->max.lo );
        }

        case STEP_OBJECT:
        default:
            /* nesting is driven by steps_read, which owns the step range an
               object wraps; a bare object step reaching here is a runner bug */
            return 0;
    }
}

static int steps_read( serialize_read_stream_t * r, step_t * steps, int count, int * stopped_at )
{
    int i = 0;
    while ( i < count )
    {
        int span = step_span( steps, i );
        if ( steps[i].kind == STEP_OBJECT )
        {
            if ( !object_read( r, steps + i + 1, (int) steps[i].width ) )
            {
                if ( stopped_at != NULL ) *stopped_at = i;
                return 0;
            }
        }
        else if ( !step_read( r, &steps[i] ) )
        {
            failed_step = &steps[i];
            if ( stopped_at != NULL ) *stopped_at = i;
            return 0;
        }
        i += span;
    }
    return 1;
}

/* ---------------------------------------------------------------------------
   the write leg
   --------------------------------------------------------------------------- */

static int steps_write( serialize_write_stream_t * w, step_t * steps, int count );

static int object_write( serialize_write_stream_t * w, step_t * steps, int count )
{
    return steps_write( w, steps, count );
}

static int step_write( serialize_write_stream_t * w, step_t * step )
{
    switch ( step->kind )
    {
        case STEP_BITS:
            return serialize_write_bits64( w, pattern_lo( step ), (int) step->width );

        case STEP_BOOL:
            return serialize_write_bool( w, step->boolean );

        case STEP_UINT128:
            return serialize_write_uint128( w, step->pattern );

        case STEP_ALIGN:
            return serialize_write_align( w );

        case STEP_INT:
            return serialize_write_int( w, (serialize_int32_t) step->number.lo,
                                        (serialize_int32_t) step->min.lo, (serialize_int32_t) step->max.lo );

        case STEP_INT64:
            return serialize_write_int64( w, (serialize_int64_t) step->number.lo,
                                          (serialize_int64_t) step->min.lo, (serialize_int64_t) step->max.lo );

        case STEP_INT128:
            return serialize_write_int128( w, step->number, step->min, step->max );

        case STEP_INT_RELATIVE:
            return serialize_write_int_relative( w, step->previous, (serialize_int32_t) step->number.lo );

        case STEP_FLOAT:
        {
            serialize_uint32_t bits = (serialize_uint32_t) pattern_lo( step );
            float value;
            memcpy( &value, &bits, 4 );
            return serialize_write_float( w, value );
        }

        case STEP_DOUBLE:
        {
            serialize_uint64_t bits = pattern_lo( step );
            double value;
            memcpy( &value, &bits, 8 );
            return serialize_write_double( w, value );
        }

        case STEP_COMPRESSED_FLOAT:
        {
            serialize_uint32_t bits = (serialize_uint32_t) pattern_lo( step );
            float value;
            memcpy( &value, &bits, 4 );
            return serialize_write_compressed_float( w, value, step->fmin, step->fmax, step->fres );
        }

        case STEP_BYTES:
            return serialize_write_bytes( w, step->buffer, (int) step->width );

        case STEP_STRING:
            return serialize_write_string( w, (const char *) step->buffer, (int) step->width );

        case STEP_WSTRING:
            return serialize_write_wstring( w, step->wbuffer, (int) step->width );

        case STEP_FIXED:
            if ( step->fixed_storage == 32 )
            {
                return serialize_write_fixed32( w, (serialize_int32_t) step->number.lo,
                                                step->integer_bits, step->fraction_bits,
                                                (serialize_int32_t) step->min.lo, (serialize_int32_t) step->max.lo );
            }
            if ( step->fixed_storage == 64 )
            {
                return serialize_write_fixed64( w, (serialize_int64_t) step->number.lo,
                                                step->integer_bits, step->fraction_bits,
                                                (serialize_int64_t) step->min.lo, (serialize_int64_t) step->max.lo );
            }
            return serialize_write_fixed128( w, step->number, step->integer_bits, step->fraction_bits,
                                             (serialize_int64_t) step->min.lo, (serialize_int64_t) step->max.lo );

        case STEP_OBJECT:
        default:
            return 0;
    }
}

static int steps_write( serialize_write_stream_t * w, step_t * steps, int count )
{
    int i = 0;
    while ( i < count )
    {
        int span = step_span( steps, i );
        if ( steps[i].kind == STEP_OBJECT )
        {
            if ( !object_write( w, steps + i + 1, (int) steps[i].width ) )
            {
                return 0;
            }
        }
        else if ( !step_write( w, &steps[i] ) )
        {
            return 0;
        }
        i += span;
    }
    return 1;
}

/* ---------------------------------------------------------------------------
   the measure leg
   --------------------------------------------------------------------------- */

static int steps_measure( serialize_measure_stream_t * m, step_t * steps, int count );

static int object_measure( serialize_measure_stream_t * m, step_t * steps, int count )
{
    return steps_measure( m, steps, count );
}

static int step_measure( serialize_measure_stream_t * m, step_t * step )
{
    switch ( step->kind )
    {
        case STEP_BITS:
            return serialize_measure_bits64( m, (int) step->width );

        case STEP_BOOL:
            return serialize_measure_bool( m );

        case STEP_UINT128:
            return serialize_measure_uint128( m );

        case STEP_ALIGN:
            return serialize_measure_align( m );

        case STEP_INT:
            return serialize_measure_int( m, (serialize_int32_t) step->min.lo, (serialize_int32_t) step->max.lo );

        case STEP_INT64:
            return serialize_measure_int64( m, (serialize_int64_t) step->min.lo, (serialize_int64_t) step->max.lo );

        case STEP_INT128:
            return serialize_measure_int128( m, step->min, step->max );

        case STEP_INT_RELATIVE:
            return serialize_measure_int_relative( m, step->previous, (serialize_int32_t) step->number.lo );

        case STEP_FLOAT:
            return serialize_measure_float( m );

        case STEP_DOUBLE:
            return serialize_measure_double( m );

        case STEP_COMPRESSED_FLOAT:
            return serialize_measure_compressed_float( m, step->fmin, step->fmax, step->fres );

        case STEP_BYTES:
            return serialize_measure_bytes( m, (int) step->width );

        case STEP_STRING:
            return serialize_measure_string( m, (const char *) step->buffer, (int) step->width );

        case STEP_WSTRING:
            return serialize_measure_wstring( m, step->wbuffer, (int) step->width );

        case STEP_FIXED:
            if ( step->fixed_storage == 32 )
            {
                return serialize_measure_fixed32( m, step->integer_bits, step->fraction_bits,
                                                  (serialize_int32_t) step->min.lo, (serialize_int32_t) step->max.lo );
            }
            if ( step->fixed_storage == 64 )
            {
                return serialize_measure_fixed64( m, step->integer_bits, step->fraction_bits,
                                                  (serialize_int64_t) step->min.lo, (serialize_int64_t) step->max.lo );
            }
            return serialize_measure_fixed128( m, step->integer_bits, step->fraction_bits,
                                               (serialize_int64_t) step->min.lo, (serialize_int64_t) step->max.lo );

        case STEP_OBJECT:
        default:
            return 0;
    }
}

static int steps_measure( serialize_measure_stream_t * m, step_t * steps, int count )
{
    int i = 0;
    while ( i < count )
    {
        int span = step_span( steps, i );
        if ( steps[i].kind == STEP_OBJECT )
        {
            if ( !object_measure( m, steps + i + 1, (int) steps[i].width ) )
            {
                return 0;
            }
        }
        else if ( !step_measure( m, &steps[i] ) )
        {
            return 0;
        }
        i += span;
    }
    return 1;
}

/* ---------------------------------------------------------------------------
   rendering a decoded value, and deciding whether it matches the corpus

   Every integer width, and the float, double and compressed_float bit
   patterns, are compared as 128-bit PATTERNS: the step's value is taken as its
   two's complement 128-bit form and the corpus expectation is parsed to the
   same form, so a hexadecimal expectation and its decimal twin are one
   expectation, and NOTHING here goes through a float. That last part is
   STANDARD.md's requirement rather than a convenience: NaN compares unequal to
   itself, -0.0 == 0.0, and no tolerance comparison can see a quieted signaling
   bit.

   The remaining kinds have textual spellings the corpus states directly.
   --------------------------------------------------------------------------- */

static void bytes_to_hex( const serialize_uint8_t * data, int count, char * out, int out_size )
{
    static const char * digits = "0123456789ABCDEF";
    int written = 0;
    int i;
    for ( i = 0; i < count && written + 4 < out_size; i++ )
    {
        if ( written > 0 )
        {
            out[written++] = ' ';
        }
        out[written++] = digits[( data[i] >> 4 ) & 0xF];
        out[written++] = digits[data[i] & 0xF];
    }
    out[written] = '\0';
}

/*
    STANDARD.md, "wstring": each 32-bit group carries one UTF-16 CODE UNIT, and
    a 4-byte wchar_t platform "converts at the boundary -- splits astral code
    points into surrogate pairs on write, recombines on read". The corpus states
    units, so a code point that came back recombined is split again here and
    both platforms compare the same text.
*/
static void units_to_hex( const wchar_t * units, char * out, int out_size )
{
    static const char * digits = "0123456789ABCDEF";
    int written = 0;
    int i;
    for ( i = 0; units[i] != 0; i++ )
    {
        unsigned int pair[2];
        int count = 1;
        int u;
        unsigned int code_point = (unsigned int) units[i];
        if ( code_point > 0xFFFFu )
        {
            unsigned int offset = code_point - 0x10000u;
            pair[0] = 0xD800u + ( offset >> 10 );
            pair[1] = 0xDC00u + ( offset & 0x3FFu );
            count = 2;
        }
        else
        {
            pair[0] = code_point;
            pair[1] = 0;
        }
        for ( u = 0; u < count; u++ )
        {
            if ( written + 6 >= out_size )
            {
                break;
            }
            if ( written > 0 )
            {
                out[written++] = ' ';
            }
            out[written++] = digits[( pair[u] >> 12 ) & 0xF];
            out[written++] = digits[( pair[u] >> 8 ) & 0xF];
            out[written++] = digits[( pair[u] >> 4 ) & 0xF];
            out[written++] = digits[pair[u] & 0xF];
        }
    }
    out[written] = '\0';
}

static int step_pattern( const step_t * step, serialize_uint128_t * out )
{
    if ( step_value_is_a_pattern( step->kind ) )
    {
        *out = step->pattern;
        return 1;
    }
    if ( step_value_is_a_number( step->kind ) )
    {
        *out = serialize_uint128_make( step->number.hi, step->number.lo );
        return 1;
    }
    return 0;
}

static void render_step_value( const step_t * step, char * out, int out_size )
{
    serialize_uint128_t pattern;

    if ( step_pattern( step, &pattern ) )
    {
        render_hex128( pattern, out );
        return;
    }

    switch ( step->kind )
    {
        case STEP_OBJECT:
        case STEP_ALIGN:
            /* neither has a value of its own; for align the corpus states the
               padding it consumed, which a conforming read always finds zero */
            strcpy( out, "0" );
            return;

        case STEP_BOOL:
            strcpy( out, step->boolean ? "true" : "false" );
            return;

        case STEP_BYTES:
            bytes_to_hex( step->buffer, (int) step->width, out, out_size );
            return;

        case STEP_STRING:
            bytes_to_hex( step->buffer, (int) strlen( (const char *) step->buffer ), out, out_size );
            return;

        case STEP_WSTRING:
            units_to_hex( step->wbuffer, out, out_size );
            return;

        default:
            strcpy( out, "?" );
            return;
    }
}

static int expectation_matches( const step_t * step, const char * expected )
{
    serialize_uint128_t pattern;
    char rendered[MAX_LINE];

    if ( step_pattern( step, &pattern ) )
    {
        serialize_int128_t wanted;
        if ( !parse_number( expected, &wanted ) )
        {
            return 0;
        }
        return serialize_uint128_equal( pattern, serialize_uint128_make( wanted.hi, wanted.lo ) );
    }

    render_step_value( step, rendered, MAX_LINE );

    return strcmp( rendered, expected ) == 0;
}

/* ---------------------------------------------------------------------------
   text helpers
   --------------------------------------------------------------------------- */

static char * trim( char * text )
{
    char * end;
    while ( *text == ' ' || *text == '\t' )
    {
        text++;
    }
    end = text + strlen( text );
    while ( end > text && ( end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n' ) )
    {
        end--;
    }
    *end = '\0';
    return text;
}

static void copy_text( char * out, int out_size, const char * in )
{
    int i;
    for ( i = 0; i < out_size - 1 && in[i] != '\0'; i++ )
    {
        out[i] = in[i];
    }
    out[i] = '\0';
}

/* splits an expect list into per step entries, on '|' */
static int split_expect( const char * text, char entries[MAX_STEPS][MAX_LINE] )
{
    int count = 0;
    const char * cursor = text;
    while ( count < MAX_STEPS )
    {
        const char * bar = strchr( cursor, '|' );
        char piece[MAX_LINE];
        int length;
        if ( bar != NULL )
        {
            length = (int) ( bar - cursor );
            if ( length > MAX_LINE - 1 ) length = MAX_LINE - 1;
            memcpy( piece, cursor, (size_t) length );
            piece[length] = '\0';
        }
        else
        {
            copy_text( piece, MAX_LINE, cursor );
        }
        copy_text( entries[count], MAX_LINE, trim( piece ) );
        count++;
        if ( bar == NULL )
        {
            break;
        }
        cursor = bar + 1;
    }
    return count;
}

/* ---------------------------------------------------------------------------
   parameters

   A parameter this runner does not understand is a FAILURE and not a silent
   default: a vector whose declaration is not the one being exercised proves
   nothing, and a corpus that grows a parameter must grow a runner to read it.
   --------------------------------------------------------------------------- */

static int operation_takes_param( const char * operation, const char * name )
{
    if ( strcmp( name, "step" ) == 0 )              return strcmp( operation, "sequence" ) == 0;
    if ( strcmp( name, "preceding_bits" ) == 0 )    return strcmp( operation, "align" ) == 0 || strcmp( operation, "bytes" ) == 0;
    if ( strcmp( name, "bits" ) == 0 )              return strcmp( operation, "bits" ) == 0;
    if ( strcmp( name, "count" ) == 0 )             return strcmp( operation, "bytes" ) == 0;
    if ( strcmp( name, "buffer_size" ) == 0 )       return strcmp( operation, "string" ) == 0 || strcmp( operation, "wstring" ) == 0;
    if ( strcmp( name, "previous" ) == 0 )          return strcmp( operation, "int_relative" ) == 0;
    if ( strcmp( name, "res" ) == 0 )               return strcmp( operation, "compressed_float" ) == 0;
    if ( strcmp( name, "integer_bits" ) == 0 || strcmp( name, "fraction_bits" ) == 0 )
    {
        return strcmp( operation, "fixed" ) == 0;
    }
    if ( strcmp( name, "min" ) == 0 || strcmp( name, "max" ) == 0 )
    {
        return strcmp( operation, "int" ) == 0 || strcmp( operation, "int64" ) == 0 || strcmp( operation, "int128" ) == 0
            || strcmp( operation, "fixed" ) == 0 || strcmp( operation, "compressed_float" ) == 0;
    }
    return 0;
}

static const char * param_text( const vector_t * v, const char * name )
{
    int i;
    for ( i = 0; i < v->param_count; i++ )
    {
        if ( strcmp( v->param_name[i], name ) == 0 )
        {
            return v->param_value[i];
        }
    }
    return NULL;
}

static int param_number( const vector_t * v, const char * name, serialize_int128_t * out )
{
    const char * text = param_text( v, name );
    if ( text == NULL )
    {
        return 0;
    }
    return parse_number( text, out );
}

static int param_int( const vector_t * v, const char * name, serialize_int64_t * out )
{
    serialize_int128_t value;
    if ( !param_number( v, name, &value ) )
    {
        return 0;
    }
    *out = (serialize_int64_t) value.lo;
    return 1;
}

static int word_float( const char * text, float * out )
{
    char * end = NULL;
    *out = (float) strtod( text, &end );
    return end != text && *end == '\0';
}

static int param_float( const vector_t * v, const char * name, float * out )
{
    const char * text = param_text( v, name );
    if ( text == NULL )
    {
        return 0;
    }
    return word_float( text, out );
}

/* ---------------------------------------------------------------------------
   building the steps
   --------------------------------------------------------------------------- */

static void step_clear( step_t * step )
{
    memset( step, 0, sizeof( *step ) );
}

static int int128_fits_int64( serialize_int128_t v )
{
    serialize_uint64_t sign = ( v.lo >> 63 ) != 0 ? ~(serialize_uint64_t) 0 : 0;
    return v.hi == sign;
}

/*
    The fixed point declaration. In C every parameter is a runtime argument, so
    there is no table of instantiations to keep: what a runner must still refuse
    is a declaration this library has no entry point for. The storage widths
    here are 32, 64 and 128 bits, and a declaration narrower than 32 rides the
    32-bit entry point -- the raw arithmetic and the wire are identical, because
    the offset encoding depends on the raw bounds and not on the storage type.
*/
static int fixed_declaration( step_t * step, serialize_int64_t integer_bits, serialize_int64_t fraction_bits,
                              serialize_int128_t min, serialize_int128_t max )
{
    serialize_int64_t total = integer_bits + fraction_bits;

    if ( integer_bits < 1 || fraction_bits < 0 || total > 128 )
    {
        return 0;
    }
    if ( !int128_fits_int64( min ) || !int128_fits_int64( max ) )
    {
        return 0;
    }

    step->integer_bits = (int) integer_bits;
    step->fraction_bits = (int) fraction_bits;
    step->min = min;
    step->max = max;
    step->fixed_storage = total <= 32 ? 32 : ( total <= 64 ? 64 : 128 );

    return 1;
}

/* a step spelled inside a sequence record */
static int step_from_words( step_t * step, const char * text )
{
    char work[MAX_TEXT];
    char * words[8];
    int word_count = 0;
    char * cursor;
    serialize_int128_t a, b, c, d;

    step_clear( step );

    copy_text( work, MAX_TEXT, text );

    cursor = work;
    while ( *cursor != '\0' && word_count < 8 )
    {
        while ( *cursor == ' ' || *cursor == '\t' ) cursor++;
        if ( *cursor == '\0' ) break;
        words[word_count++] = cursor;
        while ( *cursor != '\0' && *cursor != ' ' && *cursor != '\t' ) cursor++;
        if ( *cursor != '\0' ) *cursor++ = '\0';
    }
    if ( word_count == 0 )
    {
        return 0;
    }

    if ( strcmp( words[0], "bits" ) == 0 && word_count == 2 && parse_number( words[1], &a ) )
    {
        step->kind = STEP_BITS;
        step->width = (serialize_int64_t) a.lo;
        return 1;
    }
    if ( strcmp( words[0], "bool" ) == 0 && word_count == 1 )
    {
        step->kind = STEP_BOOL;
        return 1;
    }
    if ( strcmp( words[0], "align" ) == 0 && word_count == 1 )
    {
        step->kind = STEP_ALIGN;
        return 1;
    }
    if ( strcmp( words[0], "float" ) == 0 && word_count == 1 )
    {
        step->kind = STEP_FLOAT;
        return 1;
    }
    if ( strcmp( words[0], "double" ) == 0 && word_count == 1 )
    {
        step->kind = STEP_DOUBLE;
        return 1;
    }
    if ( strcmp( words[0], "uint128" ) == 0 && word_count == 1 )
    {
        step->kind = STEP_UINT128;
        return 1;
    }
    if ( strcmp( words[0], "int_relative" ) == 0 && word_count == 2 && parse_number( words[1], &a ) )
    {
        step->kind = STEP_INT_RELATIVE;
        step->previous = (serialize_int32_t) a.lo;
        return 1;
    }
    if ( strcmp( words[0], "compressed_float" ) == 0 && word_count == 4 )
    {
        if ( !word_float( words[1], &step->fmin ) ) return 0;
        if ( !word_float( words[2], &step->fmax ) ) return 0;
        if ( !word_float( words[3], &step->fres ) ) return 0;
        step->kind = STEP_COMPRESSED_FLOAT;
        return 1;
    }
    if ( strcmp( words[0], "object" ) == 0 && word_count == 2 && parse_number( words[1], &a ) )
    {
        step->kind = STEP_OBJECT;
        step->width = (serialize_int64_t) a.lo;
        return 1;
    }
    if ( strcmp( words[0], "bytes" ) == 0 && word_count == 2 && parse_number( words[1], &a ) )
    {
        step->kind = STEP_BYTES;
        step->width = (serialize_int64_t) a.lo;
        return step->width >= 0 && step->width <= MAX_BYTES;
    }
    if ( strcmp( words[0], "string" ) == 0 && word_count == 2 && parse_number( words[1], &a ) )
    {
        step->kind = STEP_STRING;
        step->width = (serialize_int64_t) a.lo;
        return step->width > 0 && step->width <= MAX_BYTES;
    }
    if ( strcmp( words[0], "wstring" ) == 0 && word_count == 2 && parse_number( words[1], &a ) )
    {
        step->kind = STEP_WSTRING;
        step->width = (serialize_int64_t) a.lo;
        return step->width > 0 && step->width <= MAX_BYTES;
    }
    if ( ( strcmp( words[0], "int" ) == 0 || strcmp( words[0], "int64" ) == 0 || strcmp( words[0], "int128" ) == 0 )
         && word_count == 3 && parse_number( words[1], &a ) && parse_number( words[2], &b ) )
    {
        step->kind = strcmp( words[0], "int" ) == 0 ? STEP_INT
                   : ( strcmp( words[0], "int64" ) == 0 ? STEP_INT64 : STEP_INT128 );
        step->min = a;
        step->max = b;
        return 1;
    }
    if ( strcmp( words[0], "fixed" ) == 0 && word_count == 5
         && parse_number( words[1], &a ) && parse_number( words[2], &b )
         && parse_number( words[3], &c ) && parse_number( words[4], &d ) )
    {
        step->kind = STEP_FIXED;
        return fixed_declaration( step, (serialize_int64_t) a.lo, (serialize_int64_t) b.lo, c, d );
    }

    return 0;
}

/*
    Builds the step list for a vector. A single operation vector becomes a one
    or two step sequence: the operations whose interesting behavior only exists
    at a non-zero bit index take a `preceding_bits` parameter, which becomes a
    leading bits step.
*/
static int build_steps( const vector_t * v, step_t * steps, int * out_count )
{
    int count = 0;
    step_t * step;
    serialize_int64_t preceding_bits = 0;
    serialize_int64_t width = 0;
    serialize_int64_t integer_bits = 0;
    serialize_int64_t fraction_bits = 0;
    serialize_int128_t min, max;

    *out_count = 0;

    if ( strcmp( v->operation, "sequence" ) == 0 )
    {
        int i;
        for ( i = 0; i < v->step_count; i++ )
        {
            if ( count >= MAX_STEPS )
            {
                return 0;
            }
            if ( !step_from_words( &steps[count], v->step_text[i] ) )
            {
                return 0;
            }
            count++;
        }
        *out_count = count;
        return count > 0;
    }

    if ( param_int( v, "preceding_bits", &preceding_bits ) && preceding_bits > 0 )
    {
        step_clear( &steps[count] );
        steps[count].kind = STEP_BITS;
        steps[count].width = preceding_bits;
        count++;
    }

    step = &steps[count];
    step_clear( step );

    if ( strcmp( v->operation, "bits" ) == 0 )
    {
        if ( !param_int( v, "bits", &width ) || width < 1 || width > 64 ) return 0;
        step->kind = STEP_BITS;
        step->width = width;
    }
    else if ( strcmp( v->operation, "bool" ) == 0 )
    {
        step->kind = STEP_BOOL;
    }
    else if ( strcmp( v->operation, "uint128" ) == 0 )
    {
        step->kind = STEP_UINT128;
    }
    else if ( strcmp( v->operation, "align" ) == 0 )
    {
        step->kind = STEP_ALIGN;
    }
    else if ( strcmp( v->operation, "int" ) == 0 )
    {
        if ( !param_number( v, "min", &min ) || !param_number( v, "max", &max ) ) return 0;
        step->kind = STEP_INT;
        step->min = min;
        step->max = max;
    }
    else if ( strcmp( v->operation, "int64" ) == 0 )
    {
        if ( !param_number( v, "min", &min ) || !param_number( v, "max", &max ) ) return 0;
        step->kind = STEP_INT64;
        step->min = min;
        step->max = max;
    }
    else if ( strcmp( v->operation, "int128" ) == 0 )
    {
        if ( !param_number( v, "min", &min ) || !param_number( v, "max", &max ) ) return 0;
        step->kind = STEP_INT128;
        step->min = min;
        step->max = max;
    }
    else if ( strcmp( v->operation, "int_relative" ) == 0 )
    {
        serialize_int64_t previous = 0;
        if ( !param_int( v, "previous", &previous ) ) return 0;
        step->kind = STEP_INT_RELATIVE;
        step->previous = (serialize_int32_t) previous;
    }
    else if ( strcmp( v->operation, "float" ) == 0 )
    {
        step->kind = STEP_FLOAT;
    }
    else if ( strcmp( v->operation, "double" ) == 0 )
    {
        step->kind = STEP_DOUBLE;
    }
    else if ( strcmp( v->operation, "compressed_float" ) == 0 )
    {
        if ( !param_float( v, "min", &step->fmin ) || !param_float( v, "max", &step->fmax )
             || !param_float( v, "res", &step->fres ) ) return 0;
        step->kind = STEP_COMPRESSED_FLOAT;
    }
    else if ( strcmp( v->operation, "bytes" ) == 0 )
    {
        if ( !param_int( v, "count", &width ) || width < 0 || width > MAX_BYTES ) return 0;
        step->kind = STEP_BYTES;
        step->width = width;
    }
    else if ( strcmp( v->operation, "string" ) == 0 )
    {
        if ( !param_int( v, "buffer_size", &width ) || width < 1 || width > MAX_BYTES ) return 0;
        step->kind = STEP_STRING;
        step->width = width;
    }
    else if ( strcmp( v->operation, "wstring" ) == 0 )
    {
        if ( !param_int( v, "buffer_size", &width ) || width < 1 || width > MAX_BYTES ) return 0;
        step->kind = STEP_WSTRING;
        step->width = width;
    }
    else if ( strcmp( v->operation, "fixed" ) == 0 )
    {
        if ( !param_int( v, "integer_bits", &integer_bits ) || !param_int( v, "fraction_bits", &fraction_bits ) ) return 0;
        if ( !param_number( v, "min", &min ) || !param_number( v, "max", &max ) ) return 0;
        step->kind = STEP_FIXED;
        if ( !fixed_declaration( step, integer_bits, fraction_bits, min, max ) ) return 0;
    }
    else
    {
        return 0;
    }

    count++;
    *out_count = count;

    return 1;
}

/* ---------------------------------------------------------------------------
   running one vector
   --------------------------------------------------------------------------- */

/*
    Failure is terminal (STANDARD.md, Reader Obligations), and a refused vector
    is where that rule is testable. Checked by behavior rather than by an
    accessor: a further read must fail and must leave its destination alone.
    See the note at the top of this file on why this port's bit cursor is not
    the third clause the reference runner uses.
*/
static void fail_unless_stream_is_terminal( const vector_t * v, serialize_read_stream_t * r )
{
    serialize_uint32_t after = 0xFFFFFFFFu;

    if ( serialize_read_bits( r, &after, 8 ) )
    {
        fail( v, "the stream accepted a read after the refusal: failure is not terminal" );
        return;
    }
    if ( after != 0xFFFFFFFFu )
    {
        fail( v, "the read after the refusal wrote to its destination" );
        return;
    }
    if ( !serialize_read_error( r ) )
    {
        fail( v, "the stream is not in its failed state after a refusal" );
    }
}

static void run_reader( const vector_t * v, step_t * steps, int step_count )
{
    serialize_uint8_t buffer[MAX_BYTES + MAX_SLACK];
    serialize_read_stream_t r;
    char entries[MAX_STEPS][MAX_LINE];
    int entry_count;
    int offset;
    int stopped_at = -1;
    int accepted;
    int i;

    /* the sentinels must survive the narrowing this runner performs on the way
       to each operation's own width -- 32 bits for float and for the ranged
       int -- or a destination the library correctly left alone still reads as
       written */
    const serialize_uint64_t sentinel_pattern = (serialize_uint64_t) 0xCAFEF00DUL;
    const serialize_int128_t sentinel_number = serialize_int128_from_int64( -1234567 );

    memset( buffer, SLACK_FILL, sizeof( buffer ) );
    memcpy( buffer, v->bytes, (size_t) v->byte_count );
    serialize_read_stream_init( &r, buffer, v->byte_count );

    for ( i = 0; i < step_count; i++ )
    {
        steps[i].pattern = serialize_uint128_make( 0, sentinel_pattern );
        steps[i].number = sentinel_number;
        steps[i].boolean = 1;               /* a refused bool read must leave this alone */
    }

    failed_step = NULL;
    accepted = steps_read( &r, steps, step_count, &stopped_at );

    if ( v->refused )
    {
        if ( accepted )
        {
            fail( v, "the read succeeded, the corpus requires refusal" );
            return;
        }

        /* STANDARD.md, "A refused primitive read must leave its destination
           unwritten". The rule reaches the scalars only: a read into a
           caller-owned buffer -- bytes, string and wstring -- leaves that
           buffer's contents unspecified after a refusal, and the document says
           so in as many words, so those kinds are not checked here. */
        if ( failed_step != NULL )
        {
            const step_t * step = failed_step;
            if ( ( step_value_is_a_pattern( step->kind )
                   && !serialize_uint128_equal( step->pattern, serialize_uint128_make( 0, sentinel_pattern ) ) )
              || ( step_value_is_a_number( step->kind )
                   && !serialize_int128_equal( step->number, sentinel_number ) )
              || ( step->kind == STEP_BOOL && step->boolean != 1 ) )
            {
                fail( v, "the refused read wrote to the destination" );
                return;
            }
        }

        /* Failure is terminal, and a sequence states its own successors: every
           step after the failing one must fail too, however many readable bits
           the stream still holds. The vectors are built so a reader without the
           latch passes the successor, and one of them makes the successor a
           DEGENERATE RANGE -- a read that consumes no bits and would otherwise
           always succeed. */
        if ( stopped_at >= 0 )
        {
            for ( i = stopped_at + step_span( steps, stopped_at ); i < step_count; i += step_span( steps, i ) )
            {
                if ( steps_read( &r, steps + i, step_span( steps, i ), NULL ) )
                {
                    char message[MAX_LINE];
                    sprintf( message, "step %d succeeded after step %d was refused; failure must be terminal",
                             i + 1, stopped_at + 1 );
                    fail( v, message );
                    return;
                }
            }
        }

        /* and the same rule against a read the vector does not name, so every
           refused vector carries the terminality check and not only the
           sequences that spell a successor */
        fail_unless_stream_is_terminal( v, &r );
        return;
    }

    if ( !accepted )
    {
        fail( v, "the read was refused, the corpus requires it to be accepted" );
        return;
    }

    entry_count = split_expect( v->expect, entries );

    /* one expect entry per step, objects and aligns included, which state `-`.
       A leading preceding_bits step carries no expectation of its own: it exists
       to place the stream, and the record states only the operation under test,
       so the entry list aligns to the END of the step list. */
    offset = step_count - entry_count;
    if ( offset < 0 )
    {
        fail( v, "the expect list states more values than the vector has steps" );
        return;
    }

    for ( i = 0; i < entry_count; i++ )
    {
        if ( strcmp( entries[i], "-" ) == 0 )
        {
            continue;
        }
        if ( !expectation_matches( &steps[offset + i], entries[i] ) )
        {
            char rendered[MAX_LINE];
            char message[MAX_LINE * 3];
            render_step_value( &steps[offset + i], rendered, MAX_LINE );
            sprintf( message, "step %d decoded %s, the corpus states %s", offset + i + 1, rendered, entries[i] );
            fail( v, message );
            return;
        }
    }

    if ( v->have_consumed && serialize_read_bits_processed( &r ) != v->consumed )
    {
        char message[MAX_LINE];
        sprintf( message, "consumed %ld bits, the corpus states %ld",
                 (long) serialize_read_bits_processed( &r ), (long) v->consumed );
        fail( v, message );
    }
}

/*
    The writer leg. A vector marked `writer = canonical` states the bytes a
    conforming writer emits for its value, so the runner writes the decoded
    steps back and compares. The comparison covers the whole stream, which is
    what pins the trailing-bits obligation: the unused bits of the final byte
    must be zero, and a writer leaking anything into them produces a byte the
    vector does not carry.
*/
static void run_writer( const vector_t * v, step_t * steps, int step_count )
{
    serialize_uint8_t scratch[MAX_BYTES + 64];
    serialize_write_stream_t w;
    int capacity = (int) sizeof( scratch ) & ~7;    /* the packer stores qwords */
    int written;

    writer_checks++;

    memset( scratch, SLACK_FILL, sizeof( scratch ) );
    serialize_write_stream_init( &w, scratch, capacity );

    if ( !steps_write( &w, steps, step_count ) )
    {
        fail( v, "the writer refused a canonical vector" );
        return;
    }
    serialize_write_flush( &w );

    written = serialize_write_bytes_processed( &w );
    if ( written != v->byte_count )
    {
        char message[MAX_LINE];
        sprintf( message, "the writer emitted %d bytes, the corpus states %d", written, v->byte_count );
        fail( v, message );
        return;
    }
    if ( written > 0 && memcmp( scratch, v->bytes, (size_t) written ) != 0 )
    {
        char got[MAX_LINE];
        char want[MAX_LINE];
        char message[MAX_LINE * 3];
        bytes_to_hex( scratch, written, got, MAX_LINE );
        bytes_to_hex( v->bytes, v->byte_count, want, MAX_LINE );
        sprintf( message, "the writer emitted %s, the corpus states %s", got, want );
        fail( v, message );
    }
}

/*
    The measure leg. STANDARD.md makes a measure a BOUND and not the packet
    size -- "it need not be exact, and cannot be" -- so the corpus states a
    floor and the check is an inequality. A measure that computes alignment from
    a running bit index starting at zero under-counts every unaligned start and
    falls below the floor, which is the non-conforming accounting the document
    names.
*/
static void run_measure( const vector_t * v, step_t * steps, int step_count )
{
    serialize_measure_stream_t m;

    measure_checks++;

    serialize_measure_stream_init( &m );

    if ( !steps_measure( &m, steps, step_count ) )
    {
        fail( v, "the measure refused a step; a measure refuses nothing at runtime" );
        return;
    }

    if ( (serialize_int64_t) serialize_measure_bits_processed( &m ) < v->measure_at_least )
    {
        char message[MAX_LINE];
        sprintf( message, "measured %d bits, the corpus requires at least %ld",
                 serialize_measure_bits_processed( &m ), (long) v->measure_at_least );
        fail( v, message );
    }
}

static void run_vector( const vector_t * v )
{
    step_t steps[MAX_STEPS];
    int step_count = 0;
    int failures_before;
    int i;

    vectors_run++;

    for ( i = 0; i < v->param_count; i++ )
    {
        if ( !operation_takes_param( v->operation, v->param_name[i] ) )
        {
            char message[MAX_LINE];
            sprintf( message, "no runner for parameter '%s' on operation '%s'", v->param_name[i], v->operation );
            fail( v, message );
            return;
        }
    }
    if ( v->step_count > 0 && strcmp( v->operation, "sequence" ) != 0 )
    {
        fail( v, "steps are only meaningful on a sequence" );
        return;
    }

    if ( !build_steps( v, steps, &step_count ) )
    {
        /* a corpus file this runner does not know how to drive is a gap in
           this library's conformance, not a vector to skip past */
        fail( v, "no runner for this operation, for one of its parameters, or for its fixed point declaration" );
        return;
    }

    failures_before = failures;
    run_reader( v, steps, step_count );

    /* the writer and the measure are handed the values the reader decoded, so
       running them after a reader failure reports a second failure about a
       value that was never decoded. One vector, one diagnosis. */
    if ( !v->refused && failures == failures_before )
    {
        if ( v->writer_canonical )
        {
            run_writer( v, steps, step_count );
        }
        if ( v->have_measure )
        {
            run_measure( v, steps, step_count );
        }
    }
}

/* ---------------------------------------------------------------------------
   the vector format (STANDARD.md, "The vector format")
   --------------------------------------------------------------------------- */

static int parse_bytes( vector_t * v, const char * text )
{
    v->byte_count = 0;
    while ( *text != '\0' )
    {
        int high, low;
        while ( *text == ' ' || *text == '\t' )
        {
            text++;
        }
        if ( *text == '\0' )
        {
            break;
        }
        high = hex_digit( text[0] );
        low = text[1] == '\0' ? -1 : hex_digit( text[1] );
        if ( high < 0 || low < 0 || v->byte_count >= MAX_BYTES )
        {
            return 0;
        }
        v->bytes[v->byte_count++] = (serialize_uint8_t) ( high * 16 + low );
        text += 2;
    }
    return 1;
}

static void vector_reset( vector_t * v, const char * file )
{
    memset( v, 0, sizeof( *v ) );
    v->file = file;
}

static int vector_empty( const vector_t * v )
{
    return v->operation[0] == '\0';
}

static void bad_line( const char * path, int line_number, const char * what )
{
    printf( "FAILED %s:%d: %s\n", path, line_number, what );
    failures++;
}

static void run_file( const char * path )
{
    FILE * file = fopen( path, "r" );
    char line[MAX_LINE];
    vector_t v;
    int line_number = 0;

    if ( file == NULL )
    {
        printf( "FAILED: cannot open %s\n", path );
        failures++;
        return;
    }

    vector_reset( &v, path );

    while ( fgets( line, (int) sizeof( line ), file ) != NULL )
    {
        char * text;
        char * space;
        const char * key;
        char * value;

        line_number++;

        /* a comment begins at the START of a line and nowhere else
           (STANDARD.md, the vector format's lexical rules) */
        if ( line[0] == '#' )
        {
            continue;
        }

        text = trim( line );
        if ( *text == '\0' )
        {
            if ( !vector_empty( &v ) )
            {
                run_vector( &v );
                vector_reset( &v, path );
            }
            continue;
        }

        space = strchr( text, ' ' );
        key = text;
        value = text + strlen( text );          /* an empty value, for a bare key */
        if ( space != NULL )
        {
            *space = '\0';
            value = trim( space + 1 );
        }

        if ( strcmp( key, "operation" ) == 0 )
        {
            copy_text( v.operation, MAX_TEXT, value );
        }
        else if ( strcmp( key, "name" ) == 0 )
        {
            copy_text( v.name, MAX_TEXT, value );
        }
        else if ( strcmp( key, "param" ) == 0 )
        {
            char * equals = strchr( value, '=' );
            const char * param_name;
            const char * param_value;
            if ( equals == NULL )
            {
                bad_line( path, line_number, "malformed param line" );
                continue;
            }
            *equals = '\0';
            param_name = trim( value );
            param_value = trim( equals + 1 );
            if ( strcmp( param_name, "step" ) == 0 )
            {
                if ( v.step_count >= MAX_STEPS )
                {
                    bad_line( path, line_number, "too many steps" );
                    continue;
                }
                copy_text( v.step_text[v.step_count], MAX_TEXT, param_value );
                v.step_count++;
            }
            else if ( v.param_count < MAX_PARAMS )
            {
                copy_text( v.param_name[v.param_count], MAX_TEXT, param_name );
                copy_text( v.param_value[v.param_count], MAX_TEXT, param_value );
                v.param_count++;
            }
            else
            {
                bad_line( path, line_number, "too many parameters" );
            }
        }
        else if ( strcmp( key, "bytes" ) == 0 )
        {
            if ( !parse_bytes( &v, value ) )
            {
                bad_line( path, line_number, "malformed bytes line" );
            }
        }
        else if ( strcmp( key, "expect" ) == 0 )
        {
            if ( strcmp( value, "refused" ) == 0 )
            {
                v.refused = 1;
            }
            else
            {
                char * equals = strchr( value, '=' );
                const char * kind;
                if ( equals == NULL )
                {
                    bad_line( path, line_number, "malformed expect line" );
                    continue;
                }
                *equals = '\0';
                kind = trim( value );
                /* `value` and `bits` differ only in how the corpus spells the
                   expectation: both are compared as 128-bit patterns, which is
                   what makes a hexadecimal expectation and its decimal twin one
                   expectation */
                if ( strcmp( kind, "value" ) != 0 && strcmp( kind, "bits" ) != 0 )
                {
                    bad_line( path, line_number, "unknown expect kind" );
                    continue;
                }
                copy_text( v.expect, MAX_LINE, trim( equals + 1 ) );
            }
        }
        else if ( strcmp( key, "consumed" ) == 0 )
        {
            v.consumed = (serialize_int64_t) strtol( value, NULL, 10 );
            v.have_consumed = 1;
        }
        else if ( strcmp( key, "measure_at_least" ) == 0 )
        {
            v.measure_at_least = (serialize_int64_t) strtol( value, NULL, 10 );
            v.have_measure = 1;
        }
        else if ( strcmp( key, "writer" ) == 0 )
        {
            if ( strcmp( value, "canonical" ) != 0 )
            {
                bad_line( path, line_number, "unknown writer mode" );
                continue;
            }
            v.writer_canonical = 1;
        }
        else
        {
            /* an unknown key is a format the corpus has grown and this parser
               has not; skipping it would silently drop what it constrains */
            bad_line( path, line_number, "unknown key" );
        }
    }

    if ( !vector_empty( &v ) )
    {
        run_vector( &v );
    }

    fclose( file );
}

/* ---------------------------------------------------------------------------
   the corpus directory
   --------------------------------------------------------------------------- */

static int is_vector_file( const char * name )
{
    size_t n = strlen( name );
    return n > 4 && strcmp( name + n - 4, ".txt" ) == 0;
}

static int run_directory( const char * directory )
{
    char path[MAX_LINE];
    int files = 0;

#ifdef _WIN32
    WIN32_FIND_DATAA found;
    HANDLE handle;
    sprintf( path, "%s\\*.txt", directory );
    handle = FindFirstFileA( path, &found );
    if ( handle == INVALID_HANDLE_VALUE )
    {
        printf( "FAILED: no vector files in %s\n", directory );
        failures++;
        return 0;
    }
    do
    {
        if ( is_vector_file( found.cFileName ) )
        {
            sprintf( path, "%s\\%s", directory, found.cFileName );
            run_file( path );
            files++;
        }
    }
    while ( FindNextFileA( handle, &found ) );
    FindClose( handle );
#else
    DIR * dir = opendir( directory );
    struct dirent * entry;
    if ( dir == NULL )
    {
        printf( "FAILED: cannot open %s\n", directory );
        failures++;
        return 0;
    }
    while ( ( entry = readdir( dir ) ) != NULL )
    {
        if ( is_vector_file( entry->d_name ) )
        {
            sprintf( path, "%s/%s", directory, entry->d_name );
            run_file( path );
            files++;
        }
    }
    closedir( dir );
#endif

    if ( files == 0 )
    {
        printf( "FAILED: no vector files in %s\n", directory );
        failures++;
        return 0;
    }

    return files;
}

int main( int argc, char ** argv )
{
    const char * directory = argc > 1 ? argv[1] : "conformance";
    int files = run_directory( directory );

    if ( failures > 0 || vectors_run == 0 )
    {
        /* the corpus is the conformance instrument: an empty run is a failure,
           and a disagreement is this implementation's bug */
        printf( "FAILED (%d vectors from %d files, %d failure(s))\n", vectors_run, files, failures );
        return 1;
    }

    if ( serialize_test_verbose() )
    {
        printf( "conformance: %d vectors from %d files in %s, %d writer checks, %d measure checks\n",
                vectors_run, files, directory, writer_checks, measure_checks );
    }

    printf( "OK\n" );

    return 0;
}
