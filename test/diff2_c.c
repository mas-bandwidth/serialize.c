/* Second differential: the wide operations — 128-bit, fixed point, wstring. */
#include <stdio.h>
#include "../serialize.h"

int main( void )
{
    static serialize_uint8_t buffer[1024];
    serialize_write_stream_t w;
    int i, n;
    wchar_t ws[8];

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

    /* Ranged 128-bit over a span WIDER THAN 64 BITS, and fixed128 likewise.
       Every case above fits its offset in the low lane, so the high lane of
       the two-lane emulation -- groups 2 and 3 of the 32-bit splitting -- is
       never reached by them. That is exactly where a byte order or lane
       mix-up would hide. */
    serialize_write_int128( &w,
        serialize_int128_make( 0x0000000F23456789ULL, 0xABCDEF0123456789ULL ),
        serialize_int128_make( 0xFFFFFFF000000000ULL, 0x0000000000000000ULL ),
        serialize_int128_make( 0x0000001000000000ULL, 0x0000000000000000ULL ) );
    serialize_write_fixed128( &w, serialize_int128_from_int64( -12345LL * ( (serialize_int64_t) 1 << 48 ) ), 80, 48, -1000000, 1000000 );

    serialize_write_wstring( &w, ws, 8 );

    serialize_write_flush( &w );
    if ( serialize_write_error( &w ) ) { printf( "WRITE ERROR\n" ); return 1; }

    n = serialize_write_bytes_processed( &w );
    printf( "%d bytes\n", n );
    for ( i = 0; i < n; i++ ) printf( "%02x", buffer[i] );
    printf( "\n" );
    return 0;
}
