#!/bin/sh
# Re-vendor the format from a tag of the C++ reference, mas-bandwidth/serialize:
# STANDARD.md, conformance/ and conformance-controls/, verbatim. CI's sync jobs
# hold the copies to upstream; this is how they are brought back into step.
#
#   tools/revendor.sh v2.0.1
#
# Clones the tag into a scratch directory, replaces the three vendored paths
# with its copies, and prints the resulting diff stat for review.
set -eu

tag="${1:?usage: tools/revendor.sh <tag of mas-bandwidth/serialize>}"
root="$(cd "$(dirname "$0")/.." && pwd)"
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$tag" https://github.com/mas-bandwidth/serialize.git "$scratch/serialize"
echo "upstream: mas-bandwidth/serialize $tag @ $(git -C "$scratch/serialize" rev-parse HEAD)"

cp "$scratch/serialize/STANDARD.md" "$root/STANDARD.md"
rm -rf "$root/conformance" "$root/conformance-controls"
cp -R "$scratch/serialize/conformance" "$root/conformance"
cp -R "$scratch/serialize/conformance-controls" "$root/conformance-controls"

git -C "$root" add -A STANDARD.md conformance conformance-controls
git -C "$root" diff --cached --stat -- STANDARD.md conformance conformance-controls
