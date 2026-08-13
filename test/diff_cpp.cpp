// The C++ twin of diff_c.c: the SAME sequence through the original
// serialize.h. If the two disagree by one byte, this port is not wire
// compatible and nothing else about it matters.
#include "serialize.h"
#include <stdio.h>

template <typename Stream> bool write_sequence( Stream & stream )
{
    uint32_t bits3 = 5;
    bool t = true, f = false;
    int32_t i1 = 42, i2 = -7, i3 = 5;
    uint32_t u8v = 0xAB, u16v = 0xBEEF, u32v = 0xDEADBEEF;
    uint64_t u64v = 0x0123456789ABCDEFULL;
    int64_t i64v = -1234567890123LL;
    float fv = 3.1415926f;
    double dv = 1.0 / 3.0;
    float cf = 0.75f;
    uint8_t blob[5] = { 1, 2, 3, 250, 255 };
    char str[64] = "the quick brown fox";
    int32_t r1 = 101, r2 = 104, r3 = 120, r4 = 400, r5 = 5000, r6 = 999999;

    serialize_bits( stream, bits3, 3 );
    serialize_bool( stream, t );
    serialize_bool( stream, f );
    serialize_int( stream, i1, 0, 1000 );
    serialize_int( stream, i2, -100, 100 );
    serialize_bits( stream, u8v, 8 );
    serialize_bits( stream, u16v, 16 );
    serialize_bits( stream, u32v, 32 );
    serialize_uint64( stream, u64v );
    serialize_int64( stream, i64v, -2000000000000LL, 2000000000000LL );
    serialize_float( stream, fv );
    serialize_double( stream, dv );
    serialize_compressed_float( stream, cf, 0.0f, 1.0f, 0.01f );
    serialize_align( stream );
    serialize_bytes( stream, blob, 5 );
    serialize_string( stream, str, sizeof( str ) );
    serialize_int_relative( stream, 100, r1 );
    serialize_int_relative( stream, 100, r2 );
    serialize_int_relative( stream, 100, r3 );
    serialize_int_relative( stream, 100, r4 );
    serialize_int_relative( stream, 100, r5 );
    serialize_int_relative( stream, 100, r6 );

    return true;
}

int main()
{
    static uint8_t buffer[1024];
    serialize::WriteStream stream( buffer, sizeof( buffer ) );
    if ( !write_sequence( stream ) ) { printf( "WRITE ERROR\n" ); return 1; }
    stream.Flush();
    const int n = stream.GetBytesProcessed();
    printf( "%d bytes\n", n );
    for ( int i = 0; i < n; i++ ) printf( "%02x", buffer[i] );
    printf( "\n" );
    return 0;
}
