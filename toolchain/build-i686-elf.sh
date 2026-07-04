#!/usr/bin/env bash
# toolchain/build-i686-elf.sh
#
# Builds an i686-elf cross-compiler (binutils + gcc) from source into
# $HOME/opt/cross. Run once, inside WSL2 Ubuntu - takes 20-40 minutes.
#
# Build from your Linux home (e.g. ~), not /mnt/c: compiling on the Windows
# filesystem is noticeably slower and occasionally trips over permissions.
#
# One-time setup:
#   sudo apt update
#   sudo apt install -y build-essential bison flex libgmp3-dev libmpc-dev \
#                       libmpfr-dev texinfo wget
#   bash build-i686-elf.sh
#   echo 'export PATH="$HOME/opt/cross/bin:$PATH"' >> ~/.bashrc && source ~/.bashrc
set -euo pipefail

PREFIX="$HOME/opt/cross"
TARGET=i686-elf
export PREFIX TARGET
export PATH="$PREFIX/bin:$PATH"

BINUTILS_VERSION=2.45
GCC_VERSION=15.2.0

SRC="$HOME/src/cross"
mkdir -p "$SRC"
cd "$SRC"

echo "==> Fetching sources..."
[ -f "binutils-$BINUTILS_VERSION.tar.gz" ] || \
    wget "https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VERSION.tar.gz"
[ -f "gcc-$GCC_VERSION.tar.gz" ] || \
    wget "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.gz"

echo "==> Unpacking..."
[ -d "binutils-$BINUTILS_VERSION" ] || tar -xf "binutils-$BINUTILS_VERSION.tar.gz"
[ -d "gcc-$GCC_VERSION" ]           || tar -xf "gcc-$GCC_VERSION.tar.gz"

echo "==> binutils..."
rm -rf build-binutils && mkdir build-binutils && cd build-binutils
../"binutils-$BINUTILS_VERSION"/configure \
    --target="$TARGET" --prefix="$PREFIX" \
    --with-sysroot --disable-nls --disable-werror
make -j"$(nproc)"
make install
cd "$SRC"

echo "==> gcc prerequisites..."
cd "gcc-$GCC_VERSION"
./contrib/download_prerequisites
cd "$SRC"

echo "==> gcc (C only) + libgcc..."
rm -rf build-gcc && mkdir build-gcc && cd build-gcc
../"gcc-$GCC_VERSION"/configure \
    --target="$TARGET" --prefix="$PREFIX" \
    --disable-nls --enable-languages=c --without-headers
make -j"$(nproc)" all-gcc
make -j"$(nproc)" all-target-libgcc
make install-gcc
make install-target-libgcc
cd "$SRC"

echo
echo "==> Done. Verify with: i686-elf-gcc --version"
if ! command -v i686-elf-gcc >/dev/null 2>&1; then
    echo "    Not on PATH yet - add it with:"
    echo "      export PATH=\"$PREFIX/bin:\$PATH\""
fi
