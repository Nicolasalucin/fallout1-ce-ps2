#!/usr/bin/env bash
#
# Automated in-engine smoke test for the Fallout PS2 port.
#
# Builds the smoke variant of the ELF (PS2_DEV_SMOKE_TEST=ON, see
# src/game/ps2_smoke.cc) inside the dedicated fo1-build container, boots it in
# PCSX2 in strict (no speedhack / no time-stretch) mode, waits for the harness
# verdict, then shuts PCSX2 down again. Exits 0 on SUITE_FINISHED_SUCCESS.
#
# The complete PCSX2 output (stdout + stderr + -logfile), including IOP
# console, SIF/RPC and SPU2 debug channels, goes to:
#
#   build_ps2/pcsx2_strict.log
#
# PCSX2.ini is backed up before the run and restored on exit.
#
# CONTAINER ISOLATION: this script only ever touches fo1-build. Fallout 2
# containers (fps2-build / anything with fo2/fallout2 in the name) are
# rejected outright.
#
# Usage: tools/run_smoke.sh [timeout_seconds]   (default: 600)

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT/build_ps2_smoke"
LOG_DIR="$ROOT/build_ps2"
STRICT_LOG="$LOG_DIR/pcsx2_strict.log"
CONSOLE_LOG="$LOG_DIR/pcsx2_strict_console.log"
CONTAINER="${SMOKE_BUILD_CONTAINER:-fo1-build}"
IMAGE="${SMOKE_BUILD_IMAGE:-fallout-ce-ps2-build:latest}"
EMULOG="$HOME/.var/app/net.pcsx2.PCSX2/config/PCSX2/logs/emulog.txt"
PCSX2_INI="$HOME/.var/app/net.pcsx2.PCSX2/config/PCSX2/PCSX2.ini"
PCSX2_BIN="$HOME/.local/bin/pcsx2-qt"
FLATPAK_ID="net.pcsx2.PCSX2"
TIMEOUT="${1:-600}"

say() { printf '[run_smoke] %s\n' "$*"; }

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
say "building smoke ELF in $BUILD_DIR (container: $CONTAINER) ..."
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
# "..", so the game data must be reachable from the build dir. Created from
# inside the container because the build tree is root-owned.
docker exec "$CONTAINER" sh -c \
  "[ -e /src/build_ps2_smoke/local_assets ] || ln -s ../local_assets /src/build_ps2_smoke/local_assets"

# The container builds as root; PCSX2 runs as the host user and must be able
# to write host:/debug.log next to the smoke ELF.
docker exec "$CONTAINER" chmod 777 /src/build_ps2_smoke 2>/dev/null || true

mkdir -p "$LOG_DIR"
# The container builds as root and may have created build_ps2/ that way.
# Make the directory writable for the host-side PCSX2 log and INI backup.
docker exec "$CONTAINER" chmod 777 /src/build_ps2 2>/dev/null || true
rm -f "$STRICT_LOG" "$CONSOLE_LOG"

# --------------------------------------------------- backup / restore ------
MAPS_DIR="$ROOT/local_assets/DATA/MAPS"
MAPS_BACKUP="/tmp/opencode/smoke_maps_backup"
PCSX2_INI_BACKUP="$LOG_DIR/PCSX2.ini.pre-smoke"

restore_environment() {
  if [ -f "$PCSX2_INI_BACKUP" ]; then
    cp -f "$PCSX2_INI_BACKUP" "$PCSX2_INI" 2>/dev/null || true
    rm -f "$PCSX2_INI_BACKUP"
  fi

  if [ -d "$MAPS_BACKUP" ]; then
    rm -rf "$MAPS_DIR"
    cp -a "$MAPS_BACKUP" "$MAPS_DIR"
    rm -rf "$MAPS_BACKUP"
    say "restored $MAPS_DIR"
  fi
}

wait_for_pcsx2_exit() {
  local i

  # PCSX2 writes its settings on shutdown; restoring PCSX2.ini too early
  # would let that final save overwrite the user's original profile.
  for i in $(seq 1 30); do
    if ! flatpak ps --columns=application 2>/dev/null | grep -qx "net.pcsx2.PCSX2"; then
      return 0
    fi
    sleep 1
  done

  return 0
}

cleanup() {
  flatpak kill "$FLATPAK_ID" >/dev/null 2>&1 || true
  wait_for_pcsx2_exit
  restore_environment
}

trap cleanup EXIT INT TERM

say "stopping any running PCSX2 instance ..."
flatpak kill "$FLATPAK_ID" >/dev/null 2>&1 || true
sleep 1

# The engine persists per-map state to DATA/MAPS/*.SAV when it switches maps
# (the smoke suite streams V13ENT -> JUNKENT). Back the user's playthrough map
# state up and restore it afterwards so the suite cannot clobber it.
rm -rf "$MAPS_BACKUP"
if [ -d "$MAPS_DIR" ]; then
  cp -a "$MAPS_DIR" "$MAPS_BACKUP"
  say "backed up $MAPS_DIR"
fi

# ------------------------------------------------------- strict PCSX2 ------
if [ ! -f "$PCSX2_INI" ]; then
  say "ERROR: PCSX2.ini not found at $PCSX2_INI"
  exit 1
fi

cp -f "$PCSX2_INI" "$PCSX2_INI_BACKUP"
say "applying strict emulation profile (INI backup: $PCSX2_INI_BACKUP)"

python3 - "$PCSX2_INI" <<'PY' || exit 1
import re
import sys

path = sys.argv[1]

settings = {
    "EmuCore/Speedhacks": {
        "EECycleRate": "0",
        "EECycleSkip": "0",
        "WaitLoop": "false",
        "vuFlagHack": "false",
        "vu1Instant": "false",
        "IntcStat": "false",
        "fastCDVD": "false",
    },
    "SPU2/Output": {
        "SyncMode": "Disabled",
    },
    "SPU2/Debug": {
        "Global_Enable": "true",
        "Show_Messages": "true",
        "Log_DMA_Transfers": "true",
        "Log_Register_Access": "true",
    },
    "Logging": {
        "EnableSystemConsole": "true",
        "EnableFileLogging": "true",
        "EnableVerbose": "true",
        "EnableEEConsole": "true",
        "EnableIOPConsole": "true",
    },
}

with open(path, "r", encoding="utf-8", errors="surrogateescape") as f:
    lines = f.read().splitlines()

current = None
seen = {section: set() for section in settings}
out = []


def flush_missing():
    if current in settings:
        for key, value in settings[current].items():
            if key not in seen[current]:
                out.append("%s = %s" % (key, value))
                seen[current].add(key)


header_re = re.compile(r"^\s*\[(.+?)\]\s*$")
option_re = re.compile(r"^(\s*)([^=]+?)\s*=\s*(.*)$")

for line in lines:
    header = header_re.match(line)
    if header:
        flush_missing()
        current = header.group(1)
        out.append(line)
        continue

    if current in settings:
        option = option_re.match(line)
        if option and option.group(2) in settings[current]:
            key = option.group(2)
            out.append("%s%s = %s" % (option.group(1), key, settings[current][key]))
            seen[current].add(key)
            continue

    out.append(line)

flush_missing()

with open(path, "w", encoding="utf-8", errors="surrogateescape") as f:
    f.write("\n".join(out) + "\n")
PY

# --------------------------------------------------------------- launch ----
: > "$STRICT_LOG"
: > "$CONSOLE_LOG"
say "launching PCSX2 (strict) -> $STRICT_LOG"
nohup "$PCSX2_BIN" -batch -nogui -logfile "$STRICT_LOG" "$BUILD_DIR/fallout1-ce.elf" \
  >"$CONSOLE_LOG" 2>&1 &

# ---------------------------------------------------------------- wait -----
VERDICT=""
deadline=$(( $(date +%s) + TIMEOUT ))
while [ "$(date +%s)" -lt "$deadline" ]; do
  FOUND=""
  if [ -f "$STRICT_LOG" ]; then
    FOUND="$(grep "SUITE_FINISHED" "$STRICT_LOG" 2>/dev/null | tail -n 1)"
    if [ -z "$FOUND" ] && grep -q "\[ps2\] boot failed" "$STRICT_LOG" 2>/dev/null; then
      FOUND="[ps2] boot failed"
    fi
  fi
  if [ -z "$FOUND" ] && [ -f "$CONSOLE_LOG" ]; then
    FOUND="$(grep "SUITE_FINISHED" "$CONSOLE_LOG" 2>/dev/null | tail -n 1)"
    if [ -z "$FOUND" ] && grep -q "\[ps2\] boot failed" "$CONSOLE_LOG" 2>/dev/null; then
      FOUND="[ps2] boot failed"
    fi
  fi
  if [ -n "$FOUND" ]; then
    VERDICT="$FOUND"
    break
  fi
  sleep 2
done

say "shutting down PCSX2 ..."
flatpak kill "$FLATPAK_ID" >/dev/null 2>&1 || true
wait_for_pcsx2_exit

# Merge the process stdout/stderr into the single strict log requested by the
# hardware/debug workflow (PCSX2's -logfile already holds the app log).
if [ -f "$CONSOLE_LOG" ]; then
  cat "$CONSOLE_LOG" >> "$STRICT_LOG"
fi

restore_environment
trap - EXIT INT TERM

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
if [ -f "$STRICT_LOG" ]; then
  say "strict output (ps2/debug/IOP/SPU2 lines):"
  grep -E "ps2dbg|\[ps2\]|IOP|SPU2|SIF|FileIO" "$STRICT_LOG" | tail -n 200 || true
fi

if [ -z "$VERDICT" ]; then
  say "RESULT: TIMEOUT after ${TIMEOUT}s (no SUITE_FINISHED line)"
  say "last 40 lines of $STRICT_LOG:"
  tail -n 40 "$STRICT_LOG" 2>/dev/null || true
  exit 2
fi

if printf '%s' "$VERDICT" | grep -q "SUITE_FINISHED_SUCCESS"; then
  say "RESULT: SUCCESS"
  exit 0
fi

say "RESULT: FAILED: $VERDICT"
exit 1
