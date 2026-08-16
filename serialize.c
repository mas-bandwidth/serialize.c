/*
    serialize.c 1.0 — a bitpacking serializer for C

    See serialize.h for the API and STANDARD.md for the wire format.

    The library is header-only: every definition lives in serialize.h, where
    each call site can inline it and bit widths fold to literals — the same
    shape as the header-only C++ reference (the ruling, 2026-08-17, verbatim:
    "everything in C should be inlined!!!").

    This file is kept so build systems that compile and link serialize.c keep
    working unchanged; it compiles to an empty translation unit. Linking it is
    OPTIONAL — include serialize.h and you have the whole library, the way a
    single-header library works.
*/

#include "serialize.h"
