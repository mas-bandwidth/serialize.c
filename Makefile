# serialize.c — build and test.
#
# CSTD selects the standard to build against. The library's floor is C89, and
# `make test-all-standards` proves it by building the whole thing under c89,
# c99, c11 and c17 in turn — a claim about historical C is worth nothing if
# nothing checks it.

CC      ?= cc
CSTD    ?= c99
# -ffp-contract=off is load-bearing. STANDARD.md pins compressed_float to
# float32 arithmetic with distinct roundings, and a compiler permitted to
# contract a multiply and an add into one FMA rounds once instead -- the
# writer's local guards the wire on standard-conforming compilers, but the
# reader's reconstruction and any compiler that fuses across statements
# (gcc's gnu modes default to -ffp-contract=fast) are only held by the flag.
CFLAGS  ?= -std=$(CSTD) -Wall -Wextra -pedantic -O2 -ffp-contract=off
LDLIBS  ?= -lm

# The C++ library, for the differential test. Override if it lives elsewhere.
SERIALIZE_CPP ?= ../serialize

.PHONY: all test golden wstest test-all-standards diff fuzz bench bench-lto bench-cpp bench-all clean

all: test

test: build/roundtrip build/precomputed build/precomputed_contract build/assertdeath
	./build/roundtrip
	./build/precomputed
	./build/precomputed_contract
	./build/assertdeath

# The pinned wire vectors -- core and wide. Separate from `test` because these
# are the ones that have to run on a big-endian machine: a round trip cannot
# catch a byte order bug, and these can.
golden: build/golden
	./build/golden

# STANDARD.md's worked wstring example, pinned as the document prints it. The
# wide path deliberately does NOT align where the narrow one does, and this is
# the vector that says so.
wstest: build/wstest
	./build/wstest

build/golden: test/golden.c test/vectors.h serialize.c serialize.h
	@mkdir -p build
	$(CC) $(CFLAGS) -I. test/golden.c serialize.c -o $@ $(LDLIBS)

build/wstest: test/wstest.c serialize.c serialize.h
	@mkdir -p build
	$(CC) $(CFLAGS) -I. test/wstest.c serialize.c -o $@ $(LDLIBS)

build/roundtrip: test/roundtrip.c serialize.c serialize.h
	@mkdir -p build
	$(CC) $(CFLAGS) -I. test/roundtrip.c serialize.c -o $@ $(LDLIBS)

# The mas-bandwidth/schema#82 differential: since the split, the compressed
# float entry points derive their constants with
# serialize_compressed_float_params and forward to the precomputed audited
# home, and this suite holds that composition -- plus the precomputed entry
# points a schema compiler targets -- to byte and bit identity against a
# FROZEN verbatim copy of the pre-split bodies, across the family's
# declaration corpus. ~4.7 million checks, three implementations.
build/precomputed: test/precomputed.c serialize.c serialize.h
	@mkdir -p build
	$(CC) $(CFLAGS) -I. test/precomputed.c serialize.c -o $@ $(LDLIBS)

# The same differential again, built with contraction PERMITTED. -O2 alone is
# not the discriminating build: with -ffp-contract=off the compiler is
# forbidden to fuse in either form, so a reader that folds its two roundings
# into one expression is byte and bit identical to the frozen reference and
# the differential passes -- measured, exit 0, all 4,717,569 checks OK. Fold
# the same reader with contraction permitted and it goes red immediately on
# the decoded bit pattern. So the flag that protects the WIRE is the flag
# that blinds the TEST, and the suite needs both builds to mean anything.
#
# -ffp-contract=on is appended after CFLAGS so it wins on any override: the
# sanitizer leg in CI pins -ffp-contract=off for the whole build, and this
# one differential has to keep its teeth there too.
build/precomputed_contract: test/precomputed.c serialize.c serialize.h
	@mkdir -p build
	$(CC) $(CFLAGS) -ffp-contract=on -I. test/precomputed.c serialize.c -o $@ $(LDLIBS)

# The writer contracts, proven to FIRE: with the write path checkless in a
# release build (issue #52), the debug asserts are the whole enforcement,
# and each is exercised in a forked child that must die by SIGABRT. POSIX
# only -- CI's Windows leg runs roundtrip/golden/wstest directly with cl and
# never builds this. Skips itself under NDEBUG.
build/assertdeath: test/assertdeath.c serialize.c serialize.h
	@mkdir -p build
	$(CC) $(CFLAGS) -I. test/assertdeath.c serialize.c -o $@ $(LDLIBS)

# Builds the same sequences with this library and with the C++ one and
# compares the bytes. This is the test that matters: the port exists to be wire
# compatible, and everything else is detail.
#
# Both C++ twins also check their own output against test/vectors.h -- the
# constants test/golden.c pins -- and exit nonzero if it disagrees. That is
# what keeps the golden from being a vector that agrees only with itself.
diff: build/diff_c build/diff_cpp build/diff2_c build/diff2_cpp build/diff3_c build/diff3_cpp
	@./build/diff_c   > build/out_c.txt
	@./build/diff_cpp > build/out_cpp.txt
	@./build/diff2_c   > build/out2_c.txt
	@./build/diff2_cpp > build/out2_cpp.txt
	@./build/diff3_c   > build/out3_c.txt
	@./build/diff3_cpp > build/out3_cpp.txt
	@fail=0; \
	if diff -q build/out_c.txt build/out_cpp.txt > /dev/null; then \
		echo "core operations:  IDENTICAL"; \
	else \
		echo "core operations:  MISMATCH"; \
		echo "  C:   "; cat build/out_c.txt; \
		echo "  C++: "; cat build/out_cpp.txt; fail=1; \
	fi; \
	if diff -q build/out2_c.txt build/out2_cpp.txt > /dev/null; then \
		echo "wide operations:  IDENTICAL  (128 bit, fixed point, wstring)"; \
	else \
		echo "wide operations:  MISMATCH"; \
		echo "  C:   "; cat build/out2_c.txt; \
		echo "  C++: "; cat build/out2_cpp.txt; fail=1; \
	fi; \
	if diff -q build/out3_c.txt build/out3_cpp.txt > /dev/null; then \
		echo "compressed float: IDENTICAL  (values BETWEEN quanta, where the arithmetic shows)"; \
	else \
		echo "compressed float: MISMATCH"; \
		echo "  C:   "; cat build/out3_c.txt; \
		echo "  C++: "; cat build/out3_cpp.txt; fail=1; \
	fi; \
	exit $$fail

build/diff3_c: test/diff3_c.c serialize.c serialize.h
	@mkdir -p build
	$(CC) $(CFLAGS) -I. test/diff3_c.c serialize.c -o $@ $(LDLIBS)

build/diff3_cpp: test/diff3_cpp.cpp $(SERIALIZE_CPP)/serialize.h
	@mkdir -p build
	c++ -std=c++17 -O2 -I$(SERIALIZE_CPP) test/diff3_cpp.cpp -o $@

build/diff2_c: test/diff2_c.c serialize.c serialize.h
	@mkdir -p build
	$(CC) $(CFLAGS) -I. test/diff2_c.c serialize.c -o $@ $(LDLIBS)

build/diff2_cpp: test/diff2_cpp.cpp test/vectors.h $(SERIALIZE_CPP)/serialize.h
	@mkdir -p build
	c++ -std=c++17 -O2 -I$(SERIALIZE_CPP) test/diff2_cpp.cpp -o $@

build/diff_c: test/diff_c.c serialize.c serialize.h
	@mkdir -p build
	$(CC) $(CFLAGS) -I. test/diff_c.c serialize.c -o $@ $(LDLIBS)

build/diff_cpp: test/diff_cpp.cpp test/vectors.h $(SERIALIZE_CPP)/serialize.h
	@mkdir -p build
	c++ -std=c++17 -O2 -I$(SERIALIZE_CPP) test/diff_cpp.cpp -o $@

# The libFuzzer harness, mirroring the C++ library's fuzz.cpp: a hostile read
# pass over the untrusted boundary and a differential round trip pass. Clang
# only, because libFuzzer is clang's; the link goes through clang++ because
# the fuzzer runtime is C++ and needs its standard library. Asserts stay live
# (no NDEBUG) and asan/ubsan ride along, so a hostile input that corrupts
# memory, trips undefined behavior or lands in an assert all trap.
#
# test/fuzz-corpus holds pinned seeds sitting on the reader's accept/refuse
# fence -- malformed UTF-8/UTF-16 string payloads and their valid neighbors
# (ruling #8) -- so mutation starts in the neighborhood of the content
# validation instead of hoping to construct a parseable string op from
# nothing. Seeds are copied into build/corpus first: libFuzzer writes its
# discoveries into the first directory it is given, and that churn belongs in
# build/, not in the tree.
FUZZ_CLANG   ?= clang
FUZZ_CLANGXX ?= clang++
FUZZ_CFLAGS  ?= -std=c99 -Wall -Wextra -pedantic -g -O1 -ffp-contract=off \
                -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

fuzz: build/fuzz
	@mkdir -p build/corpus
	@cp test/fuzz-corpus/* build/corpus/
	./build/fuzz build/corpus -max_total_time=60 -timeout=10 -print_final_stats=1

build/fuzz: fuzz.c serialize.c serialize.h
	@mkdir -p build
	$(FUZZ_CLANG) $(FUZZ_CFLAGS) -I. -c fuzz.c -o build/fuzz.o
	$(FUZZ_CLANG) $(FUZZ_CFLAGS) -I. -c serialize.c -o build/fuzz_serialize.o
	$(FUZZ_CLANGXX) -fsanitize=fuzzer,address,undefined build/fuzz.o build/fuzz_serialize.o -o $@ $(LDLIBS)

# bench.c mirrors the C++ library's bench.cpp operation for operation, so the two
# outputs can be read side by side. See the header comment in bench.c for the one
# thing it does not mirror and why.
#
# Three legs, because the question is whether the out-of-line call into
# serialize.c costs a consumer anything the C++ macros do not pay:
#
#   bench      the flags a consumer gets today -- separate translation unit, no LTO
#   bench-lto  the same, plus -flto, which lets the linker inline across that boundary
#   bench-cpp  the C++ library at the SAME -O2, so the comparison is like for like
#
# Absolute numbers mean nothing off a quiet machine. What means something is the
# ratio between legs run back to back, which is what bench-all produces.
bench: build/bench
	./build/bench

bench-lto: build/bench_lto
	./build/bench_lto

bench-cpp: build/bench_cpp
	./build/bench_cpp

bench-all: build/bench build/bench_lto build/bench_cpp
	@echo "=== C, no LTO (what a consumer gets today) ==="
	@./build/bench
	@echo "=== C, -flto ==="
	@./build/bench_lto
	@echo "=== C++, same -O2 ==="
	@./build/bench_cpp

# -DNDEBUG on both C legs because the C++ leg below has it: caller error is
# serialize_assert on both sides now, and an assert that is live on one side
# and compiled out on the other is not a comparison. It is the only define
# either leg gets, and it is the standard release one -- nothing here is
# opted into.
build/bench: bench.c serialize.c serialize.h
	@mkdir -p build
	$(CC) $(CFLAGS) -DNDEBUG -I. bench.c serialize.c -o $@ $(LDLIBS)

build/bench_lto: bench.c serialize.c serialize.h
	@mkdir -p build
	$(CC) $(CFLAGS) -DNDEBUG -flto -I. bench.c serialize.c -flto -o $@ $(LDLIBS)

# The C++ bench, built here rather than by its own CMake so the optimization level
# matches this repo's -O2 -- its CMake Release build is -O3, and comparing -O3 to
# -O2 would answer a different question. Nothing in $(SERIALIZE_CPP) is written to.
# No -I. here: bench.cpp includes "serialize.h" and must get the C++ one.
build/bench_cpp: $(SERIALIZE_CPP)/bench.cpp $(SERIALIZE_CPP)/serialize.h
	@mkdir -p build
	c++ -std=c++17 -O2 -DNDEBUG -DSERIALIZE_RELEASE -Wall -Wextra -fno-rtti -I$(SERIALIZE_CPP) $(SERIALIZE_CPP)/bench.cpp -o $@

test-all-standards:
	@for std in c89 c99 c11 c17; do \
		printf "%-6s " $$std; \
		$(MAKE) --no-print-directory clean > /dev/null; \
		if $(MAKE) --no-print-directory CSTD=$$std test golden wstest > /dev/null 2>&1; then \
			echo "OK"; \
		else \
			echo "FAILED"; exit 1; \
		fi; \
	done

clean:
	rm -rf build
