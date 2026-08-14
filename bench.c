/*
    serialize.c benchmark.

    A deliberate operation-for-operation mirror of the C++ library's bench.cpp,
    so the two outputs can be read side by side. Same operations in the same
    order, same iteration counts, same buffer sizes, same input data, same
    reporting format and units.

    It measures throughput of the raw bit packer with mixed bit widths, and of
    the stream path with a representative packet, and then three packet shapes
    that look like schema generated code.

    WHAT IT EXISTS TO ANSWER

    The C++ library's write_bits is a macro that expands at every call site and
    inlines into the caller. Here serialize_write_bits is a real function in a
    separate translation unit, so without link time optimization the generated
    code pays a call per field where C++ pays none. Whether that is measurable,
    and whether -flto closes it, is the question. Build `bench` and `bench-lto`
    and compare.

    WHAT IS DELIBERATELY NOT MIRRORED

    bench.cpp's bench_compile_time_pairs runs each of its three packet shapes
    TWICE: once through the runtime macros and once through the compile time
    parameter surface (serialize_int_compile_time and friends), to ask whether
    moving min/max/bits into template arguments buys anything. The compile time
    half is C++ template machinery -- there is no such surface in this library
    and no way to write one in C89 -- so only the runtime half of each pair is
    reproduced here. The three runtime rows correspond exactly; the three
    compile time rows have no counterpart and are the ONLY thing omitted.

    Nothing else is dropped. Dropping work from this side would make C look
    faster for free, which is the one result this benchmark must not produce.

    Two API level differences are worth knowing when reading the numbers, both
    properties of the libraries rather than of this harness:

      - the C measure functions take only the arguments that determine WIDTH,
        never the value, so bench_packet_measure takes no packet. The varying
        call is still made in the measure loop so the loop matches C++'s.

      - the C read stream assembles its 8 byte window byte by byte once it is
        within 8 bytes of the end of the buffer, where the C++ reader loads 8
        bytes unconditionally and relies on the caller having allocated slack.
        For small packets that tail is most of the packet.

    Only release build numbers are meaningful, and only as ratios between legs
    measured back to back on the same machine.
*/

/* clock_gettime is POSIX, and -std=c89 hides it without this. */
#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif
#endif

#include "serialize.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------------------------------------------------------------------
   harness

   64 bit constants are composed from two 32 bit halves rather than written as
   ULL literals, for the same reason the library does it: C89 has no 64 bit
   literal suffix it is happy about, and this needs none. A compiler folds it.
   --------------------------------------------------------------------------- */

#define BENCH_U64( hi, lo ) ( ( ( (serialize_uint64_t) (hi) ) << 32 ) | (serialize_uint64_t) (lo) )

#define BENCH_LCG_MUL   BENCH_U64( 0x5851F42Du, 0x4C957F2Du )    /* 6364136223846793005 */
#define BENCH_LCG_ADD   BENCH_U64( 0x14057B7Eu, 0xF767814Fu )    /* 1442695040888963407 */
#define BENCH_BIG       BENCH_U64( 0x12345678u, 0x9ABCDEF0u )
#define BENCH_MASK48    BENCH_U64( 0x0000FFFFu, 0xFFFFFFFFu )

static volatile serialize_uint64_t g_sink = 0;      /* defeats dead code elimination of computed values */

/*
    Tells the compiler the memory at data is observed, so stores to it cannot
    be dead code eliminated. Without this the optimizer proves nothing reads
    the serialized buffer inside the loop and deletes the serialization work
    entirely, reporting fictional throughput. The empty asm with a memory
    clobber is the standard escape; it emits no instructions. Spelled with the
    reserved __asm__ __volatile__ so it survives -std=c89 -pedantic.
*/

#if defined(_MSC_VER)
#include <intrin.h>
static void bench_escape( const void * data )
{
    (void) data;
    _ReadWriteBarrier();
}
#elif defined(__GNUC__) || defined(__clang__)
static void bench_escape( const void * data )
{
    __asm__ __volatile__( "" : : "g"( data ) : "memory" );
}
#else
/* Weaker than a memory clobber, but every compiler has it. If your numbers
   look impossibly fast on such a toolchain, this is the first thing to doubt. */
static const void * volatile g_escape_sink = 0;
static void bench_escape( const void * data )
{
    g_escape_sink = data;
}
#endif

static double bench_time_now( void )
{
#if defined(_WIN32)
    return (double) clock() / (double) CLOCKS_PER_SEC;
#else
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1000000000.0;
#endif
}

#define NumTrials 5

/* ---------------------------------------------------------------------------
   bit packer

   The pass counts are overridable so the harness can be built at a different
   scale and the timings checked for linearity -- a benchmark whose loop has
   been optimized away does not scale with its iteration count.
   --------------------------------------------------------------------------- */

#define BitpackerBufferSize ( 64 * 1024 )
#define NumWidths 16

#ifndef BitpackerNumPasses
#define BitpackerNumPasses 4096
#endif

static const int bench_widths[NumWidths] = { 1, 32, 7, 13, 3, 25, 8, 19, 4, 28, 11, 16, 2, 30, 6, 22 };      /* 227 bits per group */

static serialize_uint32_t bench_values[NumWidths];

static void bench_bitpacker( serialize_uint8_t * buffer )
{
    int i;
    int trial;
    int pass;
    double best_write = 1e30;
    double best_read = 1e30;
    double total_mb;
    serialize_uint64_t bytes_per_pass = 0;

    for ( i = 0; i < NumWidths; i++ )
    {
        const serialize_uint32_t mask = ( bench_widths[i] == 32 ) ? 0xFFFFFFFFu : ( ( 1u << bench_widths[i] ) - 1 );
        bench_values[i] = ( 0x9E3779B9u * (serialize_uint32_t) ( i + 1 ) ) & mask;
    }

    for ( trial = 0; trial < NumTrials; trial++ )
    {
        double start;
        double elapsed;

        start = bench_time_now();
        for ( pass = 0; pass < BitpackerNumPasses; pass++ )
        {
            serialize_write_stream_t writer;
            serialize_write_stream_init( &writer, buffer, BitpackerBufferSize );
            while ( serialize_write_bits_available( &writer ) >= 256 )
            {
                for ( i = 0; i < NumWidths; i++ )
                {
                    serialize_write_bits( &writer, bench_values[i], bench_widths[i] );
                }
            }
            serialize_write_flush( &writer );
            bench_escape( buffer );
            bytes_per_pass = (serialize_uint64_t) serialize_write_bytes_processed( &writer );
            g_sink = g_sink + bytes_per_pass;
        }
        elapsed = bench_time_now() - start;
        if ( elapsed < best_write )
        {
            best_write = elapsed;
        }

        start = bench_time_now();
        for ( pass = 0; pass < BitpackerNumPasses; pass++ )
        {
            serialize_read_stream_t reader;
            serialize_uint64_t sum = 0;
            serialize_uint32_t value = 0;
            serialize_read_stream_init( &reader, buffer, BitpackerBufferSize );
            while ( serialize_read_bits_remaining( &reader ) >= 256 )
            {
                for ( i = 0; i < NumWidths; i++ )
                {
                    serialize_read_bits( &reader, &value, bench_widths[i] );
                    sum += value;
                }
            }
            g_sink = g_sink + sum;
        }
        elapsed = bench_time_now() - start;
        if ( elapsed < best_read )
        {
            best_read = elapsed;
        }
    }

    total_mb = (double) bytes_per_pass * BitpackerNumPasses / ( 1024.0 * 1024.0 );

    printf( "bitpacker write:  %8.1f MB/s\n", total_mb / best_write );
    printf( "bitpacker read:   %8.1f MB/s\n", total_mb / best_read );
}

/* ---------------------------------------------------------------------------
   stream
   --------------------------------------------------------------------------- */

typedef struct BenchPacket
{
    serialize_int32_t a, b, c;
    serialize_uint32_t bits7, bits13, bits23;
    int flag;
    float x, y, z;
    serialize_uint64_t big;
    serialize_uint8_t blob[17];
} BenchPacket;

static void bench_packet_init( BenchPacket * packet )
{
    int i;
    packet->a = -37;
    packet->b = 12345;
    packet->c = 987654;
    packet->bits7 = 97;
    packet->bits13 = 5000;
    packet->bits23 = 1234567;
    packet->flag = 1;
    packet->x = 1.5f;
    packet->y = -3.25f;
    packet->z = 100.125f;
    packet->big = BENCH_BIG;
    for ( i = 0; i < (int) sizeof( packet->blob ); i++ )
    {
        packet->blob[i] = (serialize_uint8_t) ( i * 31 );
    }
}

/* The three halves of what C++ expresses once as a Serialize template. The
   per field failure check mirrors the early return the C++ macros perform. */

static int bench_packet_write( serialize_write_stream_t * stream, const BenchPacket * packet )
{
    if ( !serialize_write_int( stream, packet->a, -100, +100 ) ) { return 0; }
    if ( !serialize_write_int( stream, packet->b, 0, 65535 ) ) { return 0; }
    if ( !serialize_write_int( stream, packet->c, -1000000, +1000000 ) ) { return 0; }
    if ( !serialize_write_bits( stream, packet->bits7, 7 ) ) { return 0; }
    if ( !serialize_write_bits( stream, packet->bits13, 13 ) ) { return 0; }
    if ( !serialize_write_bits( stream, packet->bits23, 23 ) ) { return 0; }
    if ( !serialize_write_bool( stream, packet->flag ) ) { return 0; }
    if ( !serialize_write_float( stream, packet->x ) ) { return 0; }
    if ( !serialize_write_float( stream, packet->y ) ) { return 0; }
    if ( !serialize_write_float( stream, packet->z ) ) { return 0; }
    if ( !serialize_write_uint64( stream, packet->big ) ) { return 0; }
    if ( !serialize_write_bytes( stream, packet->blob, (int) sizeof( packet->blob ) ) ) { return 0; }
    return 1;
}

static int bench_packet_read( serialize_read_stream_t * stream, BenchPacket * packet )
{
    if ( !serialize_read_int( stream, &packet->a, -100, +100 ) ) { return 0; }
    if ( !serialize_read_int( stream, &packet->b, 0, 65535 ) ) { return 0; }
    if ( !serialize_read_int( stream, &packet->c, -1000000, +1000000 ) ) { return 0; }
    if ( !serialize_read_bits( stream, &packet->bits7, 7 ) ) { return 0; }
    if ( !serialize_read_bits( stream, &packet->bits13, 13 ) ) { return 0; }
    if ( !serialize_read_bits( stream, &packet->bits23, 23 ) ) { return 0; }
    if ( !serialize_read_bool( stream, &packet->flag ) ) { return 0; }
    if ( !serialize_read_float( stream, &packet->x ) ) { return 0; }
    if ( !serialize_read_float( stream, &packet->y ) ) { return 0; }
    if ( !serialize_read_float( stream, &packet->z ) ) { return 0; }
    if ( !serialize_read_uint64( stream, &packet->big ) ) { return 0; }
    if ( !serialize_read_bytes( stream, packet->blob, (int) sizeof( packet->blob ) ) ) { return 0; }
    return 1;
}

/* No packet argument: a C measure takes only what determines the width. */
static int bench_packet_measure( serialize_measure_stream_t * stream )
{
    if ( !serialize_measure_int( stream, -100, +100 ) ) { return 0; }
    if ( !serialize_measure_int( stream, 0, 65535 ) ) { return 0; }
    if ( !serialize_measure_int( stream, -1000000, +1000000 ) ) { return 0; }
    if ( !serialize_measure_bits( stream, 7 ) ) { return 0; }
    if ( !serialize_measure_bits( stream, 13 ) ) { return 0; }
    if ( !serialize_measure_bits( stream, 23 ) ) { return 0; }
    if ( !serialize_measure_bool( stream ) ) { return 0; }
    if ( !serialize_measure_float( stream ) ) { return 0; }
    if ( !serialize_measure_float( stream ) ) { return 0; }
    if ( !serialize_measure_float( stream ) ) { return 0; }
    if ( !serialize_measure_uint64( stream ) ) { return 0; }
    if ( !serialize_measure_bytes( stream, 17 ) ) { return 0; }
    return 1;
}

#define NumVariants 64

#ifndef StreamNumPackets
#define StreamNumPackets 1000000
#endif

/*
    Most packet fields must vary per iteration, driven by a serially dependent
    generator the compiler cannot fold. Varying just one field is not enough:
    all field bit widths are constant, so the optimizer precomputes the scratch
    words for the loop invariant fields and only patches in the varying bits,
    reporting fictional write throughput. The LCG costs a couple of cycles.
*/

static serialize_uint64_t bench_vary_packet( BenchPacket * packet, serialize_uint64_t rng )
{
    rng = rng * BENCH_LCG_MUL + BENCH_LCG_ADD;
    packet->a = (serialize_int32_t) ( ( rng >> 8 ) & 63 ) - 32;                  /* [-32,31] within [-100,+100] */
    packet->b = (serialize_int32_t) ( (serialize_uint32_t) ( rng >> 16 ) & 65535u );        /* [0,65535] */
    packet->c = (serialize_int32_t) ( ( rng >> 24 ) & 0xFFFFFu ) - 500000;       /* [-500000,548575] within [-1000000,+1000000] */
    packet->bits7 = (serialize_uint32_t) rng & 127u;
    packet->bits13 = (serialize_uint32_t) ( rng >> 3 ) & 8191u;
    packet->bits23 = (serialize_uint32_t) ( rng >> 5 ) & 8388607u;
    packet->flag = ( rng & 1 ) != 0;
    packet->x = (float) ( (serialize_uint32_t) rng & 0xFFFFu );
    packet->big = rng;
    packet->blob[0] = (serialize_uint8_t) ( rng >> 32 );
    return rng;
}

static void bench_stream( void )
{
    serialize_uint8_t buffer[256];
    serialize_uint8_t variant_buffers[NumVariants][256];
    BenchPacket packet;
    serialize_uint64_t rng;
    int bytes_per_packet = 0;
    int k;
    int trial;
    int i;
    double best_write = 1e30;
    double best_read = 1e30;
    double best_measure = 1e30;
    double total_mb;
    double packets;

    memset( buffer, 0, sizeof( buffer ) );

    bench_packet_init( &packet );

    /* pre-write variant packets for the read benchmark, using the same LCG sequence as the write loop */
    rng = 1;
    for ( k = 0; k < NumVariants; k++ )
    {
        serialize_write_stream_t stream;
        memset( variant_buffers[k], 0, sizeof( variant_buffers[k] ) );
        rng = bench_vary_packet( &packet, rng );
        serialize_write_stream_init( &stream, variant_buffers[k], (int) sizeof( variant_buffers[k] ) );
        if ( !bench_packet_write( &stream, &packet ) )
        {
            exit( 1 );
        }
        serialize_write_flush( &stream );
        bytes_per_packet = serialize_write_bytes_processed( &stream );
    }

    for ( trial = 0; trial < NumTrials; trial++ )
    {
        double start;
        double elapsed;

        rng = 1;

        start = bench_time_now();
        for ( i = 0; i < StreamNumPackets; i++ )
        {
            serialize_write_stream_t stream;
            rng = bench_vary_packet( &packet, rng );
            serialize_write_stream_init( &stream, buffer, (int) sizeof( buffer ) );
            if ( !bench_packet_write( &stream, &packet ) )
            {
                exit( 1 );
            }
            serialize_write_flush( &stream );
            bench_escape( buffer );
            g_sink = g_sink + (serialize_uint64_t) serialize_write_bytes_processed( &stream );
        }
        elapsed = bench_time_now() - start;
        if ( elapsed < best_write )
        {
            best_write = elapsed;
        }

        start = bench_time_now();
        for ( i = 0; i < StreamNumPackets; i++ )
        {
            serialize_read_stream_t stream;
            BenchPacket read_packet;
            serialize_read_stream_init( &stream, variant_buffers[i & ( NumVariants - 1 )], bytes_per_packet );
            if ( !bench_packet_read( &stream, &read_packet ) )
            {
                exit( 1 );
            }
            bench_escape( &read_packet );               /* every decoded field is observed, so the full decode must happen */
            g_sink = g_sink + (serialize_uint64_t) read_packet.b;
        }
        elapsed = bench_time_now() - start;
        if ( elapsed < best_read )
        {
            best_read = elapsed;
        }

        /* note: measure folds to near-constants at compile time by design, so this mostly
           measures loop overhead. that measure is almost free is the property worth tracking.
           the vary call stays even though a C measure never sees the packet, so the loop is
           the same loop the C++ bench times. */
        start = bench_time_now();
        for ( i = 0; i < StreamNumPackets; i++ )
        {
            serialize_measure_stream_t stream;
            rng = bench_vary_packet( &packet, rng );
            serialize_measure_stream_init( &stream );
            if ( !bench_packet_measure( &stream ) )
            {
                exit( 1 );
            }
            g_sink = g_sink + (serialize_uint64_t) serialize_measure_bits_processed( &stream );
        }
        elapsed = bench_time_now() - start;
        if ( elapsed < best_measure )
        {
            best_measure = elapsed;
        }
    }

    total_mb = (double) bytes_per_packet * StreamNumPackets / ( 1024.0 * 1024.0 );
    packets = (double) StreamNumPackets / 1000000.0;

    printf( "stream write:     %8.1f MB/s  (%.1f M packets/s)\n", total_mb / best_write, packets / best_write );
    printf( "stream read:      %8.1f MB/s  (%.1f M packets/s)\n", total_mb / best_read, packets / best_read );
    printf( "stream measure:   %19.1f M packets/s\n", packets / best_measure );
}

/* ---------------------------------------------------------------------------
   packet shapes

   The runtime half of bench.cpp's matched pairs. Same data, same serially
   dependent LCG variation pattern, same escape barriers, same trial structure.

   The driver is a macro rather than a function taking function pointers,
   because bench.cpp's driver is a TEMPLATE: it is specialized per shape with
   every call statically resolved and inlinable. Function pointers would add an
   indirect call per field that the C++ side does not pay, and that would be
   the harness lying rather than the library being slower.
   --------------------------------------------------------------------------- */

#define BENCH_PACKET_SHAPE( func, fields_t, vary_fn, write_fn, read_fn )                                                 \
static void func( const char * label )                                                                                  \
{                                                                                                                       \
    serialize_uint8_t buffer[256];                                                                                      \
    serialize_uint8_t variant_buffers[NumVariants][256];                                                                \
    fields_t packet;                                                                                                    \
    serialize_uint64_t rng;                                                                                             \
    int bytes_per_packet = 0;                                                                                           \
    int k;                                                                                                              \
    int trial;                                                                                                          \
    int i;                                                                                                              \
    double best_write = 1e30;                                                                                           \
    double best_read = 1e30;                                                                                            \
    double packets;                                                                                                     \
                                                                                                                        \
    memset( buffer, 0, sizeof( buffer ) );                                                                              \
    memset( &packet, 0, sizeof( packet ) );                                                                             \
                                                                                                                        \
    rng = 1;                                                                                                            \
    for ( k = 0; k < NumVariants; k++ )                                                                                 \
    {                                                                                                                   \
        serialize_write_stream_t stream;                                                                                \
        memset( variant_buffers[k], 0, sizeof( variant_buffers[k] ) );                                                  \
        rng = vary_fn( &packet, rng );                                                                                  \
        serialize_write_stream_init( &stream, variant_buffers[k], (int) sizeof( variant_buffers[k] ) );                 \
        if ( !write_fn( &stream, &packet ) )                                                                            \
        {                                                                                                               \
            exit( 1 );                                                                                                  \
        }                                                                                                               \
        serialize_write_flush( &stream );                                                                               \
        bytes_per_packet = serialize_write_bytes_processed( &stream );                                                  \
    }                                                                                                                   \
                                                                                                                        \
    for ( trial = 0; trial < NumTrials; trial++ )                                                                       \
    {                                                                                                                   \
        double start;                                                                                                   \
        double elapsed;                                                                                                 \
                                                                                                                        \
        rng = 1;                                                                                                        \
                                                                                                                        \
        start = bench_time_now();                                                                                       \
        for ( i = 0; i < StreamNumPackets; i++ )                                                                        \
        {                                                                                                               \
            serialize_write_stream_t stream;                                                                            \
            rng = vary_fn( &packet, rng );                                                                              \
            serialize_write_stream_init( &stream, buffer, (int) sizeof( buffer ) );                                     \
            if ( !write_fn( &stream, &packet ) )                                                                        \
            {                                                                                                           \
                exit( 1 );                                                                                              \
            }                                                                                                           \
            serialize_write_flush( &stream );                                                                           \
            bench_escape( buffer );                                                                                     \
            g_sink = g_sink + (serialize_uint64_t) serialize_write_bytes_processed( &stream );                          \
        }                                                                                                               \
        elapsed = bench_time_now() - start;                                                                             \
        if ( elapsed < best_write )                                                                                     \
        {                                                                                                               \
            best_write = elapsed;                                                                                       \
        }                                                                                                               \
                                                                                                                        \
        start = bench_time_now();                                                                                       \
        for ( i = 0; i < StreamNumPackets; i++ )                                                                        \
        {                                                                                                               \
            serialize_read_stream_t stream;                                                                             \
            fields_t read_packet;                                                                                       \
            serialize_read_stream_init( &stream, variant_buffers[i & ( NumVariants - 1 )], bytes_per_packet );          \
            if ( !read_fn( &stream, &read_packet ) )                                                                    \
            {                                                                                                           \
                exit( 1 );                                                                                              \
            }                                                                                                           \
            bench_escape( &read_packet );                                                                               \
            g_sink = g_sink + (serialize_uint64_t) *(const serialize_uint8_t *) &read_packet;                           \
        }                                                                                                               \
        elapsed = bench_time_now() - start;                                                                             \
        if ( elapsed < best_read )                                                                                      \
        {                                                                                                               \
            best_read = elapsed;                                                                                        \
        }                                                                                                               \
    }                                                                                                                   \
                                                                                                                        \
    packets = (double) StreamNumPackets / 1000000.0;                                                                    \
                                                                                                                        \
    printf( "%s  write: %6.1f M packets/s   read: %6.1f M packets/s\n", label, packets / best_write, packets / best_read ); \
}

/* shape 1: a realistic packet of ten bounded ints */

typedef struct BenchIntFields
{
    serialize_int32_t f0, f1, f2, f3, f4, f5, f6, f7, f8, f9;
} BenchIntFields;

static serialize_uint64_t bench_vary_int_fields( BenchIntFields * f, serialize_uint64_t rng )
{
    rng = rng * BENCH_LCG_MUL + BENCH_LCG_ADD;
    f->f0 = (serialize_int32_t) ( ( rng >> 8 ) & 63 ) - 32;                         /* within [-100,+100] */
    f->f1 = (serialize_int32_t) ( (serialize_uint32_t) ( rng >> 16 ) & 65535u );    /* [0,65535] */
    f->f2 = (serialize_int32_t) ( ( rng >> 24 ) & 0xFFFFFu ) - 500000;              /* within [-1000000,+1000000] */
    f->f3 = (serialize_int32_t) ( (serialize_uint32_t) ( rng >> 2 ) & 3u );         /* [0,3] */
    f->f4 = (serialize_int32_t) ( ( rng >> 11 ) & 15 ) - 8;                         /* within [-15,+15] */
    f->f5 = (serialize_int32_t) ( (serialize_uint32_t) ( rng >> 22 ) & 511u );      /* within [0,1000] */
    f->f6 = (serialize_int32_t) ( ( rng >> 33 ) & 2047 ) - 1024;                    /* within [-2048,+2047] */
    f->f7 = (serialize_int32_t) ( (serialize_uint32_t) ( rng >> 40 ) & 255u );      /* [0,255] */
    f->f8 = (serialize_int32_t) ( ( rng >> 30 ) & 0xFFFFFu ) - 500000;              /* within [-600000,+600000] */
    f->f9 = (serialize_int32_t) ( (serialize_uint32_t) ( rng >> 57 ) & 63u );       /* within [0,100] */
    return rng;
}

static int bench_int_fields_write( serialize_write_stream_t * stream, const BenchIntFields * f )
{
    if ( !serialize_write_int( stream, f->f0, -100, +100 ) ) { return 0; }
    if ( !serialize_write_int( stream, f->f1, 0, 65535 ) ) { return 0; }
    if ( !serialize_write_int( stream, f->f2, -1000000, +1000000 ) ) { return 0; }
    if ( !serialize_write_int( stream, f->f3, 0, 3 ) ) { return 0; }
    if ( !serialize_write_int( stream, f->f4, -15, +15 ) ) { return 0; }
    if ( !serialize_write_int( stream, f->f5, 0, 1000 ) ) { return 0; }
    if ( !serialize_write_int( stream, f->f6, -2048, +2047 ) ) { return 0; }
    if ( !serialize_write_int( stream, f->f7, 0, 255 ) ) { return 0; }
    if ( !serialize_write_int( stream, f->f8, -600000, +600000 ) ) { return 0; }
    if ( !serialize_write_int( stream, f->f9, 0, 100 ) ) { return 0; }
    return 1;
}

static int bench_int_fields_read( serialize_read_stream_t * stream, BenchIntFields * f )
{
    if ( !serialize_read_int( stream, &f->f0, -100, +100 ) ) { return 0; }
    if ( !serialize_read_int( stream, &f->f1, 0, 65535 ) ) { return 0; }
    if ( !serialize_read_int( stream, &f->f2, -1000000, +1000000 ) ) { return 0; }
    if ( !serialize_read_int( stream, &f->f3, 0, 3 ) ) { return 0; }
    if ( !serialize_read_int( stream, &f->f4, -15, +15 ) ) { return 0; }
    if ( !serialize_read_int( stream, &f->f5, 0, 1000 ) ) { return 0; }
    if ( !serialize_read_int( stream, &f->f6, -2048, +2047 ) ) { return 0; }
    if ( !serialize_read_int( stream, &f->f7, 0, 255 ) ) { return 0; }
    if ( !serialize_read_int( stream, &f->f8, -600000, +600000 ) ) { return 0; }
    if ( !serialize_read_int( stream, &f->f9, 0, 100 ) ) { return 0; }
    return 1;
}

/* shape 2: mixed bit widths including one wider than 32 bits */

typedef struct BenchBitsFields
{
    serialize_uint32_t b7, b13, b23, b3, b32, b11, b19;
    serialize_uint64_t b48;
} BenchBitsFields;

static serialize_uint64_t bench_vary_bits_fields( BenchBitsFields * f, serialize_uint64_t rng )
{
    rng = rng * BENCH_LCG_MUL + BENCH_LCG_ADD;
    f->b7 = (serialize_uint32_t) rng & 127u;
    f->b13 = (serialize_uint32_t) ( rng >> 3 ) & 8191u;
    f->b23 = (serialize_uint32_t) ( rng >> 5 ) & 8388607u;
    f->b3 = (serialize_uint32_t) ( rng >> 29 ) & 7u;
    f->b32 = (serialize_uint32_t) ( rng >> 16 );
    f->b11 = (serialize_uint32_t) ( rng >> 37 ) & 2047u;
    f->b19 = (serialize_uint32_t) ( rng >> 44 ) & 524287u;
    f->b48 = rng & BENCH_MASK48;
    return rng;
}

/*
    The 48 bit field goes out as the low dword then the 16 bit remainder --
    exactly what the C++ serialize_bits macro expands to for a width above 32,
    and exactly what generated C has to write by hand, since this library's
    serialize_write_bits tops out at 32 bits.
*/

static int bench_bits_fields_write( serialize_write_stream_t * stream, const BenchBitsFields * f )
{
    if ( !serialize_write_bits( stream, f->b7, 7 ) ) { return 0; }
    if ( !serialize_write_bits( stream, f->b13, 13 ) ) { return 0; }
    if ( !serialize_write_bits( stream, f->b23, 23 ) ) { return 0; }
    if ( !serialize_write_bits( stream, f->b3, 3 ) ) { return 0; }
    if ( !serialize_write_bits( stream, f->b32, 32 ) ) { return 0; }
    if ( !serialize_write_bits( stream, f->b11, 11 ) ) { return 0; }
    if ( !serialize_write_bits( stream, f->b19, 19 ) ) { return 0; }
    if ( !serialize_write_bits( stream, (serialize_uint32_t) f->b48, 32 ) ) { return 0; }
    if ( !serialize_write_bits( stream, (serialize_uint32_t) ( f->b48 >> 32 ), 16 ) ) { return 0; }
    return 1;
}

static int bench_bits_fields_read( serialize_read_stream_t * stream, BenchBitsFields * f )
{
    serialize_uint32_t lo = 0;
    serialize_uint32_t hi = 0;
    if ( !serialize_read_bits( stream, &f->b7, 7 ) ) { return 0; }
    if ( !serialize_read_bits( stream, &f->b13, 13 ) ) { return 0; }
    if ( !serialize_read_bits( stream, &f->b23, 23 ) ) { return 0; }
    if ( !serialize_read_bits( stream, &f->b3, 3 ) ) { return 0; }
    if ( !serialize_read_bits( stream, &f->b32, 32 ) ) { return 0; }
    if ( !serialize_read_bits( stream, &f->b11, 11 ) ) { return 0; }
    if ( !serialize_read_bits( stream, &f->b19, 19 ) ) { return 0; }
    if ( !serialize_read_bits( stream, &lo, 32 ) ) { return 0; }
    if ( !serialize_read_bits( stream, &hi, 16 ) ) { return 0; }
    f->b48 = BENCH_U64( hi, lo );
    return 1;
}

/* shape 3: a "generated packet" mixing bounded ints, bits and bools, the way schema generated code looks */

typedef struct BenchGenFields
{
    serialize_int32_t sequence;             /* [0,65535] */
    serialize_uint32_t ack_bits;            /* 32 bits */
    serialize_uint32_t entity_id;           /* 12 bits */
    serialize_int32_t pos_x, pos_y, pos_z;  /* [-16384,+16383] */
    serialize_uint32_t yaw;                 /* 9 bits */
    int moving;
    int firing;
    serialize_uint64_t timestamp;           /* 48 bits */
    serialize_int32_t weapon;               /* [0,15] */
} BenchGenFields;

static serialize_uint64_t bench_vary_gen_fields( BenchGenFields * f, serialize_uint64_t rng )
{
    rng = rng * BENCH_LCG_MUL + BENCH_LCG_ADD;
    f->sequence = (serialize_int32_t) ( (serialize_uint32_t) ( rng >> 8 ) & 65535u );
    f->ack_bits = (serialize_uint32_t) ( rng >> 16 );
    f->entity_id = (serialize_uint32_t) rng & 4095u;
    f->pos_x = (serialize_int32_t) ( ( rng >> 20 ) & 32767 ) - 16384;
    f->pos_y = (serialize_int32_t) ( ( rng >> 25 ) & 32767 ) - 16384;
    f->pos_z = (serialize_int32_t) ( ( rng >> 30 ) & 32767 ) - 16384;
    f->yaw = (serialize_uint32_t) ( rng >> 3 ) & 511u;
    f->moving = ( rng & 1 ) != 0;
    f->firing = ( rng & 2 ) != 0;
    f->timestamp = rng & BENCH_MASK48;
    f->weapon = (serialize_int32_t) ( (serialize_uint32_t) ( rng >> 60 ) & 15u );
    return rng;
}

static int bench_gen_fields_write( serialize_write_stream_t * stream, const BenchGenFields * f )
{
    if ( !serialize_write_int( stream, f->sequence, 0, 65535 ) ) { return 0; }
    if ( !serialize_write_bits( stream, f->ack_bits, 32 ) ) { return 0; }
    if ( !serialize_write_bits( stream, f->entity_id, 12 ) ) { return 0; }
    if ( !serialize_write_int( stream, f->pos_x, -16384, +16383 ) ) { return 0; }
    if ( !serialize_write_int( stream, f->pos_y, -16384, +16383 ) ) { return 0; }
    if ( !serialize_write_int( stream, f->pos_z, -16384, +16383 ) ) { return 0; }
    if ( !serialize_write_bits( stream, f->yaw, 9 ) ) { return 0; }
    if ( !serialize_write_bool( stream, f->moving ) ) { return 0; }
    if ( !serialize_write_bool( stream, f->firing ) ) { return 0; }
    if ( !serialize_write_bits( stream, (serialize_uint32_t) f->timestamp, 32 ) ) { return 0; }
    if ( !serialize_write_bits( stream, (serialize_uint32_t) ( f->timestamp >> 32 ), 16 ) ) { return 0; }
    if ( !serialize_write_int( stream, f->weapon, 0, 15 ) ) { return 0; }
    return 1;
}

static int bench_gen_fields_read( serialize_read_stream_t * stream, BenchGenFields * f )
{
    serialize_uint32_t lo = 0;
    serialize_uint32_t hi = 0;
    if ( !serialize_read_int( stream, &f->sequence, 0, 65535 ) ) { return 0; }
    if ( !serialize_read_bits( stream, &f->ack_bits, 32 ) ) { return 0; }
    if ( !serialize_read_bits( stream, &f->entity_id, 12 ) ) { return 0; }
    if ( !serialize_read_int( stream, &f->pos_x, -16384, +16383 ) ) { return 0; }
    if ( !serialize_read_int( stream, &f->pos_y, -16384, +16383 ) ) { return 0; }
    if ( !serialize_read_int( stream, &f->pos_z, -16384, +16383 ) ) { return 0; }
    if ( !serialize_read_bits( stream, &f->yaw, 9 ) ) { return 0; }
    if ( !serialize_read_bool( stream, &f->moving ) ) { return 0; }
    if ( !serialize_read_bool( stream, &f->firing ) ) { return 0; }
    if ( !serialize_read_bits( stream, &lo, 32 ) ) { return 0; }
    if ( !serialize_read_bits( stream, &hi, 16 ) ) { return 0; }
    f->timestamp = BENCH_U64( hi, lo );
    if ( !serialize_read_int( stream, &f->weapon, 0, 15 ) ) { return 0; }
    return 1;
}

BENCH_PACKET_SHAPE( bench_int_shape, BenchIntFields, bench_vary_int_fields, bench_int_fields_write, bench_int_fields_read )
BENCH_PACKET_SHAPE( bench_bits_shape, BenchBitsFields, bench_vary_bits_fields, bench_bits_fields_write, bench_bits_fields_read )
BENCH_PACKET_SHAPE( bench_gen_shape, BenchGenFields, bench_vary_gen_fields, bench_gen_fields_write, bench_gen_fields_read )

static void bench_packet_shapes( void )
{
    bench_int_shape ( "int packet   (runtime):     " );
    bench_bits_shape( "bits packet  (runtime):     " );
    bench_gen_shape ( "mixed packet (runtime):     " );
}

/* ------------------------------------------------------------------------------------------ */

int main( void )
{
    serialize_uint8_t * buffer;

    printf( "\n[serialize.c benchmark]\n\n" );

    buffer = (serialize_uint8_t *) malloc( BitpackerBufferSize + 8 );       /* + 8: read allocations extend 8 bytes past the data */
    if ( buffer == NULL )
    {
        return 1;
    }

    bench_bitpacker( buffer );

    bench_stream();

    printf( "\n" );

    bench_packet_shapes();

    free( buffer );

    printf( "\n(the C++ bench also prints a compile time row per shape. that surface is\n" );
    printf( " C++ template machinery with no counterpart here, and is the only thing\n" );
    printf( " this benchmark does not mirror.)\n" );

    printf( "\n" );

    return 0;
}
