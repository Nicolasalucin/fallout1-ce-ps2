/*
 * PS2 filesystem layer (F4): device probing + case-insensitive path
 * resolution + block buffering for large datafiles.
 *
 * See ps2_fs.h for the design notes.
 */

#include "ps2_fs.h"

#include "ps2_save.h"
#include "ps2_storage.h"
#include "ps2_time.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h> /* strncasecmp */
#include <unistd.h>

#define PS2_FS_MAX_ROOTS 12
#define PS2_FS_ROOT_MAX 40
#define PS2_FS_PATH_MAX 256
#define PS2_FS_DAT_BUFSIZE (64 * 1024)

/* F9 fallback chain (after the ELF-relative roots, which ps2_fs_init()
 * prepends dynamically): mass: -> mass0: -> cdrom0: -> hdd0: -> host:. */
static const char* const s_candidates[] = {
    "mass:/FALLOUT/",        /* USB stick / MX4SIO in a FALLOUT folder */
    "mass:/",                /* USB stick with the data at the root */
    "mass0:/FALLOUT/",       /* numbered mass device */
    "cdrom0:/",              /* CD/DVD root */
    "cdrom0:/FALLOUT/",      /* CD/DVD with the data in a FALLOUT folder */
    "hdd0:/FALLOUT/",        /* internal HDD (PFS partition) */
    "host:local_assets/",    /* user layout: ELF and local_assets together */
    "host:../local_assets/", /* dev layout: ELF in build_ps2/ under the repo */
    "host:/",                /* PCSX2 host filesystem / ps2client ELF dir */
};

static char s_roots[PS2_FS_MAX_ROOTS][PS2_FS_ROOT_MAX];
static int s_root_count;
static int s_probed;

#ifdef PS2_DEV_SMOKE_TEST
/* TEMP profiling (F5.2 map load): I/O call counters consumed by the smoke
 * harness. */
unsigned int ps2_fs_stat_fopen_calls;
unsigned int ps2_fs_stat_canonicalize_calls;
unsigned int ps2_fs_stat_dir_cache_hits;
unsigned int ps2_fs_stat_dir_scans;

void ps2_fs_stat_reset(void)
{
    ps2_fs_stat_fopen_calls = 0;
    ps2_fs_stat_canonicalize_calls = 0;
    ps2_fs_stat_dir_cache_hits = 0;
    ps2_fs_stat_dir_scans = 0;
}
#endif

/* opendir with a trailing-slash retry: device roots are accepted both as
 * "mass:" and "mass:/" depending on the driver. */
static DIR* fs_opendir(const char* dir)
{
    char tmp[PS2_FS_PATH_MAX];
    DIR* d;

    d = opendir(dir);
    if (d != NULL) {
        return d;
    }

    if (snprintf(tmp, sizeof(tmp), "%s/", dir) < (int)sizeof(tmp)) {
        d = opendir(tmp);
    }

    return d;
}

#ifdef PS2_DEV_SMOKE_TEST
/* The dialog tree is the largest directory the engine resolves during map
 * load and the one that intermittently wedges in PCSX2's host filesystem
 * HLE. Keep a per-entry trace here, but only for that tree, so a full boot
 * log stays readable. Fallout 1 media uses uppercase names, so match the
 * needle case-insensitively. */
static int fs_trace_dialog_dir(const char* dir)
{
    static const char needle[] = "data/text/english/dialog";
    const char* p;

    if (dir == NULL) {
        return 0;
    }

    for (p = dir; *p != '\0'; p++) {
        if (strncasecmp(p, needle, sizeof(needle) - 1) == 0) {
            return 1;
        }
    }

    return 0;
}

static struct dirent* fs_trace_readdir(DIR* d, const char* dir, int index)
{
    struct dirent* entry;

    if (!fs_trace_dialog_dir(dir)) {
        return readdir(d);
    }

    printf("[ps2] fs: dialog readdir #%d begin\n", index);
    entry = readdir(d);
    if (entry != NULL) {
        printf("[ps2] fs: dialog readdir #%d -> '%s' (type %d)\n",
            index, entry->d_name, (int)entry->d_type);
    } else {
        printf("[ps2] fs: dialog readdir #%d -> NULL\n", index);
    }

    return entry;
}
#else
#define fs_trace_readdir(d, dir, index) readdir(d)
#endif

/* --- Directory listing cache -------------------------------------------
 *
 * Resolving a path component through readdir() costs a dopen/readdir/close
 * RPC round trip to the IOP. The engine resolves thousands of art/proto
 * paths through several roots during boot, and the PCSX2 host filesystem HLE
 * is known to corrupt its SIF RPC bookkeeping under exactly that burst
 * (stray request completions, later seen as boot hangs/crashes).
 *
 * Keep recent listings in one fixed EE arena: a resolution costs no IOP call
 * until the directory is written to (save files) or the cached listing goes
 * stale. Large directories (ART/INTRFACE, the dialog tree) are cached too;
 * names are stored compactly and total memory is bounded by the arena.
 */
#define PS2_FS_DIR_CACHE_SLOTS 96
#define PS2_FS_DIR_CACHE_TTL_MS 30000
/* 320 KB of static arena. It lives in .bss, so every byte taken here is
 * taken from the engine heap, but the arena also decides how often the whole
 * listing cache is dropped: shrinking it too far trades a few KB of heap for
 * a lot of extra dopen/readdir RPC traffic. */
#define PS2_FS_DIR_CACHE_ARENA (320 * 1024)
#define PS2_FS_DIR_CACHE_NAME_MAX 255
/* Safety valve for a driver that never returns NULL from readdir(). A
 * legitimate listing (the dialog tree is the largest one in the game data)
 * is far below this limit. */
#define PS2_FS_DIR_CACHE_MAX_ENTRIES 4096

typedef struct {
    char path[PS2_FS_PATH_MAX];
    unsigned int stamp; /* ps2_time_ms() when the listing was taken */
    int valid;
    int count; /* entries in the listing */
    int offset; /* first entry, as an offset into the arena */
    int bytes; /* arena bytes used by the listing */
} fs_dir_cache_slot_t;

/* Listing entries sit one after another in the arena: one kind byte
 * (PS2_FS_KIND_*) followed by the NUL-terminated file name. */
static fs_dir_cache_slot_t s_dir_cache[PS2_FS_DIR_CACHE_SLOTS];
static unsigned char s_dir_arena[PS2_FS_DIR_CACHE_ARENA];
static int s_dir_arena_used;
static int s_dir_cache_next;

/* Call after anything changed a directory: new files (saves), removed files,
 * new mods. Cheap enough (a few flags) to call on every write open. The arena
 * is rewound as well: no directory iterator can be live across a file open
 * (canonicalization parses paths only). */
static void fs_dir_cache_invalidate(void)
{
    int i;

    for (i = 0; i < PS2_FS_DIR_CACHE_SLOTS; i++) {
        s_dir_cache[i].valid = 0;
    }

    s_dir_arena_used = 0;
}

/* Drops every cached listing on behalf of the layers that change directories
 * behind this module's back. The save-slot wipe uses this: it removes whole
 * slot directories, so there is nothing specific to keep. */
void ps2_fs_invalidate_dir_cache(void)
{
    fs_dir_cache_invalidate();
}

/* Slot keys keep the mount point spelling of the path they were built from
 * ("dev:" or "dev:/"), so directories are compared ignoring one trailing
 * separator. */
static int fs_path_same(const char* a, const char* b)
{
    size_t len_a = strlen(a);
    size_t len_b = strlen(b);

    while (len_a > 0 && a[len_a - 1] == '/') {
        len_a--;
    }
    while (len_b > 0 && b[len_b - 1] == '/') {
        len_b--;
    }

    return len_a == len_b && strncasecmp(a, b, len_a) == 0;
}

/* Drops the listing of `dir` only. A directory listing changes when a file
 * inside it appears, goes away or is renamed, and nothing else, so that is
 * the only thing the writers above this module need to invalidate. Flushing
 * the whole cache instead would send every following resolution back to the
 * IOP during a save. */
static void fs_dir_cache_invalidate_dir(const char* dir)
{
    int i;

    for (i = 0; i < PS2_FS_DIR_CACHE_SLOTS; i++) {
        fs_dir_cache_slot_t* slot = &s_dir_cache[i];

        if (slot->valid && fs_path_same(slot->path, dir)) {
            slot->valid = 0;
        }
    }
}

/* Invalidates the listing of the directory holding `resolved` (a canonical
 * device path such as "mc0:/FALLOUT1_S01/SAVE.DAT"). */
void ps2_fs_invalidate_parent_of(const char* resolved)
{
    const char* slash;
    char dir[PS2_FS_PATH_MAX];
    size_t len;

    if (resolved == NULL) {
        return;
    }

    slash = strrchr(resolved, '/');
    if (slash == NULL || slash == resolved) {
        return;
    }

    len = (size_t)(slash - resolved);
    if (len >= sizeof(dir)) {
        return;
    }

    memcpy(dir, resolved, len);
    dir[len] = '\0';

    fs_dir_cache_invalidate_dir(dir);
}

static unsigned char fs_dirent_kind(const struct dirent* entry)
{
    if (entry->d_type == DT_DIR) {
        return PS2_FS_KIND_DIR;
    }
    if (entry->d_type == DT_UNKNOWN) {
        return PS2_FS_KIND_UNKNOWN;
    }
    return PS2_FS_KIND_FILE;
}

static fs_dir_cache_slot_t* fs_dir_cache_lookup(const char* dir)
{
    unsigned int now = ps2_time_ms();
    int i;

    for (i = 0; i < PS2_FS_DIR_CACHE_SLOTS; i++) {
        fs_dir_cache_slot_t* slot = &s_dir_cache[i];

        if (!slot->valid || strcmp(slot->path, dir) != 0) {
            continue;
        }

        if ((unsigned int)(now - slot->stamp) > PS2_FS_DIR_CACHE_TTL_MS) {
            slot->valid = 0;
            return NULL;
        }

        return slot;
    }

    return NULL;
}

/* Lists `dir` into the next round-robin slot. Returns NULL when the directory
 * cannot be listed or does not fit in the arena (the caller then iterates it
 * directly). */
static fs_dir_cache_slot_t* fs_dir_cache_fill(const char* dir)
{
    fs_dir_cache_slot_t* slot;
    struct dirent* entry;
    DIR* d;
    int start;
    int count = 0;
    int index = 0;
#ifdef PS2_DEV_SMOKE_TEST
    int trace = 0;
#endif

    /* Reclaim the arena in one go once it is mostly used: listings are only
     * re-read on demand, so dropping all of them is cheap. */
    if (s_dir_arena_used > PS2_FS_DIR_CACHE_ARENA * 3 / 4) {
        fs_dir_cache_invalidate();
    }

    d = fs_opendir(dir);
    if (d == NULL) {
        return NULL;
    }

#ifdef PS2_DEV_SMOKE_TEST
    trace = fs_trace_dialog_dir(dir);
    if (trace) {
        printf("[ps2] fs: dialog dopen %s -> ok\n", dir);
    }
#endif

    start = s_dir_arena_used;

    while ((entry = fs_trace_readdir(d, dir, index)) != NULL) {
        size_t len;

        if (count >= PS2_FS_DIR_CACHE_MAX_ENTRIES) {
            count = -1;
            break;
        }

        len = strlen(entry->d_name) + 1;
        if (len > PS2_FS_DIR_CACHE_NAME_MAX
            || s_dir_arena_used + 1 + (int)len > PS2_FS_DIR_CACHE_ARENA) {
            count = -1;
            break;
        }

        s_dir_arena[s_dir_arena_used++] = fs_dirent_kind(entry);
        memcpy(&s_dir_arena[s_dir_arena_used], entry->d_name, len);
        s_dir_arena_used += (int)len;
        count++;
        index++;
    }

#ifdef PS2_DEV_SMOKE_TEST
    if (trace) {
        printf("[ps2] fs: dialog dclose %s after %d entries (count=%d)\n",
            dir, index, count);
    }
#endif
    closedir(d);
#ifdef PS2_DEV_SMOKE_TEST
    if (trace) {
        printf("[ps2] fs: dialog dclose %s done\n", dir);
    }
#endif

    if (count < 0) {
        s_dir_arena_used = start;
        return NULL;
    }

    slot = &s_dir_cache[s_dir_cache_next];
    s_dir_cache_next = (s_dir_cache_next + 1) % PS2_FS_DIR_CACHE_SLOTS;
    slot->valid = 0;
    snprintf(slot->path, sizeof(slot->path), "%s", dir);
    slot->count = count;
    slot->offset = start;
    slot->bytes = s_dir_arena_used - start;
    slot->stamp = ps2_time_ms();
    slot->valid = 1;

    return slot;
}

typedef struct {
    const fs_dir_cache_slot_t* slot; /* cached listing, when available */
    int index;
    int cursor; /* arena offset of the next entry (cached listings) */
    DIR* dir; /* direct iteration, when the directory is not cacheable */
    const char* dir_path; /* caller's path, for the diagnostic trace */
    int readdir_index;
} fs_dir_iter_t;

static int fs_dir_iter_begin(fs_dir_iter_t* it, const char* dir)
{
    const fs_dir_cache_slot_t* slot = fs_dir_cache_lookup(dir);

    it->slot = NULL;
    it->index = 0;
    it->cursor = 0;
    it->dir = NULL;
    it->dir_path = dir;
    it->readdir_index = 0;

    if (slot == NULL) {
#ifdef PS2_DEV_SMOKE_TEST
        ps2_fs_stat_dir_scans++;
#endif
        slot = fs_dir_cache_fill(dir);
    } else {
#ifdef PS2_DEV_SMOKE_TEST
        ps2_fs_stat_dir_cache_hits++;
#endif
    }

    if (slot != NULL) {
        it->slot = slot;
        it->cursor = slot->offset;
        return 0;
    }

    it->dir = fs_opendir(dir);
    if (it->dir == NULL) {
        return -1;
    }

    return 0;
}

/* Returns the next entry name and stores its kind, or NULL at the end. */
static const char* fs_dir_iter_next(fs_dir_iter_t* it, unsigned char* kind)
{
    if (it->slot != NULL) {
        const char* name;

        if (it->index >= it->slot->count) {
            return NULL;
        }

        *kind = s_dir_arena[it->cursor];
        name = (const char*)&s_dir_arena[it->cursor + 1];
        it->cursor += 1 + (int)strlen(name) + 1;
        it->index++;

        return name;
    }

    {
        struct dirent* entry;

        /* The direct fallback is used when the listing did not fit the cache
         * (or hit the runaway guard above); keep the same bound here so a
         * broken driver cannot trap the resolver in an endless scan. */
        if (it->readdir_index >= PS2_FS_DIR_CACHE_MAX_ENTRIES) {
            return NULL;
        }

        entry = fs_trace_readdir(it->dir, it->dir_path, it->readdir_index++);
        if (entry == NULL) {
            return NULL;
        }

        *kind = fs_dirent_kind(entry);
        return entry->d_name;
    }
}

static void fs_dir_iter_end(fs_dir_iter_t* it)
{
    if (it->dir != NULL) {
#ifdef PS2_DEV_SMOKE_TEST
        if (fs_trace_dialog_dir(it->dir_path)) {
            printf("[ps2] fs: dialog iter dclose %s\n", it->dir_path);
        }
#endif
        closedir(it->dir);
        it->dir = NULL;
    }
}

/* Copies `path` into `out` normalizing separators: backslashes to slashes,
 * leading "./" stripped. */
static void fs_normalize(const char* path, char* out, int out_size)
{
    int i = 0;

    while (*path == '.' && path[1] == '/') {
        path += 2;
    }

    while (*path != '\0' && i < out_size - 1) {
        char c = *path++;
        out[i++] = c == '\\' ? '/' : c;
    }

    out[i] = '\0';
}

/* A root is usable when both game datafiles open: the engine needs both, so
 * accepting a root with only MASTER.DAT would just move the failure deeper
 * into the boot. */
static void fs_validate_root(const char* root)
{
    static const char* const probes[] = { "MASTER.DAT", "CRITTER.DAT" };
    char probe[PS2_FS_PATH_MAX];
    int i;

    if (s_root_count >= PS2_FS_MAX_ROOTS) {
        return;
    }

    if (strlen(root) >= PS2_FS_ROOT_MAX) {
        printf("[ps2] fs: '%s' not available (root path too long)\n", root);
        return;
    }

    for (i = 0; i < (int)(sizeof(probes) / sizeof(probes[0])); i++) {
        int fd;

        if (snprintf(probe, sizeof(probe), "%s%s", root, probes[i]) >= (int)sizeof(probe)) {
            return;
        }

        fd = open(probe, O_RDONLY);
        if (fd < 0) {
            printf("[ps2] fs: '%s' not available (%s)\n", root, probes[i]);
            return;
        }
        close(fd);
    }

    snprintf(s_roots[s_root_count], PS2_FS_ROOT_MAX, "%s", root);
    s_root_count++;
    printf("[ps2] fs: data root %d = '%s'\n", s_root_count, root);
}

void ps2_fs_init(void)
{
    const char* elf_dir;
    int i;

    if (s_probed) {
        return;
    }
    s_probed = 1;

    printf("[ps2] fs: probing game data devices...\n");

    /* F9: the directory the ELF runs from comes first (media deployment),
     * then its local_assets/ subdirectory for the dev layouts. Both are
     * skipped when argv[0] carries no device path. */
    elf_dir = ps2_storage_elf_dir();
    if (elf_dir != NULL) {
        char elf_assets[PS2_FS_ROOT_MAX];

        fs_validate_root(elf_dir);
        if (snprintf(elf_assets, sizeof(elf_assets), "%slocal_assets/", elf_dir) < (int)sizeof(elf_assets)) {
            fs_validate_root(elf_assets);
        }
    }

    for (i = 0; i < (int)(sizeof(s_candidates) / sizeof(s_candidates[0])); i++) {
        fs_validate_root(s_candidates[i]);
    }

    if (s_root_count > 0) {
        printf("[ps2] storage: asset root detected at %s\n", s_roots[0]);
        return;
    }

    printf("[ps2] fs: WARNING no game data root found\n");

    /* Temporary F4 diagnostics: what does the PCSX2 host: root expose? */
    {
        DIR* d = opendir("host:");
        if (d == NULL) {
            d = opendir("host:/");
        }
        if (d == NULL) {
            printf("[ps2] fs: diag: opendir(host:) failed\n");
        } else {
            struct dirent* e;
            int n = 0;
            printf("[ps2] fs: diag: host: contains:");
            while ((e = readdir(d)) != NULL && n < 12) {
                printf(" [%s]", e->d_name);
                n++;
            }
            printf("\n");
            closedir(d);
        }
    }
}

int ps2_fs_root_count(void)
{
    ps2_fs_init();
    return s_root_count;
}

const char* ps2_fs_root_path(int index)
{
    if (index < 0 || index >= s_root_count) {
        return NULL;
    }
    return s_roots[index];
}

int ps2_fs_canonicalize(char* path, int allow_missing_last)
{
#ifdef PS2_DEV_SMOKE_TEST
    ps2_fs_stat_canonicalize_calls++;
#endif
    char dir[PS2_FS_PATH_MAX];
    const char* colon;
    const char* p;
    size_t dir_len;

    colon = strchr(path, ':');
    if (colon == NULL) {
        return -1;
    }

    /* Mount point: "dev:" or "dev:/" depending on the original path. */
    p = colon + 1;
    if (*p == '/') {
        p++;
        dir_len = (size_t)(p - path);
    } else {
        dir_len = (size_t)(colon + 1 - path);
    }
    if (dir_len >= sizeof(dir)) {
        return -1;
    }
    memcpy(dir, path, dir_len);
    dir[dir_len] = '\0';

    while (*p != '\0') {
        const char* sep = strchr(p, '/');
        size_t comp_len = sep != NULL ? (size_t)(sep - p) : strlen(p);
        char match[PS2_FS_PATH_MAX];
        int found = 0;
        int last = sep == NULL;
        fs_dir_iter_t it;
        const char* name;
        unsigned char kind;

        if (comp_len == 0) {
            p = sep + 1;
            continue;
        }

        if (comp_len == 1 && p[0] == '.') {
            p = last ? p + 1 : sep + 1;
            continue;
        }

        if (comp_len == 2 && p[0] == '.' && p[1] == '.') {
            /* Parent traversal: keep verbatim, no directory match. */
            if (dir_len + 4 > sizeof(dir)) {
                return -1;
            }
            if (dir[dir_len - 1] != '/') {
                dir[dir_len++] = '/';
            }
            dir[dir_len++] = '.';
            dir[dir_len++] = '.';
            dir[dir_len] = '\0';
            p = last ? p + comp_len : sep + 1;
            continue;
        }

        if (fs_dir_iter_begin(&it, dir) != 0) {
            return -1;
        }

        match[0] = '\0';
        while ((name = fs_dir_iter_next(&it, &kind)) != NULL) {
            if (strlen(name) == comp_len
                && strncasecmp(p, name, comp_len) == 0) {
                snprintf(match, sizeof(match), "%s", name);
                found = 1;
                break;
            }
        }
        fs_dir_iter_end(&it);

        /* Missing component: keep the requested spelling when the caller
         * allows creating the file, otherwise fail the resolution.
         *
         * Note: there is deliberately no direct ALL-CAPS probe open() here.
         * The readdir() scan above already resolves existing names
         * case-insensitively on every device type the port uses (FAT32,
         * ISO9660, host, PFS), so a blind uppercase open() could only add a
         * failed open for genuinely missing files -- and on PCSX2 a failing
         * open() corrupts the host filesystem HLE (SIF RPC state, the VM
         * crashes ~100 ms later). Bisection confirmed it (F9). The literal
         * uppercase probes in fs_validate_root() (MASTER.DAT / CRITTER.DAT)
         * cover the "try ALL CAPS" rule where it is safe and useful. */
        if (!found) {
            if (!(last && allow_missing_last)) {
                return -1;
            }
            /* Keep the requested spelling for not-yet-created files. */
            memcpy(match, p, comp_len);
            match[comp_len] = '\0';
        }

        if (dir_len + 1 + strlen(match) + 1 > sizeof(dir)) {
            return -1;
        }
        if (dir[dir_len - 1] != '/') {
            dir[dir_len++] = '/';
        }
        strcpy(dir + dir_len, match);
        dir_len += strlen(match);

        p = last ? p + comp_len : sep + 1;
    }

    if (dir_len + 1 > sizeof(dir)) {
        return -1;
    }
    strcpy(path, dir);

    return 0;
}

static FILE* fs_do_open(const char* path, const char* mode, int writable)
{
    FILE* f = fopen(path, mode);

    if (f != NULL && !writable) {
        size_t len = strlen(path);
        if (len > 4 && strcasecmp(path + len - 4, ".dat") == 0) {
            /* Block-read buffering for the big datafiles. */
            setvbuf(f, NULL, _IOFBF, PS2_FS_DAT_BUFSIZE);
        }
    }

    if (f != NULL && writable) {
        /* The listing of the directory this file lives in is stale now: it
         * was cached while resolving the path, before the file existed (a
         * save written and read back immediately). Every other listing stays
         * valid, so the next resolution does not have to scan it again. */
        ps2_fs_invalidate_parent_of(path);
    }

    return f;
}

int ps2_fs_resolve(const char* path, char* out, int out_size, int writable)
{
    char norm[PS2_FS_PATH_MAX];
    char candidate[PS2_FS_PATH_MAX];
    int i;

    ps2_fs_init();

    if (out_size < PS2_FS_PATH_MAX) {
        return -1;
    }

    fs_normalize(path, norm, sizeof(norm));
    if (norm[0] == '\0') {
        return -1;
    }

    /* F6: engine save slots (SAVEGAME\SLOTxx\...) are redirected to the save
     * device before any data-root probing. */
    {
        char remapped[PS2_FS_PATH_MAX];
        if (ps2_save_remap(norm, remapped, sizeof(remapped)) == 0) {
            if (ps2_fs_canonicalize(remapped, writable) != 0) {
                return -1;
            }
            strcpy(out, remapped);
            return 0;
        }
    }

    if (strchr(norm, ':') != NULL) {
        if (ps2_fs_canonicalize(norm, writable) != 0) {
            return -1;
        }
        strcpy(out, norm);
        return 0;
    }

    for (i = 0; i < s_root_count; i++) {
        snprintf(candidate, sizeof(candidate), "%s%s", s_roots[i], norm);
        if (ps2_fs_canonicalize(candidate, writable) != 0) {
            continue;
        }
        strcpy(out, candidate);
        return 0;
    }

    return -1;
}

FILE* ps2_fs_fopen(const char* path, const char* mode)
{
    char norm[PS2_FS_PATH_MAX];
    char candidate[PS2_FS_PATH_MAX];
    int writable;
    int i;

    ps2_fs_init();

    if (path == NULL || mode == NULL) {
        return NULL;
    }

    fs_normalize(path, norm, sizeof(norm));
    if (norm[0] == '\0') {
        return NULL;
    }

    writable = strchr(mode, 'w') != NULL
        || strchr(mode, 'a') != NULL
        || strchr(mode, '+') != NULL;

#ifdef PS2_DEV_SMOKE_TEST
    ps2_fs_stat_fopen_calls++;
#endif

    /* F6: save-slot files live on the save device, not in the data roots. */
    {
        char remapped[PS2_FS_PATH_MAX];
        if (ps2_save_remap(norm, remapped, sizeof(remapped)) == 0) {
            if (ps2_fs_canonicalize(remapped, writable) != 0) {
                return NULL;
            }
            return fs_do_open(remapped, mode, writable);
        }
    }

    if (strchr(norm, ':') != NULL) {
        if (ps2_fs_canonicalize(norm, writable) != 0) {
            return NULL;
        }
        return fs_do_open(norm, mode, writable);
    }

    for (i = 0; i < s_root_count; i++) {
        FILE* f;

        snprintf(candidate, sizeof(candidate), "%s%s", s_roots[i], norm);
        if (ps2_fs_canonicalize(candidate, writable) != 0) {
            continue;
        }

        f = fs_do_open(candidate, mode, writable);
        if (f != NULL) {
            return f;
        }
    }

    return NULL;
}
