/*
    Proves the caller contracts FIRE, not just that they are written down.

    The write path — and the measure path, which carries the same contracts,
    and the caller-owned parameters of the read path — perform no
    release-build checks at all (issue #52, the ruling: "C should match C++
    and have no checks on write at all (except assert). ... C and C++ should
    be equivalent."), which makes the debug asserts the entire enforcement of
    every caller contract — and an assert nothing ever proves to fire is a
    contract nothing enforces. Each case here forks, commits one deliberate
    contract violation in the child with stderr silenced, and requires the
    child to die by SIGABRT.

    What is NOT here, deliberately: the read path's judgement of the NETWORK.
    Range refusals, buffer-end refusals, malformed align padding, the string
    content refusals, the non-finite float refusals and the end's refusals
    are real checks in every build mode, and test/roundtrip.c proves those as
    ordinary failing reads, with their reasons.

    POSIX only (fork), which is fine: the Makefile's NOT_ON_MSVC keeps this
    file off CI's Windows leg. Under NDEBUG every assert compiles to
    nothing, so this test skips itself — the violations here would be
    undefined behavior, deliberately.

    The compressed float cases pin serialize fork #6 (the ruling: "it's
    non-conforming. also, attempting to send NaN or INF or anything else
    through compressed float is non-conforming and should assert out on
    write too"): a non-finite VALUE asserts at intake, and a DECLARATION
    whose span or step count does not compute finite asserts at the site
    the parameters are computed. Format 2.0 adds the declaration whose TOP
    decodes to an infinity, which asserts on write whatever the value, and
    the plain float and double writers, which are handed finite values only.
*/

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include <stdio.h>

#ifdef NDEBUG

#include "verbose.h"

int main( void )
{
    /* no asserts to prove under NDEBUG: the write and measure paths check
       nothing at all, which is the contract — see roundtrip.c's release-only
       NaN block for what remains observable there */
    if ( serialize_test_verbose() )
    {
        printf( "assertdeath: skipped (NDEBUG: the caller contracts have no checks to fire)\n" );
    }
    return 0;
}

#else

#include <float.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include "../serialize.h"

static int failed = 0;

/* one contract violation per function, each on a fresh stream */

static void violate_capacity( void )
{
    serialize_uint8_t buffer[8];                        /* 64 bits of room */
    serialize_write_stream_t w;
    serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
    serialize_write_bits( &w, 0, 32 );
    serialize_write_bits( &w, 0xFFFFFF, 24 );
    serialize_write_bits( &w, 0, 16 );                  /* 8 bits past the end */
}

/* the buffer size must be a multiple of 8: the writer commits whole 8-byte
   words, so a 100-byte buffer written to exactly its 800 bits -- inside
   capacity, every other assert satisfied -- would have its last commit store
   8 bytes at offset 96, four past the end. The C++ BitWriter asserts this at
   construction and so does init. The buffer is 104 bytes so that, should the
   assert ever go missing, the child writes inside its own memory and exits
   cleanly, which must_abort reports as the failure it is. */
static void violate_write_buffer_multiple_of_8( void )
{
    serialize_uint8_t buffer[104];
    serialize_write_stream_t w;
    int i;
    serialize_write_stream_init( &w, buffer, 100 );     /* 100 is not a multiple of 8 */
    for ( i = 0; i < 25; i++ )
    {
        serialize_write_bits( &w, 0, 32 );              /* exactly 800 bits: within capacity */
    }
    serialize_write_flush( &w );
}

static void violate_int_range( void )
{
    serialize_uint8_t buffer[8];
    serialize_write_stream_t w;
    serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
    serialize_write_int( &w, 11, 0, 10 );               /* outside its declared range */
}

static void violate_int_relative_order( void )
{
    serialize_uint8_t buffer[8];
    serialize_write_stream_t w;
    serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
    serialize_write_int_relative( &w, 100, 100 );       /* STRICTLY increasing, violated */
}

/* the domain is 0 to 2^31 - 1 and previous lives in it (STANDARD.md,
   int_relative) — the write, measure and read halves all assert it */

static void violate_int_relative_previous_domain( void )
{
    serialize_uint8_t buffer[8];
    serialize_write_stream_t w;
    serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
    serialize_write_int_relative( &w, -1, 1 );          /* previous outside the domain */
}

static void violate_string_length( void )
{
    serialize_uint8_t buffer[64];
    serialize_write_stream_t w;
    serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
    serialize_write_string( &w, "does not fit", 4 );    /* longer than its declared buffer */
}

static void violate_bits_width( void )
{
    serialize_uint8_t buffer[8];
    serialize_write_stream_t w;
    serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
    serialize_write_bits( &w, 5, 2 );                   /* 5 does not fit 2 bits. The release
                                                           writer trusts the value — no mask —
                                                           so this assert is the whole guard */
}

/* the measure stream carries the writer's contracts, as asserts, exactly
   like the write half (issue #52) — each must fire the same way */

static void violate_measure_string_length( void )
{
    serialize_measure_stream_t m;
    serialize_measure_stream_init( &m );
    serialize_measure_string( &m, "too long for its buffer", 8 );
}

/* format 2.0: float and double carry finite values only, and a writer is
   handed nothing else. Built from bit patterns so no NAN or INFINITY macro
   is needed at the C89 floor. */
static void write_float_bits( serialize_uint32_t bits )
{
    serialize_uint8_t buffer[8];
    serialize_write_stream_t w;
    float value;
    memcpy( &value, &bits, 4 );
    serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
    serialize_write_float( &w, value );
}

static void write_double_bits( serialize_uint64_t bits )
{
    serialize_uint8_t buffer[8];
    serialize_write_stream_t w;
    double value;
    memcpy( &value, &bits, 8 );
    serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
    serialize_write_double( &w, value );
}

static void violate_write_float_nan( void )        { write_float_bits( 0x7FC00000UL ); }
static void violate_write_float_signaling_nan( void ) { write_float_bits( 0x7F800001UL ); }
static void violate_write_float_infinity( void )   { write_float_bits( 0xFF800000UL ); }
static void violate_write_double_nan( void )       { write_double_bits( ( (serialize_uint64_t) 0x7FF80000UL ) << 32 ); }
static void violate_write_double_infinity( void )  { write_double_bits( ( (serialize_uint64_t) 0x7FF00000UL ) << 32 ); }
static void violate_write_double_low_payload( void ) { write_double_bits( ( ( (serialize_uint64_t) 0x7FF00000UL ) << 32 ) | 1 ); }

/* a declaration whose top decodes to an infinity -- [3 * 2^103, FLT_MAX] at
   resolution 2^104 -- is non-conforming as a whole, so a write through it
   asserts even for the declaration's own minimum, which decodes finite */
static void violate_compressed_float_infinite_top( void )
{
    serialize_uint8_t buffer[8];
    serialize_write_stream_t w;
    const float min = 30423614405477505635920876929024.0f;
    serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
    serialize_write_compressed_float( &w, min, min, FLT_MAX, 20282409603651670423947251286016.0f );
}

static void violate_measure_int_relative_order( void )
{
    serialize_measure_stream_t m;
    serialize_measure_stream_init( &m );
    serialize_measure_int_relative( &m, 100, 100 );     /* STRICTLY increasing, violated */
}

static void violate_measure_int_relative_previous_domain( void )
{
    serialize_measure_stream_t m;
    serialize_measure_stream_init( &m );
    serialize_measure_int_relative( &m, -1, 1 );        /* previous outside the domain */
}

static void violate_measure_int128_bounds( void )
{
    serialize_measure_stream_t m;
    serialize_measure_stream_init( &m );
    serialize_measure_int128( &m,
                              serialize_int128_from_int64( 1 ),
                              serialize_int128_from_int64( -1 ) );  /* min > max */
}

/* the read path's caller-owned parameters that remain contracts: the
   buffer pointer and the padded destination, asserted exactly as the C++
   BitReader and ReadStream::InitializePadded assert them. A bad DECLARATION
   on read -- bounds the wrong way round, a previous outside the int_relative
   domain, a bit count, a fixed point or precomputed declaration, a string
   buffer below 1 -- is no longer one of them: a reader never traps
   (IMPLEMENTING.md U7, U16), and roundtrip.c's test_uniform_read_rules proves
   each refuses as value_out_of_range in both build modes. */

static void violate_read_null_buffer( void )
{
    serialize_read_stream_t r;
    serialize_read_stream_init( &r, NULL, 8 );          /* the C++ BitReader asserts data; init asserts the same */
}

static void violate_read_padded_destination( void )
{
    serialize_uint8_t payload[16];
    serialize_uint8_t destination[16];                  /* room for the payload, none for the slack */
    serialize_read_stream_t r;
    memset( payload, 0, sizeof( payload ) );
    /* the destination must hold bytes + 8, and the size parameter is what
       lets this one be checked at all */
    serialize_read_stream_init_padded( &r, destination, (int) sizeof( destination ), payload, (int) sizeof( payload ) );
}

/* IMPLEMENTING.md U6: a measure makes every check its write makes, except
   room and buffer size, by the same trap. Each case below is a declaration
   its write half asserts. */

static void violate_measure_int_bounds( void )
{
    serialize_measure_stream_t m;
    serialize_measure_stream_init( &m );
    serialize_measure_int( &m, 3, 1 );                  /* min above max */
}

static void violate_measure_int64_bounds( void )
{
    serialize_measure_stream_t m;
    serialize_measure_stream_init( &m );
    serialize_measure_int64( &m, 3, 1 );                /* min above max */
}

static void violate_measure_bits_zero( void )
{
    serialize_measure_stream_t m;
    serialize_measure_stream_init( &m );
    serialize_measure_bits( &m, 0 );                    /* bits is in [1,32] */
}

static void violate_measure_bits_wide( void )
{
    serialize_measure_stream_t m;
    serialize_measure_stream_init( &m );
    serialize_measure_bits( &m, 40 );                   /* bits is in [1,32]; bits64 is the wide form */
}

static void violate_measure_bytes_negative( void )
{
    serialize_measure_stream_t m;
    serialize_measure_stream_init( &m );
    serialize_measure_bytes( &m, -1 );
}

static void violate_measure_compressed_float_infinite_top( void )
{
    /* the write half's top-decode declaration check, on measure */
    serialize_measure_stream_t m;
    const float min = 30423614405477505635920876929024.0f;
    serialize_measure_stream_init( &m );
    serialize_measure_compressed_float( &m, min, FLT_MAX, 20282409603651670423947251286016.0f );
}

/* IMPLEMENTING.md U15: serialize_copy_string with dest_size below 1 is write
   misuse, the trap (U9) */
static void violate_copy_string_dest_size_zero( void )
{
    char dest[8];
    serialize_copy_string( dest, "abc", 0 );
}

static void violate_compressed_float_nan_value( void )
{
    static const serialize_uint32_t nan_pattern = 0x7FC00000UL;
    serialize_uint8_t buffer[8];
    serialize_write_stream_t w;
    float nan_value;
    memcpy( &nan_value, &nan_pattern, 4 );
    serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
    serialize_write_compressed_float( &w, nan_value, 0.0f, 10.0f, 0.01f );
}

static void violate_compressed_float_inf_value( void )
{
    static const serialize_uint32_t inf_pattern = 0x7F800000UL;
    serialize_uint8_t buffer[8];
    serialize_write_stream_t w;
    float inf_value;
    memcpy( &inf_value, &inf_pattern, 4 );
    serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
    serialize_write_compressed_float( &w, inf_value, 0.0f, 10.0f, 0.01f );
}

static void violate_compressed_float_infinite_delta( void )
{
    /* the declaration, not the value: max - min overflows to infinity */
    serialize_uint8_t buffer[8];
    serialize_write_stream_t w;
    serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
    serialize_write_compressed_float( &w, 0.0f, -FLT_MAX, FLT_MAX, 1.0f );
}

static void violate_compressed_float_infinite_values( void )
{
    /* the declaration again: the span is finite but delta / resolution is not */
    serialize_uint8_t buffer[8];
    serialize_write_stream_t w;
    serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
    serialize_write_compressed_float( &w, 0.0f, 0.0f, 1.0e38f, 1.0e-38f );
}

/* the precomputed entry points (mas-bandwidth/schema#82) carry the same
   write-side contract as the derive-per-call path -- a non-finite value
   asserts -- plus their own: constants that are not what
   serialize_compressed_float_params derives are caller error */

static void violate_compressed_float_precomputed_nan_value( void )
{
    /* the same non-conforming NaN write, through the precomputed entry
       point directly, with the conforming constants for [0,10] at 0.01 */
    static const serialize_uint32_t nan_pattern = 0x7FC00000UL;
    serialize_uint8_t buffer[8];
    serialize_write_stream_t w;
    float nan_value;
    memcpy( &nan_value, &nan_pattern, 4 );
    serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
    serialize_write_compressed_float_precomputed( &w, nan_value, 1000, 10, 10.0f, 0.0f );
}

static void violate_compressed_float_precomputed_bits_mismatch( void )
{
    /* a wire width that disagrees with the step count is a caller bug: the
       field would not occupy the width every other conforming implementation
       of the declaration expects */
    serialize_uint8_t buffer[8];
    serialize_write_stream_t w;
    serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
    serialize_write_compressed_float_precomputed( &w, 5.0f, 1000, 11, 10.0f, 0.0f );
}

/* IMPLEMENTING.md U3 and U4: a string write is UTF-8, checked on write AND
   on measure. 0xC0 0xAF is an overlong '/'. */
static void violate_write_string_invalid_utf8( void )
{
    serialize_uint8_t buffer[64];
    serialize_write_stream_t w;
    serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
    serialize_write_string( &w, "a\xC0\xAF", 16 );
}

static void violate_measure_string_invalid_utf8( void )
{
    serialize_measure_stream_t m;
    serialize_measure_stream_init( &m );
    serialize_measure_string( &m, "a\xC0\xAF", 16 );
}

/* IMPLEMENTING.md U16 on write and measure: a fixed point declaration the Q
   format cannot carry is caller error, asserted (the C++ reference refuses
   it at compile time). The audit's fixed_qformat reproducer, Q16.16 over
   [-40000, 40000] whole units, and a Q24.16 that does not fit 32 bit
   storage. (A Q8.8 in fixed32 storage is legal: C has no 16 bit form.) */

static void violate_write_fixed_qformat_bounds( void )
{
    serialize_uint8_t buffer[16];
    serialize_write_stream_t w;
    serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
    serialize_write_fixed32( &w, 0, 16, 16, -40000, 40000 );
}

static void violate_write_fixed_storage_width( void )
{
    serialize_uint8_t buffer[16];
    serialize_write_stream_t w;
    serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
    serialize_write_fixed32( &w, 100 * 65536, 24, 16, 0, 200 );
}

static void violate_measure_fixed_qformat_bounds( void )
{
    serialize_measure_stream_t m;
    serialize_measure_stream_init( &m );
    serialize_measure_fixed64( &m, 32, 32, 0, 2147483648LL );   /* Q32.32 max is 2^31 - 1 whole units */
}

/* U16: a step count above the 4294967040 ceiling the derivation clamps to,
   with a bit count that agrees with it */
static void violate_compressed_float_precomputed_ceiling( void )
{
    serialize_uint8_t buffer[8];
    serialize_write_stream_t w;
    serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
    serialize_write_compressed_float_precomputed( &w, 5.0f, 0xFFFFFFFFu, 32, 10.0f, 0.0f );
}

/* U10: a write checks room for the whole operation before it writes any of
   it. The length field fits and the payload does not, so the assert that
   must fire is the whole-operation one, before the length is written --
   never the payload's own capacity assert after the length and align bits
   are already in the buffer. */
static void violate_string_whole_operation_room( void )
{
    serialize_uint8_t buffer[8];                        /* 64 bits: room for a 6 bit length, not 13 bytes */
    serialize_write_stream_t w;
    serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
    serialize_write_string( &w, "hello, world!", 64 );
}

static void violate_bytes_whole_operation_room( void )
{
    static const serialize_uint8_t payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    serialize_uint8_t buffer[8];
    serialize_write_stream_t w;
    serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
    serialize_write_bits( &w, 1, 1 );                   /* the align takes 7 bits, then 64 do not fit in 56 */
    serialize_write_bytes( &w, payload, 8 );
}

/*
    Forks, runs the violation in the child with its stderr captured through a
    pipe, and requires death by SIGABRT AND that the assert message names the
    expected expression. A bare SIGABRT is not enough: the audit's
    which_assert reproducer showed a case passing on an assert other than the
    one it was written for (the U1 buffer assert firing ahead of the capacity
    assert it meant to prove). The C standard requires an assert's message to
    carry the text of its expression, so the expected text is that
    expression, or a distinctive part of it.
*/
static void must_abort( void (*violation)( void ), const char * name, const char * expected )
{
    int fds[2];
    pid_t pid;
    char message[4096];
    size_t length = 0;

    if ( pipe( fds ) != 0 )
    {
        printf( "FAILED %s: pipe\n", name );
        failed = 1;
        return;
    }

    fflush( stdout );                                   /* or the child's abort replays the parent's buffered output */
    pid = fork();

    if ( pid < 0 )
    {
        printf( "FAILED %s: fork\n", name );
        failed = 1;
        close( fds[0] );
        close( fds[1] );
        return;
    }

    if ( pid == 0 )
    {
        close( fds[0] );
        dup2( fds[1], 2 );
        violation();
        _exit( 0 );                                     /* survived: the parent treats this as failure */
    }

    close( fds[1] );
    for ( ;; )
    {
        ssize_t n;
        if ( length >= sizeof( message ) - 1 )
        {
            break;
        }
        n = read( fds[0], message + length, sizeof( message ) - 1 - length );
        if ( n <= 0 )
        {
            break;
        }
        length += (size_t) n;
    }
    message[length] = '\0';
    close( fds[0] );

    {
        int status = 0;
        waitpid( pid, &status, 0 );
        if ( !( WIFSIGNALED( status ) && WTERMSIG( status ) == SIGABRT ) )
        {
            printf( "FAILED %s: expected SIGABRT, child did not assert\n", name );
            failed = 1;
        }
        else if ( strstr( message, expected ) == NULL )
        {
            printf( "FAILED %s: the wrong assert fired. expected \"%s\", got: %s\n", name, expected, message );
            failed = 1;
        }
    }
}

int main( void )
{
    must_abort( violate_capacity, "capacity", "stream->bits_written + bits <= stream->num_bits" );
    must_abort( violate_write_buffer_multiple_of_8, "write_buffer_multiple_of_8", "( bytes % 8 ) == 0" );
    must_abort( violate_int_range, "int_range", "value <= max" );
    must_abort( violate_int_relative_order, "int_relative_order", "current > previous" );
    must_abort( violate_int_relative_previous_domain, "int_relative_previous_domain", "previous >= 0" );
    must_abort( violate_string_length, "string_length", "serialize_length_fits_buffer( string_length, buffer_size )" );
    must_abort( violate_bits_width, "bits_width", "value <= (serialize_uint32_t) ( ( 1UL << bits ) - 1 )" );
    must_abort( violate_measure_string_length, "measure_string_length", "serialize_length_fits_buffer( string_length, buffer_size )" );
    must_abort( violate_write_float_nan, "write_float_nan", "serialize_float_bits_are_finite( bits )" );
    must_abort( violate_write_float_signaling_nan, "write_float_signaling_nan", "serialize_float_bits_are_finite( bits )" );
    must_abort( violate_write_float_infinity, "write_float_infinity", "serialize_float_bits_are_finite( bits )" );
    must_abort( violate_write_double_nan, "write_double_nan", "serialize_double_bits_are_finite( bits )" );
    must_abort( violate_write_double_infinity, "write_double_infinity", "serialize_double_bits_are_finite( bits )" );
    must_abort( violate_write_double_low_payload, "write_double_low_payload", "serialize_double_bits_are_finite( bits )" );
    must_abort( violate_compressed_float_infinite_top, "compressed_float_infinite_top", "serialize_float_is_finite( min + delta )" );
    must_abort( violate_measure_int_relative_order, "measure_int_relative_order", "current > previous" );
    must_abort( violate_measure_int_relative_previous_domain, "measure_int_relative_previous_domain", "previous >= 0" );
    must_abort( violate_measure_int128_bounds, "measure_int128_bounds", "serialize_int128_compare( min, max ) <= 0" );
    must_abort( violate_read_null_buffer, "read_null_buffer", "buffer" );
    must_abort( violate_read_padded_destination, "read_padded_destination", "destination_bytes - 8 >= bytes" );
    must_abort( violate_measure_int_bounds, "measure_int_bounds", "min <= max" );
    must_abort( violate_measure_int64_bounds, "measure_int64_bounds", "min <= max" );
    must_abort( violate_measure_bits_zero, "measure_bits_zero", "bits > 0" );
    must_abort( violate_measure_bits_wide, "measure_bits_wide", "bits <= 32" );
    must_abort( violate_measure_bytes_negative, "measure_bytes_negative", "bytes >= 0" );
    must_abort( violate_measure_compressed_float_infinite_top, "measure_compressed_float_infinite_top", "serialize_float_is_finite( min + delta )" );
    must_abort( violate_copy_string_dest_size_zero, "copy_string_dest_size_zero", "dest_size >= 1" );
    must_abort( violate_compressed_float_nan_value, "compressed_float_nan_value", "serialize_float_is_finite( value )" );
    must_abort( violate_compressed_float_inf_value, "compressed_float_inf_value", "serialize_float_is_finite( value )" );
    must_abort( violate_compressed_float_infinite_delta, "compressed_float_infinite_delta", "serialize_float_is_finite( delta )" );
    must_abort( violate_compressed_float_infinite_values, "compressed_float_infinite_values", "serialize_float_is_finite( values )" );
    must_abort( violate_compressed_float_precomputed_nan_value, "compressed_float_precomputed_nan_value", "serialize_float_is_finite( value )" );
    must_abort( violate_compressed_float_precomputed_bits_mismatch, "compressed_float_precomputed_bits_mismatch", "bits == serialize_bits_required( 0, max_integer_value )" );
    must_abort( violate_compressed_float_precomputed_ceiling, "compressed_float_precomputed_ceiling", "max_integer_value <= 4294967040u" );
    must_abort( violate_write_string_invalid_utf8, "write_string_invalid_utf8", "serialize_string_is_valid_utf8( string, length )" );
    must_abort( violate_measure_string_invalid_utf8, "measure_string_invalid_utf8", "serialize_string_is_valid_utf8( string, length )" );
    must_abort( violate_write_fixed_qformat_bounds, "write_fixed_qformat_bounds", "serialize_fixed_declaration_is_valid( 32," );
    must_abort( violate_write_fixed_storage_width, "write_fixed_storage_width", "serialize_fixed_declaration_is_valid( 32," );
    must_abort( violate_measure_fixed_qformat_bounds, "measure_fixed_qformat_bounds", "serialize_fixed_declaration_is_valid( 64," );
    must_abort( violate_string_whole_operation_room, "string_whole_operation_room", "serialize_write_has_room( stream, serialize_bits_required" );
    must_abort( violate_bytes_whole_operation_room, "bytes_whole_operation_room", "serialize_write_has_room( stream, 0, bytes )" );

    printf( failed ? "FAILED\n" : "OK\n" );
    return failed;
}

#endif /* NDEBUG */
