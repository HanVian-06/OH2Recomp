/*
 * stat_wrapper.c — Darwin 144-byte struct stat and 21-byte-header struct dirent64
 * translation with bundle path remapping and case-insensitive lookup.
 */

#include "libc_wrapper.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void bionic_to_darwin_stat(const struct stat *src, DarwinStat64 *dst) {
    if (!src || !dst) return;
    memset(dst, 0, sizeof(*dst));
    dst->st_dev           = (int32_t)src->st_dev;
    dst->st_mode          = (uint16_t)src->st_mode;
    dst->st_nlink         = (uint16_t)src->st_nlink;
    dst->st_ino           = (uint64_t)src->st_ino;
    dst->st_uid           = (uint32_t)src->st_uid;
    dst->st_gid           = (uint32_t)src->st_gid;
    dst->st_rdev          = (int32_t)src->st_rdev;
    dst->st_atimespec     = src->st_atim;
    dst->st_mtimespec     = src->st_mtim;
    dst->st_ctimespec     = src->st_ctim;
    dst->st_birthtimespec = src->st_ctim;
    dst->st_size          = (int64_t)src->st_size;
    dst->st_blocks        = (int64_t)src->st_blocks;
    dst->st_blksize       = (int32_t)src->st_blksize;
}

int oh2_darwin_stat(const char *path, DarwinStat64 *d_st) {
    if (!path || !d_st) {
        errno = EINVAL;
        oh2_sync_darwin_errno();
        return -1;
    }
    char translated[1024];
    oh2_libc_wrapper_translate_path(path, translated, sizeof(translated));

    struct stat b_st;
    if (stat(translated, &b_st) != 0) {
        oh2_sync_darwin_errno();
        return -1;
    }
    bionic_to_darwin_stat(&b_st, d_st);
    return 0;
}

int oh2_darwin_lstat(const char *path, DarwinStat64 *d_st) {
    if (!path || !d_st) {
        errno = EINVAL;
        oh2_sync_darwin_errno();
        return -1;
    }
    char translated[1024];
    oh2_libc_wrapper_translate_path(path, translated, sizeof(translated));

    struct stat b_st;
    if (lstat(translated, &b_st) != 0) {
        oh2_sync_darwin_errno();
        return -1;
    }
    bionic_to_darwin_stat(&b_st, d_st);
    return 0;
}

int oh2_darwin_fstat(int fd, DarwinStat64 *d_st) {
    if (!d_st) {
        errno = EINVAL;
        oh2_sync_darwin_errno();
        return -1;
    }
    struct stat b_st;
    if (fstat(fd, &b_st) != 0) {
        oh2_sync_darwin_errno();
        return -1;
    }
    bionic_to_darwin_stat(&b_st, d_st);
    oh2_pak_telemetry_record_fstat(fd, (uint64_t)b_st.st_size);
    return 0;
}

/* Per-stream wrapped DIR holding a thread-local-safe DarwinDirent64 conversion buffer */
typedef struct {
    DIR            *real_dir;
    DarwinDirent64  darwin_entry;
} Oh2DarwinDir;

DIR *oh2_darwin_opendir(const char *path) {
    if (!path) {
        errno = EINVAL;
        oh2_sync_darwin_errno();
        return NULL;
    }
    char translated[1024];
    oh2_libc_wrapper_translate_path(path, translated, sizeof(translated));

    DIR *d = opendir(translated);
    if (!d) {
        oh2_sync_darwin_errno();
        return NULL;
    }
    Oh2DarwinDir *wd = (Oh2DarwinDir *)calloc(1, sizeof(Oh2DarwinDir));
    if (!wd) {
        closedir(d);
        errno = ENOMEM;
        oh2_sync_darwin_errno();
        return NULL;
    }
    wd->real_dir = d;
    return (DIR *)wd;
}

DarwinDirent64 *oh2_darwin_readdir(DIR *dirp) {
    if (!dirp) {
        errno = EBADF;
        oh2_sync_darwin_errno();
        return NULL;
    }
    Oh2DarwinDir *wd = (Oh2DarwinDir *)dirp;
    struct dirent *b_ent = readdir(wd->real_dir);
    if (!b_ent) {
        oh2_sync_darwin_errno();
        return NULL;
    }

    size_t nlen = strlen(b_ent->d_name);
    if (nlen >= sizeof(wd->darwin_entry.d_name)) {
        nlen = sizeof(wd->darwin_entry.d_name) - 1;
    }

    wd->darwin_entry.d_ino     = (uint64_t)b_ent->d_ino;
    wd->darwin_entry.d_seekoff = (uint64_t)b_ent->d_off;
    wd->darwin_entry.d_namlen  = (uint16_t)nlen;
    wd->darwin_entry.d_type    = (uint8_t)b_ent->d_type;
    wd->darwin_entry.d_reclen  = (uint16_t)((21U + nlen + 1U + 7U) & ~7U);
    memcpy(wd->darwin_entry.d_name, b_ent->d_name, nlen);
    wd->darwin_entry.d_name[nlen] = '\0';

    return &wd->darwin_entry;
}

int oh2_darwin_closedir(DIR *dirp) {
    if (!dirp) return 0;
    Oh2DarwinDir *wd = (Oh2DarwinDir *)dirp;
    int rc = closedir(wd->real_dir);
    if (rc != 0) oh2_sync_darwin_errno();
    free(wd);
    return rc;
}
