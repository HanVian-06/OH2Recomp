/*
 * open_wrapper.c — Darwin open/fopen/lseek/read/pread/mmap wrappers,
 * bundle path translation, multi-component case-insensitive lookup, and
 * live oh2-ios.pak I/O telemetry.
 */

#include "libc_wrapper.h"

#include <android/log.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

static char g_bundle_root[512] = "/sdcard/Android/data/com.oceanhorn2.port/files";
static pthread_mutex_t g_pak_io_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    int      in_use;
    int      fd;
    uint64_t pos;
    uint64_t size;
} TrackedPakFd;

static TrackedPakFd g_tracked_fds[16];
static Oh2PakIoTelemetry g_pak_telemetry;
static void *g_pak_ro_mmap = NULL;
static size_t g_pak_ro_mmap_size = 0;

static int is_oh2_pak_path(const char *path) {
    if (!path) return 0;
    size_t len = strlen(path);
    const char *needle = "oh2-ios.pak";
    size_t nlen = strlen(needle);
    if (len < nlen) return 0;
    return strcasecmp(path + len - nlen, needle) == 0;
}

static void record_pak_span_locked(uint64_t offset, uint64_t len) {
    if (len == 0) return;
    uint64_t end = offset + len;
    g_pak_telemetry.total_bytes_read += len;

    /* Footer bounds: [2430963402, 2430963623) */
    if (offset <= OH2_PAK_FOOTER_OFFSET && end >= OH2_PAK_EXPECTED_SIZE) {
        g_pak_telemetry.footer_read = 1;
    } else if (end > OH2_PAK_FOOTER_OFFSET && offset < OH2_PAK_EXPECTED_SIZE) {
        g_pak_telemetry.footer_read = 1;
    }

    /* Index bounds: [2427412912, 2430963402) (3,550,490 bytes) */
    uint64_t ov_start = (offset > OH2_PAK_INDEX_OFFSET) ? offset : OH2_PAK_INDEX_OFFSET;
    uint64_t ov_end   = (end < OH2_PAK_INDEX_END) ? end : OH2_PAK_INDEX_END;
    if (ov_end > ov_start) {
        g_pak_telemetry.index_bytes_read += (ov_end - ov_start);
        if (g_pak_telemetry.index_bytes_read >= OH2_PAK_INDEX_SIZE) {
            g_pak_telemetry.index_read_complete = 1;
        }
    }

    /* Asset reads inside pak data region after index read */
    if (offset < OH2_PAK_INDEX_OFFSET && g_pak_telemetry.index_read_complete) {
        g_pak_telemetry.asset_reads_after_index++;
    }
}

void oh2_pak_telemetry_reset(void) {
    pthread_mutex_lock(&g_pak_io_lock);
    memset(&g_pak_telemetry, 0, sizeof(g_pak_telemetry));
    memset(g_tracked_fds, 0, sizeof(g_tracked_fds));
    pthread_mutex_unlock(&g_pak_io_lock);
}

void oh2_pak_telemetry_get(Oh2PakIoTelemetry *out) {
    if (!out) return;
    pthread_mutex_lock(&g_pak_io_lock);
    memcpy(out, &g_pak_telemetry, sizeof(*out));
    pthread_mutex_unlock(&g_pak_io_lock);
}

void oh2_pak_telemetry_track_fd_open(int fd, const char *resolved_path) {
    if (fd < 0 || !is_oh2_pak_path(resolved_path)) return;
    struct stat st;
    uint64_t sz = (fstat(fd, &st) == 0) ? (uint64_t)st.st_size : 0ULL;

    pthread_mutex_lock(&g_pak_io_lock);
    g_pak_telemetry.opened_by_ue4 = 1;
    snprintf(g_pak_telemetry.opened_path, sizeof(g_pak_telemetry.opened_path), "%s", resolved_path);
    if (sz > 0) {
        g_pak_telemetry.fstat_size = sz;
        if (!g_pak_ro_mmap) {
            void *mp = mmap(NULL, (size_t)sz, PROT_READ, MAP_PRIVATE, fd, 0);
            if (mp != MAP_FAILED) {
                g_pak_ro_mmap = mp;
                g_pak_ro_mmap_size = (size_t)sz;
            }
        }
    }
    for (int i = 0; i < 16; ++i) {
        if (!g_tracked_fds[i].in_use || g_tracked_fds[i].fd == fd) {
            g_tracked_fds[i].in_use = 1;
            g_tracked_fds[i].fd     = fd;
            g_tracked_fds[i].pos    = 0;
            g_tracked_fds[i].size   = sz;
            break;
        }
    }
    pthread_mutex_unlock(&g_pak_io_lock);
}

void oh2_pak_telemetry_track_fd_close(int fd) {
    if (fd < 0) return;
    pthread_mutex_lock(&g_pak_io_lock);
    for (int i = 0; i < 16; ++i) {
        if (g_tracked_fds[i].in_use && g_tracked_fds[i].fd == fd) {
            g_tracked_fds[i].in_use = 0;
            g_tracked_fds[i].fd     = -1;
        }
    }
    pthread_mutex_unlock(&g_pak_io_lock);
}

void oh2_pak_telemetry_record_fstat(int fd, uint64_t size) {
    if (fd < 0) return;
    pthread_mutex_lock(&g_pak_io_lock);
    for (int i = 0; i < 16; ++i) {
        if (g_tracked_fds[i].in_use && g_tracked_fds[i].fd == fd) {
            g_pak_telemetry.fstat_size = size;
            g_tracked_fds[i].size = size;
            break;
        }
    }
    pthread_mutex_unlock(&g_pak_io_lock);
}

void oh2_pak_telemetry_record_mmap(int fd, uint64_t offset, size_t len) {
    if (fd < 0) return;
    pthread_mutex_lock(&g_pak_io_lock);
    for (int i = 0; i < 16; ++i) {
        if (g_tracked_fds[i].in_use && g_tracked_fds[i].fd == fd) {
            g_pak_telemetry.mmap_count++;
            record_pak_span_locked(offset, (uint64_t)len);
            break;
        }
    }
    pthread_mutex_unlock(&g_pak_io_lock);
}

static TrackedPakFd *find_tracked_fd_locked(int fd) {
    for (int i = 0; i < 16; ++i) {
        if (g_tracked_fds[i].in_use && g_tracked_fds[i].fd == fd) {
            return &g_tracked_fds[i];
        }
    }
    return NULL;
}

void oh2_libc_wrapper_set_bundle_root(const char *root) {
    if (root && root[0]) {
        snprintf(g_bundle_root, sizeof(g_bundle_root), "%s", root);
        size_t len = strlen(g_bundle_root);
        while (len > 1 && g_bundle_root[len - 1] == '/') {
            g_bundle_root[--len] = '\0';
        }
    }
}

const char *oh2_libc_wrapper_get_bundle_root(void) {
    return g_bundle_root;
}

static void resolve_case_insensitive_path(const char *candidate, char *out_buf, size_t out_sz) {
    if (!candidate || !out_buf || out_sz == 0) return;
    struct stat st;
    if (stat(candidate, &st) == 0 || candidate[0] != '/') {
        snprintf(out_buf, out_sz, "%s", candidate);
        return;
    }

    char cur[1024];
    cur[0] = '\0';

    const char *p = candidate;
    while (*p == '/') p++;

    while (*p) {
        const char *slash = strchr(p, '/');
        size_t comp_len = slash ? (size_t)(slash - p) : strlen(p);
        char comp[256];
        if (comp_len >= sizeof(comp)) comp_len = sizeof(comp) - 1;
        memcpy(comp, p, comp_len);
        comp[comp_len] = '\0';

        if (comp_len > 0 && strcmp(comp, ".") != 0) {
            char next_try[1024];
            snprintf(next_try, sizeof(next_try), "%s/%s", cur[0] ? cur : "", comp);
            if (stat(next_try, &st) == 0) {
                snprintf(cur, sizeof(cur), "%s", next_try);
            } else {
                const char *dir_path = cur[0] ? cur : "/";
                DIR *d = opendir(dir_path);
                int found = 0;
                if (d) {
                    struct dirent *ent;
                    while ((ent = readdir(d)) != NULL) {
                        if (strcasecmp(ent->d_name, comp) == 0) {
                            snprintf(cur, sizeof(cur), "%s/%s",
                                     strcmp(dir_path, "/") == 0 ? "" : dir_path,
                                     ent->d_name);
                            found = 1;
                            break;
                        }
                    }
                    closedir(d);
                }
                if (!found) {
                    snprintf(cur, sizeof(cur), "%s/%s", cur[0] ? cur : "", comp);
                }
            }
        }

        if (!slash) break;
        p = slash + 1;
        while (*p == '/') p++;
    }

    snprintf(out_buf, out_sz, "%s", cur[0] ? cur : candidate);
}

int oh2_libc_wrapper_translate_path(const char *darwin_path, char *out_buf, size_t out_sz) {
    if (!darwin_path || !out_buf || out_sz == 0) return -1;

    char rewritten[1024];
    const char *sub = NULL;

    if (strncmp(darwin_path, "@executable_path/", 17) == 0) {
        sub = darwin_path + 17;
    } else if (strcmp(darwin_path, "@executable_path") == 0) {
        sub = "";
    } else if (strncmp(darwin_path, "/data/local/tmp/OH2.app/", 24) == 0) {
        sub = darwin_path + 24;
    } else if (strcmp(darwin_path, "/data/local/tmp/OH2.app") == 0) {
        sub = "";
    } else if (strncmp(darwin_path, "/private/var/containers/Bundle/Application/", 43) == 0) {
        const char *after_uuid = strchr(darwin_path + 43, '/');
        if (after_uuid) {
            const char *after_app = strchr(after_uuid + 1, '/');
            sub = after_app ? (after_app + 1) : "";
        }
    } else if (strncmp(darwin_path, "/var/containers/Bundle/Application/", 35) == 0) {
        const char *after_uuid = strchr(darwin_path + 35, '/');
        if (after_uuid) {
            const char *after_app = strchr(after_uuid + 1, '/');
            sub = after_app ? (after_app + 1) : "";
        }
    } else if (strncmp(darwin_path, "/private/var/mobile/", 20) == 0) {
        sub = darwin_path + 20;
    } else if (strncmp(darwin_path, "/var/mobile/", 12) == 0) {
        sub = darwin_path + 12;
    }

    /* Handle UE4 relative ../../../ or ./cookeddata/... paths resolved under bundle root */
    if (!sub) {
        const char *rel = darwin_path;
        while (strncmp(rel, "../", 3) == 0) rel += 3;
        while (strncmp(rel, "./", 2) == 0) rel += 2;
        if (strncasecmp(rel, "OH2/", 4) == 0 || strncasecmp(rel, "Engine/", 7) == 0 ||
            strncasecmp(rel, "cookeddata/", 11) == 0) {
            if (strncasecmp(rel, "OH2/", 4) == 0 &&
                strncasecmp(rel + 4, "Saved/", 6) != 0 &&
                strncasecmp(rel + 4, "Content/", 8) == 0) {
                snprintf(rewritten, sizeof(rewritten), "%s/cookeddata/%s", g_bundle_root, rel);
            } else if (strncasecmp(rel, "cookeddata/", 11) == 0 ||
                       strncasecmp(rel, "Engine/", 7) == 0) {
                snprintf(rewritten, sizeof(rewritten), "%s/%s", g_bundle_root, rel);
            } else {
                snprintf(rewritten, sizeof(rewritten), "%s/%s", g_bundle_root, rel);
            }
            resolve_case_insensitive_path(rewritten, out_buf, out_sz);
            return 0;
        }
    }

    if (sub) {
        if (sub[0]) {
            snprintf(rewritten, sizeof(rewritten), "%s/%s", g_bundle_root, sub);
        } else {
            snprintf(rewritten, sizeof(rewritten), "%s", g_bundle_root);
        }
    } else {
        snprintf(rewritten, sizeof(rewritten), "%s", darwin_path);
    }

    resolve_case_insensitive_path(rewritten, out_buf, out_sz);

    /* Fallback: if <bundle_root>/<rel> does not exist (and isn't already under cookeddata/), check <bundle_root>/cookeddata/<rel> */
    struct stat st_chk;
    size_t root_len = strlen(g_bundle_root);
    if (stat(out_buf, &st_chk) != 0 &&
        strncmp(out_buf, g_bundle_root, root_len) == 0 &&
        out_buf[root_len] == '/' &&
        strncasecmp(out_buf + root_len + 1, "cookeddata/", 11) != 0 &&
        strncasecmp(out_buf + root_len + 1, "Documents/", 10) != 0 &&
        strncasecmp(out_buf + root_len + 1, "Library/", 8) != 0 &&
        strncasecmp(out_buf + root_len + 1, "tmp/", 4) != 0) {
        char cooked_try[1024];
        snprintf(cooked_try, sizeof(cooked_try), "%s/cookeddata/%s", g_bundle_root, out_buf + root_len + 1);
        char cooked_ci[1024];
        resolve_case_insensitive_path(cooked_try, cooked_ci, sizeof(cooked_ci));
        if (stat(cooked_ci, &st_chk) == 0) {
            snprintf(out_buf, out_sz, "%s", cooked_ci);
        }
    }

    static int g_path_trace_count = 0;
    if (g_path_trace_count < 60 &&
        (strstr(out_buf, "oh2") != NULL || strstr(out_buf, "OH2") != NULL ||
         strstr(out_buf, "pak") != NULL || strstr(out_buf, "cookeddata") != NULL)) {
        g_path_trace_count++;
        __android_log_print(ANDROID_LOG_INFO, "OH2_M2", "[IO_PATH] '%s' -> '%s'", darwin_path, out_buf);
    }
    return 0;
}

int oh2_translate_darwin_open_flags(int d_flags) {
    int l_flags = d_flags & O_ACCMODE;
    if (d_flags & 0x00000004) l_flags |= O_NONBLOCK;
    if (d_flags & 0x00000008) l_flags |= O_APPEND;
    if (d_flags & 0x00000080) l_flags |= O_SYNC;
    if (d_flags & 0x00000100) l_flags |= O_NOFOLLOW;
    if (d_flags & 0x00000200) l_flags |= O_CREAT;
    if (d_flags & 0x00000400) l_flags |= O_TRUNC;
    if (d_flags & 0x00000800) l_flags |= O_EXCL;
    if (d_flags & 0x00020000) l_flags |= O_NOCTTY;
    if (d_flags & 0x00100000) l_flags |= O_DIRECTORY;
    if (d_flags & 0x01000000) l_flags |= O_CLOEXEC;
    return l_flags;
}

static void ensure_parent_dirs(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0775);
            *p = '/';
        }
    }
}

int oh2_darwin_open_impl(const char *path, int d_flags, char *darwin_sp) {
    if (!path) {
        errno = EINVAL;
        oh2_sync_darwin_errno();
        return -1;
    }
    char translated[1024];
    oh2_libc_wrapper_translate_path(path, translated, sizeof(translated));

    int l_flags = oh2_translate_darwin_open_flags(d_flags);
    mode_t mode = 0666;
    if ((d_flags & 0x00000200) && darwin_sp) {
        mode = (mode_t)(*(uint64_t *)darwin_sp);
    }
    int fd = open(translated, l_flags, mode);
    if (fd < 0 && (l_flags & O_CREAT) && errno == ENOENT) {
        ensure_parent_dirs(translated);
        fd = open(translated, l_flags, mode);
    }
    if (fd < 0) {
        oh2_sync_darwin_errno();
        return -1;
    }
    oh2_pak_telemetry_track_fd_open(fd, translated);
    return fd;
}

int oh2_darwin_open_direct(const char *path, int d_flags, ...) {
    uint64_t mode_slot = 0666;
    if (d_flags & 0x00000200) {
        va_list ap;
        va_start(ap, d_flags);
        mode_slot = (uint64_t)(unsigned int)va_arg(ap, int);
        va_end(ap);
    }
    return oh2_darwin_open_impl(path, d_flags, (char *)&mode_slot);
}

FILE *oh2_darwin_fopen(const char *path, const char *mode) {
    if (!path || !mode) {
        errno = EINVAL;
        oh2_sync_darwin_errno();
        return NULL;
    }
    char translated[1024];
    oh2_libc_wrapper_translate_path(path, translated, sizeof(translated));
    FILE *fp = fopen(translated, mode);
    if (!fp && errno == ENOENT && (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+'))) {
        ensure_parent_dirs(translated);
        fp = fopen(translated, mode);
    }
    if (!fp) {
        oh2_sync_darwin_errno();
        return NULL;
    }
    oh2_pak_telemetry_track_fd_open(fileno(fp), translated);
    return fp;
}

size_t oh2_darwin_fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!stream) return 0;
    int fd = fileno(stream);
    long pos_before = ftell(stream);
    size_t n = fread(ptr, size, nmemb, stream);
    if (n > 0 && fd >= 0 && pos_before >= 0) {
        pthread_mutex_lock(&g_pak_io_lock);
        TrackedPakFd *tf = find_tracked_fd_locked(fd);
        if (tf) {
            uint64_t bytes = (uint64_t)n * (uint64_t)size;
            record_pak_span_locked((uint64_t)pos_before, bytes);
            tf->pos = (uint64_t)pos_before + bytes;
        }
        pthread_mutex_unlock(&g_pak_io_lock);
    }
    return n;
}

int oh2_darwin_fseek(FILE *stream, long offset, int whence) {
    int rc = fseek(stream, offset, whence);
    if (rc != 0) {
        oh2_sync_darwin_errno();
        return rc;
    }
    int fd = fileno(stream);
    if (fd >= 0) {
        long p = ftell(stream);
        if (p >= 0) {
            pthread_mutex_lock(&g_pak_io_lock);
            TrackedPakFd *tf = find_tracked_fd_locked(fd);
            if (tf) tf->pos = (uint64_t)p;
            pthread_mutex_unlock(&g_pak_io_lock);
        }
    }
    return 0;
}

int oh2_darwin_fclose(FILE *stream) {
    if (!stream) return EOF;
    int fd = fileno(stream);
    if (fd >= 0) oh2_pak_telemetry_track_fd_close(fd);
    int rc = fclose(stream);
    if (rc != 0) oh2_sync_darwin_errno();
    return rc;
}

int oh2_darwin_access(const char *path, int amode) {
    if (!path) {
        errno = EINVAL;
        oh2_sync_darwin_errno();
        return -1;
    }
    char translated[1024];
    oh2_libc_wrapper_translate_path(path, translated, sizeof(translated));
    int rc = access(translated, amode);
    if (rc < 0) oh2_sync_darwin_errno();
    return rc;
}

off_t oh2_darwin_lseek(int fd, off_t offset, int whence) {
    off_t rc = lseek(fd, offset, whence);
    if (rc == (off_t)-1) {
        oh2_sync_darwin_errno();
        return rc;
    }
    pthread_mutex_lock(&g_pak_io_lock);
    TrackedPakFd *tf = find_tracked_fd_locked(fd);
    if (tf) tf->pos = (uint64_t)rc;
    pthread_mutex_unlock(&g_pak_io_lock);
    return rc;
}

ssize_t oh2_darwin_read(int fd, void *buf, size_t count) {
    ssize_t n = read(fd, buf, count);
    if (n < 0) {
        oh2_sync_darwin_errno();
        return n;
    }
    if (n > 0) {
        pthread_mutex_lock(&g_pak_io_lock);
        TrackedPakFd *tf = find_tracked_fd_locked(fd);
        if (tf) {
            record_pak_span_locked(tf->pos, (uint64_t)n);
            tf->pos += (uint64_t)n;
        }
        pthread_mutex_unlock(&g_pak_io_lock);
    }
    return n;
}

ssize_t oh2_darwin_pread(int fd, void *buf, size_t count, off_t offset) {
    ssize_t n = pread(fd, buf, count, offset);
    if (n < 0) {
        oh2_sync_darwin_errno();
        return n;
    }
    if (n > 0 && offset >= 0) {
        pthread_mutex_lock(&g_pak_io_lock);
        TrackedPakFd *tf = find_tracked_fd_locked(fd);
        if (tf) {
            record_pak_span_locked((uint64_t)offset, (uint64_t)n);
        }
        pthread_mutex_unlock(&g_pak_io_lock);
    }
    return n;
}

int oh2_darwin_close(int fd) {
    oh2_pak_telemetry_track_fd_close(fd);
    int rc = close(fd);
    if (rc < 0) oh2_sync_darwin_errno();
    return rc;
}

int oh2_darwin_unlink(const char *path) {
    char translated[1024];
    oh2_libc_wrapper_translate_path(path, translated, sizeof(translated));
    int rc = unlink(translated);
    if (rc < 0) oh2_sync_darwin_errno();
    return rc;
}

int oh2_darwin_rename(const char *oldpath, const char *newpath) {
    char t1[1024], t2[1024];
    oh2_libc_wrapper_translate_path(oldpath, t1, sizeof(t1));
    oh2_libc_wrapper_translate_path(newpath, t2, sizeof(t2));
    int rc = rename(t1, t2);
    if (rc < 0) oh2_sync_darwin_errno();
    return rc;
}

int oh2_darwin_rmdir(const char *path) {
    char translated[1024];
    oh2_libc_wrapper_translate_path(path, translated, sizeof(translated));
    int rc = rmdir(translated);
    if (rc < 0) oh2_sync_darwin_errno();
    return rc;
}

int oh2_darwin_chmod(const char *path, uint16_t mode) {
    char translated[1024];
    oh2_libc_wrapper_translate_path(path, translated, sizeof(translated));
    int rc = chmod(translated, (mode_t)mode);
    if (rc < 0) oh2_sync_darwin_errno();
    return rc;
}

int oh2_darwin_utime(const char *path, const void *times) {
    char translated[1024];
    oh2_libc_wrapper_translate_path(path, translated, sizeof(translated));
    int rc = utime(translated, (const struct utimbuf *)times);
    if (rc < 0) oh2_sync_darwin_errno();
    return rc;
}

void *oh2_darwin_malloc(size_t size) {
    /* UE4 FMallocBinned::Realloc reads up to 0xe0 (224) bytes when promoted from _malloc; clamp floor. */
    if (size < 224) {
        size = 224;
    }
    if (size >= 0x4000ULL) {
        size_t align = (size >= 0x10000ULL && (size & 0x3fffULL) == 0) ? 0x10000ULL : 0x4000ULL;
        void *p = NULL;
        if (posix_memalign(&p, align, size) == 0 && p != NULL) {
            return p;
        }
    }
    return malloc(size ? size : 1);
}

static void *alloc_aligned_anon(size_t len, int prot, int linux_flags) {
    size_t map_len = (len > 0) ? ((len + 0xfffULL) & ~0xfffULL) : 4096ULL;
    const size_t align = 0x10000ULL;
    void *raw = mmap(NULL, map_len + align, prot, linux_flags, -1, 0);
    if (raw == MAP_FAILED) {
        return mmap(NULL, map_len, prot, linux_flags, -1, 0);
    }
    uintptr_t raw_addr = (uintptr_t)raw;
    uintptr_t aligned_addr = (raw_addr + (align - 1ULL)) & ~(align - 1ULL);
    size_t prefix = aligned_addr - raw_addr;
    size_t suffix = align - prefix;
    if (prefix > 0) munmap((void *)raw_addr, prefix);
    if (suffix > 0) munmap((void *)(aligned_addr + map_len), suffix);
    return (void *)aligned_addr;
}

void *oh2_darwin_mmap(void *addr, size_t len, int prot, int darwin_flags, int fd, off_t offset) {
    int linux_flags = 0;
    if (darwin_flags & 0x0001) linux_flags |= MAP_SHARED;
    if (darwin_flags & 0x0002) linux_flags |= MAP_PRIVATE;
    if (darwin_flags & 0x0010) linux_flags |= MAP_FIXED;

    if ((darwin_flags & 0x1000) || fd < 0 || ((uint32_t)fd >> 24) != 0) {
        linux_flags |= MAP_ANONYMOUS;
        if (!(linux_flags & (MAP_SHARED | MAP_PRIVATE))) linux_flags |= MAP_PRIVATE;
        if (addr == NULL && !(linux_flags & MAP_FIXED)) {
            void *p = alloc_aligned_anon(len, prot, linux_flags);
            if (p == MAP_FAILED) oh2_sync_darwin_errno();
            return p;
        }
        void *p = mmap(addr, len ? len : 4096, prot, linux_flags, -1, 0);
        if (p == MAP_FAILED) oh2_sync_darwin_errno();
        return p;
    }

    if (!(linux_flags & (MAP_SHARED | MAP_PRIVATE))) linux_flags |= MAP_PRIVATE;
    void *p = mmap(addr, len, prot, linux_flags, fd, offset);
    if (p == MAP_FAILED) {
        oh2_sync_darwin_errno();
        return p;
    }
    if (offset >= 0) {
        oh2_pak_telemetry_record_mmap(fd, (uint64_t)offset, len);
    }
    return p;
}
