/*
    The negative controls' sabotage: serialize.h with ONE refusal rule
    deleted, for `make controls`.

        sabotage <control> <serialize.h> <out/serialize.h>

    STANDARD.md, "The conformance corpus": a gate nobody has seen go red is a
    claim, not a measurement. For each refusal rule, a reader with that one
    rule deleted is run against the real corpus, and it must fail EXACTLY the
    vectors conformance-controls/<rule>.expected names -- vendored from the
    reference, so the oracle is the family's and not this port's. A sabotage
    that breaks more than its rule, or less, measures something other than
    what it claims, and the control goes red.

    Each sabotage is a string replacement on a scratch copy of the header,
    the C twin of the C++ reference's CMakeLists.txt table. The replacement
    keeps the sabotaged line compiling warning free under the strict set and
    reading every variable the deleted rule read: the added conjunct is false
    exactly when the rule would have fired. A needle that stops matching is a
    hard failure rather than a silent no-op, because a control that has
    quietly stopped sabotaging anything is worse than no control at all: when
    the line it deletes moves or changes, update the needle here, and never
    delete the control.

    A needle marked `every` is replaced at each occurrence (the rule lives in
    several places, all of which must go); any other needle must occur
    exactly once.
*/

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    const char * control;
    const char * rule;
    int every;
    const char * needle;
    const char * patch;
} sabotage_t;

static const sabotage_t sabotages[] =
{
    { "align-padding", "a reader verifies that an align's padding bits are zero", 0,
      "        if ( padding != 0 )",
      "        if ( padding == 0xFFFFFFFFu ) /* NEGATIVE CONTROL: this rule is deleted on purpose */" },

    /* two controls over the ranged int, one per rule: the reconstruction
       decides what an accepted offset means, the range check which offsets
       are accepted at all */
    { "ranged-reconstruction", "a ranged int decodes to its offset plus min", 0,
      "    *value = (serialize_int32_t) ( (serialize_uint32_t) min + offset );",
      "    *value = (serialize_int32_t) offset; /* NEGATIVE CONTROL: min is dropped on purpose */" },

    /* the widened right hand side is at least 0xFFFFFFFF, and the offset is
       at most 0xFFFFFFFF */
    { "ranged-range", "a ranged int whose offset is past the span is refused", 0,
      "    if ( offset > (serialize_uint32_t) max - (serialize_uint32_t) min )",
      "    if ( (serialize_uint64_t) offset > (serialize_uint64_t) ( (serialize_uint32_t) max - (serialize_uint32_t) min ) + 0xFFFFFFFFu ) /* NEGATIVE CONTROL: this rule is deleted on purpose */" },

    /* the degenerate fixed point field consumes no bits and would otherwise
       always succeed; the guard lives in each fixed entry point and in the
       shared core, and all of them go */
    { "fixed-degenerate-terminal", "every read consults the failure state, zero bit reads included", 1,
      "if ( stream->error ) return 0;",
      "if ( stream->error && stream->num_bits < 0 ) return 0; /* NEGATIVE CONTROL: this rule is deleted on purpose */" },

    /* the finite rule, once per width and once for the compressed float
       decode: zero is finite, so the added conjunct never holds */
    { "float-finite", "a float read refuses NaN and the infinities", 0,
      "    if ( !serialize_float_bits_are_finite( bits ) )",
      "    if ( !serialize_float_bits_are_finite( bits ) && bits == 0u ) /* NEGATIVE CONTROL: this rule is deleted on purpose */" },

    { "double-finite", "a double read refuses NaN and the infinities", 0,
      "    if ( !serialize_double_bits_are_finite( bits ) )",
      "    if ( !serialize_double_bits_are_finite( bits ) && bits == 0u ) /* NEGATIVE CONTROL: this rule is deleted on purpose */" },

    { "compressed-float-finite", "a compressed float decode that is not finite is refused", 0,
      "    if ( !serialize_float_bits_are_finite( decoded_bits ) )",
      "    if ( !serialize_float_bits_are_finite( decoded_bits ) && decoded_bits == 0u ) /* NEGATIVE CONTROL: this rule is deleted on purpose */" },

    /* the end's two rules. Without the trailing data rule the end checks only
       the low bits of whatever remains, so whole zero bytes left over pass */
    { "end-trailing-data", "an end refuses a whole byte after the object", 0,
      "    if ( remaining >= 8 )",
      "    if ( remaining >= 8 && stream->bits_read < 0 ) /* NEGATIVE CONTROL: this rule is deleted on purpose */" },

    { "end-padding", "an end refuses non-zero padding in the final byte", 0,
      "        if ( final_padding != 0 )",
      "        if ( final_padding == 0xFFFFFFFFu ) /* NEGATIVE CONTROL: this rule is deleted on purpose */" },

    /* current is above previous here, so it is never zero */
    { "int-relative-canonical", "the absolute tier refuses a difference a bounded tier carries", 0,
      "    if ( absolute && reconstructed - (serialize_uint32_t) previous <= 69914u )",
      "    if ( absolute && reconstructed - (serialize_uint32_t) previous <= 69914u && reconstructed == 0u ) /* NEGATIVE CONTROL: this rule is deleted on purpose */" },

    { "string-embedded-nul", "a string read refuses a zero byte among its payload", 0,
      "            if ( string[i] == '\\0' )",
      "            if ( string[i] == '\\0' && i < 0 ) /* NEGATIVE CONTROL: this rule is deleted on purpose */" },

    { "string-utf8", "a string read refuses a payload that is not well formed UTF-8", 0,
      "        if ( !serialize_string_is_valid_utf8( string, length ) )",
      "        if ( !serialize_string_is_valid_utf8( string, length ) && length < 0 ) /* NEGATIVE CONTROL: this rule is deleted on purpose */" },

    /* one clause of the UTF-8 rule alone: the encoded surrogates, ED A0 80
       through ED BF BF */
    { "string-utf8-surrogate", "a string read refuses an encoded surrogate code point", 0,
      "            if ( lead == 0xED && b1 >= 0xA0 ) return 0;",
      "            if ( lead == 0xED && b1 >= 0xA0 && b1 > 0xFF ) return 0; /* NEGATIVE CONTROL: this rule is deleted on purpose */" },

    /* a reader whose ranged 64 bit integer passes through a double, as a port
       backed by a double number type would: every integer up to 2^53
       survives, and the wide vectors past it do not */
    { "int64-through-double", "a 64 bit value is carried exactly, never through a double", 0,
      "    *value = (serialize_int64_t) ( (serialize_uint64_t) min + offset );",
      "    { const double through_double = (double) (serialize_int64_t) ( (serialize_uint64_t) min + offset ); *value = through_double >= 9223372036854775807.0 ? (serialize_int64_t) ( ~(serialize_uint64_t) 0 >> 1 ) : (serialize_int64_t) through_double; } /* NEGATIVE CONTROL: the value goes through a double on purpose */" },

    /* the refusal reason: this reader refuses every stream the real one
       refuses, and names the wrong rule for one of them */
    { "wrong-reason", "a refusal names the rule that refused it", 0,
      "                return serialize_read_fail( stream, SERIALIZE_REFUSAL_EMBEDDED_NUL );",
      "                return serialize_read_fail( stream, SERIALIZE_REFUSAL_INVALID_UTF8 ); /* NEGATIVE CONTROL: the wrong reason on purpose */" },
};

static char * read_file( const char * path, long * size )
{
    FILE * file = fopen( path, "rb" );
    char * text;
    if ( file == NULL )
    {
        return NULL;
    }
    fseek( file, 0, SEEK_END );
    *size = ftell( file );
    fseek( file, 0, SEEK_SET );
    text = (char *) malloc( (size_t) *size + 1 );
    if ( text == NULL || fread( text, 1, (size_t) *size, file ) != (size_t) *size )
    {
        fclose( file );
        free( text );
        return NULL;
    }
    text[*size] = '\0';
    fclose( file );
    return text;
}

static int count_occurrences( const char * text, const char * needle )
{
    int count = 0;
    const char * cursor = text;
    size_t length = strlen( needle );
    while ( ( cursor = strstr( cursor, needle ) ) != NULL )
    {
        count++;
        cursor += length;
    }
    return count;
}

int main( int argc, char ** argv )
{
    const sabotage_t * sabotage = NULL;
    const char * cursor;
    char * text;
    long size = 0;
    int occurrences;
    size_t needle_length;
    FILE * out;
    size_t i;

    if ( argc != 4 )
    {
        printf( "usage: sabotage <control> <serialize.h> <out/serialize.h>\n" );
        return 1;
    }

    for ( i = 0; i < sizeof( sabotages ) / sizeof( sabotages[0] ); i++ )
    {
        if ( strcmp( sabotages[i].control, argv[1] ) == 0 )
        {
            sabotage = &sabotages[i];
        }
    }
    if ( sabotage == NULL )
    {
        printf( "sabotage: no control named '%s'\n", argv[1] );
        return 1;
    }

    text = read_file( argv[2], &size );
    if ( text == NULL )
    {
        printf( "sabotage: cannot read %s\n", argv[2] );
        return 1;
    }

    occurrences = count_occurrences( text, sabotage->needle );
    if ( occurrences == 0 || ( !sabotage->every && occurrences != 1 ) )
    {
        printf( "sabotage: control '%s' found %d occurrence(s) of its needle in %s, and needs %s. The line it deletes "
                "has moved or changed, so the control is no longer proving that deleting '%s' turns the corpus red. "
                "Update the needle in tools/sabotage.c; do not delete the control.\n",
                sabotage->control, occurrences, argv[2], sabotage->every ? "at least one" : "exactly one", sabotage->rule );
        free( text );
        return 1;
    }

    out = fopen( argv[3], "wb" );
    if ( out == NULL )
    {
        printf( "sabotage: cannot write %s\n", argv[3] );
        free( text );
        return 1;
    }

    needle_length = strlen( sabotage->needle );
    cursor = text;
    for ( ;; )
    {
        const char * found = strstr( cursor, sabotage->needle );
        if ( found == NULL )
        {
            fputs( cursor, out );
            break;
        }
        fwrite( cursor, 1, (size_t) ( found - cursor ), out );
        fputs( sabotage->patch, out );
        cursor = found + needle_length;
    }

    fclose( out );
    free( text );

    return 0;
}
