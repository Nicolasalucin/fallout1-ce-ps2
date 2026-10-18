#!/usr/bin/env bash
#
# Automated in-engine smoke test for the PS2 port.
#
# Builds the smoke variant of the ELF (PS2_DEV_SMOKE_TEST=ON, see
# src/game/ps2_smoke.cc), boots it in PCSX2, waits for the harness verdict in
# the emulator log, then shuts PCSX2 down again. Exits 0 on
# SUITE_FINISHED_SUCCESS, non-zero otherwise.
#
# NOTE: this kills any running PCSX2 instance (flatpak net.pcsx2.PCSX2).
#
# Usage: tools/run_smoke.sh [timeout_seconds]   (default: 240)

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT/build_ps2_smoke"
CONTAINER="${SMOKE_BUILD_CONTAINER:-fps2-build}"
EMULOG="$HOME/.var/app/net.pcsx2.PCSX2/config/PCSX2/logs/emulog.txt"
PCSX2_BIN="$HOME/.local/bin/pcsx2-qt"
FLATPAK_ID="net.pcsx2.PCSX2"
TIMEOUT="${1:-240}"

say() { printf '[run_smoke] %s\n' "$*"; }

# ---------------------------------------------------------------- build ----
say "building smoke ELF in $BUILD_DIR ..."
docker start "$CONTAINER" >/dev/null
docker exec "$CONTAINER" sh -c \
  "cmake -S /src -B /src/build_ps2_smoke \
     -DCMAKE_TOOLCHAIN_FILE=/src/cmake/toolchain-ps2.cmake \
     -DCMAKE_BUILD_TYPE=Release \
     -DPS2_DEV_SMOKE_TEST=ON" || exit 1
docker exec "$CONTAINER" sh -c "cmake --build /src/build_ps2_smoke -j4" || exit 1

if [ ! -f "$BUILD_DIR/fallout1-ce.elf" ]; then
  say "ERROR: $BUILD_DIR/fallout1-ce.elf not found"
  exit 1
fi

# PCSX2 exposes the ELF's directory as "host:" and does not resolve paths with
# "..", so the game data must be reachable from the build dir. The main build
# build_ps2/ uses the same symlink (dev layout only). Created from inside the
# container because the build tree is root-owned.
docker exec "$CONTAINER" sh -c \
  "[ -e /src/build_ps2_smoke/local_assets ] || ln -s ../local_assets /src/build_ps2_smoke/local_assets"

# --------------------------------------------------------------- launch ----
say "stopping any running PCSX2 instance ..."
flatpak kill "$FLATPAK_ID" >/dev/null 2>&1
sleep 1

# The engine persists per-map state to DATA/MAPS/*.SAV when it switches maps
# (the smoke suite streams V13ENT -> JUNKENT). Back the user's playthrough map
# state up and restore it afterwards so the suite cannot clobber it.
MAPS_DIR="$ROOT/local_assets/DATA/MAPS"
MAPS_BACKUP="/tmp/opencode/smoke_maps_backup"
rm -rf "$MAPS_BACKUP"
if [ -d "$MAPS_DIR" ]; then
  cp -a "$MAPS_DIR" "$MAPS_BACKUP"
  say "backed up $MAPS_DIR"
fi

START_LINE=0
if [ -f "$EMULOG" ]; then
  START_LINE=$(wc -l < "$EMULOG")
fi

say "launching PCSX2 with the smoke ELF ..."
nohup "$PCSX2_BIN" "$BUILD_DIR/fallout1-ce.elf" >/tmp/opencode/pcsx2_smoke.log 2>&1 &

# ---------------------------------------------------------------- wait -----
VERDICT=""
deadline=$(( $(date +%s) + TIMEOUT ))
while [ "$(date +%s)" -lt "$deadline" ]; do
  if [ -f "$EMULOG" ]; then
    LINES=$(wc -l < "$EMULOG")
    if [ "$LINES" -lt "$START_LINE" ]; then
      START_LINE=0          # PCSX2 truncated/reopened the log
    fi
    NEW="$(tail -n +$((START_LINE + 1)) "$EMULOG")"
    if printf '%s' "$NEW" | grep -q "SUITE_FINISHED"; then
      VERDICT="$(printf '%s' "$NEW" | grep "SUITE_FINISHED" | tail -n 1)"
      break
    fi
    if printf '%s' "$NEW" | grep -q "\[ps2\] boot failed"; then
      VERDICT="[ps2] boot failed"
      break
    fi
  fi
  sleep 2
done

say "shutting down PCSX2 ..."
flatpak kill "$FLATPAK_ID" >/dev/null 2>&1

if [ -d "$MAPS_BACKUP" ]; then
  rm -rf "$MAPS_DIR"
  cp -a "$MAPS_BACKUP" "$MAPS_DIR"
  rm -rf "$MAPS_BACKUP"
  say "restored $MAPS_DIR"
fi

# ------------------------------------------------- save slot (F6, host) ----
# Dev builds keep the F6 save shim on the host, so slot 1 must be visible
# from here: payload files plus the PS2 browser metadata.
SLOT_DIR="$ROOT/local_assets/saves/SLOT01"
if [ -d "$SLOT_DIR" ]; then
  say "slot 1 on disk ($SLOT_DIR):"
  ls -l "$SLOT_DIR" | tail -n +2 | sed 's/^/  /'
  if [ -s "$SLOT_DIR/SAVE.DAT" ] \
     && ls "$SLOT_DIR"/*.SAV >/dev/null 2>&1 \
     && [ "$(head -c 4 "$SLOT_DIR/icon.sys" 2>/dev/null)" = "PS2D" ] \
     && [ -s "$SLOT_DIR/icon.icn" ]; then
    say "SAVE_LOAD_FILES_HOST_OK: slot 1 payload + icon.sys/icon.icn present"
  else
    say "WARNING: slot 1 is missing expected files"
  fi
else
  say "WARNING: $SLOT_DIR not found (save stub did not run?)"
fi

# --------------------------------------------------------------- report ----
if [ -f "$EMULOG" ]; then
  say "smoke output:"
  tail -n +$((START_LINE + 1)) "$EMULOG" \
    | grep -E "\[ps2\]\[smoke\]|\[ps2\] osk:|\[ps2\] storage:|\[ps2\] fs: (diag|probe|data root)|MAP: |WORLD MAP" || true
fi

if [ -z "$VERDICT" ]; then
  say "RESULT: TIMEOUT after ${TIMEOUT}s (no SUITE_FINISHED line in emulog)"
  exit 2
fi

if printf '%s' "$VERDICT" | grep -q "SUITE_FINISHED_SUCCESS"; then
  say "RESULT: SUCCESS"
  exit 0
fi

say "RESULT: FAILED"
exit 1
