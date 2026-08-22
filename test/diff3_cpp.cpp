// The C++ twin of diff3_c.c: the SAME compressed float battery through the
// original serialize.h.
//
// compressed_float is the one operation in the format whose result depends on
// HOW the arithmetic is done, not just on what it computes. STANDARD.md pins
// it to float32 with two roundings. A port that widens to double, or that lets
// the compiler contract the multiply and add into a single FMA, produces
// different bytes for most inputs while still passing a vector built from
// values that land on quanta. This gate compares against the reference
// implementation on values that land BETWEEN quanta, where it shows.
#include "serialize.h"
#include <stdio.h>

template <typename Stream> bool write_sequence( Stream & stream )
{
    float q;

    // [0,10] res 0.01
    q = 0.005f;   serialize_compressed_float( stream, q, 0.0f, 10.0f, 0.01f );
    q = 0.025f;   serialize_compressed_float( stream, q, 0.0f, 10.0f, 0.01f );
    q = 0.155f;   serialize_compressed_float( stream, q, 0.0f, 10.0f, 0.01f );
    q = 0.165f;   serialize_compressed_float( stream, q, 0.0f, 10.0f, 0.01f );
    q = 0.275f;   serialize_compressed_float( stream, q, 0.0f, 10.0f, 0.01f );
    q = 9.995f;   serialize_compressed_float( stream, q, 0.0f, 10.0f, 0.01f );
    q = 2.5f;     serialize_compressed_float( stream, q, 0.0f, 10.0f, 0.01f );

    // [0,1] res 0.001
    // This one field rides the PRECOMPUTED entry point, mirroring diff3_c.c, which
    // crosses the other way on [0,10] res 0.01. So each side's precomputed path is
    // held to the other side's derived path, and neither crossing proves the other.
    // Constants are exactly serialize_compressed_float_params( 0, 1, 0.001 ):
    // max_integer_value 1000, bits 10, delta 1 — read out of that function, not
    // hand-derived. The value is the FMA discriminator of this group.
    q = 0.0005f;  serialize_compressed_float_precomputed( stream, q, 1000, 10, 1.0f, 0.0f );
    q = 0.0025f;  serialize_compressed_float( stream, q, 0.0f, 1.0f, 0.001f );
    q = 0.0045f;  serialize_compressed_float( stream, q, 0.0f, 1.0f, 0.001f );
    q = 0.0055f;  serialize_compressed_float( stream, q, 0.0f, 1.0f, 0.001f );

    // [-100,100] res 0.1
    q = -99.75f;  serialize_compressed_float( stream, q, -100.0f, 100.0f, 0.1f );
    q = -99.25f;  serialize_compressed_float( stream, q, -100.0f, 100.0f, 0.1f );
    q = -97.25f;  serialize_compressed_float( stream, q, -100.0f, 100.0f, 0.1f );
    q = -96.75f;  serialize_compressed_float( stream, q, -100.0f, 100.0f, 0.1f );
    q = 0.0f;     serialize_compressed_float( stream, q, -100.0f, 100.0f, 0.1f );
    q = -100.0f;  serialize_compressed_float( stream, q, -100.0f, 100.0f, 0.1f );
    q = 100.0f;   serialize_compressed_float( stream, q, -100.0f, 100.0f, 0.1f );

    // [0,360] res 0.5
    q = 20.25f;   serialize_compressed_float( stream, q, 0.0f, 360.0f, 0.5f );
    q = 42.75f;   serialize_compressed_float( stream, q, 0.0f, 360.0f, 0.5f );
    q = 69.75f;   serialize_compressed_float( stream, q, 0.0f, 360.0f, 0.5f );
    q = 74.25f;   serialize_compressed_float( stream, q, 0.0f, 360.0f, 0.5f );
    q = 359.75f;  serialize_compressed_float( stream, q, 0.0f, 360.0f, 0.5f );

    // [-1,1] res 0.0001
    q = 0.12345f;  serialize_compressed_float( stream, q, -1.0f, 1.0f, 0.0001f );
    q = -0.98765f; serialize_compressed_float( stream, q, -1.0f, 1.0f, 0.0001f );

    // out of range both ways -- the writer's clamp
    q = -5.0f;    serialize_compressed_float( stream, q, 0.0f, 10.0f, 0.01f );
    q = 15.0f;    serialize_compressed_float( stream, q, 0.0f, 10.0f, 0.01f );

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
