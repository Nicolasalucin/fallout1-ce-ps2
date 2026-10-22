#ifndef PS2_DEBUG_H_
#define PS2_DEBUG_H_

/*
 * Crash-resistant telemetry for the Fallout PS2 port (v1.0.2).
 *
 * Before the USB stack settles every PS2_LOG() line is written to a real file
 * with open/append/flush/close per call, so the very last line survives a
 * console freeze, a black screen or an emulator hang. Destinations are probed
 * in this order:
 *
 *   mass:/FALLOUT/debug.log    (USB/MX4SIO - the normal hardware target)
 *   mass:/debug.log
 *   mass0:/FALLOUT/debug.log   (second USB mount)
 *   mass0:/debug.log
 *   mc0:/FALLOUT/debug.log     (memory card fallback)
 *   mc0:/debug.log
 *
 * The memory card directory is created on demand (and MCMAN/MCSERV are
 * loaded if a raw-ELF boot left them out). Lines emitted before mass: is
 * mounted (USB driver bring-up happens later) are buffered in RAM and
 * flushed as soon as a destination becomes writable.
 *
 * Once mass: is ready ps2_init_debug_log() redirects stdout/stderr to the log
 * and ps2_debug_tick() commits it every few seconds with a dup2() close/reopen
 * cycle (the ps2sdk FAT32 driver only updates the directory entry when the
 * last descriptor is closed).
 *
 * PCSX2 note: there is deliberately no "host:" fallback. PCSX2's hostfs HLE
 * can wedge on the open/append/close-per-line pattern under strict emulation;
 * under the emulator the console mirror (captured by tools/run_smoke.sh into
 * build_ps2/pcsx2_strict.log) carries the same lines.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Probes the destination chain. Safe to call before the storage bring-up:
 * it returns without marking the probe done, so a later call can retry. */
void ps2_debug_init(void);

/* Re-probes the chain, preferring mass: again. Call after the storage
 * drivers have been brought up and the VFS roots are known. No-op while
 * stdout is already redirected. */
void ps2_debug_reprobe(void);

/* Writes one line (printf format) to the current destination. Before the
 * redirect each call does fopen("a") + vfprintf + fflush + fclose; after it
 * the line goes through the unbuffered redirected stdout. Also mirrors the
 * line to the console so PCSX2's strict log keeps a copy. */
void ps2_debug_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/* Current log path, or "" when nothing is writable yet. */
const char* ps2_debug_path(void);

/* Sets the crash flag: subsequent ps2_debug_printf() calls become no-ops so
 * an exception report cannot recurse into file I/O. Call after the crash
 * screen has been drawn. */
void ps2_debug_enter_crash_mode(void);

/* Writes the redirected log to the medium. On ps2sdk FAT32, fflush() alone
 * does not update the directory-entry size: the file is committed when its
 * last descriptor is closed. This reopens the path in append mode, atomically
 * replaces STDOUT_FILENO/STDERR_FILENO with dup2() (never leaving fd 1/2
 * vacant) and then checks the 4 MB cap. Returns 1 if the redirect is still
 * active, 0 otherwise. */
int ps2_debug_sync(void);

/* Rate-limited ps2_debug_sync(): touches the medium at most once every
 * PS2_DEBUG_SYNC_MS. Safe to call from the frame loop and from the logger. */
void ps2_debug_tick(void);

#define PS2_DEBUG_SYNC_MS 5000

/* Best-effort fflush(NULL) guarded against re-entry. Used before a clean
 * loader handoff; may block if the I/O stack is wedged. */
void ps2_debug_flush_all(void);

/* Called AFTER ps2_fs_init() + ps2_save_init() when mass: is guaranteed
 * to be mounted. Redirects stdout/stderr to the chosen log file with
 * unbuffered I/O so every printf reaches the pendrive before a freeze. */
void ps2_init_debug_log(void);

#define PS2_LOG(...) ps2_debug_printf(__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* PS2_DEBUG_H_ */
