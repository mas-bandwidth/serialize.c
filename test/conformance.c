/*
    The shared conformance corpus, run through this library's reader.

    conformance/ is a VERBATIM VENDORED COPY of the corpus in
    mas-bandwidth/serialize, the same way STANDARD.md is, and CI's
    `conformance corpus matches upstream` job holds the copy to it. The corpus
    is the family's one conformance instrument: every implementation runs the
    same vectors, so one wrong reading of the standard cannot travel to nine
    ports under green results the way it does when each port generates its own
    expectations and then checks them against itself.

    Every vector in every file is run. The directory is SCANNED rather than
    listed here, so a file upstream adds arrives with the sync and runs on the
    next build with no edit to this file, and an operation this runner does not
    know is a FAILURE rather than a silent skip.

    What each vector asserts, from STANDARD.md's "The vector format":

      accepted   the read succeeds, the decoded value is the stated one, and
                 serialize_read_bits_processed equals the stated `consumed`
      refused    the read fails, the stream is left in its failed state, and
                 the destination is UNWRITTEN -- the non-mutation rule under
                 Reader Obligations, checked here against a sentinel the
                 caller planted before the call

    After a refusal the stream position is not part of the contract, so no
    vector states one and nothing here checks one.
*/

/* MSVC's CRT deprecates sprintf in favour of sprintf_s, which no other
   toolchain has. Every use here writes a bounded string into a fixed
   destination -- failure messages from fields capped by MAX_TEXT, and vector
   file paths -- so the portable spelling stays and the warning stands down;
   without this the Windows leg carries seven C4996 lines and nothing else.
   Must precede every CRT header. */
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../serialize.h"
#include "verbose.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

#define MAX_PARAMS 8
#define MAX_BYTES 256
#define MAX_TEXT 96

typedef struct
{
    char operation[MAX_TEXT];
    char name[MAX_TEXT];
    char param_name[MAX_PARAMS][MAX_TEXT];
    char param_value[MAX_PARAMS][MAX_TEXT];
    int param_count;
    serialize_uint8_t bytes[MAX_BYTES];
    int byte_count;
    int refused;
    char expect_value[MAX_TEXT];
    serialize_int64_t consumed;
    int have_consumed;
} vector_t;

static int failed = 0;
static int vectors_run = 0;

static void fail( const vector_t * v, const char * what )
{
    printf( "FAILED %s/%s: %s\n", v->operation, v->name, what );
    failed = 1;
}

/* ---------------------------------------------------------------------------
   128-bit decimal, in and out

   The corpus states int128 parameters and values as decimal, and this
   library's 128-bit type is two 64-bit lanes rather than a native __int128
   (see serialize.h), so the conversion is done by hand in both directions.
   --------------------------------------------------------------------------- */

static serialize_uint128_t u128_shl1( serialize_uint128_t v )
{
    serialize_uint128_t r;
    r.hi = ( v.hi << 1 ) | ( v.lo >> 63 );
    r.lo = v.lo << 1;
    return r;
}

static serialize_uint128_t u128_neg( serialize_uint128_t v )
{
    serialize_uint128_t n;
    n.hi = ~v.hi;
    n.lo = ~v.lo;
    return serialize_u128_add( n, serialize_uint128_make( 0, 1 ) );
}

/* divides in place and returns the remainder, one 32-bit group at a time so
   the intermediate never needs more than 64 bits */
static unsigned int u128_divmod10( serialize_uint128_t * v )
{
    serialize_uint64_t group[4];
    serialize_uint64_t rem = 0;
    int i;

    group[3] = v->hi >> 32;
    group[2] = v->hi & 0xFFFFFFFFu;
    group[1] = v->lo >> 32;
    group[0] = v->lo & 0xFFFFFFFFu;

    for ( i = 3; i >= 0; i-- )
    {
        serialize_uint64_t cur = ( rem << 32 ) | group[i];
        group[i] = cur / 10;
        rem = cur % 10;
    }

    v->hi = ( group[3] << 32 ) | group[2];
    v->lo = ( group[1] << 32 ) | group[0];

    return (unsigned int) rem;
}

static serialize_int128_t parse_int128( const char * text )
{
    serialize_uint128_t acc = { 0, 0 };
    int negative = 0;
    int i = 0;

    if ( text[0] == '-' )
    {
        negative = 1;
        i = 1;
    }
    else if ( text[0] == '+' )
    {
        i = 1;
    }

    for ( ; text[i] >= '0' && text[i] <= '9'; i++ )
    {
        serialize_uint128_t x2 = u128_shl1( acc );
        serialize_uint128_t x8 = u128_shl1( u128_shl1( x2 ) );
        acc = serialize_u128_add( serialize_u128_add( x8, x2 ),
                                  serialize_uint128_make( 0, (serialize_uint64_t) ( text[i] - '0' ) ) );
    }

    if ( negative )
    {
        acc = u128_neg( acc );
    }

    return serialize_int128_make( acc.hi, acc.lo );
}

/* writes into a caller buffer of at least 44 bytes */
static void print_int128( char * out, serialize_int128_t value )
{
    serialize_uint128_t magnitude = serialize_uint128_make( value.hi, value.lo );
    char digits[44];
    int negative = ( value.hi >> 63 ) != 0;
    int n = 0;
    int i;
    int o = 0;

    if ( negative )
    {
        magnitude = u128_neg( magnitude );
    }

    do
    {
        digits[n++] = (char) ( '0' + u128_divmod10( &magnitude ) );
    }
    while ( magnitude.hi != 0 || magnitude.lo != 0 );

    if ( negative )
    {
        out[o++] = '-';
    }
    for ( i = n - 1; i >= 0; i-- )
    {
        out[o++] = digits[i];
    }
    out[o] = '\0';
}

/* ---------------------------------------------------------------------------
   the operations
   --------------------------------------------------------------------------- */

static const char * param( const vector_t * v, const char * name )
{
    int i;
    for ( i = 0; i < v->param_count; i++ )
    {
        if ( strcmp( v->param_name[i], name ) == 0 )
        {
            return v->param_value[i];
        }
    }
    return NULL;
}

/* Every operation reads through this: the vector's bytes are copied behind
   the 8 slack bytes serialize_read_stream_init requires, so a corpus payload
   sized exactly to its data is legal to read (see the read stream struct). */
static void open_stream( serialize_read_stream_t * r, serialize_uint8_t * copy, const vector_t * v )
{
    serialize_read_stream_init_padded( r, copy, MAX_BYTES + 8, v->bytes, v->byte_count );
}

static void run_int_relative( const vector_t * v )
{
    const serialize_int32_t sentinel = -777777;
    serialize_uint8_t copy[MAX_BYTES + 8];
    serialize_read_stream_t r;
    serialize_int32_t previous;
    serialize_int32_t got = sentinel;
    const char * previous_text = param( v, "previous" );
    int ok;

    if ( previous_text == NULL )
    {
        fail( v, "no previous parameter" );
        return;
    }
    previous = (serialize_int32_t) strtol( previous_text, NULL, 10 );

    open_stream( &r, copy, v );
    ok = serialize_read_int_relative( &r, previous, &got );

    if ( v->refused )
    {
        if ( ok )
        {
            char message[256];
            sprintf( message, "accepted, expected refused (got %ld)", (long) got );
            fail( v, message );
        }
        else if ( !serialize_read_error( &r ) )
        {
            fail( v, "refused without failing the stream" );
        }
        else if ( got != sentinel )
        {
            fail( v, "refused but wrote the destination" );
        }
        return;
    }

    if ( !ok )
    {
        fail( v, "refused, expected accepted" );
        return;
    }
    if ( got != (serialize_int32_t) strtol( v->expect_value, NULL, 10 ) )
    {
        char message[256];
        sprintf( message, "value %ld, expected %s", (long) got, v->expect_value );
        fail( v, message );
    }
    if ( v->have_consumed && serialize_read_bits_processed( &r ) != v->consumed )
    {
        char message[256];
        sprintf( message, "consumed %ld bits, expected %ld",
                 (long) serialize_read_bits_processed( &r ), (long) v->consumed );
        fail( v, message );
    }
}

static void run_int128( const vector_t * v )
{
    serialize_uint8_t copy[MAX_BYTES + 8];
    serialize_read_stream_t r;
    serialize_int128_t sentinel, min, max, expected, got;
    const char * min_text = param( v, "min" );
    const char * max_text = param( v, "max" );
    int ok;

    sentinel = serialize_int128_make( 0xDEADBEEFDEADBEEFULL, 0x0123456789ABCDEFULL );
    got = sentinel;

    if ( min_text == NULL || max_text == NULL )
    {
        fail( v, "no min/max parameters" );
        return;
    }
    min = parse_int128( min_text );
    max = parse_int128( max_text );

    open_stream( &r, copy, v );
    ok = serialize_read_int128( &r, &got, min, max );

    if ( v->refused )
    {
        if ( ok )
        {
            fail( v, "accepted, expected refused" );
        }
        else if ( !serialize_read_error( &r ) )
        {
            fail( v, "refused without failing the stream" );
        }
        else if ( !serialize_int128_equal( got, sentinel ) )
        {
            fail( v, "refused but wrote the destination" );
        }
        return;
    }

    if ( !ok )
    {
        fail( v, "refused, expected accepted" );
        return;
    }

    expected = parse_int128( v->expect_value );
    if ( !serialize_int128_equal( got, expected ) )
    {
        char message[256];
        char printed[44];
        print_int128( printed, got );
        sprintf( message, "value %s, expected %s", printed, v->expect_value );
        fail( v, message );
    }
    if ( v->have_consumed && serialize_read_bits_processed( &r ) != v->consumed )
    {
        char message[256];
        sprintf( message, "consumed %ld bits, expected %ld",
                 (long) serialize_read_bits_processed( &r ), (long) v->consumed );
        fail( v, message );
    }
}

static void run_vector( const vector_t * v )
{
    vectors_run++;

    if ( strcmp( v->operation, "int_relative" ) == 0 )
    {
        run_int_relative( v );
    }
    else if ( strcmp( v->operation, "int128" ) == 0 )
    {
        run_int128( v );
    }
    else
    {
        /* an operation the corpus carries and this runner does not drive is a
           gap in this library's conformance, not a vector to skip past */
        fail( v, "no runner for this operation" );
    }
}

/* ---------------------------------------------------------------------------
   the vector format (STANDARD.md, "The vector format")
   --------------------------------------------------------------------------- */

static int hex_digit( char c )
{
    if ( c >= '0' && c <= '9' ) return c - '0';
    if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
    if ( c >= 'A' && c <= 'F' ) return c - 'A' + 10;
    return -1;
}

static void copy_field( char * out, const char * in )
{
    int i;
    while ( *in == ' ' || *in == '\t' )
    {
        in++;
    }
    for ( i = 0; i < MAX_TEXT - 1 && in[i] != '\0'; i++ )
    {
        out[i] = in[i];
    }
    while ( i > 0 && ( out[i - 1] == ' ' || out[i - 1] == '\t' ) )
    {
        i--;
    }
    out[i] = '\0';
}

static int parse_bytes( vector_t * v, const char * text )
{
    v->byte_count = 0;
    while ( *text != '\0' )
    {
        int high, low;
        while ( *text == ' ' || *text == '\t' )
        {
            text++;
        }
        if ( *text == '\0' )
        {
            break;
        }
        high = hex_digit( text[0] );
        low = text[0] == '\0' ? -1 : hex_digit( text[1] );
        if ( high < 0 || low < 0 || v->byte_count >= MAX_BYTES )
        {
            return 0;
        }
        v->bytes[v->byte_count++] = (serialize_uint8_t) ( high * 16 + low );
        text += 2;
    }
    return 1;
}

static void clear_vector( vector_t * v )
{
    memset( v, 0, sizeof( *v ) );
}

static int run_file( const char * path )
{
    FILE * file = fopen( path, "r" );
    char line[512];
    vector_t v;
    int have_record = 0;
    int line_number = 0;

    if ( file == NULL )
    {
        printf( "FAILED: cannot open %s\n", path );
        failed = 1;
        return 0;
    }

    clear_vector( &v );

    while ( fgets( line, (int) sizeof( line ), file ) != NULL )
    {
        char * hash;
        char * key;
        char * value;
        int blank;
        int i;

        line_number++;

        /* Trimmed BEFORE the comment is stripped, because only a genuinely
           blank line separates records: a comment-only line inside a record
           would otherwise split it in two, and the halves would fail as
           records missing an operation. */
        for ( i = (int) strlen( line ); i > 0; i-- )
        {
            char c = line[i - 1];
            if ( c == '\n' || c == '\r' || c == ' ' || c == '\t' )
            {
                line[i - 1] = '\0';
            }
            else
            {
                break;
            }
        }
        blank = line[0] == '\0';

        hash = strchr( line, '#' );
        if ( hash != NULL )
        {
            *hash = '\0';
            for ( i = (int) strlen( line ); i > 0; i-- )
            {
                char c = line[i - 1];
                if ( c == ' ' || c == '\t' )
                {
                    line[i - 1] = '\0';
                }
                else
                {
                    break;
                }
            }
        }

        if ( blank )
        {
            if ( have_record )
            {
                run_vector( &v );
                clear_vector( &v );
                have_record = 0;
            }
            continue;
        }

        if ( line[0] == '\0' )
        {
            continue;                                   /* a comment-only line */
        }

        key = line;
        value = strchr( line, ' ' );
        if ( value != NULL )
        {
            *value = '\0';
            value++;
            while ( *value == ' ' )
            {
                value++;
            }
        }
        else
        {
            value = line + strlen( line );
        }

        have_record = 1;

        if ( strcmp( key, "operation" ) == 0 )
        {
            copy_field( v.operation, value );
        }
        else if ( strcmp( key, "name" ) == 0 )
        {
            copy_field( v.name, value );
        }
        else if ( strcmp( key, "param" ) == 0 )
        {
            char * equals = strchr( value, '=' );
            if ( equals == NULL || v.param_count >= MAX_PARAMS )
            {
                printf( "FAILED: %s:%d: bad param line\n", path, line_number );
                failed = 1;
                fclose( file );
                return 0;
            }
            *equals = '\0';
            copy_field( v.param_name[v.param_count], value );
            copy_field( v.param_value[v.param_count], equals + 1 );
            v.param_count++;
        }
        else if ( strcmp( key, "bytes" ) == 0 )
        {
            if ( !parse_bytes( &v, value ) )
            {
                printf( "FAILED: %s:%d: bad bytes line\n", path, line_number );
                failed = 1;
                fclose( file );
                return 0;
            }
        }
        else if ( strcmp( key, "expect" ) == 0 )
        {
            if ( strcmp( value, "refused" ) == 0 )
            {
                v.refused = 1;
            }
            else
            {
                char * equals = strchr( value, '=' );
                if ( equals == NULL )
                {
                    printf( "FAILED: %s:%d: bad expect line\n", path, line_number );
                    failed = 1;
                    fclose( file );
                    return 0;
                }
                copy_field( v.expect_value, equals + 1 );
            }
        }
        else if ( strcmp( key, "consumed" ) == 0 )
        {
            v.consumed = (serialize_int64_t) strtol( value, NULL, 10 );
            v.have_consumed = 1;
        }
        else
        {
            /* an unknown key is a format the corpus has grown and this parser
               has not; skipping it would silently drop what it constrains */
            printf( "FAILED: %s:%d: unknown key '%s'\n", path, line_number, key );
            failed = 1;
            fclose( file );
            return 0;
        }
    }

    if ( have_record )
    {
        run_vector( &v );
    }

    fclose( file );
    return 1;
}

/* ---------------------------------------------------------------------------
   the corpus directory
   --------------------------------------------------------------------------- */

static int is_vector_file( const char * name )
{
    size_t n = strlen( name );
    return n > 4 && strcmp( name + n - 4, ".txt" ) == 0;
}

static int run_directory( const char * directory )
{
    char path[512];
    int files = 0;

#ifdef _WIN32
    WIN32_FIND_DATAA found;
    HANDLE handle;
    sprintf( path, "%s\\*.txt", directory );
    handle = FindFirstFileA( path, &found );
    if ( handle == INVALID_HANDLE_VALUE )
    {
        printf( "FAILED: no vector files in %s\n", directory );
        failed = 1;
        return 0;
    }
    do
    {
        if ( is_vector_file( found.cFileName ) )
        {
            sprintf( path, "%s\\%s", directory, found.cFileName );
            run_file( path );
            files++;
        }
    }
    while ( FindNextFileA( handle, &found ) );
    FindClose( handle );
#else
    DIR * dir = opendir( directory );
    struct dirent * entry;
    if ( dir == NULL )
    {
        printf( "FAILED: cannot open %s\n", directory );
        failed = 1;
        return 0;
    }
    while ( ( entry = readdir( dir ) ) != NULL )
    {
        if ( is_vector_file( entry->d_name ) )
        {
            sprintf( path, "%s/%s", directory, entry->d_name );
            run_file( path );
            files++;
        }
    }
    closedir( dir );
#endif

    if ( files == 0 )
    {
        printf( "FAILED: no vector files in %s\n", directory );
        failed = 1;
        return 0;
    }

    return files;
}

int main( int argc, char ** argv )
{
    const char * directory = argc > 1 ? argv[1] : "conformance";
    int files = run_directory( directory );

    if ( failed )
    {
        printf( "FAILED (%d vectors from %d files)\n", vectors_run, files );
        return 1;
    }

    if ( serialize_test_verbose() )
    {
        printf( "conformance: %d vectors from %d files in %s\n", vectors_run, files, directory );
    }

    printf( "OK\n" );

    return 0;
}
