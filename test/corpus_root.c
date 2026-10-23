/* Unit test for test/corpus_root.h: where the conformance runner reads the
   corpus from. The vendored copy by default, SERIALIZE_CONFORMANCE_ROOT's
   checkout when it is set, and never a truncated path. Pure string work: it
   reads no environment and touches no file, so the variable set in the shell
   that runs it changes nothing here. */
#include <stdio.h>
#include <string.h>
#include "corpus_root.h"

static int failed = 0;
#define CHECK(c) do { if (!(c)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); failed = 1; } } while (0)

int main( void )
{
    char out[32];

    /* unset and empty both mean the vendored copy */
    CHECK( serialize_test_corpus_directory( NULL, "conformance", out, sizeof( out ) ) );
    CHECK( strcmp( out, "conformance" ) == 0 );
    CHECK( serialize_test_corpus_directory( "", "conformance-controls", out, sizeof( out ) ) );
    CHECK( strcmp( out, "conformance-controls" ) == 0 );

    /* a root names the checkout that holds both directories */
    CHECK( serialize_test_corpus_directory( "upstream", "conformance", out, sizeof( out ) ) );
    CHECK( strcmp( out, "upstream/conformance" ) == 0 );
    CHECK( serialize_test_corpus_directory( "../serialize", "conformance", out, sizeof( out ) ) );
    CHECK( strcmp( out, "../serialize/conformance" ) == 0 );

    /* exactly full fits, terminator included */
    CHECK( serialize_test_corpus_directory( "abcdefghijklmnopqrs", "conformance", out, sizeof( out ) ) );
    CHECK( strcmp( out, "abcdefghijklmnopqrs/conformance" ) == 0 );

    /* one byte over is refused and leaves no partial path behind */
    CHECK( !serialize_test_corpus_directory( "abcdefghijklmnopqrst", "conformance", out, sizeof( out ) ) );
    CHECK( out[0] == '\0' );
    CHECK( !serialize_test_corpus_directory( NULL, "conformance", out, 11 ) );
    CHECK( out[0] == '\0' );
    CHECK( !serialize_test_corpus_directory( NULL, "conformance", out, 0 ) );

    printf( failed ? "FAILED\n" : "OK\n" );
    return failed;
}
