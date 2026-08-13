# serialize.c — build and test.
#
# CSTD selects the standard to build against. The library's floor is C89, and
# `make test-all-standards` proves it by building the whole thing under c89,
# c99, c11 and c17 in turn — a claim about historical C is worth nothing if
# nothing checks it.

CC      ?= cc
CSTD    ?= c99
CFLAGS  ?= -std=$(CSTD) -Wall -Wextra -pedantic -O2
LDLIBS  ?= -lm

# The C++ library, for the differential test. Override if it lives elsewhere.
SERIALIZE_CPP ?= ../serialize

.PHONY: all test golden test-all-standards diff clean

all: test

test: build/roundtrip
	./build/roundtrip

# The pinned wire vector. Separate from `test` because this is the one that
# has to run on a big-endian machine -- a round trip cannot catch a byte order
# bug, and this can.
golden: build/golden
	./build/golden

build/golden: test/golden.c serialize.c serialize.h
	@mkdir -p build
	$(CC) $(CFLAGS) -I. test/golden.c serialize.c -o $@ $(LDLIBS)

build/roundtrip: test/roundtrip.c serialize.c serialize.h
	@mkdir -p build
	$(CC) $(CFLAGS) -I. test/roundtrip.c serialize.c -o $@ $(LDLIBS)

# Builds the same sequence with this library and with the C++ one and compares
# the bytes. This is the test that matters: the port exists to be wire
# compatible, and everything else is detail.
diff: build/diff_c build/diff_cpp build/diff2_c build/diff2_cpp
	@./build/diff_c   > build/out_c.txt
	@./build/diff_cpp > build/out_cpp.txt
	@./build/diff2_c   > build/out2_c.txt
	@./build/diff2_cpp > build/out2_cpp.txt
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
	exit $$fail

build/diff2_c: test/diff2_c.c serialize.c serialize.h
	@mkdir -p build
	$(CC) $(CFLAGS) -I. test/diff2_c.c serialize.c -o $@ $(LDLIBS)

build/diff2_cpp: test/diff2_cpp.cpp
	@mkdir -p build
	c++ -std=c++17 -O2 -I$(SERIALIZE_CPP) test/diff2_cpp.cpp -o $@

build/diff_c: test/diff_c.c serialize.c serialize.h
	@mkdir -p build
	$(CC) $(CFLAGS) -I. test/diff_c.c serialize.c -o $@ $(LDLIBS)

build/diff_cpp: test/diff_cpp.cpp
	@mkdir -p build
	c++ -std=c++17 -O2 -I$(SERIALIZE_CPP) test/diff_cpp.cpp -o $@

test-all-standards:
	@for std in c89 c99 c11 c17; do \
		printf "%-6s " $$std; \
		$(MAKE) --no-print-directory clean > /dev/null; \
		if $(MAKE) --no-print-directory CSTD=$$std test golden > /dev/null 2>&1; then \
			echo "OK"; \
		else \
			echo "FAILED"; exit 1; \
		fi; \
	done

clean:
	rm -rf build
