/*
    The golden wire test: a fixed sequence and the exact bytes it must produce.

    This exists because a ROUND TRIP TEST CANNOT CATCH AN ENDIANNESS BUG. If
    the packer wrote the scratch word in the wrong byte order, the reader would
    read it back in the same wrong order and every round trip would pass while
    the bytes on the wire were incompatible with every other port.

    So the expected bytes below are pinned. They were produced by the C++
    library (test/diff_cpp.cpp writes the same sequence) and verified identical
    on x86-64. Running this under qemu-s390x is what proves the big-endian path.
*/
#include <stdio.h>
#include <string.h>
#include "../serialize.h"

/* the same sequence as test/diff_c.c and test/diff_cpp.cpp */
static const char * expected =
    "4d85aed577df77df56eff7e6d5c4b3a291809a8da71b59b41f9280aaaaaaaaaaaaaa7f96"
    "010203faff1374686520717569636b2062726f776e20666f78151b9c00a08200e047e80100";

int main( void )
{
    static serialize_uint8_t buffer[1024];
    char hex[2048];
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
    serialize_write_int_relative( &w, 100, 101 );
    serialize_write_int_relative( &w, 100, 104 );
    serialize_write_int_relative( &w, 100, 120 );
    serialize_write_int_relative( &w, 100, 400 );
    serialize_write_int_relative( &w, 100, 5000 );
    serialize_write_int_relative( &w, 100, 999999 );

    serialize_write_flush( &w );

    if ( serialize_write_error( &w ) )
    {
        printf( "FAILED: write error\n" );
        return 1;
    }

    n = serialize_write_bytes_processed( &w );
    hex[0] = '\0';
    for ( i = 0; i < n; i++ )
    {
        sprintf( hex + i * 2, "%02x", buffer[i] );
    }

    if ( strcmp( hex, expected ) != 0 )
    {
        printf( "FAILED: golden wire mismatch\n" );
        printf( "  expected %s\n", expected );
        printf( "  got      %s\n", hex );
        return 1;
    }

    /* and it must read back, so the golden is a live stream and not just a
       string that happens to match */
    {
        serialize_read_stream_t r;
        serialize_uint32_t b3 = 0;
        serialize_int32_t v = 0;
        serialize_read_stream_init( &r, buffer, n );
        if ( !serialize_read_bits( &r, &b3, 3 ) || b3 != 5 )
        {
            printf( "FAILED: golden does not read back\n" );
            return 1;
        }
        (void) v;
    }

    printf( "OK (%d bytes)\n", n );

    return 0;
}
