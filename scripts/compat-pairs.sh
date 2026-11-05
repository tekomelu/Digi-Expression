#!/bin/bash
# Link MIDI Expression with each other Modwerk Digitakt module (and what it
# requires) into a test image, and note whether they combine and the RAM left.
#   source env.sh; scripts/compat-pairs.sh cc-recall fx-track ...  -> results/compat/pairs.txt
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" && source env.sh && cd modwerk && mkdir -p ../results/compat
declare -A dep=([cc-names]=dt-support [euclidean]=dt-support [digimono]=digichain [digipoly]=digichain [digistring]=machine-pages [ram-machines]=machine-pages)
for m in "$@"; do
  extra=""; [ -n "${dep[$m]}" ] && extra="--module digitakt:${dep[$m]}"
  r=$(timeout 170 node scripts/link-image.mjs --stock "$PWD/../Digitakt_OS1.54.syx" --version MX01 --out /tmp/pair.syx --elemod vendor/elekloader/catalog/core-2.1-os1.54.elemod --module digitakt:midi-expression --module digitakt:$m $extra 2>&1 | grep -v "^\s*at ")
  if echo "$r" | grep -q ramSpare; then echo "$m${extra:+ (+${dep[$m]})}: links, $(echo "$r" | grep -E 'ramSpare' | tr -d ' ,\"')" >> ../results/compat/pairs.txt
  else echo "$m: $(echo "$r" | grep -A4 'combine' | tail -4 | tr '\n' ' ')" >> ../results/compat/pairs.txt; fi
done
