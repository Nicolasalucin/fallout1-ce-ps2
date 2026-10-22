/*
 * Crash-resistant telemetry for the Fallout PS2 port (v1.0.1).
 *
 * Design constraint: the physical console can freeze at any instruction, so
 * the only useful log is one that is already on the medium before the freeze.
 * Every PS2_LOG() therefore opens the file in append mode, writes one line,
 * flushes and closes it immediately; there is no buffered/global FILE* that a
 * crash could lose.
 */

#include "ps2_debug.h"
#include "ps2_storage.h"
#include "ps2_time.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <kernel.h>
#include <loadfile.h>
#include <sifrpc.h>

#define PS2_DEBUG_PATH_MAX 128

/* Lines emitted before the USB stack is up (ps2_debug_init() runs first in
 * ps2_boot()) are kept here and replayed once a destination opens. */
#define PS2_DEBUG_EARLY_SIZE 16384
static char s_early[PS2_DEBUG_EARLY_SIZE];
static int s_early_len;

/* A boot log that fills the stick is worse than no log: once the cap is hit
 * the file path is closed for the rest of the boot, but the console mirror
 * keeps working. */
#define PS2_DEBUG_LOG_MAX_BYTES (4 * 1024 * 1024)

static char s_path[PS2_DEBUG_PATH_MAX];
static int s_probed;
static int s_rpc_ready;
static int s_mc_prepared;
static int s_truncated;
static unsigned int s_log_bytes;

/* Re-entrancy/crash guards: a failing IOP can make stdio call back into the
 * log path, and the exception reporter must never try to open a file. */
static volatile int s_in_log;
static volatile int s_crash_mode;

static const char* const s_candidates[] = {
    "mass:/FALLOUT/debug.log",   /* prioridad 1: estructura oficial */
    "mass:/debug.log",           /* fallback: usuario copia ELF a raiz del pendrive */
    "mc0:/FALLOUT/debug.log",    /* Memory Card */
    "mc0:/debug.log",            /* fallback final */
};
static const int s_candidate_count = (int)(sizeof(s_candidates) / sizeof(s_candidates[0]));

static int debug_file_writable(const char* path)
{
    FILE* f = fopen(path, "a");
    if (f == NULL) {
        return 0;
    }

    fclose(f);
    return 1;
}

/* Memory cards need the FALLOUT directory and, on a raw-ELF boot, the card
 * drivers themselves (uLaunchELF/OPL usually leave MCMAN/MCSERV resident). */
static void debug_prepare_mc0(void)
{
    if (s_mc_prepared) {
        return;
    }
    s_mc_prepared = 1;

    if (!ps2_storage_device_present("mc0:")) {
        SifLoadModule("rom0:SIO2MAN", 0, NULL);
        SifLoadModule("rom0:MCMAN", 0, NULL);
        SifLoadModule("rom0:MCSERV", 0, NULL);
    }

    if (ps2_storage_device_present("mc0:")) {
        mkdir("mc0:/FALLOUT", 0755);
    }
}

static int debug_probe(void)
{
#ifdef PS2_DEV_SMOKE_TEST
    /* Smoke runs are captured from the PCSX2 console. Avoid touching mc0:
     * from the harness: the folder-backed card and hostfs reads do not get
     * along in PCSX2, and an automated run does not need the file copy. */
    return 0;
#endif

    int i;

    if (!s_rpc_ready) {
        sceSifInitRpc(0);
        s_rpc_ready = 1;
    }

    s_path[0] = '\0';

    for (i = 0; i < s_candidate_count; i++) {
        if (i == 2) {
            debug_prepare_mc0();
        }

        if (debug_file_writable(s_candidates[i])) {
            strncpy(s_path, s_candidates[i], sizeof(s_path) - 1);
            s_path[sizeof(s_path) - 1] = '\0';
            return 1;
        }
    }

    return 0;
}

static void debug_truncate(void)
{
    FILE* f;

    if (s_path[0] == '\0') {
        return;
    }

    f = fopen(s_path, "w");
    if (f != NULL) {
        fclose(f);
    }
    s_log_bytes = 0;
}

static int debug_write_line(uint32_t ms, const char* msg)
{
    FILE* f;
    int n;

    if (s_path[0] == '\0' || s_crash_mode || s_in_log) {
        return 0;
    }

    if (s_log_bytes >= PS2_DEBUG_LOG_MAX_BYTES) {
        return 0;
    }

    s_in_log = 1;
    f = fopen(s_path, "a");
    if (f == NULL) {
        s_in_log = 0;
        return 0;
    }

    n = fprintf(f, "[ps2dbg] %8u ms | %s\n", (unsigned int)ms, msg);
    fflush(f);
    fclose(f);
    if (n > 0) {
        s_log_bytes += (unsigned int)n;
    }
    s_in_log = 0;

    return 1;
}

static void debug_flush_early(void)
{
    FILE* f;

    if (s_early_len == 0 || s_path[0] == '\0'
        || s_crash_mode || s_in_log) {
        return;
    }

    if (s_log_bytes >= PS2_DEBUG_LOG_MAX_BYTES) {
        s_early_len = 0;
        return;
    }

    s_in_log = 1;
    f = fopen(s_path, "a");
    if (f != NULL) {
        fwrite(s_early, 1, (size_t)s_early_len, f);
        fflush(f);
        fclose(f);
        s_log_bytes += (unsigned int)s_early_len;
        s_early_len = 0;
    }
    s_in_log = 0;
}

static void debug_buffer(uint32_t ms, const char* msg)
{
    int n;
    int room = (int)sizeof(s_early) - s_early_len;

    if (room <= 0) {
        return;
    }

    n = snprintf(s_early + s_early_len, (size_t)room, "[ps2dbg] %8u ms | %s\n", (unsigned int)ms, msg);
    if (n > 0 && n < room) {
        s_early_len += n;
    }
}

void ps2_debug_init(void)
{
    if (s_probed) {
        return;
    }
    s_probed = 1;

    if (debug_probe()) {
        if (!s_truncated) {
            debug_truncate();
            s_truncated = 1;
        }
        debug_flush_early();
    }
}

void ps2_debug_reprobe(void)
{
    char previous[PS2_DEBUG_PATH_MAX];

    strncpy(previous, s_path, sizeof(previous) - 1);
    previous[sizeof(previous) - 1] = '\0';

    if (!debug_probe()) {
        return;
    }

    debug_flush_early();

    if (strcmp(previous, s_path) != 0) {
        ps2_debug_printf("[ps2] debug: log destination -> %s", s_path);
    }
}

const char* ps2_debug_path(void)
{
    return s_path;
}

/* Redirects stdout/stderr to the chosen debug log file with unbuffered I/O.
 * This captures ALL printf output, not just PS2_LOG() lines. */
static int debug_redirect_open(const char* path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        return 0;
    }

    if (fd != STDOUT_FILENO && dup2(fd, STDOUT_FILENO) < 0) {
        close(fd);
        return 0;
    }
    if (fd != STDERR_FILENO && dup2(fd, STDERR_FILENO) < 0) {
        close(fd);
        return 0;
    }
    if (fd != STDOUT_FILENO && fd != STDERR_FILENO) {
        close(fd);
    }

    /* Critico: sin buffer. Cada printf llega al pendrive antes de congelarse. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    return 1;
}

void ps2_init_debug_log(void)
{
    if (s_path[0] == '\0') {
        /* Probe the candidates now that mass: is mounted. */
        if (!debug_probe()) {
            PS2_LOG("[ps2] init_debug_log: no writable destination found");
            return;
        }

        if (!s_truncated) {
            debug_truncate();
            s_truncated = 1;
        }
        debug_flush_early();
    }

#ifdef PS2_DEV_SMOKE_TEST
    /* The smoke runner reads its SUITE_FINISHED verdict from the PCSX2
     * console. Redirecting stdout to a memory-card file would hide it (and
     * MCSERV does not like a long-lived descriptor plus one open per log
     * line). PS2_LOG() still appends to debug.log. */
    PS2_LOG("[ps2] init_debug_log: smoke build keeps stdout on the console");
    return;
#else
    /* A long-lived stdout descriptor on a memory card conflicts with the
     * open/append/close-per-line logger: MCSERV rejects the second open and
     * the re-probe path would churn the card. Only redirect on USB mass
     * storage, where concurrent opens are safe; on mc0: the per-line logger
     * already keeps the crash-resistant copy. */
    if (strncmp(s_path, "mass:", 5) != 0) {
        PS2_LOG("[ps2] init_debug_log: keeping stdout on the console for '%s'", s_path);
        return;
    }

    if (!debug_redirect_open(s_path)) {
        PS2_LOG("[ps2] init_debug_log: redirect failed for '%s'", s_path);
    }
#endif
}

void ps2_debug_printf(const char* fmt, ...)
{
    char msg[512];
    uint32_t ms;
    va_list ap;

    if (fmt == NULL || s_crash_mode || s_in_log) {
        return;
    }

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    ms = ps2_time_ms();

    /* Mirror to the console: the strict PCSX2 run and the hardware serial
     * adapter both benefit from seeing the same lines in order. */
    printf("[ps2dbg] %8u ms | %s\n", (unsigned int)ms, msg);

    if (s_path[0] == '\0') {
        /* Boot-time lines (before the USB stack exists) are replayed by
         * ps2_storage_init()/ps2_debug_reprobe() once a destination opens.
         * Do not probe on every line: loading MCMAN/MCSERV repeatedly during
         * storage bring-up would be pointless and slow. */
        debug_buffer(ms, msg);
        return;
    }

    if (!debug_write_line(ms, msg)) {
        if (s_log_bytes >= PS2_DEBUG_LOG_MAX_BYTES) {
            return;
        }

        /* USB stick pulled, card removed, host: gone: re-probe and retry. */
        if (debug_probe()) {
            debug_flush_early();
            debug_write_line(ms, msg);
        } else {
            debug_buffer(ms, msg);
        }
    }
}

void ps2_debug_enter_crash_mode(void)
{
    if (s_crash_mode) {
        return;
    }

    ps2_debug_flush_all();
    s_crash_mode = 1;
}

void ps2_debug_flush_all(void)
{
    if (s_in_log) {
        return;
    }

    s_in_log = 1;
    fflush(NULL);
    s_in_log = 0;
}
