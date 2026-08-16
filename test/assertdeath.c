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
    Range refusals, buffer-end refusals, malformed align padding and the
    string/wstring content refusals are real checks in every build mode, and
    test/roundtrip.c proves those as ordinary failing reads.

    POSIX only (fork), which is fine: CI's Windows leg compiles and runs
    roundtrip/golden/wstest directly and never builds this file. Under
    NDEBUG every assert compiles to nothing, so this test skips itself —
    the violations here would be undefined behavior, deliberately.

    The compressed float cases pin serialize fork #6 (the ruling: "it's
    non-conforming. also, attempting to send NaN or INF or anything else
    through compressed float is non-conforming and should assert out on
    write too"): a non-finite VALUE asserts at intake, and a DECLARATION
    whose span or step count does not compute finite asserts at the site
    the parameters are computed.
*/

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include <stdio.h>

#ifdef NDEBUG

int main( void )
{
    /* no asserts to prove under NDEBUG: the write and measure paths check
       nothing at all, which is the contract — see roundtrip.c's release-only
       NaN block for what remains observable there */
    printf( "assertdeath: skipped (NDEBUG: the caller contracts have no checks to fire)\n" );
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
    serialize_uint8_t buffer[4];                        /* 32 bits of room */
    serialize_write_stream_t w;
    serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
    serialize_write_bits( &w, 0xFFFFFF, 24 );
    serialize_write_bits( &w, 0, 16 );                  /* 8 bits past the end */
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

static void violate_measure_wstring_length( void )
{
    serialize_measure_stream_t m;
    wchar_t ws[4];
    ws[0] = 0x043C; ws[1] = 0x0438; ws[2] = 0x0440; ws[3] = 0;
    serialize_measure_stream_init( &m );
    serialize_measure_wstring( &m, ws, 3 );             /* three units cannot fit a buffer of 3 */
}

static void violate_measure_int_relative_order( void )
{
    serialize_measure_stream_t m;
    serialize_measure_stream_init( &m );
    serialize_measure_int_relative( &m, 100, 100 );     /* STRICTLY increasing, violated */
}

static void violate_measure_int128_bounds( void )
{
    serialize_measure_stream_t m;
    serialize_measure_stream_init( &m );
    serialize_measure_int128( &m,
                              serialize_int128_from_int64( 1 ),
                              serialize_int128_from_int64( -1 ) );  /* min > max */
}

/* the read path's caller-owned parameters are contracts too — the bounds
   are the caller's, not the network's, asserted exactly as the C++
   ReadStream asserts them. The DATA is still checked for real: those
   refusals live in roundtrip.c, in every build mode. */

static void violate_read_int128_bounds( void )
{
    serialize_uint8_t buffer[16 + 8];                   /* + 8: read buffer allocations extend 8 bytes past the data */
    serialize_read_stream_t r;
    serialize_int128_t out;
    memset( buffer, 0, sizeof( buffer ) );
    serialize_read_stream_init( &r, buffer, 16 );
    serialize_read_int128( &r, &out,
                           serialize_int128_from_int64( 1 ),
                           serialize_int128_from_int64( -1 ) );     /* min > max */
}

static void violate_read_null_buffer( void )
{
    serialize_read_stream_t r;
    serialize_read_stream_init( &r, NULL, 8 );          /* the C++ BitReader asserts data; init asserts the same */
}

static void violate_write_after_fail( void )
{
    serialize_uint8_t buffer[8];
    serialize_write_stream_t w;
    serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
    serialize_write_fail( &w );
    serialize_write_bits( &w, 1, 1 );                   /* the poisoned limit catches this */
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

/*
    Forks, runs the violation in the child with stderr pointed at /dev/null
    so the expected assert message does not read as a failing test, and
    requires death by SIGABRT — anything else (a clean exit most of all)
    means the contract did not fire.
*/
static void must_abort( void (*violation)( void ), const char * name )
{
    pid_t pid = fork();

    if ( pid < 0 )
    {
        printf( "FAILED %s: fork\n", name );
        failed = 1;
        return;
    }

    if ( pid == 0 )
    {
        int devnull = open( "/dev/null", O_WRONLY );
        if ( devnull >= 0 )
        {
            dup2( devnull, 2 );
        }
        violation();
        _exit( 0 );                                     /* survived: the parent treats this as failure */
    }

    {
        int status = 0;
        waitpid( pid, &status, 0 );
        if ( !( WIFSIGNALED( status ) && WTERMSIG( status ) == SIGABRT ) )
        {
            printf( "FAILED %s: expected SIGABRT, child did not assert\n", name );
            failed = 1;
        }
    }
}

int main( void )
{
    must_abort( violate_capacity,                        "capacity" );
    must_abort( violate_int_range,                       "int_range" );
    must_abort( violate_int_relative_order,              "int_relative_order" );
    must_abort( violate_string_length,                   "string_length" );
    must_abort( violate_bits_width,                      "bits_width" );
    must_abort( violate_measure_string_length,           "measure_string_length" );
    must_abort( violate_measure_wstring_length,          "measure_wstring_length" );
    must_abort( violate_measure_int_relative_order,      "measure_int_relative_order" );
    must_abort( violate_measure_int128_bounds,           "measure_int128_bounds" );
    must_abort( violate_read_int128_bounds,              "read_int128_bounds" );
    must_abort( violate_read_null_buffer,                "read_null_buffer" );
    must_abort( violate_write_after_fail,                "write_after_fail" );
    must_abort( violate_compressed_float_nan_value,      "compressed_float_nan_value" );
    must_abort( violate_compressed_float_inf_value,      "compressed_float_inf_value" );
    must_abort( violate_compressed_float_infinite_delta, "compressed_float_infinite_delta" );
    must_abort( violate_compressed_float_infinite_values, "compressed_float_infinite_values" );

    printf( failed ? "FAILED\n" : "OK\n" );
    return failed;
}

#endif /* NDEBUG */
