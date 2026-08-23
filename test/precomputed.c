/* The mas-bandwidth/schema#82 differential: the derive-per-call compressed
   float against the precomputed one.

   Since the split, serialize_write/read/measure_compressed_float derive
   their constants with serialize_compressed_float_params and forward to the
   *_precomputed entry points — one audited home for the quantization
   arithmetic, per the C++ reference (serialize PR #83). This file is what
   makes that split a proof rather than a claim. Three implementations must
   be indistinguishable in measured bits, wire bytes, read acceptance and
   decoded BIT PATTERNS on every input:

     frozen      -- the frozen_* functions below, verbatim copies of the
                    serialize.h compressed float bodies as they stood BEFORE
                    the split. FROZEN: never edit them. they are the code the
                    audited home replaced, kept so the differential proves
                    the split changed nothing, forever.
     legacy      -- serialize_write/read/measure_compressed_float, which
                    since the split derive per call and forward to the
                    audited home.
     precomputed -- serialize_write/read/measure_compressed_float_precomputed
                    with constants derived once per declaration by
                    serialize_compressed_float_params, exactly as a schema
                    compiler derives them at generation time.

   The declaration corpus is the C++ reference differential's: every
   compressed float declaration the schema compiler's examples, bench corpus
   and test data emit (the first eleven rows, whose constants the schema
   PR #79 differential published), plus this repo's own declarations — the
   golden wire declaration, the fuzz harness declaration, diff3's battery,
   roundtrip's unsigned-ceiling declaration — plus shapes at the edges of
   the derivation itself: resolution coarser than the range, a step count
   that exactly fills its wire width, a million steps, and the clamp at the
   largest float below 2^32.

   Inputs per declaration: a dense sweep with overshoot past both bounds,
   the quantization step edges and midpoints with their one-ulp neighbors
   (the midpoints are where a fused or widened writer diverges —
   STANDARD.md's 0.005-over-[0,10] class), specials, LCG uniform in-range
   values and LCG uniform float32 bit patterns — and on the read side every
   representable wire integer including the bit headroom, exhaustively up to
   16-bit widths and sampled with the boundary codes pinned above that.
   Decoded values compare by BIT PATTERN, never by tolerance: the divergence
   a fused or widened decode produces is one ulp, invisible to any
   tolerance. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../serialize.h"

/* The caption for this build, so the two runs `make test` performs are
   telling apart in a CI log. The Makefile defines it on the second build;
   the default covers a hand build and says nothing it cannot know. */
#ifndef SERIALIZE_TEST_FP_CONTRACT
#define SERIALIZE_TEST_FP_CONTRACT "the default flags"
#endif

static int failed = 0;
#define CHECK(c) do { if (!(c)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); failed = 1; } } while (0)

static unsigned long check_count = 0;
#define DCHECK(c) do { CHECK(c); check_count++; } while (0)

/* ---------------------------------------------------------------------------
   the frozen pre-split implementation. Verbatim from serialize.h as it stood
   before the mas-bandwidth/schema#82 split, comments elided, statements
   identical — locals included: the float stores are what pin the two
   roundings. Two substitutions, neither touching the arithmetic: the frozen_
   prefix, so the oracle cannot drift if the live functions ever change, and
   the storage class spelled SERIALIZE_INLINE, as serialize.h spells these
   itself. The finite helper additionally carries SERIALIZE_UNUSED, the
   library's own marker for a definition a translation unit may not use: it
   is referenced only from serialize_assert, so under NDEBUG it goes unused,
   and clang exempts an unused static inline from warning only in a header.
   DO NOT EDIT.
   --------------------------------------------------------------------------- */

SERIALIZE_INLINE SERIALIZE_UNUSED int frozen_float_is_finite( float value )
{
    return value - value == 0.0f;
}

SERIALIZE_INLINE int frozen_compressed_float_bits( float min, float max, float res, serialize_uint32_t * max_integer_value )
{
    float delta = max - min;
    float values = delta / res;
    serialize_assert( min < max && res > 0.0f );
    serialize_assert( frozen_float_is_finite( delta ) );
    serialize_assert( frozen_float_is_finite( values ) );
    if ( !( values >= 1.0f ) )
    {
        values = 1.0f;
    }
    else if ( values > 4294967040.0f )
    {
        values = 4294967040.0f;
    }
    *max_integer_value = (serialize_uint32_t) ceil( (double) values );
    return serialize_bits_required( 0, *max_integer_value );
}

SERIALIZE_INLINE int frozen_write_compressed_float( serialize_write_stream_t * stream, float value, float min, float max, float res )
{
    float delta;
    serialize_uint32_t max_integer_value;
    int bits;
    float normalized;
    float scaled;
    serialize_uint32_t integer_value;

    serialize_assert( frozen_float_is_finite( value ) );

    delta = max - min;
    bits = frozen_compressed_float_bits( min, max, res, &max_integer_value );

    normalized = ( value - min ) / delta;
    if ( !( normalized >= 0.0f ) )
    {
        normalized = 0.0f;
    }
    else if ( !( normalized <= 1.0f ) )
    {
        normalized = 1.0f;
    }

    scaled = normalized * (float) max_integer_value;
    integer_value = (serialize_uint32_t) floor( (double) ( scaled + 0.5f ) );
    /* STANDARD.md: the integer clamp is normative (2026-08-23, schema#109) --
       same clamp as the audited home in serialize.h, same reason. The one
       amendment the frozen oracle carries, exactly as the C++ reference
       amended its own frozen reference (serialize PR #88). */
    if ( integer_value > max_integer_value )
    {
        integer_value = max_integer_value;
    }

    return serialize_write_bits( stream, integer_value, bits );
}

SERIALIZE_INLINE int frozen_read_compressed_float( serialize_read_stream_t * stream, float * value, float min, float max, float res )
{
    float delta;
    serialize_uint32_t max_integer_value;
    int bits;
    serialize_uint32_t integer_value = 0;
    float normalized;
    float scaled;

    if ( stream->error )
    {
        return 0;
    }

    delta = max - min;
    bits = frozen_compressed_float_bits( min, max, res, &max_integer_value );

    if ( !serialize_read_bits( stream, &integer_value, bits ) )
    {
        return 0;
    }

    if ( integer_value > max_integer_value )
    {
        return serialize_read_fail( stream );
    }

    normalized = (float) integer_value / (float) max_integer_value;
    scaled = normalized * delta;
    *value = scaled + min;

    return 1;
}

SERIALIZE_INLINE int frozen_measure_compressed_float( serialize_measure_stream_t * stream, float min, float max, float res )
{
    serialize_uint32_t max_integer_value;
    stream->bits_written += frozen_compressed_float_bits( min, max, res, &max_integer_value );
    return 1;
}

/* ---------------------------------------------------------------------------
   one-ulp neighbors, spelled with the bit pattern because nextafterf is C99
   and this file builds at the library's C89 floor. IEEE754 float32 ordering:
   for a finite value, the adjacent representable value toward +/- infinity
   is one step in the integer bit pattern, with the sign fold at zero.
   --------------------------------------------------------------------------- */

static float float_ulp_up( float value )
{
    serialize_uint32_t bits;
    memcpy( &bits, &value, 4 );
    if ( bits == 0x80000000UL )         /* -0 steps to the smallest positive subnormal */
    {
        bits = 0x00000001UL;
    }
    else if ( bits & 0x80000000UL )     /* negative: toward zero */
    {
        bits--;
    }
    else                                /* positive or +0: away from zero */
    {
        bits++;
    }
    memcpy( &value, &bits, 4 );
    return value;
}

static float float_ulp_down( float value )
{
    serialize_uint32_t bits;
    memcpy( &bits, &value, 4 );
    if ( bits == 0x00000000UL )         /* +0 steps to the smallest negative subnormal */
    {
        bits = 0x80000001UL;
    }
    else if ( bits & 0x80000000UL )     /* negative: away from zero */
    {
        bits++;
    }
    else                                /* positive: toward zero */
    {
        bits--;
    }
    memcpy( &value, &bits, 4 );
    return value;
}

/* ---------------------------------------------------------------------------
   THE NEGATIVE CONTROLS -- proof that this differential's eyes are open.

   Every check below compares three implementations that are supposed to
   agree, and a test where everything agrees cannot distinguish "the split
   changed nothing" from "the comparison cannot see this class of change".
   The distinction is not hypothetical here: the whole FMA discipline in the
   audited home is a pair of stores through float locals, and whether
   removing them is VISIBLE depends on the build.

   Measured on this port, clang 21 / arm64, folding the reader's two
   roundings into one expression in serialize.h:

       -ffp-contract=off   the falsified build PASSES  -- no teeth
       -ffp-contract=on    the falsified build is RED  -- teeth
       -ffp-contract=fast  the falsified build PASSES  -- no teeth

   which is the C confirmation of the C++ reference's finding
   (mas-bandwidth/schema#82, comment 5364784769): the discriminating build is
   the compiler's DEFAULT contraction, not the most aggressive one, because
   `fast` fuses the frozen oracle too and the two forms stop being
   distinguishable. This repo pins -ffp-contract=off for the wire (see the
   Makefile), which is correct for the wire and toothless for this property,
   so `make test` builds this suite a SECOND time at -ffp-contract=on.

   That covers the compiler. These two sentinels cover the rest, and they do
   it on EVERY host and every flag: they are the same one-rounding
   perturbations spelled in double, where no FMA hardware and no contraction
   setting is required to produce the divergence. Each is run alongside the
   real path over the whole corpus, and the differential FAILS if either ever
   stops diverging -- i.e. if a future edit ever makes the wire bytes or the
   decoded bit patterns blind to a single-rounding quantization. The
   divergence counts are printed, so the mass is visible and not just the
   verdict.

   These are deliberately NOT compared against the frozen oracle for
   equality. They are supposed to disagree. That is the whole point.
   --------------------------------------------------------------------------- */

static unsigned long sentinel_write_divergences = 0;
static unsigned long sentinel_read_divergences = 0;

/* the writer's quantization with the two float roundings collapsed: the
   product and the +0.5 both taken in double, rounded once by the floor.
   This is the "widened to double" perturbation, permanently on watch. */
static serialize_uint32_t sentinel_write_code_one_rounding( float value, serialize_uint32_t max_integer_value, float delta, float min )
{
    float normalized;

    normalized = ( value - min ) / delta;
    if ( !( normalized >= 0.0f ) )
    {
        normalized = 0.0f;
    }
    else if ( !( normalized <= 1.0f ) )
    {
        normalized = 1.0f;
    }

    return (serialize_uint32_t) floor( (double) normalized * (double) max_integer_value + 0.5 );
}

/* the reader's reconstruction with the multiply and the add collapsed into
   one rounding -- exactly what an FMA contraction produces, spelled in
   double so it happens on hosts without FMA too. */
static float sentinel_read_value_one_rounding( serialize_uint32_t integer_value, serialize_uint32_t max_integer_value, float delta, float min )
{
    float normalized = (float) integer_value / (float) max_integer_value;
    return (float) ( (double) normalized * (double) delta + (double) min );
}

/* ---------------------------------------------------------------------------
   Does THIS build actually fuse? -ffp-contract=on only has teeth where the
   target has an FMA instruction: on x86-64 without -mfma the compiler cannot
   contract even when permitted, and the flag then buys nothing. Rather than
   let a build claim a discipline it is not exercising, measure it and say so.

   The probe is the reconstruction under test: one statement the compiler may
   contract, against the same arithmetic forced through a volatile store,
   which must round to float32 and cannot be fused across.
   --------------------------------------------------------------------------- */

static int fp_contraction_is_live( void )
{
    volatile float vnorm;
    volatile float vprod;
    volatile float vdelta = 200.0f;
    volatile float vmin = -100.0f;
    serialize_uint32_t code;

    for ( code = 1; code < 20000; code++ )
    {
        float norm, fused, unfused;
        serialize_uint32_t pattern_fused, pattern_unfused;

        vnorm = (float) code / 20000.0f;
        norm = vnorm;

        fused = norm * vdelta + vmin;       /* one statement: contractible */
        vprod = norm * vdelta;              /* volatile: the store must round */
        unfused = vprod + vmin;

        memcpy( &pattern_fused, &fused, 4 );
        memcpy( &pattern_unfused, &unfused, 4 );
        if ( pattern_fused != pattern_unfused )
        {
            return 1;
        }
    }
    return 0;
}

/* Does this build contract ACROSS a statement boundary? That is the `fast`
   behaviour, and it is not a stricter version of `on` -- it is a different
   thing, in which the frozen oracle fuses too, every spelling of the
   arithmetic becomes indistinguishable, and STANDARD.md's requirement of
   distinct roundings simply does not hold. The library therefore does not
   support such a build: this repo pins -ffp-contract=off for exactly that
   reason, and a build at =fast fails the pinned decoded patterns below.

   It has to be DETECTED rather than assumed from the flag, because GCC
   before 14 mapped -ffp-contract=on onto fast. On such a compiler, asking
   for `on` gets `fast`, and the pinned patterns would fail with a message
   about a bit pattern instead of the truth, which is that the build is
   unsupported. So the patterns are skipped and the reason is printed.

   The discriminator is a plain local store against a volatile one. ISO C
   requires the plain store to round to float32; only a compiler contracting
   across the statement can make the two disagree. */
static int fp_contraction_crosses_statements( void )
{
    volatile float vnorm;
    volatile float vprod;
    volatile float vdelta = 200.0f;
    volatile float vmin = -100.0f;
    serialize_uint32_t code;

    for ( code = 1; code < 20000; code++ )
    {
        float norm, plain_prod, via_plain, via_volatile;
        serialize_uint32_t pattern_plain, pattern_volatile;

        vnorm = (float) code / 20000.0f;
        norm = vnorm;

        plain_prod = norm * vdelta;         /* a plain local: ISO C rounds here */
        via_plain = plain_prod + vmin;

        vprod = norm * vdelta;              /* volatile: nothing may fuse through it */
        via_volatile = vprod + vmin;

        memcpy( &pattern_plain, &via_plain, 4 );
        memcpy( &pattern_volatile, &via_volatile, 4 );
        if ( pattern_plain != pattern_volatile )
        {
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
   the two check primitives
   --------------------------------------------------------------------------- */

/* one written value through all three implementations: measured bits, wire
   bytes and decoded bit patterns must agree exactly */
static void check_value_agrees( float value, float min, float max, float res,
                                serialize_uint32_t max_integer_value, int bits, float delta )
{
    serialize_uint8_t buffer_frozen[8 + 8];         /* + 8: read buffer allocations extend 8 bytes past the data */
    serialize_uint8_t buffer_legacy[8 + 8];
    serialize_uint8_t buffer_precomputed[8 + 8];
    serialize_write_stream_t wf, wl, wp;
    serialize_read_stream_t rf, rl, rp;
    serialize_measure_stream_t mf, ml, mp;
    float decoded_frozen, decoded_legacy, decoded_precomputed;
    serialize_uint32_t pattern_frozen, pattern_legacy, pattern_precomputed;

    /* measure: all three agree on cost */
    serialize_measure_stream_init( &mf );
    serialize_measure_stream_init( &ml );
    serialize_measure_stream_init( &mp );
    DCHECK( frozen_measure_compressed_float( &mf, min, max, res ) == 1 );
    DCHECK( serialize_measure_compressed_float( &ml, min, max, res ) == 1 );
    DCHECK( serialize_measure_compressed_float_precomputed( &mp, bits ) == 1 );
    DCHECK( serialize_measure_bits_processed( &mf ) == serialize_measure_bits_processed( &ml ) );
    DCHECK( serialize_measure_bits_processed( &mf ) == serialize_measure_bits_processed( &mp ) );

    /* write: byte identical wire from all three */
    memset( buffer_frozen, 0, sizeof( buffer_frozen ) );
    memset( buffer_legacy, 0, sizeof( buffer_legacy ) );
    memset( buffer_precomputed, 0, sizeof( buffer_precomputed ) );

    serialize_write_stream_init( &wf, buffer_frozen, 8 );
    serialize_write_stream_init( &wl, buffer_legacy, 8 );
    serialize_write_stream_init( &wp, buffer_precomputed, 8 );
    DCHECK( frozen_write_compressed_float( &wf, value, min, max, res ) == 1 );
    DCHECK( serialize_write_compressed_float( &wl, value, min, max, res ) == 1 );
    DCHECK( serialize_write_compressed_float_precomputed( &wp, value, max_integer_value, bits, delta, min ) == 1 );
    serialize_write_flush( &wf );
    serialize_write_flush( &wl );
    serialize_write_flush( &wp );

    DCHECK( serialize_write_bits_processed( &wf ) == serialize_write_bits_processed( &wl ) );
    DCHECK( serialize_write_bits_processed( &wf ) == serialize_write_bits_processed( &wp ) );
    DCHECK( memcmp( buffer_frozen, buffer_legacy, 8 ) == 0 );
    DCHECK( memcmp( buffer_frozen, buffer_precomputed, 8 ) == 0 );

    /* read: decoded BIT PATTERNS agree exactly -- one ulp of divergence must fail */
    serialize_read_stream_init( &rf, buffer_frozen, 8 );
    serialize_read_stream_init( &rl, buffer_frozen, 8 );
    serialize_read_stream_init( &rp, buffer_frozen, 8 );
    decoded_frozen = 0.0f;
    decoded_legacy = 0.0f;
    decoded_precomputed = 0.0f;
    DCHECK( frozen_read_compressed_float( &rf, &decoded_frozen, min, max, res ) == 1 );
    DCHECK( serialize_read_compressed_float( &rl, &decoded_legacy, min, max, res ) == 1 );
    DCHECK( serialize_read_compressed_float_precomputed( &rp, &decoded_precomputed, max_integer_value, bits, delta, min ) == 1 );

    memcpy( &pattern_frozen, &decoded_frozen, 4 );
    memcpy( &pattern_legacy, &decoded_legacy, 4 );
    memcpy( &pattern_precomputed, &decoded_precomputed, 4 );
    DCHECK( pattern_frozen == pattern_legacy );
    DCHECK( pattern_frozen == pattern_precomputed );

    /* the write-side negative control: the one-rounding quantization must
       still be able to produce a DIFFERENT wire code than the audited home
       does, or the byte comparison above has gone blind. Read the code back
       off the frozen wire rather than recomputing it, so the control is
       measured against the bytes the comparison actually made. */
    {
        serialize_read_stream_t rs;
        serialize_uint32_t wire_code = 0;
        serialize_read_stream_init( &rs, buffer_frozen, 8 );
        if ( serialize_read_bits( &rs, &wire_code, bits ) )
        {
            if ( sentinel_write_code_one_rounding( value, max_integer_value, delta, min ) != wire_code )
            {
                sentinel_write_divergences++;
            }
        }
    }
}

/* one wire integer through all three read paths: acceptance must agree (the
   headroom refusal), and accepted codes must decode to identical bit patterns */
static void check_code_agrees( serialize_uint32_t code, float min, float max, float res,
                               serialize_uint32_t max_integer_value, int bits, float delta )
{
    serialize_uint8_t buffer[8 + 8];                /* + 8: read buffer allocations extend 8 bytes past the data */
    serialize_write_stream_t w;
    serialize_read_stream_t rf, rl, rp;
    float decoded_frozen, decoded_legacy, decoded_precomputed;
    int ok_frozen, ok_legacy, ok_precomputed;

    memset( buffer, 0, sizeof( buffer ) );
    serialize_write_stream_init( &w, buffer, 8 );
    DCHECK( serialize_write_bits( &w, code, bits ) == 1 );
    serialize_write_flush( &w );

    serialize_read_stream_init( &rf, buffer, 8 );
    serialize_read_stream_init( &rl, buffer, 8 );
    serialize_read_stream_init( &rp, buffer, 8 );
    decoded_frozen = 0.0f;
    decoded_legacy = 0.0f;
    decoded_precomputed = 0.0f;
    ok_frozen = frozen_read_compressed_float( &rf, &decoded_frozen, min, max, res );
    ok_legacy = serialize_read_compressed_float( &rl, &decoded_legacy, min, max, res );
    ok_precomputed = serialize_read_compressed_float_precomputed( &rp, &decoded_precomputed, max_integer_value, bits, delta, min );

    DCHECK( ok_frozen == ok_legacy );
    DCHECK( ok_frozen == ok_precomputed );
    DCHECK( ok_frozen == ( code <= max_integer_value ) );       /* the headroom refusal itself */

    if ( ok_frozen )
    {
        serialize_uint32_t pattern_frozen, pattern_legacy, pattern_precomputed;
        float sentinel_value;
        serialize_uint32_t pattern_sentinel;
        memcpy( &pattern_frozen, &decoded_frozen, 4 );
        memcpy( &pattern_legacy, &decoded_legacy, 4 );
        memcpy( &pattern_precomputed, &decoded_precomputed, 4 );
        DCHECK( pattern_frozen == pattern_legacy );
        DCHECK( pattern_frozen == pattern_precomputed );

        /* the read-side negative control: a one-rounding reconstruction must
           still be able to land on a DIFFERENT bit pattern, or the pattern
           comparison above has gone blind. The divergence is one ulp, which
           is exactly why this file never compares with a tolerance. */
        sentinel_value = sentinel_read_value_one_rounding( code, max_integer_value, delta, min );
        memcpy( &pattern_sentinel, &sentinel_value, 4 );
        if ( pattern_sentinel != pattern_frozen )
        {
            sentinel_read_divergences++;
        }
    }
    else
    {
        /* refusal is the read-failure convention: sticky, on every path */
        DCHECK( serialize_read_error( &rf ) );
        DCHECK( serialize_read_error( &rl ) );
        DCHECK( serialize_read_error( &rp ) );
    }
}

/* ---------------------------------------------------------------------------
   the declaration corpus
   --------------------------------------------------------------------------- */

typedef struct compressed_float_shape_t
{
    float min;
    float max;
    float res;
    serialize_uint32_t expected_max_integer_value;  /* pinned: the constants a schema compiler emits for this declaration */
    int expected_bits;                              /* (the first eleven rows are the values the schema PR #79 differential published) */
} compressed_float_shape_t;

static const compressed_float_shape_t compressed_float_shapes[] =
{
    /* the schema compiler's corpus: examples, bench/corpus/RealWorld.schema and its test data */
    { 0.0f,       2000.0f,        0.1f,       20000,       15 },
    { -2.0f,      2.0f,           0.25f,      16,          5  },
    { -90.0f,     90.0f,          0.5f,       360,         9  },
    { 0.0f,       30.0f,          0.5f,       60,          6  },
    { -100.0f,    100.0f,         0.25f,      800,         10 },
    { 0.0f,       2000.0f,        1.0f,       2000,        11 },
    { 0.0f,       10.0f,          0.02f,      500,         9  },
    { 0.0f,       100.0f,         0.01f,      10000,       14 },
    { -180.0f,    180.0f,         0.01f,      36000,       16 },
    { 0.0f,       10.0f,          0.01f,      1000,        10 },       /* also diff3's first battery */
    { -5.0f,      5.0f,           0.001f,     10000,       14 },
    /* this repo's own declarations */
    { 0.0f,       1.0f,           0.01f,      100,         7  },       /* the golden wire declaration (test/golden.c) */
    { -10.0f,     10.0f,          0.01f,      2000,        11 },       /* the fuzz harness declaration (fuzz.c) */
    { 0.0f,       1.0f,           0.001f,     1000,        10 },       /* diff3: the tightest normalized steps */
    { -100.0f,    100.0f,         0.1f,       2000,        11 },       /* diff3: a range straddling zero */
    { 0.0f,       360.0f,         0.5f,       720,         10 },       /* diff3: the angle shape */
    { -1.0f,      1.0f,           0.0001f,    20000,       15 },       /* diff3: the no-divergence range */
    { -100.0f,    100.0f,         0.01f,      20000,       15 },       /* the family's non-zero-min conformance vector, pinned below */
    { 0.0f,       1.0e9f,         0.0001f,    4294967040u, 32 },       /* roundtrip's unsigned-ceiling declaration: values clamps down */
    /* shapes at the edges of the derivation itself */
    { 0.0f,       1.0f,           2.0f,       1,           1  },       /* resolution coarser than the range: values clamps up to 1 */
    { 0.0f,       15.0f,          1.0f,       15,          4  },       /* step count exactly fills the wire width: no headroom to refuse */
    { 0.0f,       1000000.0f,     1.0f,       1000000,     20 },       /* a million steps */
    { 0.0f,       10000000000.0f, 1.0f,       4294967040u, 32 },       /* values clamps down to the largest float below 2^32 */
    /* shapes that discriminate the rounding rule itself: a fractional step count BELOW the
       half step, where ceil and round disagree. Every row above lands on an integer or within
       half a step of one, so all of them derive the same constants under either rule -- the
       corpus could not see a swap (mas-bandwidth/schema#108). */
    { 0.0f,       10.0f,          0.3f,       34,          6  },       /* 33.333332 steps: ceil 34, round 33 -- same width, different step count */
    { 0.0f,       63.3f,          1.0f,       64,          7  },       /* 63.3 steps: ceil 64 (7 bits), round 63 (6 bits) -- straddles a power of two, so the WIRE WIDTH moves */
    /* shapes in [2^23, 2^24), where the float32 ulp reaches 1 and the +0.5 rounding can push
       the code past max_integer_value before the normative clamp (2026-08-23, schema#109).
       The corpus was empty in this band, which is how the defect hid. */
    { 0.0f,       8388609.0f,     1.0f,       8388609u,    24 },       /* 2^23+1: the reader-rejects witness */
    { 0.0f,       16777215.0f,    1.0f,       16777215u,   24 }        /* 2^24-1: the wire-divergence witness */
};

/* ---------------------------------------------------------------------------
   the tests
   --------------------------------------------------------------------------- */

/* the constants the legacy path derives on every call, derived once instead:
   the precomputed read must refuse the same smuggled integers and accept the
   same conforming ones as the derive-per-call path */
static void test_precomputed_validation( void )
{
    serialize_uint32_t max_integer_value = 0;
    int bits = 0;
    float delta = 0.0f;

    serialize_compressed_float_params( 0.0f, 10.0f, 0.01f, &max_integer_value, &bits, &delta );
    CHECK( max_integer_value == 1000 );
    CHECK( bits == 10 );
    CHECK( delta == 10.0f );

    /* a malicious packet can encode integer values above max_integer_value
       in the bit headroom. reads must reject them. */
    {
        serialize_uint8_t buffer[8 + 8];        /* + 8: read buffer allocations extend 8 bytes past the data */
        serialize_write_stream_t w;
        serialize_read_stream_t r;
        float value = 0.0f;

        memset( buffer, 0, sizeof( buffer ) );
        serialize_write_stream_init( &w, buffer, 8 );
        CHECK( serialize_write_bits( &w, 1023, 10 ) );      /* max_integer_value is 1000 for [0,10] at res 0.01 -> 10 bits */
        serialize_write_flush( &w );

        serialize_read_stream_init( &r, buffer, 8 );
        CHECK( serialize_read_compressed_float_precomputed( &r, &value, max_integer_value, bits, delta, 0.0f ) == 0 );
        CHECK( serialize_read_error( &r ) );                /* refusal is sticky, like every read refusal */
    }

    /* the highest conforming integer still decodes */
    {
        serialize_uint8_t buffer[8 + 8];        /* + 8: read buffer allocations extend 8 bytes past the data */
        serialize_write_stream_t w;
        serialize_read_stream_t r;
        float value = 0.0f;

        memset( buffer, 0, sizeof( buffer ) );
        serialize_write_stream_init( &w, buffer, 8 );
        CHECK( serialize_write_bits( &w, 1000, 10 ) );
        serialize_write_flush( &w );

        serialize_read_stream_init( &r, buffer, 8 );
        CHECK( serialize_read_compressed_float_precomputed( &r, &value, max_integer_value, bits, delta, 0.0f ) == 1 );
        CHECK( !serialize_read_error( &r ) );
        CHECK( value == 10.0f );                /* 1000 / 1000 * 10 + 0: exact at the top of the range */
    }
}

/* the family's non-zero-min conformance vector, through the PRECOMPUTED entry
   points, with the constants a schema compiler derives for [-100,100] at
   resolution 0.01 written as literals -- exactly what generated code would
   pass. Same pinned bytes, same pinned decoded bit patterns as the C++
   reference's test_compressed_float_precomputed_conformance: the precomputed
   path is held to the conformance law directly, independently of the
   differential. */
static void test_precomputed_conformance( void )
{
    static const serialize_uint8_t pinned_bytes[6] = { 0x10, 0xA7, 0x06, 0x80, 0x82, 0x06 };

    /* write side: the precomputed quantization must produce exactly the pinned bytes */
    {
        serialize_uint8_t buffer[64];
        serialize_write_stream_t w;

        memset( buffer, 0, sizeof( buffer ) );
        serialize_write_stream_init( &w, buffer, sizeof( buffer ) );
        CHECK( serialize_write_compressed_float_precomputed( &w, 0.0f,    20000, 15, 200.0f, -100.0f ) );
        CHECK( serialize_write_compressed_float_precomputed( &w, -99.875f, 20000, 15, 200.0f, -100.0f ) );
        CHECK( serialize_write_compressed_float_precomputed( &w, -33.34f, 20000, 15, 200.0f, -100.0f ) );
        CHECK( serialize_write_align( &w ) );
        serialize_write_flush( &w );
        CHECK( !serialize_write_error( &w ) );
        CHECK( serialize_write_bytes_processed( &w ) == (int) sizeof( pinned_bytes ) );
        CHECK( memcmp( buffer, pinned_bytes, sizeof( pinned_bytes ) ) == 0 );
    }

    /* read side: the decoded floats are pinned bit-exactly */
    {
        serialize_uint8_t buffer[64];           /* comfortably more than data + 8: read allocations extend 8 bytes past the data */
        serialize_read_stream_t r;
        float a, b, c;
        serialize_uint32_t bits_a, bits_b, bits_c;

        memset( buffer, 0, sizeof( buffer ) );
        memcpy( buffer, pinned_bytes, sizeof( pinned_bytes ) );
        serialize_read_stream_init( &r, buffer, (int) sizeof( pinned_bytes ) );
        a = -1.0f;
        b = -1.0f;
        c = -1.0f;
        CHECK( serialize_read_compressed_float_precomputed( &r, &a, 20000, 15, 200.0f, -100.0f ) );
        CHECK( serialize_read_compressed_float_precomputed( &r, &b, 20000, 15, 200.0f, -100.0f ) );
        CHECK( serialize_read_compressed_float_precomputed( &r, &c, 20000, 15, 200.0f, -100.0f ) );
        CHECK( serialize_read_align( &r ) );
        CHECK( !serialize_read_error( &r ) );
        memcpy( &bits_a, &a, 4 );
        memcpy( &bits_b, &b, 4 );
        memcpy( &bits_c, &c, 4 );
        /* the decoded patterns are pinned only where the build keeps the
           roundings distinct. A build that contracts across statements
           reconstructs one of these one ulp away by design, and the honest
           report is that the build is unsupported -- not a bit pattern
           mismatch that reads like a port bug. The wire bytes above are
           pinned unconditionally: they do not move under contraction. */
        if ( fp_contraction_crosses_statements() )
        {
            printf( "precomputed differential: SKIPPING the pinned decoded patterns -- this build contracts ACROSS statements (-ffp-contract=fast, or a GCC before 14 mapping =on onto =fast), which STANDARD.md's compressed float does not admit\n" );
        }
        else
        {
            CHECK( bits_a == 0x00000000UL );
            CHECK( bits_b == 0xC2C7BD71UL );
            CHECK( bits_c == 0xC2055C2AUL );
        }
    }
}

static void test_precomputed_differential( void )
{
    const float float_max = 3.402823466e+38f;               /* FLT_MAX, spelled so no float.h is needed */

    serialize_uint64_t lcg = 0xC0FFEE1234567890ULL;         /* fixed seed: failures reproduce */

#define next_lcg() ( lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL )

    const int num_shapes = (int) ( sizeof( compressed_float_shapes ) / sizeof( compressed_float_shapes[0] ) );
    int s;

    for ( s = 0; s < num_shapes; s++ )
    {
        const float min = compressed_float_shapes[s].min;
        const float max = compressed_float_shapes[s].max;
        const float res = compressed_float_shapes[s].res;

        serialize_uint32_t max_integer_value = 0;
        int bits = 0;
        float delta = 0.0f;

        double dmin, ddelta;

        serialize_compressed_float_params( min, max, res, &max_integer_value, &bits, &delta );

        /* the derived constants are pinned against the schema compiler's generation-time table */
        DCHECK( max_integer_value == compressed_float_shapes[s].expected_max_integer_value );
        DCHECK( bits == compressed_float_shapes[s].expected_bits );
        DCHECK( delta == max - min );

        dmin = (double) min;
        ddelta = (double) delta;

        /* dense sweep with overshoot a quarter of the range past both bounds */
        {
            const int sweep_steps = 2048;
            const double lo = dmin - 0.25 * ddelta;
            const double span = 1.5 * ddelta;
            int i;
            for ( i = 0; i <= sweep_steps; i++ )
            {
                const float value = (float) ( lo + span * i / sweep_steps );
                check_value_agrees( value, min, max, res, max_integer_value, bits, delta );
            }
        }

        /* quantization step edges and midpoints, with their one-ulp neighbors.
           the midpoints are the discriminating band: 0.005 over [0,10] at 0.01
           quantizes to 1 under the required two roundings and to 0 under a
           fused or widened writer (STANDARD.md) */
        {
            const serialize_uint32_t stride = max_integer_value / 512 + 1;
            serialize_uint64_t k;
            for ( k = 0; k <= max_integer_value; k += stride )      /* 64 bit: k += stride must not wrap at the 2^32-clamped shapes */
            {
                const float on_quantum = (float) ( dmin + ddelta * ( (double) k / (double) max_integer_value ) );
                const float midpoint = (float) ( dmin + ddelta * ( ( (double) k + 0.5 ) / (double) max_integer_value ) );
                check_value_agrees( on_quantum, min, max, res, max_integer_value, bits, delta );
                check_value_agrees( float_ulp_down( on_quantum ), min, max, res, max_integer_value, bits, delta );
                check_value_agrees( float_ulp_up( on_quantum ), min, max, res, max_integer_value, bits, delta );
                check_value_agrees( midpoint, min, max, res, max_integer_value, bits, delta );
                check_value_agrees( float_ulp_down( midpoint ), min, max, res, max_integer_value, bits, delta );
                check_value_agrees( float_ulp_up( midpoint ), min, max, res, max_integer_value, bits, delta );
            }
        }

        /* specials: the bounds and their one-ulp neighbors, both zeros,
           subnormals, extremes */
        {
            float specials[24];
            int i;
            specials[0]  = min;
            specials[1]  = max;
            specials[2]  = float_ulp_down( min );
            specials[3]  = float_ulp_up( min );
            specials[4]  = float_ulp_down( max );
            specials[5]  = float_ulp_up( max );
            specials[6]  = min - res;
            specials[7]  = max + res;
            specials[8]  = min + res * 0.5f;
            specials[9]  = max - res * 0.5f;
            specials[10] = min - delta;
            specials[11] = max + delta;
            specials[12] = 0.0f;
            specials[13] = -0.0f;
            specials[14] = res;
            specials[15] = -res;
            specials[16] = float_max;
            specials[17] = -float_max;
            specials[18] = 1.175494351e-38f;                /* FLT_MIN */
            specials[19] = -1.175494351e-38f;
            specials[20] = 1.401298464e-45f;                /* the smallest subnormal */
            specials[21] = -1.401298464e-45f;
            specials[22] = 1.0e30f;
            specials[23] = -1.0e30f;
            for ( i = 0; i < (int) ( sizeof( specials ) / sizeof( specials[0] ) ); i++ )
            {
                check_value_agrees( specials[i], min, max, res, max_integer_value, bits, delta );
            }
        }

#if defined( NDEBUG )
        /* non-finite inputs are non-conforming and assert in a debug build
           (test/assertdeath.c proves they do), so a release build is where
           the differential can drive them: the clamp must force NaN and both
           infinities to the same wire in all three implementations */
        {
            static const serialize_uint32_t non_finite_patterns[5] = { 0x7F800000UL, 0xFF800000UL, 0x7FC00000UL, 0x7F800001UL, 0xFFC00001UL };
            int i;
            for ( i = 0; i < 5; i++ )
            {
                float value;
                memcpy( &value, &non_finite_patterns[i], 4 );
                check_value_agrees( value, min, max, res, max_integer_value, bits, delta );
            }
        }
#endif /* NDEBUG */

        /* LCG uniform values across the range and its overshoot band */
        {
            int i;
            for ( i = 0; i < 2048; i++ )
            {
                const double fraction = (double) ( next_lcg() >> 11 ) * ( 1.0 / 9007199254740992.0 );   /* [0,1) in 53 bits */
                const float value = (float) ( dmin - 0.25 * ddelta + fraction * 1.5 * ddelta );
                check_value_agrees( value, min, max, res, max_integer_value, bits, delta );
            }
        }

        /* LCG uniform float32 bit patterns, finite ones (non-finite writes
           assert in a debug build; the release-only block above drives those
           deliberately) */
        {
            int i;
            for ( i = 0; i < 2048; i++ )
            {
                const serialize_uint32_t pattern = (serialize_uint32_t) ( next_lcg() >> 32 );
                float value;
                memcpy( &value, &pattern, 4 );
                if ( value - value == 0.0f )
                {
                    check_value_agrees( value, min, max, res, max_integer_value, bits, delta );
                }
            }
        }

        /* the read side: every representable wire integer, including the bit
           headroom a malicious packet can encode into. exhaustive up to
           16-bit widths; above that the boundary codes are pinned and the
           interior is sampled */
        {
            const serialize_uint32_t top_code = ( bits == 32 ) ? 0xFFFFFFFFUL : ( ( 1UL << bits ) - 1UL );
            if ( bits <= 16 )
            {
                serialize_uint32_t code;
                for ( code = 0; code <= top_code; code++ )
                {
                    check_code_agrees( code, min, max, res, max_integer_value, bits, delta );
                }
            }
            else
            {
                serialize_uint32_t code;
                serialize_uint32_t window_lo, window_hi;
                int i;
                for ( code = 0; code <= 1024; code++ )
                {
                    check_code_agrees( code, min, max, res, max_integer_value, bits, delta );
                }
                window_lo = max_integer_value - 512;                    /* max_integer_value >= 2^16 here, so no underflow */
                window_hi = ( top_code - max_integer_value < 512 ) ? top_code : max_integer_value + 512;
                for ( code = window_lo; code <= window_hi && code >= window_lo; code++ )
                {
                    check_code_agrees( code, min, max, res, max_integer_value, bits, delta );
                }
                for ( code = top_code - 64; code <= top_code && code >= top_code - 64; code++ )
                {
                    check_code_agrees( code, min, max, res, max_integer_value, bits, delta );
                }
                for ( i = 0; i < 2048; i++ )
                {
                    code = ( (serialize_uint32_t) ( next_lcg() >> 32 ) ) & top_code;
                    check_code_agrees( code, min, max, res, max_integer_value, bits, delta );
                }
            }
        }
    }

#undef next_lcg

    /* the coverage floor: if the differential ever silently shrinks below the
       mass it was built with, that is a test bug, and it fails here instead
       of fading quietly */
    CHECK( check_count >= 3000000UL );

    printf( "precomputed differential: %lu checks, three implementations, %d declarations\n",
            check_count, num_shapes );

    /* the negative controls, checked rather than merely reported: if a
       one-rounding writer ever stops producing a different wire code, or a
       one-rounding reader ever stops producing a different bit pattern,
       then the comparisons above agree for a reason that has nothing to do
       with the split being correct, and this suite is decoration. */
    CHECK( sentinel_write_divergences > 0 );
    CHECK( sentinel_read_divergences > 0 );
    printf( "precomputed differential: negative controls diverge on %lu wire codes and %lu decoded patterns (both must be nonzero, or the comparison cannot see a single-rounding quantization)\n",
            sentinel_write_divergences, sentinel_read_divergences );
}

int main( void )
{
#ifdef SERIALIZE_TEST_FP_CONTRACT_REQUESTED_ON
    /* The Makefile's second build asked for STATEMENT-LOCAL contraction. GCC
       before 14 mapped -ffp-contract=on onto =fast and gives cross-statement
       contraction instead, which is a different and unsupported thing. That
       is a fact about the toolchain, not a defect in the library, so this
       build stands down rather than reporting a failure it did not find.
       The -ffp-contract=off run is still the gate on such a compiler, and
       the negative controls below carry the discrimination there. */
    if ( fp_contraction_crosses_statements() )
    {
        printf( "precomputed differential: asked for -ffp-contract=on and this compiler produced CROSS-STATEMENT contraction (GCC before 14 maps =on onto =fast), so the discriminating build is not available on this toolchain -- standing down\n" );
        printf( "OK (skipped)\n" );
        return 0;
    }
#endif

    /* which build this is, and whether its contraction setting is buying
       anything on this target. Reported, never asserted: a host without an
       FMA instruction cannot fuse however the flag is set, and that is a
       fact about the host, not a failure. */
    printf( "precomputed differential: built with %s; fp contraction is %s in this build\n",
            SERIALIZE_TEST_FP_CONTRACT,
            !fp_contraction_is_live()             ? "not available -- this target does not fuse, so the float stores are compiled but not exercised"
            : fp_contraction_crosses_statements() ? "LIVE but crossing statements -- a =fast build, in which no spelling of this arithmetic is distinguishable from another"
                                                  : "LIVE and statement-local -- the audited home's float stores are under test" );

    test_precomputed_validation();
    test_precomputed_conformance();
    test_precomputed_differential();

    printf( failed ? "FAILED\n" : "OK\n" );
    return failed;
}
