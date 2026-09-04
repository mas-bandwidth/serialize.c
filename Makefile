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

# THE SUITE LIST (issue 37). One enumeration, derived everywhere: `make test`
# runs TEST_SUITES; golden and wstest keep their own targets because they are
# the pinned-wire suites a big-endian machine must run; CI's big-endian leg
# drives this same Makefile through the s390x cross compiler and qemu, so it
# inherits all of it with no list of its own. The MSVC CI leg has no make
# (and these recipes are not cl-shaped anyway), so it PARSES the MSVC_SUITES
# line with findstr -- keep that line a single fully expanded line: no
# continuations, no $(...) -- and the parse-time guard below turns any drift
# between the two variables into a red `make` on every Makefile-driven leg.
# Adding a suite therefore reaches every CI leg by construction, or fails
# loudly; it never skips silently.
TEST_SUITES = roundtrip conformance precomputed precomputed-fma assertdeath

# Not on MSVC, with reasons: assertdeath forks (POSIX); precomputed-fma is
# precomputed rebuilt at -ffp-contract=on, and cl has no contraction flag
# to vary.
NOT_ON_MSVC = precomputed-fma assertdeath

MSVC_SUITES = roundtrip conformance precomputed golden wstest

ifneq ($(sort $(MSVC_SUITES)),$(sort $(filter-out $(NOT_ON_MSVC),$(TEST_SUITES) golden wstest)))
$(error MSVC_SUITES is out of step: it must equal TEST_SUITES + golden + wstest minus NOT_ON_MSVC (issue 37))
endif

.PHONY: all test golden wstest test-all-standards diff fuzz clean

all: test

test: $(TEST_SUITES:%=build/%)
	@for suite in $(TEST_SUITES); do echo "./build/$$suite"; ./build/$$suite || exit 1; done

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

# The shared conformance corpus, run through this library's reader. conformance/
# is a verbatim vendored copy of the corpus in mas-bandwidth/serialize, held to
# it by CI's sync job, and this binary scans that directory rather than naming
# its files: a vector file the corpus gains runs on the next build. It takes
# the directory as an optional argument and defaults to conformance/, which is
# what lets CI's Windows leg run it with no arguments from the repository root.
build/conformance: test/conformance.c test/verbose.h serialize.c serialize.h
	@mkdir -p build
	$(CC) $(CFLAGS) -I. test/conformance.c serialize.c -o $@ $(LDLIBS)

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

# The same differential a SECOND time, at -ffp-contract=on. This is not
# belt and braces; it is the only build in which the property has teeth.
#
# The audited home's FMA discipline is a pair of stores through float locals
# that keep the quantization's roundings distinct. Whether DELETING them is
# visible depends entirely on the contraction setting, measured on this port
# (clang 21 / arm64) by folding the reader's two roundings into one and
# rebuilding:
#
#   -ffp-contract=off    the falsified build PASSES -- contraction is
#                        forbidden in the frozen oracle too, so the fused
#                        spelling and the stored one compute the same thing
#   -ffp-contract=on     the falsified build is RED on the decoded bit
#                        patterns -- the compiler fuses one form and not
#                        the other, which is the divergence being guarded
#   -ffp-contract=fast   the falsified build PASSES -- `fast` fuses ACROSS
#                        statements, so the frozen oracle fuses as well and
#                        the two forms stop being distinguishable
#
# The mode that looks strictest is the one with no teeth, and the mode this
# repo pins for the wire is the other one with no teeth. Same result the C++
# reference reached (mas-bandwidth/schema#82, comment 5364784769). So the
# default build above stays at -ffp-contract=off, because that is what the
# wire requires, and this build adds back the discrimination it costs.
#
# -ffp-contract=on comes AFTER $(CFLAGS) so it wins over the =off in there,
# including when CI overrides CFLAGS wholesale (the sanitizers job does).
# Nothing new is required of the compiler: the default CFLAGS already spell
# -ffp-contract, so any compiler that can build the suite at all can build
# this. The binary reports whether contraction is actually LIVE on the
# target -- x86-64 without FMA cannot fuse however the flag is set -- so a
# green log never claims a discipline it did not exercise.
#
# And it detects the one compiler trap here: GCC before 14 mapped
# -ffp-contract=on onto =fast, so on gcc 13 asking for statement-local
# contraction gets cross-statement contraction, which is the unsupported
# mode. SERIALIZE_TEST_FP_CONTRACT_REQUESTED_ON tells the binary this build
# asked for =on, and it stands down with a printed reason instead of failing
# for a toolchain quirk that is not a defect in the library.
build/precomputed-fma: test/precomputed.c serialize.c serialize.h
	@mkdir -p build
	$(CC) $(CFLAGS) -ffp-contract=on -DSERIALIZE_TEST_FP_CONTRACT='"-ffp-contract=on"' -DSERIALIZE_TEST_FP_CONTRACT_REQUESTED_ON=1 -I. test/precomputed.c serialize.c -o $@ $(LDLIBS)

# The writer contracts, proven to FIRE: with the write path checkless in a
# release build (issue #52), the debug asserts are the whole enforcement,
# and each is exercised in a forked child that must die by SIGABRT. POSIX
# only -- NOT_ON_MSVC above excludes it from CI's Windows leg. Skips itself
# under NDEBUG.
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

# The libFuzzer harness, mirroring the C++ library's fuzz.cpp: two hostile read
# passes over the untrusted boundary and a differential round trip pass. Clang
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
