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
    serialize_write_wstring( &w, ws, 8 );

    serialize_write_flush( &w );
    if ( serialize_write_error( &w ) ) { printf( "WRITE ERROR\n" ); return 1; }

    n = serialize_write_bytes_processed( &w );
    printf( "%d bytes\n", n );
    for ( i = 0; i < n; i++ ) printf( "%02x", buffer[i] );
    printf( "\n" );
    return 0;
}
