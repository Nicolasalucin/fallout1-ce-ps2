/*
 * PlayStation 2 implementation of the engine's platform compatibility layer.
 *
 * This file replaces src/platform_compat.cc in the PS2 build (the upstream
 * file is excluded by CMake while PS2=TRUE). It keeps the exact same API and
 * semantics, but resolves engine paths against the PS2 storage devices
 * through src/platform/ps2/fs.c instead of a POSIX working directory.
 */

#include "platform_compat.h"

#include <string.h>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL.h>

#include "ps2_fs.h"
#include "ps2_save.h"
#include "ps2_time.h"

#include "game/combat.h"

namespace fallout {

int compat_stricmp(const char* string1, const char* string2)
{
    return SDL_strcasecmp(string1, string2);
}

int compat_strnicmp(const char* string1, const char* string2, size_t size)
{
    return SDL_strncasecmp(string1, string2, size);
}

char* compat_strupr(char* string)
{
    return SDL_strupr(string);
}

char* compat_strlwr(char* string)
{
    return SDL_strlwr(string);
}

char* compat_itoa(int value, char* buffer, int radix)
{
    return SDL_itoa(value, buffer, radix);
}

void compat_splitpath(const char* path, char* drive, char* dir, char* fname, char* ext)
{
    const char* driveStart = path;
    if (path[0] == '/' && path[1] == '/') {
        path += 2;
        while (*path != '\0' && *path != '/' && *path != '.') {
            path++;
        }
    }

    if (drive != NULL) {
        size_t driveSize = path - driveStart;
        if (driveSize > COMPAT_MAX_DRIVE - 1) {
            driveSize = COMPAT_MAX_DRIVE - 1;
        }
        strncpy(drive, path, driveSize);
        drive[driveSize] = '\0';
    }

    const char* dirStart = path;
    const char* fnameStart = path;
    const char* extStart = NULL;

    const char* end = path;
    while (*end != '\0') {
        if (*end == '/') {
            fnameStart = end + 1;
        } else if (*end == '.') {
            extStart = end;
        }
        end++;
    }

    if (extStart == NULL) {
        extStart = end;
    }

    if (dir != NULL) {
        size_t dirSize = fnameStart - dirStart;
        if (dirSize > COMPAT_MAX_DIR - 1) {
            dirSize = COMPAT_MAX_DIR - 1;
        }
        strncpy(dir, path, dirSize);
        dir[dirSize] = '\0';
    }

    if (fname != NULL) {
        size_t fileNameSize = extStart - fnameStart;
        if (fileNameSize > COMPAT_MAX_FNAME - 1) {
            fileNameSize = COMPAT_MAX_FNAME - 1;
        }
        strncpy(fname, fnameStart, fileNameSize);
        fname[fileNameSize] = '\0';
    }

    if (ext != NULL) {
        size_t extSize = end - extStart;
        if (extSize > COMPAT_MAX_EXT - 1) {
            extSize = COMPAT_MAX_EXT - 1;
        }
        strncpy(ext, extStart, extSize);
        ext[extSize] = '\0';
    }
}

void compat_makepath(char* path, const char* drive, const char* dir, const char* fname, const char* ext)
{
    path[0] = '\0';

    if (drive != NULL) {
        if (*drive != '\0') {
            strcpy(path, drive);
            path = strchr(path, '\0');

            if (path[-1] == '/') {
                path--;
            } else {
                *path = '/';
            }
        }
    }

    if (dir != NULL) {
        if (*dir != '\0') {
            if (*dir != '/' && *path == '/') {
                path++;
            }

            strcpy(path, dir);
            path = strchr(path, '\0');

            if (path[-1] == '/') {
                path--;
            } else {
                *path = '/';
            }
        }
    }

    if (fname != NULL && *fname != '\0') {
        if (*fname != '/' && *path == '/') {
            path++;
        }

        strcpy(path, fname);
        path = strchr(path, '\0');
    } else {
        if (*path == '/') {
            path++;
        }
    }

    if (ext != NULL) {
        if (*ext != '\0') {
            if (*ext != '.') {
                *path++ = '.';
            }

            strcpy(path, ext);
            path = strchr(path, '\0');
        }
    }

    *path = '\0';
}

int compat_read(int fileHandle, void* buf, unsigned int size)
{
    return read(fileHandle, buf, size);
}

int compat_write(int fileHandle, const void* buf, unsigned int size)
{
    return write(fileHandle, buf, size);
}

long compat_lseek(int fileHandle, long offset, int origin)
{
    return lseek(fileHandle, offset, origin);
}

long compat_tell(int fd)
{
    return lseek(fd, 0, SEEK_CUR);
}

long compat_filelength(int fd)
{
    long originalOffset = lseek(fd, 0, SEEK_CUR);
    lseek(fd, 0, SEEK_SET);
    long filesize = lseek(fd, 0, SEEK_END);
    lseek(fd, originalOffset, SEEK_SET);
    return filesize;
}

int compat_mkdir(const char* path)
{
    char resolved[COMPAT_MAX_PATH];
    if (ps2_fs_resolve(path, resolved, sizeof(resolved), 1) != 0) {
        return -1;
    }

    int rc = mkdir(resolved, 0755);

    /* F6: creating a save-slot directory materializes the PS2 browser
     * metadata (icon.sys + icon.icn) inside it. */
    ps2_save_slot_ensure_meta(resolved);

    /* The parent listing was cached while resolving [path], before the
     * directory existed. */
    if (rc == 0) {
        ps2_fs_invalidate_parent_of(resolved);
    }

    return rc;
}

unsigned int compat_timeGetTime(void)
{
    /* EE system timer in milliseconds (ps2_time.c). */
    return (unsigned int)ps2_time_ms();
}

FILE* compat_fopen(const char* path, const char* mode)
{
    return ps2_fs_fopen(path, mode);
}

int compat_remove(const char* path)
{
    char resolved[COMPAT_MAX_PATH];
    if (ps2_fs_resolve(path, resolved, sizeof(resolved), 0) != 0) {
        return -1;
    }

    /* ps2sdk's fileio.h documents fioRemove as broken: its RPC dispatch
     * lacks a break and falls through into "mkdir", so every successful
     * remove() leaves an empty directory with the same name. Remove that
     * phantom as well; rmdir() fails harmlessly when the path is gone. */
    int rc = remove(resolved);
    int rmdirRc = rmdir(resolved);
    if (rc == 0 || rmdirRc == 0) {
        /* The parent listing still shows the erased name otherwise. */
        ps2_fs_invalidate_parent_of(resolved);
    }
    return rc;
}

/* Copy fallback for compat_rename().
 *
 * ps2sdk's fileio backend registers no rename() handler, so the C library
 * fails the call locally with ENOSYS before issuing any RPC (no rename ever
 * reaches the emulator log). Fallout 1 uses rename() for every save backup
 * (.SAV/.DAT -> .BAK), so copy the bytes and delete the source instead. */
static int compat_rename_copy(const char* resolvedOld, const char* resolvedNew)
{
    /* 16 KiB static buffer: large enough to amortize the SIF RPC round
     * trips, small enough to keep off the EE main-thread stack. */
    static char buffer[16 * 1024];

    FILE* source = fopen(resolvedOld, "rb");
    if (source == nullptr) {
        printf("[ps2] fs: rename fallback: cannot open source %s\n", resolvedOld);
        return -1;
    }

    FILE* dest = fopen(resolvedNew, "wb");
    if (dest == nullptr) {
        fclose(source);
        printf("[ps2] fs: rename fallback: cannot create %s\n", resolvedNew);
        return -1;
    }

    int rc = 0;
    for (;;) {
        size_t readCount = fread(buffer, 1, sizeof(buffer), source);
        if (readCount > 0 && fwrite(buffer, 1, readCount, dest) != readCount) {
            rc = -1;
            break;
        }
        if (readCount < sizeof(buffer)) {
            if (ferror(source)) {
                rc = -1;
            }
            break;
        }
    }

    /* Close the destination before touching the source: the delete below
     * must not race the last sector flush. */
    if (fclose(dest) != 0) {
        rc = -1;
    }
    fclose(source);

    if (rc == 0) {
        /* compat_remove() also clears the phantom directory the broken
         * fioRemove leaves behind and drops the parent listing. */
        rc = compat_remove(resolvedOld);
        if (rc != 0) {
            /* Source still in place: the rename did not happen, so drop the
             * copy to keep the tree all-or-nothing. */
            remove(resolvedNew);
            rmdir(resolvedNew);
        }
    } else {
        /* Do not leave a half-written copy behind. */
        remove(resolvedNew);
        rmdir(resolvedNew);
    }

    if (rc != 0) {
        printf("[ps2] fs: rename fallback failed: %s -> %s\n", resolvedOld, resolvedNew);
    }

    return rc;
}

int compat_rename(const char* oldFileName, const char* newFileName)
{
    char resolvedOld[COMPAT_MAX_PATH];
    if (ps2_fs_resolve(oldFileName, resolvedOld, sizeof(resolvedOld), 0) != 0) {
        return -1;
    }

    char resolvedNew[COMPAT_MAX_PATH];
    if (ps2_fs_resolve(newFileName, resolvedNew, sizeof(resolvedNew), 1) != 0) {
        return -1;
    }

    /* Renaming a file onto itself is a POSIX no-op, and the copy fallback
     * must not touch the file in that case. */
    if (strcmp(resolvedOld, resolvedNew) == 0) {
        return 0;
    }

    int rc = rename(resolvedOld, resolvedNew);

    if (rc != 0) {
        rc = compat_rename_copy(resolvedOld, resolvedNew);
    }

    if (rc == 0) {
        /* Both parent listings changed. */
        ps2_fs_invalidate_parent_of(resolvedOld);
        ps2_fs_invalidate_parent_of(resolvedNew);
    }

    return rc;
}

void compat_windows_path_to_native(char* path)
{
    char* pch = path;
    while (*pch != '\0') {
        if (*pch == '\\') {
            *pch = '/';
        }
        pch++;
    }
}

void compat_resolve_path(char* path)
{
    /* F6: engine save directories (SAVEGAME\SLOTxx\...) are enumerated on
     * the save device; the engine never sees the redirected location. */
    {
        char remapped[COMPAT_MAX_PATH];
        if (ps2_save_remap(path, remapped, sizeof(remapped)) == 0) {
            if (ps2_fs_canonicalize(remapped, 0) == 0) {
                strcpy(path, remapped);
            }
            return;
        }
    }

    /* Best effort: canonicalize in place. Device paths resolve directly;
     * relative paths are tried against every data root. */
    if (strchr(path, ':') != NULL) {
        ps2_fs_canonicalize(path, 0);
        return;
    }

    int roots = ps2_fs_root_count();
    for (int i = 0; i < roots; i++) {
        char candidate[COMPAT_MAX_PATH];
        const char* root = ps2_fs_root_path(i);
        if (root == NULL) {
            continue;
        }
        if (snprintf(candidate, sizeof(candidate), "%s%s", root, path) >= (int)sizeof(candidate)) {
            continue;
        }
        if (ps2_fs_canonicalize(candidate, 0) == 0) {
            strcpy(path, candidate);
            return;
        }
    }
}

char* compat_strdup(const char* string)
{
    return SDL_strdup(string);
}

// It's a replacement for compat_filelength(fileno(stream)) on platforms without
// fileno defined.
long getFileSize(FILE* stream)
{
    long originalOffset = ftell(stream);
    fseek(stream, 0, SEEK_END);
    long filesize = ftell(stream);
    fseek(stream, originalOffset, SEEK_SET);
    return filesize;
}

// F8: bridge for the context-sensitive R1 mapping (input_pad.c decides
// between attack and end-combat attempt). C linkage so the plain-C
// platform layer can call it; reads a global, safe before boot.
extern "C" int ps2_platform_in_combat(void)
{
    return isInCombat() ? 1 : 0;
}

} // namespace fallout
