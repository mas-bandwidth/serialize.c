/*
    Shared by the test binaries: is informational narration wanted?

    Under a passing test the binary prints its verdict and nothing else. The
    suite's informational narration -- check counts, negative-control
    statistics, skip reasons, which contraction discipline this build
    exercised -- is opt-in: set SERIALIZE_TEST_VERBOSE=1 in the environment to
    restore it. Failures print everything relevant regardless, and no check
    runs or does not run because of this switch: it gates narration only.

    Same name and same semantics as the C++ serialize suite, deliberately, so
    one environment variable covers the family.
*/

#ifndef SERIALIZE_TEST_VERBOSE_H
#define SERIALIZE_TEST_VERBOSE_H

#include <stdlib.h>

static inline int serialize_test_verbose( void )
{
    const char * value = getenv( "SERIALIZE_TEST_VERBOSE" );
    return value != NULL && value[0] != '\0' && !( value[0] == '0' && value[1] == '\0' );
}

#endif /* SERIALIZE_TEST_VERBOSE_H */
