/*
    Runs fuzz.c's four passes (hostile read, padded read, sentinel read,
    differential round trip) over 3000 deterministic xorshift inputs on
    every platform, C standard and compiler in the matrix, without
    requiring clang or libFuzzer.
*/
#define SERIALIZE_FUZZ_REPLAY 1
#include "../fuzz.c"
