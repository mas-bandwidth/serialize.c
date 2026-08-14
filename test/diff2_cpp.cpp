// The C++ twin for the wide operations. Like diff_cpp.cpp it also checks its
// own output against the vector test/golden.c pins, so the pin is agreed by
// two independent implementations rather than recorded from one.
#include "serialize.h"
#include "vectors.h"
#include <stdio.h>
#include <string.h>

template <typename Stream> bool write_wide( Stream & stream )
{
    serialize::uint128_t u128 = ( serialize::uint128_t( 0x0123456789ABCDEFULL ) << 64 ) | serialize::uint128_t( 0xFEDCBA9876543210ULL );
    serialize::int128_t i128 = serialize::int128_t( -1234567890123LL );
    serialize::int128_t i128min = serialize::int128_t( -2000000000000LL );
    serialize::int128_t i128max = serialize::int128_t( 2000000000000LL );
    int32_t f32 = 100 * 65536;
    int64_t f64 = -5000LL * 65536;
    serialize::int128_t f128 = serialize::int128_t( 777LL * 65536 );
    int32_t fticks = 12345;

    // the span here is wider than 64 bits, so the offset reaches the high lane
    serialize::int128_t wide = ( serialize::int128_t( serialize::uint128_t( 0x0000000F23456789ULL ) << 64 ) ) | serialize::int128_t( serialize::uint128_t( 0xABCDEF0123456789ULL ) );
    serialize::int128_t widemin = -( serialize::int128_t( 1 ) << 100 );
    serialize::int128_t widemax =  ( serialize::int128_t( 1 ) << 100 );
    serialize::int128_t f128wide = serialize::int128_t( -12345LL * ( int64_t( 1 ) << 48 ) );

    wchar_t ws[8];
    ws[0] = 0x043C; ws[1] = 0x0438; ws[2] = 0x0440; ws[3] = 0;

    serialize_uint128( stream, u128 );
    serialize_int128( stream, i128, i128min, i128max );
    serialize_fixed( stream, f32, 16, 16, -180, 180 );
    serialize_fixed( stream, f64, 48, 16, -1000000, 1000000 );
    serialize_fixed( stream, f128, 112, 16, -1000000, 1000000 );
    serialize_fixed( stream, fticks, 32, 0, 0, 1000000 );
    serialize_int128( stream, wide, widemin, widemax );
    serialize_fixed( stream, f128wide, 80, 48, -1000000, 1000000 );
    serialize_wstring( stream, ws, 8 );
    return true;
}

int main()
{
    static uint8_t buffer[1024];
    serialize::WriteStream stream( buffer, sizeof( buffer ) );
    if ( !write_wide( stream ) ) { printf( "WRITE ERROR\n" ); return 1; }
    stream.Flush();
    const int n = stream.GetBytesProcessed();
    char hex[4096];
    hex[0] = '\0';
    for ( int i = 0; i < n; i++ ) snprintf( hex + i * 2, 3, "%02x", buffer[i] );
    if ( strcmp( hex, SERIALIZE_GOLDEN_WIDE ) != 0 )
    {
        fprintf( stderr, "GOLDEN MISMATCH: the C++ library disagrees with the pinned wide vector\n" );
        fprintf( stderr, "  pinned %s\n", SERIALIZE_GOLDEN_WIDE );
        fprintf( stderr, "  C++    %s\n", hex );
        return 1;
    }
    printf( "%d bytes\n", n );
    printf( "%s\n", hex );
    return 0;
}
