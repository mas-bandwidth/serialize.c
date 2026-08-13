/* Writes a fixed sequence with serialize.c and prints the bytes as hex.
   Its C++ twin, diff_cpp.cpp, writes the SAME sequence with the C++
   serialize.h. Identical output is the whole point of this port. */
#include <stdio.h>
#include "../serialize.h"

int main( void )
{
    static serialize_uint8_t buffer[1024];
    serialize_write_stream_t w;
    int i, n;

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
    {
        static const serialize_uint8_t blob[5] = { 1, 2, 3, 250, 255 };
        serialize_write_bytes( &w, blob, 5 );
    }
    serialize_write_string( &w, "the quick brown fox", 64 );
    serialize_write_int_relative( &w, 100, 101 );     /* one bit */
    serialize_write_int_relative( &w, 100, 104 );
    serialize_write_int_relative( &w, 100, 120 );
    serialize_write_int_relative( &w, 100, 400 );
    serialize_write_int_relative( &w, 100, 5000 );
    serialize_write_int_relative( &w, 100, 999999 );

    serialize_write_flush( &w );

    if ( serialize_write_error( &w ) )
    {
        printf( "WRITE ERROR\n" );
        return 1;
    }

    n = serialize_write_bytes_processed( &w );
    printf( "%d bytes\n", n );
    for ( i = 0; i < n; i++ )
    {
        printf( "%02x", buffer[i] );
    }
    printf( "\n" );

    return 0;
}
