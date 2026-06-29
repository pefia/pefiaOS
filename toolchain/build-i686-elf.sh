#!/usr/bin/env bash
# toolchain/build-i686-elf.sh
# -----------------------------------------------------------------------------
# Builds the i686-elf cross-compiler (binutils + gcc) from source and installs
# it to $HOME/opt/cross. Run this ONCE inside WSL2 Ubuntu. Takes ~20-40 min.
#
# IMPORTANT: run this from your Linux home (e.g. ~), NOT from /mnt/c, because
# building on the Windows filesystem is slow and can trip on permissions.
#
# Usage:
#   sudo apt update
#   sudo apt install -y build-essential bison flex libgmp3-dev libmpc-dev \
#                       libmpfr-dev texinfo wget
#   bash build-i686-elf.sh
#   echo 'export PATH="$HOME/opt/cross/bin:$PATH"' >> ~/.bashrc && source ~/.bashrc
# -----------------------------------------------------------------------------
set -euo pipefail

export PREFIX="$HOME/opt/cross"
export TARGET=i686-elf
export PATH="$PREFIX/bin:$PATH"

BINUTILS_VERSION=2.45
GCC_VERSION=15.2.0

SRC="$HOME/src/cross"
mkdir -p "$SRC"
cd "$SRC"

echo "==> Downloading sources (if not already present)..."
[ -f "binutils-$BINUTILS_VERSION.tar.gz" ] || \
    wget "https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VERSION.tar.gz"
[ -f "gcc-$GCC_VERSION.tar.gz" ] || \
    wget "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.gz"

echo "==> Extracting..."
[ -d "binutils-$BINUTILS_VERSION" ] || tar -xf "binutils-$BINUTILS_VERSION.tar.gz"
[ -d "gcc-$GCC_VERSION" ]           || tar -xf "gcc-$GCC_VERSION.tar.gz"

echo "==> Building binutils..."
rm -rf build-binutils && mkdir build-binutils && cd build-binutils
../"binutils-$BINUTILS_VERSION"/configure \
    --target="$TARGET" --prefix="$PREFIX" \
    --with-sysroot --disable-nls --disable-werror
make -j"$(nproc)"
make install
cd "$SRC"

echo "==> Downloading GCC prerequisites..."
cd "gcc-$GCC_VERSION"
./contrib/download_prerequisites
cd "$SRC"

echo "==> Building gcc (C only) + libgcc..."
rm -rf build-gcc && mkdir build-gcc && cd build-gcc
../"gcc-$GCC_VERSION"/configure \
    --target="$TARGET" --prefix="$PREFIX" \
    --disable-nls --enable-languages=c --without-headers
make -j"$(nproc)" all-gcc
make -j"$(nproc)" all-target-libgcc
make install-gcc
make install-target-libgcc
cd "$SRC"

echo ""
echo "==> Done. Verify with:  i686-elf-gcc --version"
echo "    If 'command not found', add the toolchain to your PATH:"
echo "      export PATH=\"\$HOME/opt/cross/bin:\$PATH\""
