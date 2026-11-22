/*
 * socket_wrapper.c — Darwin BSD sockaddr_in (sin_len) / AF_INET6 / setsockopt
 * <-> Linux ARM64 socket ABI translation.
 */

#include "libc_wrapper.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

#define DARWIN_AF_UNSPEC 0
#define DARWIN_AF_UNIX   1
#define DARWIN_AF_INET   2
#define DARWIN_AF_INET6  30

#define DARWIN_SOL_SOCKET  0xffff
#define DARWIN_SO_REUSEADDR 0x0004
#define DARWIN_SO_KEEPALIVE 0x0008
#define DARWIN_SO_BROADCAST 0x0020
#define DARWIN_SO_LINGER    0x0080
#define DARWIN_SO_SNDBUF    0x1001
#define DARWIN_SO_RCVBUF    0x1002
#define DARWIN_SO_SNDTIMEO  0x1005
#define DARWIN_SO_RCVTIMEO  0x1006
#define DARWIN_SO_ERROR     0x1007
#define DARWIN_SO_NOSIGPIPE 0x1022

static int darwin_to_linux_af(int d_af) {
    if (d_af == DARWIN_AF_INET6) return AF_INET6;
    return d_af;
}

static int linux_to_darwin_af(int l_af) {
    if (l_af == AF_INET6) return DARWIN_AF_INET6;
    return l_af;
}

static void darwin_to_linux_sockaddr(const DarwinSockAddr *d_sa,
                                     struct sockaddr_storage *l_ss,
                                     socklen_t *l_len) {
    memset(l_ss, 0, sizeof(*l_ss));
    if (!d_sa) {
        if (l_len) *l_len = 0;
        return;
    }

    uint8_t fam = d_sa->sa_family;
    if (fam == DARWIN_AF_INET) {
        const DarwinSockAddrIn *dsin = (const DarwinSockAddrIn *)d_sa;
        struct sockaddr_in *lsin = (struct sockaddr_in *)l_ss;
        lsin->sin_family      = AF_INET;
        lsin->sin_port        = dsin->sin_port;
        lsin->sin_addr.s_addr = dsin->sin_addr;
        if (l_len) *l_len = (socklen_t)sizeof(struct sockaddr_in);
    } else if (fam == DARWIN_AF_INET6) {
        const DarwinSockAddrIn6 *dsin6 = (const DarwinSockAddrIn6 *)d_sa;
        struct sockaddr_in6 *lsin6 = (struct sockaddr_in6 *)l_ss;
        lsin6->sin6_family   = AF_INET6;
        lsin6->sin6_port     = dsin6->sin6_port;
        lsin6->sin6_flowinfo = dsin6->sin6_flowinfo;
        memcpy(&lsin6->sin6_addr, dsin6->sin6_addr, 16);
        lsin6->sin6_scope_id = dsin6->sin6_scope_id;
        if (l_len) *l_len = (socklen_t)sizeof(struct sockaddr_in6);
    } else {
        l_ss->ss_family = (sa_family_t)darwin_to_linux_af(fam);
        size_t copy_len = (d_sa->sa_len > 2) ? (size_t)(d_sa->sa_len - 2) : 14;
        if (copy_len > 14) copy_len = 14;
        memcpy((char *)l_ss + 2, d_sa->sa_data, copy_len);
        if (l_len) *l_len = (socklen_t)sizeof(struct sockaddr);
    }
}

static void linux_to_darwin_sockaddr(const struct sockaddr *l_sa,
                                     socklen_t l_len,
                                     DarwinSockAddr *d_sa,
                                     uint32_t *d_len_inout) {
    if (!l_sa || !d_sa) return;
    uint32_t max_out = d_len_inout ? *d_len_inout : (uint32_t)sizeof(DarwinSockAddrIn6);

    if (l_sa->sa_family == AF_INET && l_len >= (socklen_t)sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *lsin = (const struct sockaddr_in *)l_sa;
        DarwinSockAddrIn tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.sin_len    = 16;
        tmp.sin_family = DARWIN_AF_INET;
        tmp.sin_port   = lsin->sin_port;
        tmp.sin_addr   = lsin->sin_addr.s_addr;
        uint32_t to_copy = (max_out < sizeof(tmp)) ? max_out : (uint32_t)sizeof(tmp);
        memcpy(d_sa, &tmp, to_copy);
        if (d_len_inout) *d_len_inout = 16U;
    } else if (l_sa->sa_family == AF_INET6 && l_len >= (socklen_t)sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *lsin6 = (const struct sockaddr_in6 *)l_sa;
        DarwinSockAddrIn6 tmp6;
        memset(&tmp6, 0, sizeof(tmp6));
        tmp6.sin6_len      = 28;
        tmp6.sin6_family   = DARWIN_AF_INET6;
        tmp6.sin6_port     = lsin6->sin6_port;
        tmp6.sin6_flowinfo = lsin6->sin6_flowinfo;
        memcpy(tmp6.sin6_addr, &lsin6->sin6_addr, 16);
        tmp6.sin6_scope_id = lsin6->sin6_scope_id;
        uint32_t to_copy = (max_out < sizeof(tmp6)) ? max_out : (uint32_t)sizeof(tmp6);
        memcpy(d_sa, &tmp6, to_copy);
        if (d_len_inout) *d_len_inout = 28U;
    } else {
        DarwinSockAddr tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.sa_len    = (uint8_t)(l_len <= 255 ? l_len : 255);
        tmp.sa_family = (uint8_t)linux_to_darwin_af(l_sa->sa_family);
        if (l_len > 2) {
            size_t data_len = (size_t)(l_len - 2);
            if (data_len > 14) data_len = 14;
            memcpy(tmp.sa_data, l_sa->sa_data, data_len);
        }
        uint32_t to_copy = (max_out < sizeof(tmp)) ? max_out : (uint32_t)sizeof(tmp);
        memcpy(d_sa, &tmp, to_copy);
        if (d_len_inout) *d_len_inout = tmp.sa_len;
    }
}

int oh2_darwin_socket(int domain, int type, int protocol) {
    int rc = socket(darwin_to_linux_af(domain), type, protocol);
    if (rc < 0) oh2_sync_darwin_errno();
    return rc;
}

int oh2_darwin_bind(int sockfd, const DarwinSockAddr *d_addr, uint32_t d_addrlen) {
    (void)d_addrlen;
    struct sockaddr_storage l_ss;
    socklen_t l_len = 0;
    darwin_to_linux_sockaddr(d_addr, &l_ss, &l_len);
    int rc = bind(sockfd, (const struct sockaddr *)&l_ss, l_len);
    if (rc < 0) oh2_sync_darwin_errno();
    return rc;
}

int oh2_darwin_connect(int sockfd, const DarwinSockAddr *d_addr, uint32_t d_addrlen) {
    (void)d_addrlen;
    struct sockaddr_storage l_ss;
    socklen_t l_len = 0;
    darwin_to_linux_sockaddr(d_addr, &l_ss, &l_len);
    int rc = connect(sockfd, (const struct sockaddr *)&l_ss, l_len);
    if (rc < 0) oh2_sync_darwin_errno();
    return rc;
}

int oh2_darwin_accept(int sockfd, DarwinSockAddr *d_addr, uint32_t *d_addrlen) {
    struct sockaddr_storage l_ss;
    socklen_t l_len = sizeof(l_ss);
    int rc = accept(sockfd, d_addr ? (struct sockaddr *)&l_ss : NULL, d_addr ? &l_len : NULL);
    if (rc < 0) {
        oh2_sync_darwin_errno();
        return rc;
    }
    if (d_addr) {
        linux_to_darwin_sockaddr((const struct sockaddr *)&l_ss, l_len, d_addr, d_addrlen);
    }
    return rc;
}

ssize_t oh2_darwin_sendto(int sockfd, const void *buf, size_t len, int flags,
                          const DarwinSockAddr *d_dest, uint32_t d_destlen) {
    (void)d_destlen;
    ssize_t rc;
    if (!d_dest) {
        rc = sendto(sockfd, buf, len, flags, NULL, 0);
    } else {
        struct sockaddr_storage l_ss;
        socklen_t l_len = 0;
        darwin_to_linux_sockaddr(d_dest, &l_ss, &l_len);
        rc = sendto(sockfd, buf, len, flags, (const struct sockaddr *)&l_ss, l_len);
    }
    if (rc < 0) oh2_sync_darwin_errno();
    return rc;
}

ssize_t oh2_darwin_recvfrom(int sockfd, void *buf, size_t len, int flags,
                            DarwinSockAddr *d_src, uint32_t *d_srclen) {
    struct sockaddr_storage l_ss;
    socklen_t l_len = sizeof(l_ss);
    ssize_t rc = recvfrom(sockfd, buf, len, flags,
                          d_src ? (struct sockaddr *)&l_ss : NULL,
                          d_src ? &l_len : NULL);
    if (rc < 0) {
        oh2_sync_darwin_errno();
        return rc;
    }
    if (d_src) {
        linux_to_darwin_sockaddr((const struct sockaddr *)&l_ss, l_len, d_src, d_srclen);
    }
    return rc;
}

int oh2_darwin_getsockname(int sockfd, DarwinSockAddr *d_addr, uint32_t *d_addrlen) {
    struct sockaddr_storage l_ss;
    socklen_t l_len = sizeof(l_ss);
    int rc = getsockname(sockfd, (struct sockaddr *)&l_ss, &l_len);
    if (rc < 0) {
        oh2_sync_darwin_errno();
        return rc;
    }
    if (d_addr) {
        linux_to_darwin_sockaddr((const struct sockaddr *)&l_ss, l_len, d_addr, d_addrlen);
    }
    return 0;
}

int oh2_darwin_getpeername(int sockfd, DarwinSockAddr *d_addr, uint32_t *d_addrlen) {
    struct sockaddr_storage l_ss;
    socklen_t l_len = sizeof(l_ss);
    int rc = getpeername(sockfd, (struct sockaddr *)&l_ss, &l_len);
    if (rc < 0) {
        oh2_sync_darwin_errno();
        return rc;
    }
    if (d_addr) {
        linux_to_darwin_sockaddr((const struct sockaddr *)&l_ss, l_len, d_addr, d_addrlen);
    }
    return 0;
}

static int translate_sockopt(int d_level, int d_optname, int *l_level, int *l_optname) {
    if (d_level == DARWIN_SOL_SOCKET) {
        *l_level = SOL_SOCKET;
        switch (d_optname) {
            case DARWIN_SO_REUSEADDR: *l_optname = SO_REUSEADDR; return 0;
            case DARWIN_SO_KEEPALIVE: *l_optname = SO_KEEPALIVE; return 0;
            case DARWIN_SO_BROADCAST: *l_optname = SO_BROADCAST; return 0;
            case DARWIN_SO_LINGER:    *l_optname = SO_LINGER;    return 0;
            case DARWIN_SO_SNDBUF:    *l_optname = SO_SNDBUF;    return 0;
            case DARWIN_SO_RCVBUF:    *l_optname = SO_RCVBUF;    return 0;
            case DARWIN_SO_SNDTIMEO:  *l_optname = SO_SNDTIMEO;  return 0;
            case DARWIN_SO_RCVTIMEO:  *l_optname = SO_RCVTIMEO;  return 0;
            case DARWIN_SO_ERROR:     *l_optname = SO_ERROR;     return 0;
            case DARWIN_SO_NOSIGPIPE: return 1; /* no-op on Linux */
            default:                  *l_optname = d_optname;    return 0;
        }
    }
    *l_level = d_level;
    *l_optname = d_optname;
    return 0;
}

int oh2_darwin_getsockopt(int sockfd, int level, int optname, void *optval, uint32_t *optlen) {
    int l_lvl = 0, l_opt = 0;
    if (translate_sockopt(level, optname, &l_lvl, &l_opt) == 1) {
        if (optval && optlen && *optlen >= 4) *(int *)optval = 0;
        return 0;
    }
    socklen_t sl = optlen ? (socklen_t)(*optlen) : 0;
    int rc = getsockopt(sockfd, l_lvl, l_opt, optval, optlen ? &sl : NULL);
    if (optlen) *optlen = (uint32_t)sl;
    if (rc < 0) {
        oh2_sync_darwin_errno();
    } else if (l_lvl == SOL_SOCKET && l_opt == SO_ERROR && optval && optlen && *optlen >= 4) {
        *(int *)optval = oh2_linux_to_darwin_errno(*(int *)optval);
    }
    return rc;
}

int oh2_darwin_setsockopt(int sockfd, int level, int optname, const void *optval, uint32_t optlen) {
    int l_lvl = 0, l_opt = 0;
    if (translate_sockopt(level, optname, &l_lvl, &l_opt) == 1) {
        return 0; /* SO_NOSIGPIPE gracefully ignored */
    }
    int rc = setsockopt(sockfd, l_lvl, l_opt, optval, (socklen_t)optlen);
    if (rc < 0) oh2_sync_darwin_errno();
    return rc;
}

int oh2_darwin_getaddrinfo(const char *node, const char *service,
                           const DarwinAddrInfo *d_hints, DarwinAddrInfo **d_res) {
    if (!d_res) return EAI_FAIL;
    *d_res = NULL;

    struct addrinfo l_hints;
    memset(&l_hints, 0, sizeof(l_hints));
    if (d_hints) {
        l_hints.ai_flags    = d_hints->ai_flags;
        l_hints.ai_family   = darwin_to_linux_af(d_hints->ai_family);
        l_hints.ai_socktype = d_hints->ai_socktype;
        l_hints.ai_protocol = d_hints->ai_protocol;
    }

    struct addrinfo *l_res = NULL;
    int rc = getaddrinfo(node, service, d_hints ? &l_hints : NULL, &l_res);
    if (rc != 0 || !l_res) return rc;

    DarwinAddrInfo *head = NULL;
    DarwinAddrInfo *tail = NULL;
    for (struct addrinfo *cur = l_res; cur; cur = cur->ai_next) {
        size_t alloc_sz = sizeof(DarwinAddrInfo) + sizeof(DarwinSockAddrIn6) +
                          (cur->ai_canonname ? strlen(cur->ai_canonname) + 1 : 0);
        uint8_t *blk = (uint8_t *)calloc(1, alloc_sz);
        if (!blk) {
            oh2_darwin_freeaddrinfo(head);
            freeaddrinfo(l_res);
            return EAI_MEMORY;
        }
        DarwinAddrInfo *node_d = (DarwinAddrInfo *)blk;
        node_d->ai_flags    = cur->ai_flags;
        node_d->ai_family   = linux_to_darwin_af(cur->ai_family);
        node_d->ai_socktype = cur->ai_socktype;
        node_d->ai_protocol = cur->ai_protocol;
        if (cur->ai_addr) {
            DarwinSockAddr *sa_dst = (DarwinSockAddr *)(blk + sizeof(DarwinAddrInfo));
            uint32_t dlen = (uint32_t)sizeof(DarwinSockAddrIn6);
            linux_to_darwin_sockaddr(cur->ai_addr, cur->ai_addrlen, sa_dst, &dlen);
            node_d->ai_addr    = sa_dst;
            node_d->ai_addrlen = dlen;
        }
        if (cur->ai_canonname) {
            char *cn = (char *)(blk + sizeof(DarwinAddrInfo) + sizeof(DarwinSockAddrIn6));
            strcpy(cn, cur->ai_canonname);
            node_d->ai_canonname = cn;
        }
        if (!head) head = node_d;
        else tail->ai_next = node_d;
        tail = node_d;
    }
    freeaddrinfo(l_res);
    *d_res = head;
    return 0;
}

void oh2_darwin_freeaddrinfo(DarwinAddrInfo *d_res) {
    while (d_res) {
        DarwinAddrInfo *next = d_res->ai_next;
        free(d_res);
        d_res = next;
    }
}

int oh2_darwin_getnameinfo(const DarwinSockAddr *d_sa, uint32_t d_salen,
                           char *host, uint32_t hostlen,
                           char *serv, uint32_t servlen, int flags) {
    (void)d_salen;
    struct sockaddr_storage l_ss;
    socklen_t l_len = 0;
    darwin_to_linux_sockaddr(d_sa, &l_ss, &l_len);
    return getnameinfo((const struct sockaddr *)&l_ss, l_len,
                       host, (socklen_t)hostlen,
                       serv, (socklen_t)servlen, flags);
}

int oh2_darwin_inet_pton(int af, const char *src, void *dst) {
    int rc = inet_pton(darwin_to_linux_af(af), src, dst);
    if (rc < 0) oh2_sync_darwin_errno();
    return rc;
}
