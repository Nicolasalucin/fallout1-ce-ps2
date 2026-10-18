#!/usr/bin/env bash
#
# Assembles the release bundle in dist/FALLOUT/.
#
# Builds the shipping ELF in a clean release configuration (Release, no smoke
# harness), strips it with mips64r5900el-ps2-elf-strip and drops it next to
# the checked-in SYSTEM.CNF and README_PS2.txt. Verifies the bundle (tree,
# boot descriptor, smoke markers absent) and prints a SHA-256.
#
# Usage: tools/make_dist.sh

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT/build_ps2_release"
DIST_DIR="$ROOT/dist/FALLOUT"
CONTAINER="${DIST_BUILD_CONTAINER:-fps2-build}"

say() { printf '[make_dist] %s\n' "$*"; }

# ---------------------------------------------------------------- build ----
say "building release ELF in $BUILD_DIR ..."
docker start "$CONTAINER" >/dev/null
docker exec "$CONTAINER" sh -c \
  "cmake -S /src -B /src/build_ps2_release \
     -DCMAKE_TOOLCHAIN_FILE=/src/cmake/toolchain-ps2.cmake \
     -DCMAKE_BUILD_TYPE=Release \
     -DPS2_DEV_SMOKE_TEST=OFF" || exit 1
docker exec "$CONTAINER" sh -c "cmake --build /src/build_ps2_release -j4" || exit 1

if [ ! -f "$BUILD_DIR/fallout1-ce.elf" ]; then
  say "ERROR: $BUILD_DIR/fallout1-ce.elf not found"
  exit 1
fi

# ----------------------------------------------------------------- strip ---
mkdir -p "$DIST_DIR"
say "stripping -> dist/FALLOUT/FALLOUT.ELF ..."
docker exec "$CONTAINER" sh -c \
  "mips64r5900el-ps2-elf-strip -o /src/dist/FALLOUT/FALLOUT.ELF /src/build_ps2_release/fallout1-ce.elf" || exit 1

# ---------------------------------------------------------------- verify ---
for f in FALLOUT.ELF SYSTEM.CNF README_PS2.txt; do
  if [ ! -s "$DIST_DIR/$f" ]; then
    say "ERROR: dist/FALLOUT/$f is missing"
    exit 1
  fi
done

# The release build must not carry the smoke harness (it would take over the
# game flow). The harness string literals are the simplest marker.
if grep -aq "SUITE_FINISHED" "$DIST_DIR/FALLOUT.ELF"; then
  say "ERROR: smoke harness strings found in the release ELF"
  exit 1
fi

# SYSTEM.CNF must boot the ELF that ships next to it.
if ! grep -qF 'BOOT2 = cdrom0:\FALLOUT.ELF;1' "$DIST_DIR/SYSTEM.CNF"; then
  say "ERROR: SYSTEM.CNF does not boot FALLOUT.ELF"
  exit 1
fi

say "bundle tree:"
( cd "$DIST_DIR" && ls -l && echo && sha256sum FALLOUT.ELF )

say "OK: dist/FALLOUT ready ($(du -sh "$DIST_DIR" | cut -f1))"
