#!/bin/bash
# Build the mod into a test image and set it up in digiemu, writing the env
# file the emulator scripts read.
#   source env.sh; scripts/emu-image.sh NAME     -> build/NAME.syx, toolchain/emu-NAME.env
# Local only: images and the emulator's copies never go into Git.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NAME="${1:?name}"; shift || true
rm -f "$ROOT"/modwerk/sdk/digitakt/modules/midi-expression/src/*
"$ROOT/scripts/build-mod.sh" "$ROOT/build/$NAME.syx" "$@" | grep -E '"(ramSpare|flashHeadroom)"' || true
cd "$ROOT/digiemu"
.venv/bin/python -m emu.portable --add "$ROOT/build/$NAME.syx" --yes | tail -1
DIR=$(ls -td "$ROOT"/digiemu/portable/firmware/dt1-mx01-* | head -1)
SYX=$(ls "$DIR"/*.syx | head -1)
cat > "$ROOT/toolchain/emu-$NAME.env" <<EOF
export DT2_SYX='$SYX'
export DT2_SECTIONS='$DIR/sections'
export DT2_SNAPSHOTS='$DIR/snapshots'
export DT2_PLUSDRIVE='$DIR/plusdrive.img'
export DT2_MAIN_IMG='$DIR/sections/section_3_MAIN_OS.bin'
export DT2_DEVICES='$DIR/devices'
export DT2_SNAPSHOT='$DIR/snapshots/Digitakt_OSMX01/gui.snap'
EOF
echo "toolchain/emu-$NAME.env -> $DIR"
