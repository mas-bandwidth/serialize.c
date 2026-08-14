/* Round-trips every operation and checks the read path rejects what it must.
   Wire compatibility is proven separately by diff_c.c against the C++
   library; this is about the read half being correct and safe. */
#include <stdio.h>
#include <string.h>
#include "../serialize.h"

static int failed = 0;
#define CHECK(c) do { if (!(c)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); failed = 1; } } while (0)

/*
    Performs one operation on a write stream and its measure counterpart on a
    measure stream, from the same starting point, and requires the two counts
    to agree EXACTLY. A measure that does not equal the bits actually emitted
    is worse than no measure at all: it sizes a buffer that then overflows.

    The 3-bit prelude is not decoration. align, bytes, string -- everything
    that pads -- costs a number of bits that depends on where in the byte the
    operation begins, and starting from zero would measure the one case where
    alignment happens to be free.
*/
#define MEASURED(w_op, m_op) do {                                                                \
        serialize_write_stream_init( &w, buffer, sizeof( buffer ) );                              \
        serialize_measure_stream_init( &m );                                                      \
        CHECK( serialize_write_bits( &w, 5, 3 ) );                                                \
        CHECK( serialize_measure_bits( &m, 3 ) );                                                 \
        CHECK( w_op );                                                                            \
        CHECK( m_op );                                                                            \
        CHECK( !serialize_write_error( &w ) );                                                     \
        CHECK( serialize_write_bits_processed( &w ) == serialize_measure_bits_processed( &m ) );  \
    } while ( 0 )

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

    /* ---- a degenerate range costs zero bits, per STANDARD.md ----

            At EVERY width. The wide operations get their span from a 128-bit
            subtraction whose bit length is then taken, and a span of zero is
            the one input a count-leading-zeros instruction is undefined on --
            so this is the case an implementation reaching for that instruction
            gets wrong, silently, in exactly one place. */
    {
        serialize_measure_stream_t m;
        serialize_int32_t v = 0;
        serialize_int64_t v64 = 0;
        serialize_int128_t v128 = serialize_int128_from_int64( 42 );
        serialize_int128_t out128;
        serialize_int32_t f32 = 0;
        serialize_int64_t f64 = 0;
        serialize_int128_t f128;

        serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
        serialize_measure_stream_init( &m );
        CHECK( serialize_write_int( &w, 5, 5, 5 ) );                            CHECK( serialize_measure_int( &m, 5, 5 ) );
        CHECK( serialize_write_int64( &w, 7, 7, 7 ) );                          CHECK( serialize_measure_int64( &m, 7, 7 ) );
        CHECK( serialize_write_int128( &w, v128, v128, v128 ) );                CHECK( serialize_measure_int128( &m, v128, v128 ) );
        CHECK( serialize_write_fixed32( &w, 5 * 65536, 16, 16, 5, 5 ) );        CHECK( serialize_measure_fixed32( &m, 16, 16, 5, 5 ) );
        CHECK( serialize_write_fixed64( &w, 7LL * 65536, 48, 16, 7, 7 ) );      CHECK( serialize_measure_fixed64( &m, 48, 16, 7, 7 ) );
        CHECK( serialize_write_fixed128( &w, serialize_int128_from_int64( 9LL * 65536 ), 112, 16, 9, 9 ) );
        CHECK( serialize_measure_fixed128( &m, 112, 16, 9, 9 ) );
        CHECK( !serialize_write_error( &w ) );
        CHECK( serialize_write_bits_processed( &w ) == 0 );
        CHECK( serialize_measure_bits_processed( &m ) == 0 );
        serialize_write_flush( &w );

        serialize_read_stream_init( &r, buffer, 8 );
        CHECK( serialize_read_int( &r, &v, 5, 5 ) );                            CHECK( v == 5 );
        CHECK( serialize_read_int64( &r, &v64, 7, 7 ) );                        CHECK( v64 == 7 );
        CHECK( serialize_read_int128( &r, &out128, v128, v128 ) );              CHECK( serialize_int128_equal( out128, v128 ) );
        CHECK( serialize_read_fixed32( &r, &f32, 16, 16, 5, 5 ) );              CHECK( f32 == 5 * 65536 );
        CHECK( serialize_read_fixed64( &r, &f64, 48, 16, 7, 7 ) );              CHECK( f64 == 7LL * 65536 );
        CHECK( serialize_read_fixed128( &r, &f128, 112, 16, 9, 9 ) );
        CHECK( serialize_int128_equal( f128, serialize_int128_from_int64( 9LL * 65536 ) ) );
        CHECK( !serialize_read_error( &r ) );
        CHECK( serialize_read_bits_processed( &r ) == 0 );
    }

    /* ---- failure is sticky whatever caused it ----

            The bit limit is what every operation tests against, and failing a
            stream poisons it -- which is what makes one comparison do the work
            of two. A failure path that set the error flag and left the limit
            alone would leave a stream that reports failure and keeps reading,
            so each KIND of failure is checked here, not just running out of
            buffer. */
    {
        serialize_int32_t v = 0;
        serialize_uint32_t raw = 0;
        serialize_uint8_t u8 = 0;

        /* a value out of its declared range */
        serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
        serialize_write_bits( &w, 127, 7 );                  /* outside [0,100] */
        serialize_write_bits( &w, 0xAB, 8 );                 /* and a valid field behind it */
        serialize_write_flush( &w );
        serialize_read_stream_init( &r, buffer, serialize_write_bytes_processed( &w ) );
        CHECK( !serialize_read_int( &r, &v, 0, 100 ) );
        CHECK( serialize_read_error( &r ) );
        CHECK( !serialize_read_bits( &r, &raw, 8 ) );        /* the field behind it must NOT be readable */
        CHECK( !serialize_read_uint8( &r, &u8 ) );
        CHECK( !serialize_read_int( &r, &v, 0, 255 ) );

        /* nonzero align padding */
        serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
        serialize_write_bits( &w, 1, 1 );
        serialize_write_bits( &w, 0x7F, 7 );
        serialize_write_bits( &w, 0xCD, 8 );
        serialize_write_flush( &w );
        serialize_read_stream_init( &r, buffer, serialize_write_bytes_processed( &w ) );
        CHECK( serialize_read_bits( &r, &raw, 1 ) );
        CHECK( !serialize_read_align( &r ) );
        CHECK( serialize_read_error( &r ) );
        CHECK( !serialize_read_bits( &r, &raw, 8 ) );

        /* including the operations that would otherwise do nothing at all: a
           zero length bytes on a failed stream is still a failure, and used
           not to be, because it reached neither a read nor an align that
           would have noticed */
        {
            serialize_uint8_t none[1];
            CHECK( !serialize_read_bytes( &r, none, 0 ) );
            CHECK( !serialize_read_align( &r ) );
        }

        /* and on the write side: a write that does not fit stops every later
           write, including one that would have fitted */
        serialize_write_stream_init( &w, buffer, 4 );        /* 32 bits of room */
        CHECK( serialize_write_bits( &w, 0xFFFFFF, 24 ) );
        CHECK( !serialize_write_bits( &w, 0, 16 ) );         /* 8 bits past the end */
        CHECK( serialize_write_error( &w ) );
        CHECK( !serialize_write_bits( &w, 1, 1 ) );          /* would have fitted */
        CHECK( !serialize_write_int( &w, 5, 0, 10 ) );
        CHECK( !serialize_write_bool( &w, 1 ) );
        CHECK( serialize_write_bits_processed( &w ) == 24 );
    }

    /* ---- the measure stream agrees with the writer, ONE OPERATION AT A TIME.

            There is a measure counterpart for every write operation, and the
            README says so; this is what makes that true rather than claimed.
            The variable-width ones matter most -- int_relative picks its tier
            from the difference and wstring its length field from the buffer
            size, so neither can be worked around with measure_bits by a
            caller who does not want to reimplement the ladder. ---- */
    {
        serialize_measure_stream_t m;
        static const serialize_uint8_t src[3] = { 9, 8, 7 };
        serialize_int128_t i128lo = serialize_int128_from_int64( -2000000000000LL );
        serialize_int128_t i128hi = serialize_int128_from_int64( 2000000000000LL );
        serialize_int128_t widelo = serialize_int128_make( 0xFFFFFFF000000000ULL, 0x0000000000000000ULL );
        serialize_int128_t widehi = serialize_int128_make( 0x0000001000000000ULL, 0x0000000000000000ULL );
        serialize_int128_t wideval = serialize_int128_make( 0x0000000F23456789ULL, 0xABCDEF0123456789ULL );
        wchar_t ws[8];
        int i;

        ws[0] = 0x043C; ws[1] = 0x0438; ws[2] = 0x0440; ws[3] = 0;

        MEASURED( serialize_write_bits( &w, 5, 3 ),  serialize_measure_bits( &m, 3 ) );
        MEASURED( serialize_write_bool( &w, 1 ),     serialize_measure_bool( &m ) );
        MEASURED( serialize_write_align( &w ),       serialize_measure_align( &m ) );

        MEASURED( serialize_write_int( &w, 42, 0, 1000 ),    serialize_measure_int( &m, 0, 1000 ) );
        MEASURED( serialize_write_int( &w, -7, -100, 100 ),  serialize_measure_int( &m, -100, 100 ) );
        MEASURED( serialize_write_int( &w, 5, 5, 5 ),        serialize_measure_int( &m, 5, 5 ) );

        MEASURED( serialize_write_int64( &w, -1234567890123LL, -2000000000000LL, 2000000000000LL ),
                  serialize_measure_int64( &m, -2000000000000LL, 2000000000000LL ) );
        MEASURED( serialize_write_int64( &w, 3, 0, 7 ), serialize_measure_int64( &m, 0, 7 ) );

        MEASURED( serialize_write_uint8( &w, 0xAB ),        serialize_measure_uint8( &m ) );
        MEASURED( serialize_write_uint16( &w, 0xBEEF ),     serialize_measure_uint16( &m ) );
        MEASURED( serialize_write_uint32( &w, 0xDEADBEEF ), serialize_measure_uint32( &m ) );
        MEASURED( serialize_write_uint64( &w, 0x0123456789ABCDEFULL ), serialize_measure_uint64( &m ) );

        MEASURED( serialize_write_float( &w, 3.1415926f ), serialize_measure_float( &m ) );
        MEASURED( serialize_write_double( &w, 1.0 / 3.0 ), serialize_measure_double( &m ) );
        MEASURED( serialize_write_compressed_float( &w, 0.75f, 0.0f, 1.0f, 0.01f ),
                  serialize_measure_compressed_float( &m, 0.0f, 1.0f, 0.01f ) );
        MEASURED( serialize_write_compressed_float( &w, -3.5f, -10.0f, 10.0f, 0.001f ),
                  serialize_measure_compressed_float( &m, -10.0f, 10.0f, 0.001f ) );

        MEASURED( serialize_write_bytes( &w, src, 3 ),  serialize_measure_bytes( &m, 3 ) );
        MEASURED( serialize_write_bytes( &w, src, 0 ),  serialize_measure_bytes( &m, 0 ) );
        MEASURED( serialize_write_string( &w, "the quick brown fox", 64 ),
                  serialize_measure_string( &m, "the quick brown fox", 64 ) );
        MEASURED( serialize_write_string( &w, "", 64 ), serialize_measure_string( &m, "", 64 ) );

        MEASURED( serialize_write_wstring( &w, ws, 8 ),  serialize_measure_wstring( &m, ws, 8 ) );
        ws[0] = 0;
        MEASURED( serialize_write_wstring( &w, ws, 8 ),  serialize_measure_wstring( &m, ws, 8 ) );
        ws[0] = 0x043C;

        MEASURED( serialize_write_uint128( &w, serialize_uint128_make( 0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL ) ),
                  serialize_measure_uint128( &m ) );
        MEASURED( serialize_write_int128( &w, serialize_int128_from_int64( -1234567890123LL ), i128lo, i128hi ),
                  serialize_measure_int128( &m, i128lo, i128hi ) );
        /* a span wider than 64 bits, so the count covers the high lane too */
        MEASURED( serialize_write_int128( &w, wideval, widelo, widehi ),
                  serialize_measure_int128( &m, widelo, widehi ) );

        MEASURED( serialize_write_fixed32( &w, 100 * 65536, 16, 16, -180, 180 ),
                  serialize_measure_fixed32( &m, 16, 16, -180, 180 ) );
        MEASURED( serialize_write_fixed32( &w, 12345, 32, 0, 0, 1000000 ),
                  serialize_measure_fixed32( &m, 32, 0, 0, 1000000 ) );
        MEASURED( serialize_write_fixed64( &w, -5000LL * 65536, 48, 16, -1000000, 1000000 ),
                  serialize_measure_fixed64( &m, 48, 16, -1000000, 1000000 ) );
        MEASURED( serialize_write_fixed128( &w, serialize_int128_from_int64( 777LL * 65536 ), 112, 16, -1000000, 1000000 ),
                  serialize_measure_fixed128( &m, 112, 16, -1000000, 1000000 ) );
        MEASURED( serialize_write_fixed128( &w, serialize_int128_from_int64( -12345LL * ( (serialize_int64_t) 1 << 48 ) ), 80, 48, -1000000, 1000000 ),
                  serialize_measure_fixed128( &m, 80, 48, -1000000, 1000000 ) );

        /* every int_relative tier, including the raw fallback */
        {
            static const serialize_int32_t deltas[] = { 1, 2, 6, 7, 23, 24, 280, 281, 4377, 4378, 69914, 69915, 1000000 };
            for ( i = 0; i < (int) ( sizeof( deltas ) / sizeof( deltas[0] ) ); i++ )
            {
                MEASURED( serialize_write_int_relative( &w, 1000, 1000 + deltas[i] ),
                          serialize_measure_int_relative( &m, 1000, 1000 + deltas[i] ) );
            }
        }

        /* a whole message at once, so nothing that depends on the position
           within the byte can agree operation by operation and still drift
           over a sequence */
        serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
        serialize_measure_stream_init( &m );
        CHECK( serialize_write_bits( &w, 5, 3 ) );            CHECK( serialize_measure_bits( &m, 3 ) );
        CHECK( serialize_write_bool( &w, 1 ) );               CHECK( serialize_measure_bool( &m ) );
        CHECK( serialize_write_int( &w, 42, 0, 1000 ) );      CHECK( serialize_measure_int( &m, 0, 1000 ) );
        CHECK( serialize_write_int_relative( &w, 100, 400 ) ); CHECK( serialize_measure_int_relative( &m, 100, 400 ) );
        CHECK( serialize_write_align( &w ) );                 CHECK( serialize_measure_align( &m ) );
        CHECK( serialize_write_bytes( &w, src, 3 ) );         CHECK( serialize_measure_bytes( &m, 3 ) );
        CHECK( serialize_write_string( &w, "fox", 64 ) );     CHECK( serialize_measure_string( &m, "fox", 64 ) );
        CHECK( serialize_write_wstring( &w, ws, 8 ) );        CHECK( serialize_measure_wstring( &m, ws, 8 ) );
        CHECK( serialize_write_fixed64( &w, 65536, 48, 16, -1000000, 1000000 ) );
        CHECK( serialize_measure_fixed64( &m, 48, 16, -1000000, 1000000 ) );
        CHECK( serialize_write_compressed_float( &w, 0.5f, 0.0f, 1.0f, 0.01f ) );
        CHECK( serialize_measure_compressed_float( &m, 0.0f, 1.0f, 0.01f ) );
        CHECK( !serialize_write_error( &w ) );
        CHECK( serialize_write_bits_processed( &w ) == serialize_measure_bits_processed( &m ) );
        CHECK( serialize_write_bytes_processed( &w ) == serialize_measure_bytes_processed( &m ) );

        /* measure refuses what the writer would refuse, and counts nothing
           when it does -- a count the writer could never produce is the one
           way a measure stream can do real damage */
        {
            serialize_measure_stream_init( &m );
            CHECK( !serialize_measure_string( &m, "too long for its buffer", 8 ) );
            CHECK( !serialize_measure_wstring( &m, ws, 3 ) );
            CHECK( !serialize_measure_int_relative( &m, 100, 100 ) );
            CHECK( !serialize_measure_int_relative( &m, 100, 99 ) );
            CHECK( !serialize_measure_int128( &m, i128hi, i128lo ) );
            CHECK( serialize_measure_bits_processed( &m ) == 0 );
        }
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

    /* ---- NaN through compressed float writes as min: the family behavior.
       C++, C#, Go and Rust all clamp with the !>= form, which is false for
       NaN, so a NaN value is forced to normalized = 0, the wire carries
       integer 0, and the reader reconstructs min exactly. The bytes must be
       identical to writing min itself. The NaN is built from its bit pattern
       rather than any NAN macro so ubsan actually exercises this path and
       finite-math builds still compile. */
    {
        static const serialize_uint32_t nan_patterns[2] = { 0x7FC00000UL, 0xFFC00000UL };  /* quiet NaN, and its negative */
        serialize_uint8_t nan_buffer[16];
        serialize_uint8_t min_buffer[16];
        serialize_write_stream_t w2;
        float nan_value;
        float out;
        int i;

        for ( i = 0; i < 2; i++ )
        {
            memset( nan_buffer, 0, sizeof( nan_buffer ) );
            memset( min_buffer, 0, sizeof( min_buffer ) );
            memcpy( &nan_value, &nan_patterns[i], 4 );

            serialize_write_stream_init( &w, nan_buffer, sizeof( nan_buffer ) );
            CHECK( serialize_write_compressed_float( &w, nan_value, 0.0f, 10.0f, 0.01f ) );
            serialize_write_flush( &w );
            CHECK( !serialize_write_error( &w ) );

            serialize_write_stream_init( &w2, min_buffer, sizeof( min_buffer ) );
            CHECK( serialize_write_compressed_float( &w2, 0.0f, 0.0f, 10.0f, 0.01f ) );
            serialize_write_flush( &w2 );
            CHECK( !serialize_write_error( &w2 ) );

            CHECK( serialize_write_bits_processed( &w ) == serialize_write_bits_processed( &w2 ) );
            CHECK( memcmp( nan_buffer, min_buffer, sizeof( nan_buffer ) ) == 0 );

            out = 42.0f;
            serialize_read_stream_init( &r, nan_buffer, serialize_write_bytes_processed( &w ) );
            CHECK( serialize_read_compressed_float( &r, &out, 0.0f, 10.0f, 0.01f ) );
            CHECK( out == 0.0f );       /* integer 0 reconstructs to min exactly */
        }
    }

    printf( failed ? "FAILED\n" : "OK\n" );
    return failed;
}
