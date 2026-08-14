/*
    The golden wire test: fixed sequences and the exact bytes they must
    produce.

    This exists because a ROUND TRIP TEST CANNOT CATCH AN ENDIANNESS BUG. If
    the packer wrote the scratch word in the wrong byte order, the reader would
    read it back in the same wrong order and every round trip would pass while
    the bytes on the wire were incompatible with every other port.

    So the expected bytes are pinned, in test/vectors.h. They were produced by
    the C++ library -- test/diff_cpp.cpp and test/diff2_cpp.cpp write the same
    sequences and check the same constants, so `make diff` keeps the pin
    honest rather than letting it agree only with itself. Running this under
    qemu-s390x is what proves the big-endian path.

    TWO sequences, because one could not cover the surface. The core one is
    everything built on 32-bit groups; the wide one is the 128-bit, fixed
    point and wide string paths, which are a hand-rolled two-lane 64-bit
    emulation and so the likeliest place in the library for a byte order bug.

    Both sequences are also READ BACK, field by field, so the golden is a live
    stream and not a string that happens to match.
*/
#include <stdio.h>
#include <string.h>
#include "../serialize.h"
#include "vectors.h"

static int failed = 0;
#define CHECK(c) do { if (!(c)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); failed = 1; } } while (0)

/* the same sequence as test/diff_c.c and test/diff_cpp.cpp */
static const char * expected_core = SERIALIZE_GOLDEN_CORE;

/* the same sequence as test/diff2_c.c and test/diff2_cpp.cpp */
static const char * expected_wide = SERIALIZE_GOLDEN_WIDE;

static int check_bytes( const char * label, const serialize_uint8_t * buffer, int n, const char * expected )
{
    static const char digits[] = "0123456789abcdef";
    static char hex[4096];
    int i;

    /* a nibble at a time rather than sprintf: C89 has no snprintf to reach for
       instead, and this needs neither */
    for ( i = 0; i < n; i++ )
    {
        hex[i * 2]     = digits[ ( buffer[i] >> 4 ) & 0x0F ];
        hex[i * 2 + 1] = digits[ buffer[i] & 0x0F ];
    }
    hex[n * 2] = '\0';

    if ( strcmp( hex, expected ) != 0 )
    {
        printf( "FAILED: %s wire mismatch\n", label );
        printf( "  expected %s\n", expected );
        printf( "  got      %s\n", hex );
        return 0;
    }

    return 1;
}

static int golden_core( void )
{
    static serialize_uint8_t buffer[1024];
    static const serialize_uint8_t blob[5] = { 1, 2, 3, 250, 255 };
    serialize_write_stream_t w;
    int n;

    serialize_write_stream_init( &w, buffer, sizeof( buffer ) );

    serialize_write_bits( &w, 5, 3 );
    serialize_write_bool( &w, 1 );
    serialize_write_bool( &w, 0 );
    serialize_write_int( &w, 42, 0, 1000 );
    serialize_write_int( &w, -7, -100, 100 );
    serialize_write_uint8( &w, 0xAB );
    serialize_write_uint16( &w, 0xBEEF );
    serialize_write_uint32( &w, 0xDEADBEEF );
    serialize_write_uint64( &w, 0x0123456789ABCDEFULL );
    serialize_write_int64( &w, -1234567890123LL, -2000000000000LL, 2000000000000LL );
    serialize_write_float( &w, 3.1415926f );
    serialize_write_double( &w, 1.0 / 3.0 );
    serialize_write_compressed_float( &w, 0.75f, 0.0f, 1.0f, 0.01f );
    serialize_write_align( &w );
    serialize_write_bytes( &w, blob, 5 );
    serialize_write_string( &w, "the quick brown fox", 64 );
    serialize_write_int_relative( &w, 100, 101 );
    serialize_write_int_relative( &w, 100, 104 );
    serialize_write_int_relative( &w, 100, 120 );
    serialize_write_int_relative( &w, 100, 400 );
    serialize_write_int_relative( &w, 100, 5000 );
    serialize_write_int_relative( &w, 100, 999999 );

    serialize_write_flush( &w );

    if ( serialize_write_error( &w ) )
    {
        printf( "FAILED: core write error\n" );
        return 0;
    }

    n = serialize_write_bytes_processed( &w );

    if ( !check_bytes( "core", buffer, n, expected_core ) )
    {
        return 0;
    }

    /* and every field must read back */
    {
        serialize_read_stream_t r;
        serialize_uint32_t b3 = 0;
        int bt = 0, bf = 1;
        serialize_int32_t i1 = 0, i2 = 0;
        serialize_uint8_t u8 = 0;
        serialize_uint16_t u16 = 0;
        serialize_uint32_t u32 = 0;
        serialize_uint64_t u64 = 0;
        serialize_int64_t i64 = 0;
        float fv = 0.0f, cf = 0.0f;
        double dv = 0.0;
        serialize_uint8_t blob_out[5];
        char str[64];
        serialize_int32_t rel = 0;

        serialize_read_stream_init( &r, buffer, n );

        CHECK( serialize_read_bits( &r, &b3, 3 ) );             CHECK( b3 == 5 );
        CHECK( serialize_read_bool( &r, &bt ) );                CHECK( bt == 1 );
        CHECK( serialize_read_bool( &r, &bf ) );                CHECK( bf == 0 );
        CHECK( serialize_read_int( &r, &i1, 0, 1000 ) );        CHECK( i1 == 42 );
        CHECK( serialize_read_int( &r, &i2, -100, 100 ) );      CHECK( i2 == -7 );
        CHECK( serialize_read_uint8( &r, &u8 ) );               CHECK( u8 == 0xAB );
        CHECK( serialize_read_uint16( &r, &u16 ) );             CHECK( u16 == 0xBEEF );
        CHECK( serialize_read_uint32( &r, &u32 ) );             CHECK( u32 == 0xDEADBEEF );
        CHECK( serialize_read_uint64( &r, &u64 ) );             CHECK( u64 == 0x0123456789ABCDEFULL );
        CHECK( serialize_read_int64( &r, &i64, -2000000000000LL, 2000000000000LL ) );
        CHECK( i64 == -1234567890123LL );
        CHECK( serialize_read_float( &r, &fv ) );               CHECK( fv == 3.1415926f );
        CHECK( serialize_read_double( &r, &dv ) );              CHECK( dv == 1.0 / 3.0 );
        CHECK( serialize_read_compressed_float( &r, &cf, 0.0f, 1.0f, 0.01f ) );
        CHECK( cf > 0.74f && cf < 0.76f );                      /* lossy by construction */
        CHECK( serialize_read_align( &r ) );
        CHECK( serialize_read_bytes( &r, blob_out, 5 ) );
        CHECK( memcmp( blob_out, blob, 5 ) == 0 );
        CHECK( serialize_read_string( &r, str, 64 ) );
        CHECK( strcmp( str, "the quick brown fox" ) == 0 );
        CHECK( serialize_read_int_relative( &r, 100, &rel ) );  CHECK( rel == 101 );
        CHECK( serialize_read_int_relative( &r, 100, &rel ) );  CHECK( rel == 104 );
        CHECK( serialize_read_int_relative( &r, 100, &rel ) );  CHECK( rel == 120 );
        CHECK( serialize_read_int_relative( &r, 100, &rel ) );  CHECK( rel == 400 );
        CHECK( serialize_read_int_relative( &r, 100, &rel ) );  CHECK( rel == 5000 );
        CHECK( serialize_read_int_relative( &r, 100, &rel ) );  CHECK( rel == 999999 );
        CHECK( !serialize_read_error( &r ) );

        /* the reader consumed exactly what the writer produced */
        CHECK( serialize_read_bits_processed( &r ) == serialize_write_bits_processed( &w ) );
    }

    printf( "core: OK (%d bytes)\n", n );

    return !failed;
}

static int golden_wide( void )
{
    static serialize_uint8_t buffer[1024];
    serialize_write_stream_t w;
    wchar_t ws[8];
    int n;

    /* the same three characters STANDARD.md uses in its worked example */
    ws[0] = 0x043C; ws[1] = 0x0438; ws[2] = 0x0440; ws[3] = 0;

    serialize_write_stream_init( &w, buffer, sizeof( buffer ) );

    serialize_write_uint128( &w, serialize_uint128_make( 0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL ) );
    serialize_write_int128( &w,
        serialize_int128_from_int64( -1234567890123LL ),
        serialize_int128_from_int64( -2000000000000LL ),
        serialize_int128_from_int64( 2000000000000LL ) );
    serialize_write_fixed32( &w, 100 * 65536, 16, 16, -180, 180 );
    serialize_write_fixed64( &w, -5000LL * 65536, 48, 16, -1000000, 1000000 );
    serialize_write_fixed128( &w, serialize_int128_from_int64( 777LL * 65536 ), 112, 16, -1000000, 1000000 );
    serialize_write_fixed32( &w, 12345, 32, 0, 0, 1000000 );

    /* spans wider than 64 bits, so the offset reaches the HIGH LANE of the
       two-lane emulation -- groups 2 and 3 of the 32-bit splitting, which
       nothing above this line touches */
    serialize_write_int128( &w,
        serialize_int128_make( 0x0000000F23456789ULL, 0xABCDEF0123456789ULL ),
        serialize_int128_make( 0xFFFFFFF000000000ULL, 0x0000000000000000ULL ),
        serialize_int128_make( 0x0000001000000000ULL, 0x0000000000000000ULL ) );
    serialize_write_fixed128( &w, serialize_int128_from_int64( -12345LL * ( (serialize_int64_t) 1 << 48 ) ), 80, 48, -1000000, 1000000 );

    serialize_write_wstring( &w, ws, 8 );

    serialize_write_flush( &w );

    if ( serialize_write_error( &w ) )
    {
        printf( "FAILED: wide write error\n" );
        return 0;
    }

    n = serialize_write_bytes_processed( &w );

    if ( !check_bytes( "wide", buffer, n, expected_wide ) )
    {
        return 0;
    }

    {
        serialize_read_stream_t r;
        serialize_uint128_t u128 = { 0, 0 };
        serialize_int128_t i128 = { 0, 0 };
        serialize_int32_t f32 = 0;
        serialize_int64_t f64 = 0;
        serialize_int128_t f128 = { 0, 0 };
        wchar_t ws_out[8];

        serialize_read_stream_init( &r, buffer, n );

        CHECK( serialize_read_uint128( &r, &u128 ) );
        CHECK( serialize_uint128_equal( u128, serialize_uint128_make( 0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL ) ) );

        CHECK( serialize_read_int128( &r, &i128,
                   serialize_int128_from_int64( -2000000000000LL ),
                   serialize_int128_from_int64( 2000000000000LL ) ) );
        CHECK( serialize_int128_equal( i128, serialize_int128_from_int64( -1234567890123LL ) ) );

        CHECK( serialize_read_fixed32( &r, &f32, 16, 16, -180, 180 ) );
        CHECK( f32 == 100 * 65536 );

        CHECK( serialize_read_fixed64( &r, &f64, 48, 16, -1000000, 1000000 ) );
        CHECK( f64 == -5000LL * 65536 );

        CHECK( serialize_read_fixed128( &r, &f128, 112, 16, -1000000, 1000000 ) );
        CHECK( serialize_int128_equal( f128, serialize_int128_from_int64( 777LL * 65536 ) ) );

        CHECK( serialize_read_fixed32( &r, &f32, 32, 0, 0, 1000000 ) );
        CHECK( f32 == 12345 );

        CHECK( serialize_read_int128( &r, &i128,
                   serialize_int128_make( 0xFFFFFFF000000000ULL, 0x0000000000000000ULL ),
                   serialize_int128_make( 0x0000001000000000ULL, 0x0000000000000000ULL ) ) );
        CHECK( serialize_int128_equal( i128, serialize_int128_make( 0x0000000F23456789ULL, 0xABCDEF0123456789ULL ) ) );

        CHECK( serialize_read_fixed128( &r, &f128, 80, 48, -1000000, 1000000 ) );
        CHECK( serialize_int128_equal( f128, serialize_int128_from_int64( -12345LL * ( (serialize_int64_t) 1 << 48 ) ) ) );

        CHECK( serialize_read_wstring( &r, ws_out, 8 ) );
        CHECK( ws_out[0] == 0x043C && ws_out[1] == 0x0438 && ws_out[2] == 0x0440 && ws_out[3] == 0 );

        CHECK( !serialize_read_error( &r ) );
        CHECK( serialize_read_bits_processed( &r ) == serialize_write_bits_processed( &w ) );
    }

    printf( "wide: OK (%d bytes)\n", n );

    return !failed;
}

int main( void )
{
    if ( !golden_core() )
    {
        return 1;
    }

    if ( !golden_wide() )
    {
        return 1;
    }

    printf( "OK\n" );

    return 0;
}
