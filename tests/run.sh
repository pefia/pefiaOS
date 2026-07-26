#!/bin/sh
# Native self-check for the Python interpreter (kernel/interp.c), run on the
# host with stub kernel headers - no cross toolchain or QEMU needed:
#   sh tests/run.sh
set -e
cd "$(dirname "$0")"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cp ../kernel/interp.c ../kernel/interp.h ../kernel/util.h stubs/*.h test_interp.c "$tmp"
gcc -std=gnu99 -Wall -Wextra -I "$tmp" "$tmp/test_interp.c" -o "$tmp/ti"
"$tmp/ti"
