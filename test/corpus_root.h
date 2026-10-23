/*
    Shared by the conformance runner and its unit test: where is the corpus?

    By default the runner reads the vendored copy, conformance/ beside the
    Makefile. SERIALIZE_CONFORMANCE_ROOT, set in the environment, names a
    directory that holds conformance/ and conformance-controls/ instead -- a
    checkout of mas-bandwidth/serialize -- which is how the nightly
    conformance job runs this port against the CURRENT upstream corpus rather
    than the copy it vendored. Unset or empty means the vendored copy. The
    Makefile's `controls` target honours the same variable, so the negative
    controls read the same corpus and the same .expected lists.

    Same name and same semantics across the family, so one environment
    variable points every port at one corpus.
*/

#ifndef SERIALIZE_TEST_CORPUS_ROOT_H
#define SERIALIZE_TEST_CORPUS_ROOT_H

#include <string.h>

/*
    Writes root/name into out, or name alone when root is NULL or empty.
    Returns 0, leaving out an empty string, when the result does not fit in
    size bytes: a truncated path would read some other directory, so it is
    refused rather than shortened.
*/
static int serialize_test_corpus_directory( const char * root, const char * name, char * out, size_t size )
{
    size_t root_length = ( root != NULL ) ? strlen( root ) : 0;
    size_t name_length = strlen( name );
    size_t length = ( root_length > 0 ) ? root_length + 1 + name_length : name_length;

    if ( size == 0 )
    {
        return 0;
    }
    if ( length + 1 > size )
    {
        out[0] = '\0';
        return 0;
    }
    if ( root_length > 0 )
    {
        memcpy( out, root, root_length );
        out[root_length] = '/';
        memcpy( out + root_length + 1, name, name_length + 1 );
    }
    else
    {
        memcpy( out, name, name_length + 1 );
    }
    return 1;
}

#endif /* SERIALIZE_TEST_CORPUS_ROOT_H */
