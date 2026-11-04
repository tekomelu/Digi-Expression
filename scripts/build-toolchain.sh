#!/bin/bash
# Builds the m68k-elf (ColdFire) binutils 2.47 and GCC 16.2.0 used by Modwerk's Digitakt modules.
# Sources are checksum-verified in toolchain/src. Installs into toolchain/m68k-elf.
set -euo pipefail
T="$(cd "$(dirname "$0")/.." && pwd)/toolchain"
PREFIX="$T/m68k-elf"
W="$HOME/tcbuild"
mkdir -p "$W" "$PREFIX"
cd "$W"
echo "[$(date)] unpack"
[ -d binutils-2.47 ] || tar -xJf "$T/src/binutils-2.47.tar.xz"
[ -d gcc-16.2.0 ] || tar -xJf "$T/src/gcc-16.2.0.tar.xz"
echo "[$(date)] gcc prerequisites (GMP, MPFR, MPC; sha512-checked by GCC's script)"
(cd gcc-16.2.0 && [ -e gmp ] || ./contrib/download_prerequisites --no-isl --no-graphite)
echo "[$(date)] binutils"
mkdir -p b-binutils && cd b-binutils
[ -x "$PREFIX/bin/m68k-elf-as" ] || { ../binutils-2.47/configure --target=m68k-elf --prefix="$PREFIX" --disable-nls --disable-werror --disable-gprofng --disable-gdb --disable-sim && make -j4 && make install; }
cd "$W"
echo "[$(date)] gcc"
mkdir -p b-gcc && cd b-gcc
export PATH="$PREFIX/bin:$PATH"
[ -x "$PREFIX/bin/m68k-elf-gcc" ] || { ../gcc-16.2.0/configure --target=m68k-elf --prefix="$PREFIX" --enable-languages=c --without-headers --disable-bootstrap --disable-multilib --disable-nls --disable-shared --disable-threads --disable-libssp --disable-lto && make -j4 all-gcc && make install-gcc; }
echo "[$(date)] done"
"$PREFIX/bin/m68k-elf-as" --version | head -1
"$PREFIX/bin/m68k-elf-gcc" --version | head -1
