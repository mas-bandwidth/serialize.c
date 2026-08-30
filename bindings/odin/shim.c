/*
    serialize.c is header-only: every function is SERIALIZE_INLINE, so an
    ordinary build emits no symbols for Odin to link against. Both inline
    macros are #ifndef-guarded for exactly this kind of consumer — blank
    them and this one translation unit compiles the whole library as plain
    external functions, under their real names.

        cc -O2 -c shim.c -o shim.o
*/
#define SERIALIZE_INLINE
#define SERIALIZE_ALWAYS_INLINE
#include "../../serialize.h"

/* Layout guards for the opaque stream storage declared on the Odin side. */
int serialize_shim_sizeof_write_stream( void ) { return (int) sizeof( serialize_write_stream_t ); }
int serialize_shim_sizeof_read_stream( void )  { return (int) sizeof( serialize_read_stream_t ); }
