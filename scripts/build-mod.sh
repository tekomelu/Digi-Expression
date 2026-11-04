#!/bin/bash
# Copy module/ into the local Modwerk clone and build a test image from your own OS 1.54.
#   source env.sh; scripts/build-mod.sh [OUT.syx] [--module digitakt:other ...]
# The image is a local test build: it stays on this computer and never goes into Git.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/build/midi-expression.syx}"; shift || true
DEST="$ROOT/modwerk/sdk/digitakt/modules/midi-expression"
mkdir -p "$DEST/src" "$DEST/media"
cp "$ROOT"/module/midi-expression/{build.json,modwerk.module.json,README.md,TESTING.md,LICENSE} "$DEST/"
cp "$ROOT"/module/midi-expression/src/* "$DEST/src/"
cp "$ROOT"/module/midi-expression/media/* "$DEST/media/"
cp "$ROOT/module/changelog/digitakt-midi-expression.json" "$ROOT/modwerk/src/community/module-changelogs/"
cd "$ROOT/modwerk"
node scripts/link-image.mjs --stock "$ROOT/Digitakt_OS1.54.syx" --version MX01 --out "$OUT" \
  --elemod vendor/elekloader/catalog/core-2.1-os1.54.elemod --module digitakt:midi-expression "$@"
