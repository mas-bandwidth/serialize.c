/* STANDARD.md's worked example: a 3-character wstring in a wchar_t[8] buffer,
   starting byte aligned, must produce exactly 13 documented bytes. */
#include <stdio.h>
#include <string.h>
#include "../serialize.h"
int main( void )
{
    static serialize_uint8_t buffer[64];
    serialize_write_stream_t w;
    wchar_t ws[8];
    int i, n;
    static const serialize_uint8_t expected[13] = {
        0xE3, 0x21, 0x00, 0x00, 0xC0, 0x21, 0x00, 0x00, 0x00, 0x22, 0x00, 0x00, 0x00
    };
    ws[0] = 0x043C; ws[1] = 0x0438; ws[2] = 0x0440; ws[3] = 0;
    serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
    serialize_write_wstring( &w, ws, 8 );
    serialize_write_flush( &w );
    n = serialize_write_bytes_processed( &w );
    printf( "%d bytes: ", n );
    for ( i = 0; i < n; i++ ) printf( "%02x ", buffer[i] );
    printf( "\n" );
    if ( n != 13 || memcmp( buffer, expected, 13 ) != 0 ) { printf( "MISMATCH vs STANDARD.md\n" ); return 1; }
    printf( "matches STANDARD.md's worked example\n" );
    return 0;
}
