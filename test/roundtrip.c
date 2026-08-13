/* Round-trips every operation and checks the read path rejects what it must.
   Wire compatibility is proven separately by diff_c.c against the C++
   library; this is about the read half being correct and safe. */
#include <stdio.h>
#include <string.h>
#include "../serialize.h"

static int failed = 0;
#define CHECK(c) do { if (!(c)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); failed = 1; } } while (0)

int main( void )
{
    static serialize_uint8_t buffer[2048];
    serialize_write_stream_t w;
    serialize_read_stream_t r;

    /* ---- round trip every operation ---- */
    {
        serialize_uint32_t b3 = 0; int t = 0, f = 1;
        serialize_int32_t i1 = 0, i2 = 0;
        serialize_uint8_t u8 = 0; serialize_uint16_t u16 = 0; serialize_uint32_t u32 = 0;
        serialize_uint64_t u64 = 0; serialize_int64_t i64 = 0;
        float fv = 0.0f, cf = 0.0f; double dv = 0.0;
        serialize_uint8_t blob[5];
        char str[64];
        serialize_int32_t rel = 0;

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
        { static const serialize_uint8_t src[5] = { 1, 2, 3, 250, 255 };
          serialize_write_bytes( &w, src, 5 ); }
        serialize_write_string( &w, "the quick brown fox", 64 );
        serialize_write_int_relative( &w, 100, 999999 );
        serialize_write_flush( &w );
        CHECK( !serialize_write_error( &w ) );

        serialize_read_stream_init( &r, buffer, serialize_write_bytes_processed( &w ) );
        CHECK( serialize_read_bits( &r, &b3, 3 ) );      CHECK( b3 == 5 );
        CHECK( serialize_read_bool( &r, &t ) );          CHECK( t == 1 );
        CHECK( serialize_read_bool( &r, &f ) );          CHECK( f == 0 );
        CHECK( serialize_read_int( &r, &i1, 0, 1000 ) ); CHECK( i1 == 42 );
        CHECK( serialize_read_int( &r, &i2, -100, 100 ) ); CHECK( i2 == -7 );
        CHECK( serialize_read_uint8( &r, &u8 ) );        CHECK( u8 == 0xAB );
        CHECK( serialize_read_uint16( &r, &u16 ) );      CHECK( u16 == 0xBEEF );
        CHECK( serialize_read_uint32( &r, &u32 ) );      CHECK( u32 == 0xDEADBEEF );
        CHECK( serialize_read_uint64( &r, &u64 ) );      CHECK( u64 == 0x0123456789ABCDEFULL );
        CHECK( serialize_read_int64( &r, &i64, -2000000000000LL, 2000000000000LL ) );
        CHECK( i64 == -1234567890123LL );
        CHECK( serialize_read_float( &r, &fv ) );        CHECK( fv == 3.1415926f );
        CHECK( serialize_read_double( &r, &dv ) );       CHECK( dv == 1.0 / 3.0 );
        CHECK( serialize_read_compressed_float( &r, &cf, 0.0f, 1.0f, 0.01f ) );
        CHECK( cf > 0.74f && cf < 0.76f );               /* lossy by construction */
        CHECK( serialize_read_bytes( &r, blob, 5 ) );
        CHECK( blob[0] == 1 && blob[3] == 250 && blob[4] == 255 );
        CHECK( serialize_read_string( &r, str, 64 ) );
        CHECK( strcmp( str, "the quick brown fox" ) == 0 );
        CHECK( serialize_read_int_relative( &r, 100, &rel ) ); CHECK( rel == 999999 );
        CHECK( !serialize_read_error( &r ) );
    }

    /* ---- every int_relative tier, including the 4378..69914 one that
            STANDARD.md omitted ---- */
    {
        static const serialize_int32_t deltas[] = { 1, 2, 6, 7, 23, 24, 280, 281, 4377, 4378, 69914, 69915, 1000000 };
        int i;
        for ( i = 0; i < (int) ( sizeof( deltas ) / sizeof( deltas[0] ) ); i++ )
        {
            serialize_int32_t got = 0;
            serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
            serialize_write_int_relative( &w, 1000, 1000 + deltas[i] );
            serialize_write_flush( &w );
            CHECK( !serialize_write_error( &w ) );
            serialize_read_stream_init( &r, buffer, serialize_write_bytes_processed( &w ) );
            CHECK( serialize_read_int_relative( &r, 1000, &got ) );
            CHECK( got == 1000 + deltas[i] );
        }
    }

    /* ---- the read path must REJECT, not clamp ---- */
    {
        serialize_int32_t v = 0;

        /* A value outside the declared range fails the read. [0,100] needs 7
           bits, so the injected value has to exceed 100 IN 7 BITS -- writing a
           wider value would just be truncated into range by the reader, which
           is a decode, not a rejection. */
        serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
        serialize_write_bits( &w, 127, 7 );          /* 127 is outside [0,100] */
        serialize_write_flush( &w );
        serialize_read_stream_init( &r, buffer, serialize_write_bytes_processed( &w ) );
        CHECK( !serialize_read_int( &r, &v, 0, 100 ) );
        CHECK( serialize_read_error( &r ) );

        /* nonzero align padding is malformed and must fail */
        serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
        serialize_write_bits( &w, 1, 1 );
        serialize_write_bits( &w, 0x7F, 7 );         /* padding that is not zero */
        serialize_write_flush( &w );
        serialize_read_stream_init( &r, buffer, serialize_write_bytes_processed( &w ) );
        { serialize_uint32_t one = 0; CHECK( serialize_read_bits( &r, &one, 1 ) ); }
        CHECK( !serialize_read_align( &r ) );
        CHECK( serialize_read_error( &r ) );

        /* reading past the end fails rather than reading whatever is there */
        serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
        serialize_write_bits( &w, 1, 8 );
        serialize_write_flush( &w );
        serialize_read_stream_init( &r, buffer, 1 );
        { serialize_uint32_t x = 0;
          CHECK( serialize_read_bits( &r, &x, 8 ) );
          CHECK( !serialize_read_bits( &r, &x, 8 ) ); }
        CHECK( serialize_read_error( &r ) );

        /* errors are sticky: once failed, later operations do not succeed */
        CHECK( !serialize_read_int( &r, &v, 0, 10 ) );
    }

    /* ---- a degenerate range costs zero bits, per STANDARD.md ---- */
    {
        serialize_int32_t v = 0;
        serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
        CHECK( serialize_write_int( &w, 5, 5, 5 ) );
        CHECK( serialize_write_bits_processed( &w ) == 0 );
        serialize_write_flush( &w );
        serialize_read_stream_init( &r, buffer, 8 );
        CHECK( serialize_read_int( &r, &v, 5, 5 ) );
        CHECK( v == 5 );
        CHECK( serialize_read_bits_processed( &r ) == 0 );
    }

    /* ---- the measure stream agrees with the writer ---- */
    {
        serialize_measure_stream_t m;
        serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
        serialize_measure_stream_init( &m );
        serialize_write_bits( &w, 5, 3 );        serialize_measure_bits( &m, 3 );
        serialize_write_int( &w, 42, 0, 1000 );  serialize_measure_int( &m, 0, 1000 );
        serialize_write_align( &w );             serialize_measure_align( &m );
        { static const serialize_uint8_t src[3] = { 9, 8, 7 };
          serialize_write_bytes( &w, src, 3 );   serialize_measure_bytes( &m, 3 ); }
        CHECK( serialize_write_bits_processed( &w ) == serialize_measure_bits_processed( &m ) );
    }

    /* ---- 128-bit, fixed point and wide strings ---- */
    {
        serialize_uint128_t u = serialize_uint128_make( 0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL );
        serialize_uint128_t u_out;
        serialize_int128_t i_in = serialize_int128_from_int64( -1234567890123LL );
        serialize_int128_t i_out;
        serialize_int128_t lo = serialize_int128_from_int64( -2000000000000LL );
        serialize_int128_t hi = serialize_int128_from_int64( 2000000000000LL );
        serialize_int32_t f32 = 0;
        serialize_int64_t f64 = 0;
        serialize_int128_t f128;
        wchar_t ws_in[8], ws_out[8];

        ws_in[0] = 0x043C; ws_in[1] = 0x0438; ws_in[2] = 0x0440; ws_in[3] = 0;

        serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
        serialize_write_uint128( &w, u );
        serialize_write_int128( &w, i_in, lo, hi );
        serialize_write_fixed32( &w, 100 * 65536, 16, 16, -180, 180 );
        serialize_write_fixed64( &w, -5000LL * 65536, 48, 16, -1000000, 1000000 );
        serialize_write_fixed128( &w, serialize_int128_from_int64( 777LL * 65536 ), 112, 16, -1000000, 1000000 );
        serialize_write_wstring( &w, ws_in, 8 );
        serialize_write_flush( &w );
        CHECK( !serialize_write_error( &w ) );

        serialize_read_stream_init( &r, buffer, serialize_write_bytes_processed( &w ) );
        CHECK( serialize_read_uint128( &r, &u_out ) );
        CHECK( serialize_uint128_equal( u_out, u ) );
        CHECK( serialize_read_int128( &r, &i_out, lo, hi ) );
        CHECK( serialize_int128_equal( i_out, i_in ) );
        CHECK( serialize_read_fixed32( &r, &f32, 16, 16, -180, 180 ) );
        CHECK( f32 == 100 * 65536 );
        CHECK( serialize_read_fixed64( &r, &f64, 48, 16, -1000000, 1000000 ) );
        CHECK( f64 == -5000LL * 65536 );
        CHECK( serialize_read_fixed128( &r, &f128, 112, 16, -1000000, 1000000 ) );
        CHECK( serialize_int128_equal( f128, serialize_int128_from_int64( 777LL * 65536 ) ) );
        CHECK( serialize_read_wstring( &r, ws_out, 8 ) );
        CHECK( ws_out[0] == 0x043C && ws_out[1] == 0x0438 && ws_out[2] == 0x0440 && ws_out[3] == 0 );
        CHECK( !serialize_read_error( &r ) );

        /* fixed point is EXACT -- that is the whole reason it exists, so a
           round trip that merely lands nearby would defeat the purpose */
        CHECK( f32 == 100 * 65536 );

        /* fraction_bits = 0 makes it a plain ranged integer */
        serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
        serialize_write_fixed32( &w, 12345, 32, 0, 0, 1000000 );
        serialize_write_flush( &w );
        serialize_read_stream_init( &r, buffer, serialize_write_bytes_processed( &w ) );
        CHECK( serialize_read_fixed32( &r, &f32, 32, 0, 0, 1000000 ) );
        CHECK( f32 == 12345 );

        /* and the read still rejects an out-of-range 128-bit offset */
        serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
        serialize_write_bits( &w, 0xFFFFFFFFu, 32 );
        serialize_write_bits( &w, 0xFFFFFFFFu, 32 );
        serialize_write_flush( &w );
        serialize_read_stream_init( &r, buffer, serialize_write_bytes_processed( &w ) );
        CHECK( !serialize_read_int128( &r, &i_out, lo, hi ) );
        CHECK( serialize_read_error( &r ) );
    }

    printf( failed ? "FAILED\n" : "OK\n" );
    return failed;
}
