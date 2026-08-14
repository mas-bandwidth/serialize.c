/*
    The pinned wire vectors, in one place so that nothing has to trust itself.

    A pinned vector that only agrees with the implementation that produced it
    proves nothing at all. These constants are therefore consumed from BOTH
    sides of the wire-compatibility argument:

      test/golden.c      writes the sequences with THIS library and checks the
                         bytes. Run under qemu-s390x by CI, this is what proves
                         the byte order, which no round trip can.

      test/diff_cpp.cpp  write the same sequences with the C++ library and
      test/diff2_cpp.cpp check the bytes against the SAME constants. So the pin
                         is cross-verified against a second, independent
                         implementation on every `make diff`.

    Between them the triangle closes: if golden.c's sequence ever drifts from
    the differential's sequence, the C++ side stops matching the pin and
    `make diff` fails loudly rather than the golden quietly becoming a string
    that agrees only with itself.

    THESE BYTES ARE THE FORMAT. Changing one means the wire changed, which is
    a breaking change for every stream ever written by any of the five ports.
    Adding coverage means APPENDING a new sequence, never editing an old one.

    Plain #defines and no C++ so both languages can include it.
*/

#ifndef SERIALIZE_TEST_VECTORS_H
#define SERIALIZE_TEST_VECTORS_H

/*
    The core surface: bits, bool, int, the fixed-width helpers, int64, float,
    double, compressed_float, align, bytes, string and every int_relative
    tier. 73 bytes.
*/
#define SERIALIZE_GOLDEN_CORE \
    "4d85aed577df77df56eff7e6d5c4b3a291809a8da71b59b41f9280aaaaaaaaaaaaaa7f96" \
    "010203faff1374686520717569636b2062726f776e20666f78151b9c00a08200e047e801" \
    "00"

/*
    The wide surface: uint128, ranged int128, fixed32/64/128 and wstring.
    70 bytes.

    This section exists because the core one cannot cover the 128-bit, fixed
    point and wide string paths, and those are the hand-rolled two-lane 64-bit
    emulation -- the likeliest place in the library for a byte order bug, and
    for a long while the only part of it a big-endian run never touched.

    The last two ranged cases span MORE THAN 64 BITS on purpose. Everything
    before them fits its offset in the low lane, so groups 2 and 3 of the
    32-bit splitting are never reached; a lane mix-up would have gone
    unnoticed.
*/
#define SERIALIZE_GOLDEN_WIDE \
    "1032547698badcfeefcdab8967452301351b4f37b20000600400c07579000049452f0706" \
    "12cf8a4602de9b5713cf8a463e0000000000800389371e0200001c02000020020000"

/*
    STANDARD.md's worked wstring example is pinned separately, as the byte
    array the document itself prints, in test/wstest.c. It stays in that shape
    on purpose: it is a conformance check against the specification's own
    worked example, not against this family's implementations.
*/

#endif /* SERIALIZE_TEST_VECTORS_H */
