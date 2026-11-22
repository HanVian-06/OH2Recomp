/*
 * errno_wrapper.c — Darwin ___error() <-> Linux errno translation.
 *
 * Critical errno numeric differences between Linux ARM64 and Darwin ARM64:
 *   EAGAIN / EWOULDBLOCK : Linux = 11  <-> Darwin = 35
 *   EDEADLK              : Linux = 35  <-> Darwin = 11
 *   EINPROGRESS          : Linux = 115 <-> Darwin = 36
 *   EALREADY             : Linux = 114 <-> Darwin = 37
 *   ENOTSOCK             : Linux = 88  <-> Darwin = 38
 *   EOPNOTSUPP           : Linux = 95  <-> Darwin = 45
 *   EAFNOSUPPORT         : Linux = 97  <-> Darwin = 47
 *   EADDRINUSE           : Linux = 98  <-> Darwin = 48
 *   EADDRNOTAVAIL        : Linux = 99  <-> Darwin = 49
 *   ECONNRESET           : Linux = 104 <-> Darwin = 54
 *   ETIMEDOUT            : Linux = 110 <-> Darwin = 60
 *   ECONNREFUSED         : Linux = 111 <-> Darwin = 61
 *   ELOOP                : Linux = 40  <-> Darwin = 62
 *   ENAMETOOLONG         : Linux = 36  <-> Darwin = 63
 *   ENOTEMPTY            : Linux = 39  <-> Darwin = 66
 *   ENOSYS               : Linux = 38  <-> Darwin = 78
 */

#include "libc_wrapper.h"

#include <errno.h>

static __thread int tl_darwin_errno = 0;
static __thread int tl_last_linux_errno = 0;
static __thread int tl_last_seen_darwin = 0;

int oh2_linux_to_darwin_errno(int linux_err) {
    switch (linux_err) {
        case EAGAIN:       return 35; /* Darwin EAGAIN / EWOULDBLOCK */
        case EDEADLK:      return 11; /* Darwin EDEADLK */
        case ENAMETOOLONG: return 63; /* Darwin ENAMETOOLONG */
        case ENOSYS:       return 78; /* Darwin ENOSYS */
        case ENOTEMPTY:    return 66; /* Darwin ENOTEMPTY */
        case ELOOP:        return 62; /* Darwin ELOOP */
        case EINPROGRESS:  return 36; /* Darwin EINPROGRESS */
        case EALREADY:     return 37; /* Darwin EALREADY */
        case ENOTSOCK:     return 38; /* Darwin ENOTSOCK */
        case EOPNOTSUPP:   return 45; /* Darwin EOPNOTSUPP */
        case EAFNOSUPPORT: return 47; /* Darwin EAFNOSUPPORT */
        case EADDRINUSE:   return 48; /* Darwin EADDRINUSE */
        case EADDRNOTAVAIL:return 49; /* Darwin EADDRNOTAVAIL */
        case ECONNRESET:   return 54; /* Darwin ECONNRESET */
        case ENOBUFS:      return 55; /* Darwin ENOBUFS */
        case EISCONN:      return 56; /* Darwin EISCONN */
        case ENOTCONN:     return 57; /* Darwin ENOTCONN */
        case ETIMEDOUT:    return 60; /* Darwin ETIMEDOUT */
        case ECONNREFUSED: return 61; /* Darwin ECONNREFUSED */
        case EHOSTDOWN:    return 64; /* Darwin EHOSTDOWN */
        case EHOSTUNREACH: return 65; /* Darwin EHOSTUNREACH */
        default:           return linux_err; /* EPERM..ERANGE (1..34) are identical */
    }
}

int oh2_darwin_to_linux_errno(int darwin_err) {
    switch (darwin_err) {
        case 35: return EAGAIN;       /* Darwin EAGAIN / EWOULDBLOCK */
        case 11: return EDEADLK;      /* Darwin EDEADLK */
        case 63: return ENAMETOOLONG; /* Darwin ENAMETOOLONG */
        case 78: return ENOSYS;       /* Darwin ENOSYS */
        case 66: return ENOTEMPTY;    /* Darwin ENOTEMPTY */
        case 62: return ELOOP;        /* Darwin ELOOP */
        case 36: return EINPROGRESS;  /* Darwin EINPROGRESS */
        case 37: return EALREADY;     /* Darwin EALREADY */
        case 38: return ENOTSOCK;     /* Darwin ENOTSOCK */
        case 45: return EOPNOTSUPP;   /* Darwin EOPNOTSUPP */
        case 47: return EAFNOSUPPORT; /* Darwin EAFNOSUPPORT */
        case 48: return EADDRINUSE;   /* Darwin EADDRINUSE */
        case 49: return EADDRNOTAVAIL;/* Darwin EADDRNOTAVAIL */
        case 54: return ECONNRESET;   /* Darwin ECONNRESET */
        case 55: return ENOBUFS;      /* Darwin ENOBUFS */
        case 56: return EISCONN;      /* Darwin EISCONN */
        case 57: return ENOTCONN;     /* Darwin ENOTCONN */
        case 60: return ETIMEDOUT;    /* Darwin ETIMEDOUT */
        case 61: return ECONNREFUSED; /* Darwin ECONNREFUSED */
        case 64: return EHOSTDOWN;    /* Darwin EHOSTDOWN */
        case 65: return EHOSTUNREACH; /* Darwin EHOSTUNREACH */
        default: return darwin_err;
    }
}

void oh2_sync_darwin_errno(void) {
    int le = errno;
    tl_last_linux_errno = le;
    tl_darwin_errno     = oh2_linux_to_darwin_errno(le);
    tl_last_seen_darwin = tl_darwin_errno;
}

int *oh2_darwin_error(void) {
    int cur_linux = errno;
    if (cur_linux != tl_last_linux_errno) {
        tl_last_linux_errno = cur_linux;
        tl_darwin_errno     = oh2_linux_to_darwin_errno(cur_linux);
        tl_last_seen_darwin = tl_darwin_errno;
    } else if (tl_darwin_errno != tl_last_seen_darwin) {
        int translated = oh2_darwin_to_linux_errno(tl_darwin_errno);
        errno = translated;
        tl_last_linux_errno = translated;
        tl_last_seen_darwin = tl_darwin_errno;
    }
    return &tl_darwin_errno;
}
