/* The compressed float battery, written with serialize.c and printed as hex.
   Its C++ twin, diff3_cpp.cpp, writes the SAME sequence with the C++
   serialize.h.

   This exists because the core vector's only compressed float is 0.75 over
   [0,1], which lands EXACTLY on quantum 75 -- and a value on a quantum
   agrees under float32, under double, and under a fused multiply-add alike.
   So the core gate could not see arithmetic divergence, and for a while it
   did not: this library quantized in double and disagreed with every other
   port on most inputs, with every test green.

   STANDARD.md requires float32 with TWO roundings -- the product rounds
   before 0.5 is added. The values below were found by sweeping each range
   for inputs where the variants disagree. The comment on each line is the
   required float32 result, followed by what the wrong arithmetic yields. */
#include <stdio.h>
#include "../serialize.h"

int main( void )
{
    static serialize_uint8_t buffer[1024];
    serialize_write_stream_t w;
    int i, n;

    serialize_write_stream_init( &w, buffer, sizeof( buffer ) );

    /* [0,10] res 0.01 -- max_integer_value 1000 */
    /* The FMA-boundary value rides the PRECOMPUTED entry point (schema #107): constants
       exactly what the derived path computes for (0, 10, 0.01), so this differential now
       proves the precomputed path bit-identical with the derived one beside it. */
    serialize_write_compressed_float_precomputed( &w, 0.005f, 1000, 10, 10.0f, 0.0f );  /* 1     double 0, FMA 0 */
    serialize_write_compressed_float( &w, 0.025f, 0.0f, 10.0f, 0.01f );  /* 3     double 2 */
    serialize_write_compressed_float( &w, 0.155f, 0.0f, 10.0f, 0.01f );  /* 16    double 15 */
    serialize_write_compressed_float( &w, 0.165f, 0.0f, 10.0f, 0.01f );  /* 17    double 16 */
    serialize_write_compressed_float( &w, 0.275f, 0.0f, 10.0f, 0.01f );  /* 28    double 27 */
    serialize_write_compressed_float( &w, 9.995f, 0.0f, 10.0f, 0.01f );  /* 1000  double 999 */
    serialize_write_compressed_float( &w, 2.5f,   0.0f, 10.0f, 0.01f );  /* 250   exact quantum */

    /* [0,1] res 0.001 -- the tightest normalized steps */
    serialize_write_compressed_float( &w, 0.0005f, 0.0f, 1.0f, 0.001f ); /* 1     double 0, FMA 0 */
    serialize_write_compressed_float( &w, 0.0025f, 0.0f, 1.0f, 0.001f ); /* 3     double 2 */
    serialize_write_compressed_float( &w, 0.0045f, 0.0f, 1.0f, 0.001f ); /* 5     double 4 */
    serialize_write_compressed_float( &w, 0.0055f, 0.0f, 1.0f, 0.001f ); /* 6     double 5 */

    /* [-100,100] res 0.1 -- a range straddling zero */
    serialize_write_compressed_float( &w, -99.75f, -100.0f, 100.0f, 0.1f ); /* 3   double 2 */
    serialize_write_compressed_float( &w, -99.25f, -100.0f, 100.0f, 0.1f ); /* 8   double 7 */
    serialize_write_compressed_float( &w, -97.25f, -100.0f, 100.0f, 0.1f ); /* 28  double 27 */
    serialize_write_compressed_float( &w, -96.75f, -100.0f, 100.0f, 0.1f ); /* 33  double 32 */
    serialize_write_compressed_float( &w, 0.0f,    -100.0f, 100.0f, 0.1f ); /* midpoint */
    serialize_write_compressed_float( &w, -100.0f, -100.0f, 100.0f, 0.1f ); /* min boundary */
    serialize_write_compressed_float( &w, 100.0f,  -100.0f, 100.0f, 0.1f ); /* max boundary */

    /* [0,360] res 0.5 -- an angle, the shape a game schema actually declares */
    serialize_write_compressed_float( &w, 20.25f,  0.0f, 360.0f, 0.5f ); /* 41   double 40 */
    serialize_write_compressed_float( &w, 42.75f,  0.0f, 360.0f, 0.5f ); /* 86   double 85 */
    serialize_write_compressed_float( &w, 69.75f,  0.0f, 360.0f, 0.5f ); /* 140  double 139 */
    serialize_write_compressed_float( &w, 74.25f,  0.0f, 360.0f, 0.5f ); /* 149  double 148 */
    serialize_write_compressed_float( &w, 359.75f, 0.0f, 360.0f, 0.5f ); /* near max */

    /* [-1,1] res 0.0001 -- a range where the sweep found no divergence, kept
       so a future change that INTRODUCES one here is caught */
    serialize_write_compressed_float( &w, 0.12345f,  -1.0f, 1.0f, 0.0001f );
    serialize_write_compressed_float( &w, -0.98765f, -1.0f, 1.0f, 0.0001f );

    /* out of range both ways -- the writer's own clamp, the !>= / !<= form
       that also forces NaN into range */
    serialize_write_compressed_float( &w, -5.0f, 0.0f, 10.0f, 0.01f );   /* clamps to 0 */
    serialize_write_compressed_float( &w, 15.0f, 0.0f, 10.0f, 0.01f );   /* clamps to 1000 */

    /* The normative INTEGER clamp's witnesses (STANDARD.md, schema#109),
       writing max. Step counts in [2^23, 2^24) are where the float32 ulp of
       the scaled product reaches 1: an unclamped writer quantizes these to a
       code its own reader rejects ([0,8388609]: code 8388610) or to a code
       one bit wider than the field ([0,16777215]: code 2^24). Until these
       rows existed the clamp was proven in-language only (serialize#94):
       this gate stayed green against the pre-clamp v1.11.0 reference because
       no cross-language value exercised the band. */
    serialize_write_compressed_float( &w, 8388609.0f, 0.0f, 8388609.0f, 1.0f );   /* witness A: reader-rejects class */
    serialize_write_compressed_float( &w, 16777215.0f, 0.0f, 16777215.0f, 1.0f ); /* witness B: wire-divergence class */

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
