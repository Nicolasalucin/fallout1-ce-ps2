/*
 * PS2 filesystem layer (F4): device probing + case-insensitive path
 * resolution + block buffering for large datafiles.
 *
 * See ps2_fs.h for the design notes.
 */

#include "ps2_fs.h"

#include "ps2_save.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h> /* strncasecmp */
#include <unistd.h>

#define PS2_FS_MAX_ROOTS 4
#define PS2_FS_ROOT_MAX 40
#define PS2_FS_PATH_MAX 256
#define PS2_FS_DAT_BUFSIZE (64 * 1024)

static const char* const s_candidates[PS2_FS_MAX_ROOTS] = {
    "host:local_assets/",  /* user layout: ELF and local_assets together */
    "host:../local_assets/", /* dev layout: ELF in build_ps2/ under the repo */
    "mass:/FALLOUT/",      /* USB stick / MX4SIO */
    "hdd0:/FALLOUT/",      /* internal HDD (PFS partition) */
};

static char s_roots[PS2_FS_MAX_ROOTS][PS2_FS_ROOT_MAX];
static int s_root_count;
static int s_probed;

#ifdef PS2_DEV_SMOKE_TEST
/* TEMP profiling (F5.2 map load): I/O call counters consumed by the smoke
 * harness. */
unsigned int ps2_fs_stat_fopen_calls;
unsigned int ps2_fs_stat_canonicalize_calls;

void ps2_fs_stat_reset(void)
{
    ps2_fs_stat_fopen_calls = 0;
    ps2_fs_stat_canonicalize_calls = 0;
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

static void fs_validate_root(const char* root)
{
    char probe[PS2_FS_PATH_MAX];
    int fd;

    if (s_root_count >= PS2_FS_MAX_ROOTS) {
        return;
    }

    if (snprintf(probe, sizeof(probe), "%sMASTER.DAT", root) >= (int)sizeof(probe)) {
        return;
    }

    fd = open(probe, O_RDONLY);
    if (fd < 0) {
        printf("[ps2] fs: '%s' not available\n", root);
        return;
    }
    close(fd);

    snprintf(s_roots[s_root_count], PS2_FS_ROOT_MAX, "%s", root);
    s_root_count++;
    printf("[ps2] fs: data root %d = '%s'\n", s_root_count, root);
}

void ps2_fs_init(void)
{
    int i;

    if (s_probed) {
        return;
    }
    s_probed = 1;

    printf("[ps2] fs: probing game data devices...\n");
    for (i = 0; i < PS2_FS_MAX_ROOTS; i++) {
        fs_validate_root(s_candidates[i]);
    }

    if (s_root_count == 0) {
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
        DIR* d;
        struct dirent* entry;

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

        d = fs_opendir(dir);
        if (d == NULL) {
            return -1;
        }

        match[0] = '\0';
        while ((entry = readdir(d)) != NULL) {
            if (strlen(entry->d_name) == comp_len
                && strncasecmp(p, entry->d_name, comp_len) == 0) {
                snprintf(match, sizeof(match), "%s", entry->d_name);
                found = 1;
                break;
            }
        }
        closedir(d);

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
