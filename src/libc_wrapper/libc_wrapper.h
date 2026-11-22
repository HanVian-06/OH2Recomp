#ifndef OH2_LIBC_WRAPPER_H
#define OH2_LIBC_WRAPPER_H

#include <dirent.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Darwin ARM64 144-byte struct stat
 * ============================================================================ */
typedef struct {
    int32_t         st_dev;
    uint16_t        st_mode;
    uint16_t        st_nlink;
    uint64_t        st_ino;
    uint32_t        st_uid;
    uint32_t        st_gid;
    int32_t         st_rdev;
    int32_t         __pad0;
    struct timespec st_atimespec;
    struct timespec st_mtimespec;
    struct timespec st_ctimespec;
    struct timespec st_birthtimespec;
    int64_t         st_size;
    int64_t         st_blocks;
    int32_t         st_blksize;
    uint32_t        st_flags;
    uint32_t        st_gen;
    int32_t         st_lspare;
    int64_t         st_qspare[2];
} DarwinStat64;

#ifdef __cplusplus
static_assert(sizeof(DarwinStat64) == 144, "DarwinStat64 must be 144 bytes");
#else
_Static_assert(sizeof(DarwinStat64) == 144, "DarwinStat64 must be 144 bytes");
#endif

/* ============================================================================
 * Darwin ARM64 64-bit struct dirent (d_name begins at byte offset 21)
 * ============================================================================ */
#pragma pack(push, 1)
typedef struct {
    uint64_t d_ino;
    uint64_t d_seekoff;
    uint16_t d_reclen;
    uint16_t d_namlen;
    uint8_t  d_type;
    char     d_name[1024];
} DarwinDirent64;
#pragma pack(pop)

#ifdef __cplusplus
static_assert(offsetof(DarwinDirent64, d_name) == 21, "DarwinDirent64 d_name must be at offset 21");
#else
_Static_assert(offsetof(DarwinDirent64, d_name) == 21, "DarwinDirent64 d_name must be at offset 21");
#endif

/* ============================================================================
 * Darwin BSD sockaddr_in / sockaddr_in6 / addrinfo
 * ============================================================================ */
typedef struct {
    uint8_t  sa_len;
    uint8_t  sa_family;
    char     sa_data[14];
} DarwinSockAddr;

typedef struct {
    uint8_t  sin_len;
    uint8_t  sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    char     sin_zero[8];
} DarwinSockAddrIn;

typedef struct {
    uint8_t  sin6_len;
    uint8_t  sin6_family;
    uint16_t sin6_port;
    uint32_t sin6_flowinfo;
    uint8_t  sin6_addr[16];
    uint32_t sin6_scope_id;
} DarwinSockAddrIn6;

typedef struct DarwinAddrInfoTag {
    int32_t                   ai_flags;
    int32_t                   ai_family;
    int32_t                   ai_socktype;
    int32_t                   ai_protocol;
    uint32_t                  ai_addrlen;
    uint32_t                  __pad0;
    char                     *ai_canonname;
    DarwinSockAddr           *ai_addr;
    struct DarwinAddrInfoTag *ai_next;
} DarwinAddrInfo;

/* ============================================================================
 * Live UE4 Pak I/O Telemetry (oh2-ios.pak ground truth bounds)
 * ============================================================================ */
#define OH2_PAK_EXPECTED_SIZE  2430963623ULL
#define OH2_PAK_FOOTER_OFFSET  (2430963623ULL - 221ULL)
#define OH2_PAK_INDEX_OFFSET   2427412912ULL
#define OH2_PAK_INDEX_SIZE     3550490ULL
#define OH2_PAK_INDEX_END      (OH2_PAK_INDEX_OFFSET + OH2_PAK_INDEX_SIZE)

typedef struct {
    int      opened_by_ue4;
    char     opened_path[512];
    uint64_t fstat_size;
    int      footer_read;
    uint64_t index_bytes_read;
    int      index_read_complete;
    uint64_t asset_reads_after_index;
    uint64_t total_bytes_read;
    uint64_t mmap_count;
} Oh2PakIoTelemetry;

void oh2_pak_telemetry_reset(void);
void oh2_pak_telemetry_get(Oh2PakIoTelemetry *out);
void oh2_pak_telemetry_track_fd_open(int fd, const char *resolved_path);
void oh2_pak_telemetry_track_fd_close(int fd);
void oh2_pak_telemetry_record_fstat(int fd, uint64_t size);
void oh2_pak_telemetry_record_mmap(int fd, uint64_t offset, size_t len);

/* ============================================================================
 * Path Translation & Registration API
 * ============================================================================ */
void        oh2_libc_wrapper_set_bundle_root(const char *root);
const char *oh2_libc_wrapper_get_bundle_root(void);
int         oh2_libc_wrapper_translate_path(const char *darwin_path, char *out_buf, size_t out_sz);

void oh2_libc_wrapper_register_symbols(void (*reg_fn)(const char *sym_name, void *addr));

/* ============================================================================
 * Individual Wrapper Entry Points
 * ============================================================================ */
/* errno_wrapper.c */
int  oh2_linux_to_darwin_errno(int linux_err);
int  oh2_darwin_to_linux_errno(int darwin_err);
void oh2_sync_darwin_errno(void);
int *oh2_darwin_error(void);

/* open_wrapper.c */
int   oh2_translate_darwin_open_flags(int d_flags);
int   oh2_darwin_open_impl(const char *path, int d_flags, char *darwin_sp);
int   oh2_darwin_open_direct(const char *path, int d_flags, ...);
FILE *oh2_darwin_fopen(const char *path, const char *mode);
size_t oh2_darwin_fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
int   oh2_darwin_fseek(FILE *stream, long offset, int whence);
int   oh2_darwin_fclose(FILE *stream);
int   oh2_darwin_access(const char *path, int amode);
off_t oh2_darwin_lseek(int fd, off_t offset, int whence);
ssize_t oh2_darwin_read(int fd, void *buf, size_t count);
ssize_t oh2_darwin_pread(int fd, void *buf, size_t count, off_t offset);
int   oh2_darwin_close(int fd);
int   oh2_darwin_unlink(const char *path);
int   oh2_darwin_rename(const char *oldpath, const char *newpath);
int   oh2_darwin_rmdir(const char *path);
int   oh2_darwin_chmod(const char *path, uint16_t mode);
int   oh2_darwin_utime(const char *path, const void *times);
void *oh2_darwin_malloc(size_t size);
void *oh2_darwin_mmap(void *addr, size_t len, int prot, int darwin_flags, int fd, off_t offset);

/* stat_wrapper.c */
int             oh2_darwin_stat(const char *path, DarwinStat64 *d_st);
int             oh2_darwin_lstat(const char *path, DarwinStat64 *d_st);
int             oh2_darwin_fstat(int fd, DarwinStat64 *d_st);
DIR            *oh2_darwin_opendir(const char *path);
DarwinDirent64 *oh2_darwin_readdir(DIR *dirp);
int             oh2_darwin_closedir(DIR *dirp);

/* socket_wrapper.c */
int     oh2_darwin_socket(int domain, int type, int protocol);
int     oh2_darwin_bind(int sockfd, const DarwinSockAddr *d_addr, uint32_t d_addrlen);
int     oh2_darwin_connect(int sockfd, const DarwinSockAddr *d_addr, uint32_t d_addrlen);
int     oh2_darwin_accept(int sockfd, DarwinSockAddr *d_addr, uint32_t *d_addrlen);
ssize_t oh2_darwin_sendto(int sockfd, const void *buf, size_t len, int flags,
                          const DarwinSockAddr *d_dest, uint32_t d_destlen);
ssize_t oh2_darwin_recvfrom(int sockfd, void *buf, size_t len, int flags,
                            DarwinSockAddr *d_src, uint32_t *d_srclen);
int     oh2_darwin_getsockname(int sockfd, DarwinSockAddr *d_addr, uint32_t *d_addrlen);
int     oh2_darwin_getpeername(int sockfd, DarwinSockAddr *d_addr, uint32_t *d_addrlen);
int     oh2_darwin_getsockopt(int sockfd, int level, int optname, void *optval, uint32_t *optlen);
int     oh2_darwin_setsockopt(int sockfd, int level, int optname, const void *optval, uint32_t optlen);
int     oh2_darwin_getaddrinfo(const char *node, const char *service,
                               const DarwinAddrInfo *d_hints, DarwinAddrInfo **d_res);
void    oh2_darwin_freeaddrinfo(DarwinAddrInfo *d_res);
int     oh2_darwin_getnameinfo(const DarwinSockAddr *d_sa, uint32_t d_salen,
                               char *host, uint32_t hostlen,
                               char *serv, uint32_t servlen, int flags);
int     oh2_darwin_inet_pton(int af, const char *src, void *dst);

/* pthread_wrapper.c */
int       oh2_darwin_pthread_mutex_init(void *m, const void *attr);
int       oh2_darwin_pthread_mutex_lock(void *m);
int       oh2_darwin_pthread_mutex_trylock(void *m);
int       oh2_darwin_pthread_mutex_unlock(void *m);
int       oh2_darwin_pthread_mutex_destroy(void *m);
int       oh2_darwin_pthread_mutexattr_init(void *attr);
int       oh2_darwin_pthread_mutexattr_destroy(void *attr);
int       oh2_darwin_pthread_mutexattr_settype(void *attr, int darwin_type);
int       oh2_darwin_pthread_mutexattr_setprotocol(void *attr, int protocol);
int       oh2_darwin_pthread_cond_init(void *c, const void *attr);
int       oh2_darwin_pthread_cond_wait(void *c, void *m);
int       oh2_darwin_pthread_cond_timedwait(void *c, void *m, const struct timespec *abstime);
int       oh2_darwin_pthread_cond_signal(void *c);
int       oh2_darwin_pthread_cond_broadcast(void *c);
int       oh2_darwin_pthread_cond_destroy(void *c);
int       oh2_darwin_pthread_rwlock_init(void *rwl, const void *attr);
int       oh2_darwin_pthread_rwlock_rdlock(void *rwl);
int       oh2_darwin_pthread_rwlock_wrlock(void *rwl);
int       oh2_darwin_pthread_rwlock_unlock(void *rwl);
int       oh2_darwin_pthread_rwlock_destroy(void *rwl);
int       oh2_darwin_pthread_key_create(uint64_t *key_out, void (*destructor)(void *));
int       oh2_darwin_pthread_key_delete(uint64_t key);
void     *oh2_darwin_pthread_getspecific(uint64_t key);
int       oh2_darwin_pthread_setspecific(uint64_t key, const void *val);
int       oh2_darwin_pthread_create(void *thread_out, const void *attr, void *(*start)(void *), void *arg);
int       oh2_darwin_pthread_attr_setstacksize(void *attr, size_t stacksize);
int       oh2_darwin_pthread_setname_np(const char *name);
int       oh2_darwin_pthread_cancel(uintptr_t th);
uint32_t  oh2_darwin_pthread_mach_thread_np(uintptr_t th);
uintptr_t oh2_darwin_pthread_from_mach_thread_np(uint32_t port);

#ifdef __cplusplus
}
#endif

#endif /* OH2_LIBC_WRAPPER_H */
