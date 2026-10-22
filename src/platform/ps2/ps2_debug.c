/*
 * Crash-resistant telemetry for the Fallout PS2 port (v1.0.2).
 *
 * Design constraint: the physical console can freeze at any instruction, so
 * the only useful log is one that is already on the medium before the freeze.
 * Before stdout redirection every PS2_LOG() opens the file in append mode,
 * writes one line, flushes and closes it immediately. Once the USB stack is
 * up ps2_init_debug_log() points stdout/stderr at the same file and the
 * logger switches to a single unbuffered writer; ps2_debug_tick() periodically
 * closes/reopens it through dup2() so the FAT32 directory entry is committed.
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
static int s_redirected;
static unsigned int s_log_bytes;
static uint32_t s_last_sync_ms;

/* Re-entrancy/crash guards: a failing IOP can make stdio call back into the
 * log path, and the exception reporter must never try to open a file. */
static volatile int s_in_log;
static volatile int s_crash_mode;

static const char* const s_candidates[] = {
    "mass:/FALLOUT/debug.log",   /* prioridad 1: estructura oficial */
    "mass:/debug.log",           /* fallback: usuario copia ELF a raiz del pendrive */
    "mass0:/FALLOUT/debug.log",  /* segundo puerto / montaje alternativo */
    "mass0:/debug.log",          /* fallback del segundo montaje */
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
        if (i == 4) {
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
    /* The IOP USB stack is not up yet when ps2_boot() reaches this point.
     * Probing mass:/mc0: here would fail through the ps2sdk fileio RPC and,
     * worse, can race the still-loading modules. Do not mark s_probed so the
     * later ps2_debug_reprobe()/ps2_init_debug_log() can do the real probe. */
    if (s_probed || !ps2_storage_is_initialized()) {
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

    if (!ps2_storage_is_initialized()) {
        return;
    }

    /* Once stdout is redirected, ps2_debug_sync() owns the descriptor and
     * the FAT32 directory entry. Re-probing here would open a second
     * descriptor and could interleave the boot log. */
    if (s_redirected) {
        return;
    }

    strncpy(previous, s_path, sizeof(previous) - 1);
    previous[sizeof(previous) - 1] = '\0';

    if (!debug_probe()) {
        /* Keep the previous destination if the re-probe failed because the
         * stick was unplugged mid-boot. */
        strncpy(s_path, previous, sizeof(s_path) - 1);
        s_path[sizeof(s_path) - 1] = '\0';
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
 * `append` selects O_APPEND (1) or O_TRUNC (0). This captures ALL printf
 * output, not just PS2_LOG() lines.
 *
 * fd 1 and fd 2 are never closed without a valid replacement: dup2() performs
 * the replacement atomically and closes the old descriptor only after the new
 * one is installed. That close is also what makes the ps2sdk FAT32 driver
 * commit the previous directory-entry size. */
static int debug_redirect_open(const char* path, int append)
{
    int flags;
    int fd;
    int out_fd;
    int err_fd;

    if (path == NULL || path[0] == '\0') {
        return 0;
    }

    flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
    fd = open(path, flags, 0644);
    if (fd < 0) {
        return 0;
    }

    out_fd = dup2(fd, STDOUT_FILENO);
    if (out_fd < 0) {
        if (fd != STDOUT_FILENO && fd != STDERR_FILENO) {
            close(fd);
        }
        return 0;
    }

    err_fd = dup2(fd, STDERR_FILENO);
    if (err_fd < 0) {
        /* stdout is already attached to a valid descriptor. Do not close it:
         * the next engine open() could otherwise reuse fd 1 and corrupt a
         * game asset. */
        if (fd != STDOUT_FILENO && fd != STDERR_FILENO) {
            close(fd);
        }
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
    int append;

    if (s_redirected) {
        return;
    }

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
    if (strncmp(s_path, "mass:", 5) != 0
        && strncmp(s_path, "mass0:", 6) != 0) {
        PS2_LOG("[ps2] init_debug_log: keeping stdout on the console for '%s'", s_path);
        return;
    }

    /* The per-line logger may already contain boot lines written before the
     * USB stack settled. If so we must append; O_TRUNC is only allowed on the
     * truly first redirect when the file has no content yet. */
    if (!s_truncated) {
        debug_truncate();
        s_truncated = 1;
    }

    /* Replay the pre-storage buffer before redirecting. If this fails, the
     * buffer is retried after the redirect, and append mode is forced so a
     * later stdout write cannot overwrite the replayed lines. */
    debug_flush_early();

    append = (s_log_bytes > 0 || s_early_len > 0) ? 1 : 0;
    if (!debug_redirect_open(s_path, append)) {
        PS2_LOG("[ps2] init_debug_log: redirect failed for '%s'", s_path);
        return;
    }

    s_redirected = 1;
    s_last_sync_ms = ps2_time_ms();

    /* Retry the early buffer now that stdout is valid. */
    debug_flush_early();

    /* If the medium already holds a full log (previous run, or a card that
     * was not truncated), stop here. Leave fd 1/2 attached to the file: they
     * must never be left vacant. */
    {
        struct stat st;
        if (stat(s_path, &st) == 0
            && st.st_size >= PS2_DEBUG_LOG_MAX_BYTES) {
            s_log_bytes = PS2_DEBUG_LOG_MAX_BYTES;
            s_redirected = 0;
            return;
        }
    }

    printf("[ps2] init_debug_log: stdout/stderr -> %s\n", s_path);
#endif
}

int ps2_debug_sync(void)
{
    int fd;
    int out_fd;
    int err_fd;
    struct stat st;

    if (!s_redirected || s_path[0] == '\0' || s_crash_mode) {
        return 0;
    }

    /* Flush libc first: printf() is unbuffered, but a stray stdio buffer must
     * not be lost when dup2() closes the old descriptor. */
    fflush(stdout);
    fflush(stderr);

    /* Open a fresh append descriptor. O_APPEND is mandatory here: using
     * O_TRUNC every 5 s would erase the entire boot log. */
    fd = open(s_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        /* Keep fd 1/2 attached to the old file. Closing them without a
         * replacement would let the next engine open() reuse fd 1. */
        s_redirected = 0;
        return 0;
    }

    out_fd = dup2(fd, STDOUT_FILENO);
    if (out_fd < 0) {
        if (fd != STDOUT_FILENO && fd != STDERR_FILENO) {
            close(fd);
        }
        s_redirected = 0;
        return 0;
    }

    err_fd = dup2(fd, STDERR_FILENO);
    if (err_fd < 0) {
        /* stdout is valid; stderr stays on the previous valid descriptor. */
        if (fd != STDOUT_FILENO && fd != STDERR_FILENO) {
            close(fd);
        }
        s_redirected = 0;
        return 0;
    }

    if (fd != STDOUT_FILENO && fd != STDERR_FILENO) {
        close(fd);
    }

    s_last_sync_ms = ps2_time_ms();

    if (stat(s_path, &st) == 0 && st.st_size >= PS2_DEBUG_LOG_MAX_BYTES) {
        /* Stop redirecting/writing from the logger, but leave fd 1/2 open:
         * they are still valid descriptors, which is the important invariant. */
        s_log_bytes = PS2_DEBUG_LOG_MAX_BYTES;
        s_redirected = 0;
        return 0;
    }

    return 1;
}

void ps2_debug_tick(void)
{
    uint32_t now;

    if (!s_redirected || s_crash_mode || s_in_log) {
        return;
    }

    now = ps2_time_ms();
    if ((uint32_t)(now - s_last_sync_ms) < PS2_DEBUG_SYNC_MS) {
        return;
    }

    /* Rate-limit even when the sync fails (medium pulled): the next attempt
     * waits another full interval instead of hammering open() per log line. */
    s_last_sync_ms = now;
    ps2_debug_sync();
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

    if (s_redirected) {
        /* stdout is the log. Do not open a second descriptor per line: that
         * interleaves with the redirected stream and defeats the FAT32
         * close/commit performed by ps2_debug_sync(). */
        ps2_debug_tick();
        return;
    }

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
