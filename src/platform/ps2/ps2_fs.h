#ifndef PS2_FS_H_
#define PS2_FS_H_

/*
 * PS2 filesystem layer (F4/F9).
 *
 * The engine asks for game data with relative names ("master.dat",
 * "sound/music/01hub.acm", data files in backslash notation, ...). This layer
 * maps those names onto the PS2 storage devices with this probe order:
 *
 *   <ELF dir>/            (relative to the running ELF, from argv[0])
 *   <ELF dir>/local_assets/
 *   mass:/FALLOUT/        (USB stick / MX4SIO)
 *   mass:/                (USB stick, data at the root)
 *   mass0:/FALLOUT/       (numbered mass device)
 *   cdrom0:/              (CD/DVD)
 *   cdrom0:/FALLOUT/
 *   hdd0:/FALLOUT/        (internal HDD, PFS partition)
 *   host:local_assets/    (PCSX2 host filesystem / ps2client dev layout)
 *   host:../local_assets/ (ELF in build_ps2/ under the repo)
 *   host:/                (PCSX2 host root)
 *
 * A root is only accepted when both MASTER.DAT and CRITTER.DAT open there.
 * Lookups are case-insensitive (the shipped media is uppercase, the engine
 * uses lowercase): every component is matched with readdir() against the
 * real directory entries, which works on FAT32, ISO9660, host and PFS
 * alike. Large ".dat" files get a block-sized stdio buffer so random reads
 * inside master.dat/critter.dat do not degrade into tiny driver
 * transactions.
 */

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Probes the storage devices once and logs the result. Idempotent; every
 * entry point calls it lazily, but ps2_boot() calls it eagerly for the log. */
void ps2_fs_init(void);

/* Re-runs the root probe from scratch. Use this when the storage stack
 * mounted late (for example, a USB stick that appeared after the initial
 * probe) or after a device change. Clears the directory-listing cache and
 * the known roots before probing again. */
void ps2_fs_reprobe(void);

/* Number of usable data roots found (0 when nothing is mounted). */
int ps2_fs_root_count(void);

/* Root path for diagnostics, e.g. "host:local_assets/". */
const char* ps2_fs_root_path(int index);

/* Resolves `path` (relative engine path or absolute iomanX path) to a real,
 * correctly cased device path. `writable` allows resolving a not-yet-existing
 * last component and restricts relative lookups to writable roots if any
 * exist. Returns 0 on success (result copied to `out`). */
int ps2_fs_resolve(const char* path, char* out, int out_size, int writable);

/* Resolves and opens `path` with `mode` (fopen semantics). Returns NULL when
 * the file cannot be found in any root. */
FILE* ps2_fs_fopen(const char* path, const char* mode);

/* Kinds of a directory entry, taken from readdir()'s d_type. */
#define PS2_FS_KIND_UNKNOWN 0
#define PS2_FS_KIND_FILE 1
#define PS2_FS_KIND_DIR 2

/* In-place case-insensitive canonicalization of a device path such as
 * "host:local_assets/master.dat". When `allow_missing_last` is non-zero the
 * final component may be absent (for creating files). Returns 0 on success. */
int ps2_fs_canonicalize(char* path, int allow_missing_last);

/* Drops every cached directory listing (the save-slot wipe removes whole
 * directories). */
void ps2_fs_invalidate_dir_cache(void);

/* Drops the cached listing of the directory holding `resolved` (a canonical
 * device path). compat_mkdir/compat_remove/compat_rename call this after
 * changing a directory outside the fopen() path. */
void ps2_fs_invalidate_parent_of(const char* resolved);

#ifdef PS2_DEV_SMOKE_TEST
/* Dev profiling counters: how much directory resolution work the engine
 * caused. Reported by the smoke suite so VFS changes can be told apart from
 * rendering work. */
extern unsigned int ps2_fs_stat_fopen_calls;
extern unsigned int ps2_fs_stat_canonicalize_calls;
extern unsigned int ps2_fs_stat_dir_cache_hits;
extern unsigned int ps2_fs_stat_dir_scans;

void ps2_fs_stat_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* PS2_FS_H_ */
