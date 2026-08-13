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

.PHONY: all test test-all-standards diff clean

all: test

test: build/roundtrip
	./build/roundtrip

build/roundtrip: test/roundtrip.c serialize.c serialize.h
	@mkdir -p build
	$(CC) $(CFLAGS) -I. test/roundtrip.c serialize.c -o $@ $(LDLIBS)

# Builds the same sequence with this library and with the C++ one and compares
# the bytes. This is the test that matters: the port exists to be wire
# compatible, and everything else is detail.
diff: build/diff_c build/diff_cpp
	@./build/diff_c  > build/out_c.txt
	@./build/diff_cpp > build/out_cpp.txt
	@if diff -q build/out_c.txt build/out_cpp.txt > /dev/null; then \
		echo "WIRE IDENTICAL to the C++ library"; \
		cat build/out_c.txt; \
	else \
		echo "WIRE MISMATCH:"; \
		echo "  C:  "; cat build/out_c.txt; \
		echo "  C++:"; cat build/out_cpp.txt; \
		exit 1; \
	fi

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
		if $(MAKE) --no-print-directory CSTD=$$std test > /dev/null 2>&1; then \
			echo "OK"; \
		else \
			echo "FAILED"; exit 1; \
		fi; \
	done

clean:
	rm -rf build
