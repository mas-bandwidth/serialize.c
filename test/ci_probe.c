/*
    Negative control for issue #37: a dummy suite added ONLY to the
    Makefile's suite list, to prove the MSVC and big-endian CI legs pick
    up a new suite with no workflow edit. Removed in the same PR once the
    run log shows it ran on both.
*/

#include <stdio.h>

int main( void )
{
    printf( "ci_probe: issue 37 negative control -- this suite exists only in the Makefile's list\n" );
    return 0;
}
