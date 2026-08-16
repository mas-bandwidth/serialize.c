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
    measure stream, from the same starting point, and requires the measured
    count to be write + surplus EXACTLY, where surplus is the operation's
    align worst case minus the padding actually written here.

    A measure is a CONSERVATIVE bound, not an exact size (the fork ruling:
    "measure must be large enough to serialize the message but doesn't need
    to be exact", because "align is going to be different in 1st and 2nd
    times serialize is called. it is bit position dependent") -- align
    charges 7 bits wherever it appears, like the C++ MeasureStream. So a
    position-independent operation must still measure EXACTLY (surplus 0 --
    an overcount there is drift, and drift in a measure sizes every buffer
    wrong), an align-bearing one measures its pinned worst case, and in
    particular the measure is NEVER BELOW the write: an under-count sizes a
    buffer that then overflows.

    The 3-bit prelude is not decoration. align, bytes, string -- everything
    that pads -- costs a number of bits that depends on where in the byte the
    operation begins, and an exact-from-zero measure under-counts at exactly
    these unaligned starts. This prelude is what showed it.
*/
#define MEASURED(w_op, m_op, surplus) do {                                                       \
        serialize_write_stream_init( &w, buffer, sizeof( buffer ) );                              \
        serialize_measure_stream_init( &m );                                                      \
        CHECK( serialize_write_bits( &w, 5, 3 ) );                                                \
        CHECK( serialize_measure_bits( &m, 3 ) );                                                 \
        CHECK( w_op );                                                                            \
        CHECK( m_op );                                                                            \
        CHECK( !serialize_write_error( &w ) );                                                     \
        CHECK( serialize_measure_bits_processed( &m ) >= serialize_write_bits_processed( &w ) );  \
        CHECK( serialize_measure_bits_processed( &m ) == serialize_write_bits_processed( &w ) + (surplus) ); \
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
        /* negative degenerate bounds: the raw min is negative, and the reader
           must still recover it exactly from the range alone */
        CHECK( serialize_write_fixed64( &w, -7LL * 65536, 48, 16, -7, -7 ) );   CHECK( serialize_measure_fixed64( &m, 48, 16, -7, -7 ) );
        CHECK( serialize_write_fixed128( &w, serialize_int128_from_int64( -9LL * 65536 ), 112, 16, -9, -9 ) );
        CHECK( serialize_measure_fixed128( &m, 112, 16, -9, -9 ) );
        /* Q64.64 over min == max == 0: the fraction alone spans a full 64-bit
           field, so a wide path that cost fraction_bits of zeros -- instead of
           the zero bits STANDARD.md requires on every storage width -- would
           show here as 64 bits */
        CHECK( serialize_write_fixed128( &w, serialize_int128_from_int64( 0 ), 64, 64, 0, 0 ) );
        CHECK( serialize_measure_fixed128( &m, 64, 64, 0, 0 ) );
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
        CHECK( serialize_read_fixed64( &r, &f64, 48, 16, -7, -7 ) );            CHECK( f64 == -7LL * 65536 );
        CHECK( serialize_read_fixed128( &r, &f128, 112, 16, -9, -9 ) );
        CHECK( serialize_int128_equal( f128, serialize_int128_from_int64( -9LL * 65536 ) ) );
        f128 = serialize_int128_from_int64( 1 );        /* a wrong value, so recovery is observable */
        CHECK( serialize_read_fixed128( &r, &f128, 64, 64, 0, 0 ) );
        CHECK( serialize_int128_equal( f128, serialize_int128_from_int64( 0 ) ) );
        CHECK( !serialize_read_error( &r ) );
        CHECK( serialize_read_bits_processed( &r ) == 0 );
    }

    /* ---- the quantization ceiling lives in the unsigned domain ----

            The compressed float ceiling clamps at 4294967040 quantization
            steps, above INT32_MAX. The width computation must stay in the
            unsigned domain the whole way: narrowing the ceiling through a
            signed parameter is implementation-defined at this library's C89
            floor, and on two's-complement machines it happened to produce the
            right width -- which is exactly the kind of accident that holds
            until the compiler changes. The clamped field is 32 bits, and a
            value must survive the round trip through it. */
    {
        serialize_measure_stream_t m;
        float out = 0.0f;

        CHECK( serialize_bits_required( 0, 4294967040u ) == 32 );
        CHECK( serialize_bits_required( 0, 0x7FFFFFFFu ) == 31 );

        serialize_measure_stream_init( &m );
        CHECK( serialize_measure_compressed_float( &m, 0.0f, 1.0e9f, 0.0001f ) );
        CHECK( serialize_measure_bits_processed( &m ) == 32 );

        serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
        CHECK( serialize_write_compressed_float( &w, 123456.0f, 0.0f, 1.0e9f, 0.0001f ) );
        CHECK( !serialize_write_error( &w ) );
        CHECK( serialize_write_bits_processed( &w ) == 32 );
        serialize_write_flush( &w );

        serialize_read_stream_init( &r, buffer, serialize_write_bytes_processed( &w ) );
        CHECK( serialize_read_compressed_float( &r, &out, 0.0f, 1.0e9f, 0.0001f ) );
        CHECK( !serialize_read_error( &r ) );
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

        /* The write side has no counterpart to any of this: writes are
           trusted and perform no release-build checks at all (issue #52) —
           a write past the end of the buffer is caller error, asserted in a
           debug build like every other writer contract, so there is no
           return value to observe here. test/assertdeath.c is where that
           contract is proven to fire. */
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

        MEASURED( serialize_write_bits( &w, 5, 3 ),  serialize_measure_bits( &m, 3 ), 0 );
        MEASURED( serialize_write_bool( &w, 1 ),     serialize_measure_bool( &m ), 0 );
        /* the named before/after case for the fork ruling: align at bit 3
           pads 5, so this used to measure exactly (8 == 8) and now measures
           the pinned worst case of 7 (10 == 8 + 2) */
        MEASURED( serialize_write_align( &w ),       serialize_measure_align( &m ), 2 );

        MEASURED( serialize_write_int( &w, 42, 0, 1000 ),    serialize_measure_int( &m, 0, 1000 ), 0 );
        MEASURED( serialize_write_int( &w, -7, -100, 100 ),  serialize_measure_int( &m, -100, 100 ), 0 );
        MEASURED( serialize_write_int( &w, 5, 5, 5 ),        serialize_measure_int( &m, 5, 5 ), 0 );

        MEASURED( serialize_write_int64( &w, -1234567890123LL, -2000000000000LL, 2000000000000LL ),
                  serialize_measure_int64( &m, -2000000000000LL, 2000000000000LL ), 0 );
        MEASURED( serialize_write_int64( &w, 3, 0, 7 ), serialize_measure_int64( &m, 0, 7 ), 0 );

        MEASURED( serialize_write_uint8( &w, 0xAB ),        serialize_measure_uint8( &m ), 0 );
        MEASURED( serialize_write_uint16( &w, 0xBEEF ),     serialize_measure_uint16( &m ), 0 );
        MEASURED( serialize_write_uint32( &w, 0xDEADBEEF ), serialize_measure_uint32( &m ), 0 );
        MEASURED( serialize_write_uint64( &w, 0x0123456789ABCDEFULL ), serialize_measure_uint64( &m ), 0 );

        MEASURED( serialize_write_float( &w, 3.1415926f ), serialize_measure_float( &m ), 0 );
        MEASURED( serialize_write_double( &w, 1.0 / 3.0 ), serialize_measure_double( &m ), 0 );
        MEASURED( serialize_write_compressed_float( &w, 0.75f, 0.0f, 1.0f, 0.01f ),
                  serialize_measure_compressed_float( &m, 0.0f, 1.0f, 0.01f ), 0 );
        MEASURED( serialize_write_compressed_float( &w, -3.5f, -10.0f, 10.0f, 0.001f ),
                  serialize_measure_compressed_float( &m, -10.0f, 10.0f, 0.001f ), 0 );

        /* bytes aligns first: at bit 3 the write pads 5, the measure charges 7 */
        MEASURED( serialize_write_bytes( &w, src, 3 ),  serialize_measure_bytes( &m, 3 ), 2 );
        MEASURED( serialize_write_bytes( &w, src, 0 ),  serialize_measure_bytes( &m, 0 ), 2 );
        /* string: the 6-bit length field lands the align at bit 9, where the
           actual padding IS the worst case 7 -- the bound is tight here */
        MEASURED( serialize_write_string( &w, "the quick brown fox", 64 ),
                  serialize_measure_string( &m, "the quick brown fox", 64 ), 0 );
        MEASURED( serialize_write_string( &w, "", 64 ), serialize_measure_string( &m, "", 64 ), 0 );

        /* wstring deliberately never aligns, so it must still measure EXACTLY */
        MEASURED( serialize_write_wstring( &w, ws, 8 ),  serialize_measure_wstring( &m, ws, 8 ), 0 );
        ws[0] = 0;
        MEASURED( serialize_write_wstring( &w, ws, 8 ),  serialize_measure_wstring( &m, ws, 8 ), 0 );
        ws[0] = 0x043C;

        MEASURED( serialize_write_uint128( &w, serialize_uint128_make( 0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL ) ),
                  serialize_measure_uint128( &m ), 0 );
        MEASURED( serialize_write_int128( &w, serialize_int128_from_int64( -1234567890123LL ), i128lo, i128hi ),
                  serialize_measure_int128( &m, i128lo, i128hi ), 0 );
        /* a span wider than 64 bits, so the count covers the high lane too */
        MEASURED( serialize_write_int128( &w, wideval, widelo, widehi ),
                  serialize_measure_int128( &m, widelo, widehi ), 0 );

        MEASURED( serialize_write_fixed32( &w, 100 * 65536, 16, 16, -180, 180 ),
                  serialize_measure_fixed32( &m, 16, 16, -180, 180 ), 0 );
        MEASURED( serialize_write_fixed32( &w, 12345, 32, 0, 0, 1000000 ),
                  serialize_measure_fixed32( &m, 32, 0, 0, 1000000 ), 0 );
        MEASURED( serialize_write_fixed64( &w, -5000LL * 65536, 48, 16, -1000000, 1000000 ),
                  serialize_measure_fixed64( &m, 48, 16, -1000000, 1000000 ), 0 );
        MEASURED( serialize_write_fixed128( &w, serialize_int128_from_int64( 777LL * 65536 ), 112, 16, -1000000, 1000000 ),
                  serialize_measure_fixed128( &m, 112, 16, -1000000, 1000000 ), 0 );
        MEASURED( serialize_write_fixed128( &w, serialize_int128_from_int64( -12345LL * ( (serialize_int64_t) 1 << 48 ) ), 80, 48, -1000000, 1000000 ),
                  serialize_measure_fixed128( &m, 80, 48, -1000000, 1000000 ), 0 );

        /* every int_relative tier, including the raw fallback */
        {
            static const serialize_int32_t deltas[] = { 1, 2, 6, 7, 23, 24, 280, 281, 4377, 4378, 69914, 69915, 1000000 };
            for ( i = 0; i < (int) ( sizeof( deltas ) / sizeof( deltas[0] ) ); i++ )
            {
                MEASURED( serialize_write_int_relative( &w, 1000, 1000 + deltas[i] ),
                          serialize_measure_int_relative( &m, 1000, 1000 + deltas[i] ), 0 );
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
        /* never below the write -- the guarantee a fits-check depends on --
           and above it by exactly the three aligns' worst-case surplus:
           the standalone align lands at bit 32 and pads 0 (surplus 7), the
           bytes align likewise (surplus 7), the string align lands at bit
           62 and pads 2 (surplus 5). 7 + 7 + 5 = 19 bits over. */
        CHECK( serialize_measure_bits_processed( &m ) >= serialize_write_bits_processed( &w ) );
        CHECK( serialize_measure_bits_processed( &m ) == serialize_write_bits_processed( &w ) + 19 );
        CHECK( serialize_measure_bytes_processed( &m ) >= serialize_write_bytes_processed( &w ) );

        /* measure carries the writer's contracts as debug asserts and
           nothing else — a string that does not fit, an int_relative that
           does not increase, inverted 128-bit bounds are all caller error,
           asserted like every other writer contract (issue #52) and checked
           by nothing in a release build, exactly like the C++ MeasureStream.
           test/assertdeath.c proves each of those asserts fires. */
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

    /* ---- wstring transmits UTF-16 CODE UNITS: an astral code point is a
            surrogate pair on the wire, and 2- and 4-byte wchar_t platforms
            produce IDENTICAL bytes -- the 4-byte platform converts at the
            boundary (STANDARD.md, adopted 2026-08-15). The input is built per
            the local width, the expected bytes are the code-unit stream
            spelled out directly, and the two must agree everywhere. ---- */
    {
        const int wide_wchar = sizeof( wchar_t ) >= 4 ? 1 : 0;
        serialize_uint8_t expected_buffer[64];
        serialize_write_stream_t we;
        serialize_measure_stream_t m;
        serialize_uint32_t astral = 0x1F600u;           /* U+1F600, the pair 0xD83D 0xDE00 */
        wchar_t ws_in[8];
        wchar_t ws_out[8];
        int n;

        if ( wide_wchar )
        {
            ws_in[0] = (wchar_t) astral;                /* one character, two units */
            ws_in[1] = (wchar_t) 0x0041;
            ws_in[2] = 0;
        }
        else
        {
            ws_in[0] = (wchar_t) 0xD83Du;               /* already the pair: 2-byte wchar_t IS UTF-16 */
            ws_in[1] = (wchar_t) 0xDE00u;
            ws_in[2] = (wchar_t) 0x0041;
            ws_in[3] = 0;
        }

        /* the wire, spelled out: three units in a [0,7] length field, then
           each unit as a 32-bit group */
        serialize_write_stream_init( &we, expected_buffer, sizeof( expected_buffer ) );
        CHECK( serialize_write_int( &we, 3, 0, 7 ) );
        CHECK( serialize_write_bits( &we, 0xD83Du, 32 ) );
        CHECK( serialize_write_bits( &we, 0xDE00u, 32 ) );
        CHECK( serialize_write_bits( &we, 0x0041u, 32 ) );
        serialize_write_flush( &we );
        CHECK( !serialize_write_error( &we ) );

        serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
        CHECK( serialize_write_wstring( &w, ws_in, 8 ) );
        serialize_write_flush( &w );
        CHECK( !serialize_write_error( &w ) );
        n = serialize_write_bytes_processed( &w );
        CHECK( n == serialize_write_bytes_processed( &we ) );
        CHECK( memcmp( buffer, expected_buffer, (size_t) n ) == 0 );

        /* the measure counts the units transmitted, not the characters held */
        serialize_measure_stream_init( &m );
        CHECK( serialize_measure_wstring( &m, ws_in, 8 ) );
        CHECK( serialize_measure_bits_processed( &m ) == serialize_write_bits_processed( &w ) );

        /* and the read arrives back at the platform's own representation:
           recombined on 4-byte wchar_t, the pair itself on 2-byte */
        serialize_read_stream_init( &r, buffer, n );
        CHECK( serialize_read_wstring( &r, ws_out, 8 ) );
        CHECK( !serialize_read_error( &r ) );
        if ( wide_wchar )
        {
            CHECK( ws_out[0] == ws_in[0] && ws_out[1] == ws_in[1] && ws_out[2] == 0 );
        }
        else
        {
            CHECK( ws_out[0] == ws_in[0] && ws_out[1] == ws_in[1] && ws_out[2] == ws_in[2] && ws_out[3] == 0 );
        }
    }

    /* ---- the read path refuses malformed string CONTENT (ruling #8,
            adopted 2026-08-15): (a) invalid UTF-8, (b) an interior NUL in a
            narrow string, (c) unpaired/invalid surrogates in a wide string,
            (d) an interior zero unit in a wide string.

            Every stream here is DOCTORED through the primitives -- write_int
            for the length, then write_bytes/write_bits for the payload --
            because write_string/write_wstring debug-assert these contracts
            away and could never produce the bytes. The wire shape is exactly
            what the reader parses; only the content is hostile. And every
            refusal class is fenced by an accepted neighbor as close to the
            boundary as the format allows: a vector that cannot fail is not
            evidence, and a refusal test that over-refuses is a different
            bug. ---- */
    {
        char str_out[64];
        wchar_t ws_out[8];
        int k;

        /* (a) invalid UTF-8, one payload per malformation class. All are
           refused by the well-formedness definition, not by byte shape:
           the overlongs are correctly-shaped lead+continuation sequences
           and are the classic filter bypass. */
        {
            static const struct { unsigned char byte[4]; int length; } bad_utf8[] =
            {
                { { 0xC0, 0xAF, 0x00, 0x00 }, 2 },  /* overlong '/': the classic bypass */
                { { 0xE0, 0x80, 0x80, 0x00 }, 3 },  /* overlong NUL, 3-byte form */
                { { 0xF0, 0x80, 0x80, 0x80 }, 4 },  /* overlong, 4-byte form */
                { { 0xED, 0xA0, 0x80, 0x00 }, 3 },  /* U+D800: surrogate as UTF-8 */
                { { 0xED, 0xBF, 0xBF, 0x00 }, 3 },  /* U+DFFF: surrogate as UTF-8 */
                { { 0xF4, 0x90, 0x80, 0x80 }, 4 },  /* above U+10FFFF */
                { { 0xF5, 0x80, 0x80, 0x80 }, 4 },  /* lead byte can never appear */
                { { 0xE2, 0x82, 0x41, 0x00 }, 3 },  /* continuation replaced by ASCII */
                { { 0x41, 0xC3, 0x00, 0x00 }, 2 },  /* sequence truncated by the length */
                { { 0x80, 0x41, 0x00, 0x00 }, 2 }   /* bare continuation byte */
            };
            for ( k = 0; k < (int) ( sizeof( bad_utf8 ) / sizeof( bad_utf8[0] ) ); k++ )
            {
                serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
                CHECK( serialize_write_int( &w, bad_utf8[k].length, 0, 63 ) );
                CHECK( serialize_write_bytes( &w, bad_utf8[k].byte, bad_utf8[k].length ) );
                serialize_write_flush( &w );
                serialize_read_stream_init( &r, buffer, serialize_write_bytes_processed( &w ) );
                CHECK( !serialize_read_string( &r, str_out, 64 ) );
                CHECK( serialize_read_error( &r ) );
            }
        }

        /* the accepted fence for (a): the boundary code points on each side
           of every refusal above -- U+D7FF/U+E000 around the surrogate
           block, U+10FFFF itself, the shortest legal 2- and 4-byte forms --
           doctored through the same primitives, must round trip intact */
        {
            static const unsigned char good_utf8[] =
            {
                0x41,                       /* 'A' */
                0xC2, 0x80,                 /* U+0080: shortest legal 2-byte */
                0xED, 0x9F, 0xBF,           /* U+D7FF: last before the surrogate block */
                0xEE, 0x80, 0x80,           /* U+E000: first after it */
                0xF0, 0x90, 0x80, 0x80,     /* U+10000: shortest legal 4-byte */
                0xF4, 0x8F, 0xBF, 0xBF      /* U+10FFFF: the ceiling itself */
            };
            const int good_length = (int) sizeof( good_utf8 );
            serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
            CHECK( serialize_write_int( &w, good_length, 0, 63 ) );
            CHECK( serialize_write_bytes( &w, good_utf8, good_length ) );
            serialize_write_flush( &w );
            serialize_read_stream_init( &r, buffer, serialize_write_bytes_processed( &w ) );
            CHECK( serialize_read_string( &r, str_out, 64 ) );
            CHECK( !serialize_read_error( &r ) );
            CHECK( (int) strlen( str_out ) == good_length );
            CHECK( memcmp( str_out, good_utf8, (size_t) good_length ) == 0 );
        }

        /* (b) an interior NUL in a narrow string. Every byte is valid UTF-8
           -- NUL is U+0000 -- so this is the case the UTF-8 validator CANNOT
           catch, which is why it is a separate refusal: the wire says 3
           bytes, strlen would say 1, and that disagreement is the smuggling
           primitive. The fence is the same bytes with the NUL replaced. */
        {
            static const unsigned char nul_inside[3] = { 0x41, 0x00, 0x42 };    /* "A\0B" */
            static const unsigned char nul_fence[3]  = { 0x41, 0x20, 0x42 };    /* "A B"  */

            serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
            CHECK( serialize_write_int( &w, 3, 0, 63 ) );
            CHECK( serialize_write_bytes( &w, nul_inside, 3 ) );
            serialize_write_flush( &w );
            serialize_read_stream_init( &r, buffer, serialize_write_bytes_processed( &w ) );
            CHECK( !serialize_read_string( &r, str_out, 64 ) );
            CHECK( serialize_read_error( &r ) );

            serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
            CHECK( serialize_write_int( &w, 3, 0, 63 ) );
            CHECK( serialize_write_bytes( &w, nul_fence, 3 ) );
            serialize_write_flush( &w );
            serialize_read_stream_init( &r, buffer, serialize_write_bytes_processed( &w ) );
            CHECK( serialize_read_string( &r, str_out, 64 ) );
            CHECK( strcmp( str_out, "A B" ) == 0 );
        }

        /* (c) unpaired/invalid surrogates in a wide string, every ordering:
           high without its low, low with no high before it, a payload that
           ends inside a pair, and a group that is not a UTF-16 code unit at
           all -- refused on BOTH wchar_t widths now, not just where wchar_t
           cannot hold the value. */
        {
            static const struct { serialize_uint32_t unit[3]; int units; } bad_utf16[] =
            {
                { { 0xD83Du, 0x0041u, 0 }, 2 },     /* high surrogate, then a non-low */
                { { 0xDE00u, 0x0041u, 0 }, 2 },     /* low surrogate with no high */
                { { 0x0041u, 0xD83Du, 0 }, 2 },     /* payload ends inside a pair */
                { { 0xD83Du, 0xD83Du, 0 }, 2 },     /* high followed by another high */
                { { 0x110000u, 0, 0 },     1 },     /* not a UTF-16 code unit */
                { { 0xFFFFFFFFu, 0, 0 },   1 }      /* not a UTF-16 code unit */
            };
            int j;
            for ( k = 0; k < (int) ( sizeof( bad_utf16 ) / sizeof( bad_utf16[0] ) ); k++ )
            {
                serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
                CHECK( serialize_write_int( &w, bad_utf16[k].units, 0, 7 ) );
                for ( j = 0; j < bad_utf16[k].units; j++ )
                {
                    CHECK( serialize_write_bits( &w, bad_utf16[k].unit[j], 32 ) );
                }
                serialize_write_flush( &w );
                serialize_read_stream_init( &r, buffer, serialize_write_bytes_processed( &w ) );
                CHECK( !serialize_read_wstring( &r, ws_out, 8 ) );
                CHECK( serialize_read_error( &r ) );
            }
        }

        /* the accepted fence for (c): the boundary units around every
           refusal -- 0xD7FF and 0xE000 hugging the surrogate block, 0xFFFF
           the last code unit, and a correctly ordered pair -- doctored
           through the same primitives, must be accepted and arrive as the
           platform's own representation */
        {
            const int wide_wchar = sizeof( wchar_t ) >= 4 ? 1 : 0;
            serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
            CHECK( serialize_write_int( &w, 5, 0, 7 ) );
            CHECK( serialize_write_bits( &w, 0xD7FFu, 32 ) );
            CHECK( serialize_write_bits( &w, 0xE000u, 32 ) );
            CHECK( serialize_write_bits( &w, 0xFFFFu, 32 ) );
            CHECK( serialize_write_bits( &w, 0xD83Du, 32 ) );
            CHECK( serialize_write_bits( &w, 0xDE00u, 32 ) );
            serialize_write_flush( &w );
            serialize_read_stream_init( &r, buffer, serialize_write_bytes_processed( &w ) );
            CHECK( serialize_read_wstring( &r, ws_out, 8 ) );
            CHECK( !serialize_read_error( &r ) );
            CHECK( ws_out[0] == (wchar_t) 0xD7FF && ws_out[1] == (wchar_t) 0xE000 && ws_out[2] == (wchar_t) 0xFFFF );
            if ( wide_wchar )
            {
                CHECK( (serialize_uint32_t) ws_out[3] == 0x1F600u && ws_out[4] == 0 );
            }
            else
            {
                CHECK( ws_out[3] == (wchar_t) 0xD83Du && ws_out[4] == (wchar_t) 0xDE00u && ws_out[5] == 0 );
            }
        }

        /* (d) an interior zero unit in a wide string: the wire says 3
           units, wcslen would say 1 -- the same smuggling primitive as (b),
           in units instead of bytes. The fence is the same three units with
           the zero replaced. */
        {
            serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
            CHECK( serialize_write_int( &w, 3, 0, 7 ) );
            CHECK( serialize_write_bits( &w, 0x0041u, 32 ) );
            CHECK( serialize_write_bits( &w, 0x0000u, 32 ) );
            CHECK( serialize_write_bits( &w, 0x0042u, 32 ) );
            serialize_write_flush( &w );
            serialize_read_stream_init( &r, buffer, serialize_write_bytes_processed( &w ) );
            CHECK( !serialize_read_wstring( &r, ws_out, 8 ) );
            CHECK( serialize_read_error( &r ) );

            serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
            CHECK( serialize_write_int( &w, 3, 0, 7 ) );
            CHECK( serialize_write_bits( &w, 0x0041u, 32 ) );
            CHECK( serialize_write_bits( &w, 0x0020u, 32 ) );
            CHECK( serialize_write_bits( &w, 0x0042u, 32 ) );
            serialize_write_flush( &w );
            serialize_read_stream_init( &r, buffer, serialize_write_bytes_processed( &w ) );
            CHECK( serialize_read_wstring( &r, ws_out, 8 ) );
            CHECK( !serialize_read_error( &r ) );
            CHECK( ws_out[0] == (wchar_t) 0x41 && ws_out[1] == (wchar_t) 0x20 && ws_out[2] == (wchar_t) 0x42 && ws_out[3] == 0 );
        }

        /* refusal is the standard read-failure convention: sticky */
        {
            serialize_int32_t v = 0;
            serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
            CHECK( serialize_write_int( &w, 1, 0, 63 ) );
            { static const unsigned char bad[1] = { 0x80 };
              CHECK( serialize_write_bytes( &w, bad, 1 ) ); }
            CHECK( serialize_write_int( &w, 7, 0, 100 ) );
            serialize_write_flush( &w );
            serialize_read_stream_init( &r, buffer, serialize_write_bytes_processed( &w ) );
            CHECK( !serialize_read_string( &r, str_out, 64 ) );
            CHECK( !serialize_read_int( &r, &v, 0, 100 ) );         /* the in-range int behind it is unreachable */
            CHECK( serialize_read_error( &r ) );
        }
    }

    /* ---- valid string round trips through the PUBLIC writer are unchanged
            by the refusals: multi-byte UTF-8 through write_string, an astral
            pair through write_wstring (the latter also proven above and in
            the code-unit block) ---- */
    {
        static const char cafe[] = { (char) 0x63, (char) 0x61, (char) 0x66,
                                     (char) 0xC3, (char) 0xA9,                                  /* "café" */
                                     (char) 0x20,
                                     (char) 0xE2, (char) 0x82, (char) 0xAC,                     /* the euro sign */
                                     (char) 0x20,
                                     (char) 0xF0, (char) 0x9F, (char) 0x98, (char) 0x80,        /* U+1F600 */
                                     (char) 0x00 };
        char str_out[64];

        serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
        CHECK( serialize_write_string( &w, cafe, 64 ) );
        serialize_write_flush( &w );
        CHECK( !serialize_write_error( &w ) );
        serialize_read_stream_init( &r, buffer, serialize_write_bytes_processed( &w ) );
        CHECK( serialize_read_string( &r, str_out, 64 ) );
        CHECK( !serialize_read_error( &r ) );
        CHECK( strcmp( str_out, cafe ) == 0 );
    }

    /* ---- NaN through compressed float, in a RELEASE build only.

       A non-finite value is non-conforming input (serialize fork #6, the
       ruling: "attempting to send NaN or INF or anything else through
       compressed float is non-conforming and should assert out on write
       too"), so in a debug build this write ASSERTS — test/assertdeath.c
       proves that it does, and this block cannot run there. What survives
       under NDEBUG is the deterministic fallback: the clamp's !>= form is
       false for NaN, so the value is forced to normalized = 0, the wire
       carries integer 0, and the reader reconstructs min exactly — a
       misbehaving release writer produces the bytes of min, on every
       platform, rather than undefined bytes. The NaN is built from its bit
       pattern rather than any NAN macro so ubsan actually exercises this
       path and finite-math builds still compile. */
#ifdef NDEBUG
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
#endif /* NDEBUG */

    printf( failed ? "FAILED\n" : "OK\n" );
    return failed;
}
