#!/usr/bin/env bash
#
# check_fp_contract.sh — verify every C/C++ build in this repository passes
# -ffp-contract=off (MSVC: /fp:precise or /fp:strict). The estate rule is
# that the flag is passed, not assumed: a compiler permitted to contract a
# multiply and an add into one FMA rounds once instead of twice, which breaks
# the compressed-float wire.
#
# This lists every C/C++ compiler invocation in the Makefile and the CI
# workflows and fails, naming each line that lacks the flag, except:
#   - a line carrying a CFLAGS/CXXFLAGS-style variable whose own definition
#     carries the flag, and
#   - the precomputed-fma target, which builds at -ffp-contract=on on
#     purpose, to prove the suite catches contraction.
set -euo pipefail

cd "$(dirname "$0")/.."

GOOD_VARS=""
while IFS= read -r ml; do
  if [[ "$ml" =~ ^[[:space:]]*([A-Z][A-Z0-9_]*)[[:space:]]*(\?|\+)*=[[:space:]]*(.*)$ ]]; then
    v="${BASH_REMATCH[1]}"
    val="${BASH_REMATCH[3]}"
    if [[ "$val" == *-ffp-contract=off* ]]; then
      GOOD_VARS+="$v"$'\n'
    fi
  fi
done < Makefile

fail=0
declare -a problems

has_compiler_token() {
  local l="$1"
  [[ "$l" == *'$('CC')'* || "$l" == *'$('CXX')'* || \
     "$l" == *'$('FUZZ_CLANG')'* || "$l" == *'$('FUZZ_CLANGXX')'* || \
     "$l" =~ (^|[^A-Za-z0-9_])c\+\+([^A-Za-z0-9_]|$) || \
     "$l" =~ (^|[^A-Za-z0-9_])gcc([^A-Za-z0-9_]|$) || \
     "$l" =~ (^|[^A-Za-z0-9_])clang([^A-Za-z0-9_]|$) || \
     "$l" =~ (^|[^A-Za-z0-9_])cl([^A-Za-z0-9_]|$) || \
     "$l" =~ (^|[^A-Za-z0-9_])cc([^A-Za-z0-9_]|$) ]]
}

is_source_line() {
  local l="$1"
  [[ "$l" =~ \.(c|cpp|cc|C|cxx)([^A-Za-z0-9_]|$) ]]
}

refs_good_var() {
  local l="$1" m
  while [[ "$l" =~ \$\(([A-Z][A-Z0-9_]*)\) ]]; do
    m="${BASH_REMATCH[1]}"
    if printf '%s\n' "$GOOD_VARS" | grep -qxF "$m"; then
      return 0
    fi
    l="${l//\$($m)/}"
  done
  return 1
}

scan_file() {
  local file="$1" n=0 l strip is_msvc
  while IFS= read -r l; do
    n=$((n + 1))
    strip="${l#"${l%%[![:space:]]*}"}"
    [[ -z "$strip" ]] && continue
    [[ "$strip" == \#* ]] && continue
    if has_compiler_token "$l" && is_source_line "$l"; then
      if [[ "$l" == *-ffp-contract=on* ]]; then
        continue
      fi
      is_msvc=0
      if [[ "$l" =~ (^|[^A-Za-z0-9_])cl([^A-Za-z0-9_]|$) ]]; then
        is_msvc=1
      fi
      if [[ "$is_msvc" -eq 1 ]]; then
        if [[ "$l" != */fp:precise* && "$l" != */fp:strict* ]]; then
          problems+=("$file:$n: cl invocation lacks /fp:precise or /fp:strict")
          fail=1
        fi
      else
        if [[ "$l" != *-ffp-contract=off* ]] && ! refs_good_var "$l"; then
          problems+=("$file:$n: compiler invocation lacks -ffp-contract=off")
          fail=1
        fi
      fi
    fi
  done < "$file"
}

scan_file Makefile
for f in .github/workflows/*.yml; do
  scan_file "$f"
done

if [[ "$fail" -ne 0 ]]; then
  printf 'check_fp_contract: FP-contraction flag missing from:\n' >&2
  for p in "${problems[@]}"; do
    printf '  %s\n' "$p" >&2
  done
  exit 1
fi

printf 'check_fp_contract: every C/C++ build passes -ffp-contract=off\n'
