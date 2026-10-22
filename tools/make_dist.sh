#!/usr/bin/env bash
#
# Assembles the release bundle in dist/FALLOUT/.
#
# Builds the shipping ELF in a clean Release configuration (Release, no smoke
# harness) with PS2_STRIP_ELF=ON, then copies the stripped build artifact to
# dist/FALLOUT/FALLOUT.ELF next to the checked-in SYSTEM.CNF and
# README_PS2.txt. Verifies the bundle (tree, boot descriptor, byte-identical
# ELF, smoke markers absent) and prints a SHA-256.
#
# Usage: tools/make_dist.sh

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT/build_ps2_release"
DIST_DIR="$ROOT/dist/FALLOUT"
CONTAINER="${DIST_BUILD_CONTAINER:-fo1-build}"
IMAGE="${DIST_BUILD_IMAGE:-fallout-ce-ps2-build:latest}"

say() { printf '[make_dist] %s\n' "$*"; }

# ------------------------------------------------------ container guard ----
case "$CONTAINER" in
  *fo2*|*FO2*|*fallout2*|*Fallout2*|*fps2*)
    say "ERROR: refusing to touch Fallout 2 container '$CONTAINER'"
    exit 1
    ;;
esac

if ! docker inspect "$CONTAINER" >/dev/null 2>&1; then
  say "creating dedicated container $CONTAINER ..."
  docker run -d --name "$CONTAINER" -v "$ROOT":/src -w /src "$IMAGE" sleep infinity || exit 1
elif [ "$(docker inspect -f '{{.State.Running}}' "$CONTAINER")" != "true" ]; then
  docker start "$CONTAINER" >/dev/null || exit 1
fi

if ! docker exec "$CONTAINER" test -f /src/CMakeLists.txt; then
  say "ERROR: $CONTAINER does not have this repository mounted at /src"
  exit 1
fi

# ---------------------------------------------------------------- build ----
say "building release ELF in $BUILD_DIR (container: $CONTAINER) ..."
docker exec "$CONTAINER" sh -c \
  "cmake -S /src -B /src/build_ps2_release \
     -DCMAKE_TOOLCHAIN_FILE=/src/cmake/toolchain-ps2.cmake \
     -DCMAKE_BUILD_TYPE=Release \
     -DPS2_DEV_SMOKE_TEST=OFF \
     -DPS2_STRIP_ELF=ON" || exit 1
docker exec "$CONTAINER" sh -c "cmake --build /src/build_ps2_release -j4" || exit 1

if [ ! -f "$BUILD_DIR/fallout1-ce.elf" ]; then
  say "ERROR: $BUILD_DIR/fallout1-ce.elf not found"
  exit 1
fi

# ---------------------------------------------------------------- package --
mkdir -p "$DIST_DIR"
say "copying stripped ELF -> dist/FALLOUT/FALLOUT.ELF ..."
cp -f "$BUILD_DIR/fallout1-ce.elf" "$DIST_DIR/FALLOUT.ELF" || exit 1

# The shipped ELF must be exactly the stripped build artifact, not a stale
# file left over from a previous release.
if ! cmp -s "$BUILD_DIR/fallout1-ce.elf" "$DIST_DIR/FALLOUT.ELF"; then
  say "ERROR: dist/FALLOUT/FALLOUT.ELF differs from the build artifact"
  exit 1
fi

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
