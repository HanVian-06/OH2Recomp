#include "macho_loader.h"

#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MH_MAGIC_64             0xfeedfacfU
#define CPU_TYPE_ARM64          0x0100000cU
#define MH_EXECUTE              0x2U

#define LC_SEGMENT_64           0x19U
#define LC_SYMTAB               0x02U
#define LC_DYSYMTAB             0x0bU
#define LC_LOAD_DYLIB           0x0cU
#define LC_LOAD_WEAK_DYLIB      0x80000018U
#define LC_DYLD_INFO            0x22U
#define LC_DYLD_INFO_ONLY       0x80000022U
#define LC_ENCRYPTION_INFO_64   0x2cU
#define LC_MAIN                 0x80000028U

#define S_MOD_INIT_FUNC_POINTERS 0x9U

typedef struct {
    uint32_t magic;
    uint32_t cputype;
    uint32_t cpusubtype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t flags;
    uint32_t reserved;
} mach_header_64_t;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
} load_command_t;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    char     segname[16];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    uint32_t maxprot;
    uint32_t initprot;
    uint32_t nsects;
    uint32_t flags;
} segment_command_64_t;

typedef struct {
    char     sectname[16];
    char     segname[16];
    uint64_t addr;
    uint64_t size;
    uint32_t offset;
    uint32_t align;
    uint32_t reloff;
    uint32_t nreloc;
    uint32_t flags;
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t reserved3;
} section_64_t;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t name_offset;
    uint32_t timestamp;
    uint32_t current_version;
    uint32_t compatibility_version;
} dylib_command_t;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t rebase_off;
    uint32_t rebase_size;
    uint32_t bind_off;
    uint32_t bind_size;
    uint32_t weak_bind_off;
    uint32_t weak_bind_size;
    uint32_t lazy_bind_off;
    uint32_t lazy_bind_size;
    uint32_t export_off;
    uint32_t export_size;
} dyld_info_command_t;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t symoff;
    uint32_t nsyms;
    uint32_t stroff;
    uint32_t strsize;
} symtab_command_t;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t ilocalsym;
    uint32_t nlocalsym;
    uint32_t iextdefsym;
    uint32_t nextdefsym;
    uint32_t iundefsym;
    uint32_t nundefsym;
    uint32_t tocoff;
    uint32_t ntoc;
    uint32_t modtaboff;
    uint32_t nmodtab;
    uint32_t extrefsymoff;
    uint32_t nextrefsyms;
    uint32_t indirectsymoff;
    uint32_t nindirectsyms;
    uint32_t extreloff;
    uint32_t nextrel;
    uint32_t locreloff;
    uint32_t nlocrel;
} dysymtab_command_t;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t cryptoff;
    uint32_t cryptsize;
    uint32_t cryptid;
    uint32_t pad;
} encryption_info_command_64_t;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint64_t entryoff;
    uint64_t stacksize;
} entry_point_command_t;

static MachOImage *g_active_image = NULL;

const MachOImage *macho_get_active_image(void) {
    return g_active_image;
}

void oh2_log_info(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, "OH2_M1", "%s", buf);
    fprintf(stdout, "[OH2_M1] %s\n", buf);
    fflush(stdout);
}

void oh2_log_error(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_ERROR, "OH2_M1", "%s", buf);
    fprintf(stderr, "[OH2_M1][ERROR] %s\n", buf);
    fflush(stderr);
}

const MachOSection *macho_find_section(const MachOImage *img, const char *segname, const char *sectname) {
    if (!img) return NULL;
    for (uint32_t i = 0; i < img->num_sections; ++i) {
        if (strcmp(img->sections[i].segname, segname) == 0 &&
            strcmp(img->sections[i].sectname, sectname) == 0) {
            return &img->sections[i];
        }
    }
    return NULL;
}

static uint64_t read_uleb128(const uint8_t **pp, const uint8_t *end) {
    uint64_t result = 0;
    int shift = 0;
    const uint8_t *p = *pp;
    while (p < end) {
        uint8_t byte = *p++;
        result |= (uint64_t)(byte & 0x7fU) << shift;
        if ((byte & 0x80U) == 0) break;
        shift += 7;
    }
    *pp = p;
    return result;
}

int macho_load_at_offset(const char *path, off_t base_file_offset, size_t image_size, MachOImage *out_img) {
    if (!path || !out_img) return -1;
    if ((base_file_offset & 0x3FFFLL) != 0) {
        oh2_log_error("macho_load_at_offset: offset 0x%llx is not 16 KiB aligned",
                      (unsigned long long)base_file_offset);
        return -1;
    }
    memset(out_img, 0, sizeof(*out_img));
    snprintf(out_img->path, sizeof(out_img->path), "%s", path);
    out_img->fd = -1;
    out_img->base_file_offset = base_file_offset;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        oh2_log_error("Failed to open Mach-O binary '%s': %s", path, strerror(errno));
        return -1;
    }
    out_img->fd = fd;

    struct stat st;
    if (fstat(fd, &st) < 0 || (uint64_t)st.st_size < (uint64_t)base_file_offset + sizeof(mach_header_64_t)) {
        oh2_log_error("Invalid file size for '%s'", path);
        close(fd);
        out_img->fd = -1;
        return -1;
    }
    out_img->file_size = (image_size > 0) ? image_size : (size_t)((uint64_t)st.st_size - (uint64_t)base_file_offset);

    uint8_t header_buf[65536];
    ssize_t nread = pread(fd, header_buf, sizeof(header_buf), base_file_offset);
    if (nread < (ssize_t)sizeof(mach_header_64_t)) {
        oh2_log_error("Failed reading mach_header_64 at offset 0x%llx", (unsigned long long)base_file_offset);
        close(fd);
        out_img->fd = -1;
        return -1;
    }

    const mach_header_64_t *hdr = (const mach_header_64_t *)header_buf;
    if (hdr->magic != MH_MAGIC_64 || hdr->cputype != CPU_TYPE_ARM64 || hdr->filetype != MH_EXECUTE) {
        oh2_log_error("Unsupported Mach-O header: magic=0x%08x cputype=0x%08x filetype=%u",
                      hdr->magic, hdr->cputype, hdr->filetype);
        close(fd);
        out_img->fd = -1;
        return -1;
    }

    out_img->ncmds = hdr->ncmds;
    out_img->sizeofcmds = hdr->sizeofcmds;
    out_img->header_flags = hdr->flags;

    if (sizeof(mach_header_64_t) + hdr->sizeofcmds > (size_t)nread) {
        oh2_log_error("Load commands exceed 64KB header buffer (%u)", hdr->sizeofcmds);
        close(fd);
        out_img->fd = -1;
        return -1;
    }

    uint64_t min_vmaddr = UINT64_MAX;
    uint64_t max_vmaddr = 0;

    const uint8_t *cmd_ptr = header_buf + sizeof(mach_header_64_t);
    const uint8_t *cmd_end = cmd_ptr + hdr->sizeofcmds;

    for (uint32_t i = 0; i < hdr->ncmds && cmd_ptr + sizeof(load_command_t) <= cmd_end; ++i) {
        const load_command_t *lc = (const load_command_t *)cmd_ptr;
        if (lc->cmdsize < sizeof(load_command_t) || cmd_ptr + lc->cmdsize > cmd_end) {
            oh2_log_error("Corrupt load command %u (cmdsize=%u)", i, lc->cmdsize);
            close(fd);
            out_img->fd = -1;
            return -1;
        }

        if (lc->cmd == LC_SEGMENT_64) {
            const segment_command_64_t *seg = (const segment_command_64_t *)cmd_ptr;
            if (out_img->num_segments >= OH2_MAX_SEGMENTS) {
                oh2_log_error("Too many segments");
                close(fd);
                out_img->fd = -1;
                return -1;
            }
            MachOSegment *dst_seg = &out_img->segments[out_img->num_segments++];
            memcpy(dst_seg->segname, seg->segname, 16);
            dst_seg->segname[16] = '\0';
            dst_seg->vmaddr   = seg->vmaddr;
            dst_seg->vmsize   = seg->vmsize;
            dst_seg->fileoff  = seg->fileoff;
            dst_seg->filesize = seg->filesize;
            dst_seg->maxprot  = seg->maxprot;
            dst_seg->initprot = seg->initprot;
            dst_seg->nsects   = seg->nsects;
            dst_seg->flags    = seg->flags;

            if (strcmp(dst_seg->segname, "__PAGEZERO") != 0 && dst_seg->vmsize > 0) {
                if (dst_seg->vmaddr < min_vmaddr) min_vmaddr = dst_seg->vmaddr;
                if (dst_seg->vmaddr + dst_seg->vmsize > max_vmaddr) {
                    max_vmaddr = dst_seg->vmaddr + dst_seg->vmsize;
                }
            }

            const section_64_t *sects = (const section_64_t *)(cmd_ptr + sizeof(segment_command_64_t));
            for (uint32_t s = 0; s < seg->nsects; ++s) {
                if (out_img->num_sections >= OH2_MAX_SECTIONS) break;
                MachOSection *dst_sec = &out_img->sections[out_img->num_sections++];
                memcpy(dst_sec->sectname, sects[s].sectname, 16);
                dst_sec->sectname[16] = '\0';
                memcpy(dst_sec->segname, sects[s].segname, 16);
                dst_sec->segname[16] = '\0';
                dst_sec->addr      = sects[s].addr;
                dst_sec->size      = sects[s].size;
                dst_sec->offset    = sects[s].offset;
                dst_sec->align     = sects[s].align;
                dst_sec->flags     = sects[s].flags;
                dst_sec->reserved1 = sects[s].reserved1;
                dst_sec->reserved2 = sects[s].reserved2;
            }
        } else if (lc->cmd == LC_LOAD_DYLIB || lc->cmd == LC_LOAD_WEAK_DYLIB) {
            const dylib_command_t *dl = (const dylib_command_t *)cmd_ptr;
            uint32_t ord = ++out_img->num_dylibs;
            if (ord < OH2_MAX_DYLIBS && dl->name_offset < lc->cmdsize) {
                const char *dname = (const char *)(cmd_ptr + dl->name_offset);
                snprintf(out_img->dylibs[ord], sizeof(out_img->dylibs[ord]), "%s", dname);
                out_img->dylib_weak[ord] = (lc->cmd == LC_LOAD_WEAK_DYLIB);
            }
        } else if (lc->cmd == LC_DYLD_INFO || lc->cmd == LC_DYLD_INFO_ONLY) {
            const dyld_info_command_t *di = (const dyld_info_command_t *)cmd_ptr;
            out_img->dyld_info.rebase_off     = di->rebase_off;
            out_img->dyld_info.rebase_size    = di->rebase_size;
            out_img->dyld_info.bind_off       = di->bind_off;
            out_img->dyld_info.bind_size      = di->bind_size;
            out_img->dyld_info.weak_bind_off  = di->weak_bind_off;
            out_img->dyld_info.weak_bind_size = di->weak_bind_size;
            out_img->dyld_info.lazy_bind_off  = di->lazy_bind_off;
            out_img->dyld_info.lazy_bind_size = di->lazy_bind_size;
            out_img->dyld_info.export_off     = di->export_off;
            out_img->dyld_info.export_size    = di->export_size;
        } else if (lc->cmd == LC_SYMTAB) {
            const symtab_command_t *st_cmd = (const symtab_command_t *)cmd_ptr;
            out_img->symoff  = st_cmd->symoff;
            out_img->nsyms   = st_cmd->nsyms;
            out_img->stroff  = st_cmd->stroff;
            out_img->strsize = st_cmd->strsize;
        } else if (lc->cmd == LC_DYSYMTAB) {
            const dysymtab_command_t *ds_cmd = (const dysymtab_command_t *)cmd_ptr;
            out_img->indirectsymoff = ds_cmd->indirectsymoff;
            out_img->nindirectsyms  = ds_cmd->nindirectsyms;
        } else if (lc->cmd == LC_ENCRYPTION_INFO_64) {
            const encryption_info_command_64_t *enc = (const encryption_info_command_64_t *)cmd_ptr;
            out_img->has_encryption_info = true;
            out_img->cryptoff  = enc->cryptoff;
            out_img->cryptsize = enc->cryptsize;
            out_img->cryptid   = enc->cryptid;
        } else if (lc->cmd == LC_MAIN) {
            const entry_point_command_t *ep = (const entry_point_command_t *)cmd_ptr;
            out_img->entry_offset = ep->entryoff;
        }

        cmd_ptr += lc->cmdsize;
    }

    if (out_img->has_encryption_info && out_img->cryptid != 0) {
        oh2_log_error("FATAL: FairPlay DRM encryption detected (cryptid=%u != 0). Aborting.",
                      out_img->cryptid);
        close(fd);
        out_img->fd = -1;
        return -2;
    }
    oh2_log_info("FairPlay check passed: LC_ENCRYPTION_INFO_64.cryptid=%u (UNENCRYPTED)",
                 out_img->cryptid);

    out_img->preferred_base = min_vmaddr;
    out_img->mapped_span    = (size_t)(max_vmaddr - min_vmaddr);

    void *res_base = mmap((void *)(uintptr_t)min_vmaddr,
                          out_img->mapped_span,
                          PROT_NONE,
                          MAP_PRIVATE | MAP_ANONYMOUS,
                          -1,
                          0);
    if (res_base == MAP_FAILED) {
        oh2_log_error("Failed to reserve %zu bytes for Mach-O image: %s",
                      out_img->mapped_span, strerror(errno));
        close(fd);
        out_img->fd = -1;
        return -1;
    }

    out_img->base_addr = res_base;
    out_img->slide     = (intptr_t)((uintptr_t)res_base - (uintptr_t)min_vmaddr);
    oh2_log_info("Reserved contiguous VA range: base=%p span=0x%zx slide=0x%lx",
                 out_img->base_addr, out_img->mapped_span, (long)out_img->slide);

    for (uint32_t i = 0; i < out_img->num_segments; ++i) {
        MachOSegment *seg = &out_img->segments[i];
        if (strcmp(seg->segname, "__PAGEZERO") == 0 || seg->vmsize == 0) {
            seg->mapped_addr = NULL;
            continue;
        }

        void *seg_target = (void *)((uintptr_t)seg->vmaddr + (uintptr_t)out_img->slide);
        int prot = 0;
        if (seg->initprot & 1) prot |= PROT_READ;
        if (seg->initprot & 2) prot |= PROT_WRITE;
        if (seg->initprot & 4) prot |= PROT_EXEC;

        if (seg->filesize > 0) {
            void *m = mmap(seg_target,
                           (size_t)seg->filesize,
                           prot,
                           MAP_PRIVATE | MAP_FIXED,
                           fd,
                           base_file_offset + (off_t)seg->fileoff);
            if (m == MAP_FAILED && (prot & PROT_EXEC)) {
                /*
                 * External storage (/storage/emulated/0/Android/data/...) is mounted noexec,
                 * and Android 10+ SELinux blocks file-backed PROT_EXEC from app writable dirs.
                 * Fall back to anonymous RW mapping + pread + ARM64 icache flush + mprotect(RX).
                 */
                size_t page_sz = (size_t)sysconf(_SC_PAGESIZE);
                size_t map_len = ((size_t)seg->filesize + page_sz - 1) & ~(page_sz - 1);
                m = mmap(seg_target,
                         map_len,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                         -1,
                         0);
                if (m != MAP_FAILED) {
                    size_t done = 0;
                    off_t src_off = base_file_offset + (off_t)seg->fileoff;
                    while (done < (size_t)seg->filesize) {
                        ssize_t r = pread(fd, (uint8_t *)m + done, (size_t)seg->filesize - done, src_off + (off_t)done);
                        if (r <= 0) {
                            if (r < 0 && errno == EINTR) continue;
                            m = MAP_FAILED;
                            break;
                        }
                        done += (size_t)r;
                    }
                    if (m != MAP_FAILED) {
                        __builtin___clear_cache((char *)m, (char *)m + seg->filesize);
                        if (mprotect(m, map_len, prot) != 0) {
                            oh2_log_error("mprotect(prot=%d) failed for anonymous fallback %s: %s",
                                          prot, seg->segname, strerror(errno));
                            m = MAP_FAILED;
                        } else {
                            oh2_log_info("Loaded executable segment %s via anonymous pread+mprotect fallback (%zu bytes)",
                                         seg->segname, (size_t)seg->filesize);
                        }
                    }
                }
            }
            if (m == MAP_FAILED) {
                oh2_log_error("Failed mapping segment %s at %p (filesize=0x%llx fileoff=0x%llx baseoff=0x%llx): %s",
                              seg->segname, seg_target,
                              (unsigned long long)seg->filesize,
                              (unsigned long long)seg->fileoff,
                              (unsigned long long)base_file_offset,
                              strerror(errno));
                return -1;
            }
        }

        if (seg->vmsize > seg->filesize) {
            size_t page_sz = (size_t)sysconf(_SC_PAGESIZE);
            size_t aligned_filesz = ((size_t)seg->filesize + page_sz - 1) & ~(page_sz - 1);
            if ((size_t)seg->vmsize > aligned_filesz) {
                void *tail_addr = (void *)((uintptr_t)seg_target + aligned_filesz);
                size_t tail_sz  = (size_t)seg->vmsize - aligned_filesz;
                void *tm = mmap(tail_addr,
                                tail_sz,
                                prot,
                                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                                -1,
                                0);
                if (tm == MAP_FAILED) {
                    oh2_log_error("Failed mapping zero-fill tail for %s at %p (size=0x%zx): %s",
                                  seg->segname, tail_addr, tail_sz, strerror(errno));
                    return -1;
                }
            }
        }

        seg->mapped_addr = seg_target;
        if (strcmp(seg->segname, "__LINKEDIT") == 0) {
            out_img->linkedit_file_base = (const uint8_t *)((uintptr_t)seg->mapped_addr - (uintptr_t)seg->fileoff);
        }
        oh2_log_info("Mapped segment %-12s -> %p (vmsize=0x%08llx filesize=0x%08llx prot=%d)",
                     seg->segname, seg->mapped_addr,
                     (unsigned long long)seg->vmsize,
                     (unsigned long long)seg->filesize,
                     prot);
    }

    for (uint32_t i = 0; i < out_img->num_sections; ++i) {
        MachOSection *sec = &out_img->sections[i];
        sec->mapped_addr = (void *)((uintptr_t)sec->addr + (uintptr_t)out_img->slide);
        if ((sec->flags & 0xffU) == S_MOD_INIT_FUNC_POINTERS) {
            out_img->mod_init_funcs = (void **)sec->mapped_addr;
            out_img->mod_init_count = (uint32_t)(sec->size / sizeof(void *));
        }
    }

    out_img->entry_point = (void *)((uintptr_t)out_img->base_addr + (uintptr_t)out_img->entry_offset);

    /* TLS Safety Verification: scan __TEXT,__text for MRS Xt, TPIDRRO_EL0 (0xd53bd060..0xd53bd07f) */
    const MachOSection *text_sec = macho_find_section(out_img, "__TEXT", "__text");
    if (text_sec && text_sec->mapped_addr) {
        const uint32_t *insns = (const uint32_t *)text_sec->mapped_addr;
        size_t word_count = (size_t)(text_sec->size / 4);
        uint32_t tpidrro_hits = 0;
        for (size_t w = 0; w < word_count; ++w) {
            if ((insns[w] & 0xffffffe0U) == 0xd53bd060U) {
                tpidrro_hits++;
            }
        }
        out_img->tpidrro_el0_reads = tpidrro_hits;
        if (tpidrro_hits != 0) {
            oh2_log_error("Unexpected MRS TPIDRRO_EL0 instructions found: %u", tpidrro_hits);
            return -1;
        }
        oh2_log_info("TLS safety verified: tpidrro_el0_reads=0 across %zu instructions (Bionic TPIDR_EL0 preserved)",
                     word_count);
    }

    g_active_image = out_img;
    oh2_symbol_resolver_init(out_img);

    if (dyld_apply_all_fixups(out_img) != 0) {
        oh2_log_error("dyld_apply_all_fixups failed");
        return -1;
    }

    if (oh2_unwind_register(out_img) != 0) {
        oh2_log_error("oh2_unwind_register failed");
        return -1;
    }

    if (oh2_objc_register_image(out_img) != 0) {
        oh2_log_error("oh2_objc_register_image failed");
        return -1;
    }

    return 0;
}

static inline uint16_t zip_u16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t zip_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int macho_load(const char *path, MachOImage *out_img) {
    return macho_load_at_offset(path, 0, 0, out_img);
}

int macho_load_from_apk(const char *apk_path, const char *entry_name, MachOImage *out_img) {
    if (!apk_path || !entry_name || !out_img) return -1;
    int fd = open(apk_path, O_RDONLY);
    if (fd < 0) {
        oh2_log_error("macho_load_from_apk: open('%s') failed: %s", apk_path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 22) {
        close(fd);
        return -1;
    }

    /* Search last 65 KiB for ZIP End of Central Directory (0x06054b50) */
    size_t tail_sz = (st.st_size > 66000) ? 66000 : (size_t)st.st_size;
    off_t tail_off = st.st_size - (off_t)tail_sz;
    uint8_t *tail = (uint8_t *)malloc(tail_sz);
    if (!tail) {
        close(fd);
        return -1;
    }
    if (pread(fd, tail, tail_sz, tail_off) != (ssize_t)tail_sz) {
        free(tail);
        close(fd);
        return -1;
    }

    off_t eocd_pos = -1;
    for (ssize_t i = (ssize_t)tail_sz - 22; i >= 0; --i) {
        if (tail[i] == 0x50 && tail[i + 1] == 0x4b && tail[i + 2] == 0x05 && tail[i + 3] == 0x06) {
            uint16_t comment_len = zip_u16(tail + i + 20);
            if ((size_t)i + 22 + comment_len == tail_sz) {
                eocd_pos = i;
                break;
            }
        }
    }
    if (eocd_pos < 0) {
        oh2_log_error("macho_load_from_apk: ZIP EOCD not found in '%s'", apk_path);
        free(tail);
        close(fd);
        return -1;
    }

    uint32_t cd_size = zip_u32(tail + eocd_pos + 12);
    uint32_t cd_off  = zip_u32(tail + eocd_pos + 16);
    free(tail);

    if ((uint64_t)cd_off + cd_size > (uint64_t)st.st_size || cd_size < 46) {
        oh2_log_error("macho_load_from_apk: invalid CD bounds in '%s'", apk_path);
        close(fd);
        return -1;
    }

    uint8_t *cd = (uint8_t *)malloc(cd_size);
    if (!cd) {
        close(fd);
        return -1;
    }
    if (pread(fd, cd, cd_size, (off_t)cd_off) != (ssize_t)cd_size) {
        free(cd);
        close(fd);
        return -1;
    }

    size_t target_len = strlen(entry_name);
    off_t found_data_off = -1;
    size_t found_size = 0;

    size_t pos = 0;
    while (pos + 46 <= cd_size) {
        if (zip_u32(cd + pos) != 0x02014b50U) break;
        uint16_t method    = zip_u16(cd + pos + 10);
        uint32_t uncomp_sz = zip_u32(cd + pos + 24);
        uint16_t name_len  = zip_u16(cd + pos + 28);
        uint16_t extra_len = zip_u16(cd + pos + 30);
        uint16_t cmt_len   = zip_u16(cd + pos + 32);
        uint32_t lfh_off   = zip_u32(cd + pos + 42);
        if (pos + 46 + name_len > cd_size) break;

        if (name_len == target_len && memcmp(cd + pos + 46, entry_name, target_len) == 0) {
            if (method != 0) {
                oh2_log_error("macho_load_from_apk: entry '%s' must be ZIP_STORED (method=%u)", entry_name, method);
                free(cd);
                close(fd);
                return -1;
            }
            uint8_t lfh[30];
            if (pread(fd, lfh, sizeof(lfh), (off_t)lfh_off) != sizeof(lfh) || zip_u32(lfh) != 0x04034b50U) {
                free(cd);
                close(fd);
                return -1;
            }
            uint16_t lfh_nlen = zip_u16(lfh + 26);
            uint16_t lfh_xlen = zip_u16(lfh + 28);
            found_data_off = (off_t)lfh_off + 30 + (off_t)lfh_nlen + (off_t)lfh_xlen;
            found_size = (size_t)uncomp_sz;
            break;
        }
        pos += 46 + name_len + extra_len + cmt_len;
    }

    free(cd);
    close(fd);

    if (found_data_off < 0) {
        oh2_log_error("macho_load_from_apk: entry '%s' not found in '%s'", entry_name, apk_path);
        return -1;
    }
    oh2_log_info("APK Mach-O entry '%s': data_offset=0x%llx size=%zu (mod16k=%lld)",
                 entry_name, (unsigned long long)found_data_off, found_size,
                 (long long)(found_data_off & 0x3FFFLL));
    return macho_load_at_offset(apk_path, found_data_off, found_size, out_img);
}

void *macho_lookup_export(const MachOImage *img, const char *sym_name) {
    if (!img || !sym_name || !img->linkedit_file_base || img->dyld_info.export_size == 0) {
        return NULL;
    }
    const uint8_t *trie_start = img->linkedit_file_base + img->dyld_info.export_off;
    const uint8_t *trie_end   = trie_start + img->dyld_info.export_size;
    const uint8_t *node       = trie_start;
    const char    *s          = sym_name;

    while (node < trie_end) {
        const uint8_t *p = node;
        uint64_t term_size = read_uleb128(&p, trie_end);
        const uint8_t *children = p + term_size;
        if (*s == '\0') {
            if (term_size == 0) return NULL;
            uint64_t flags = read_uleb128(&p, trie_end);
            (void)flags;
            uint64_t sym_off = read_uleb128(&p, trie_end);
            return (void *)((uintptr_t)img->base_addr + (uintptr_t)sym_off);
        }
        if (children >= trie_end) return NULL;
        uint8_t edge_count = *children++;
        const uint8_t *ep = children;
        const uint8_t *next_node = NULL;
        for (uint8_t i = 0; i < edge_count && ep < trie_end; ++i) {
            const char *edge_str = (const char *)ep;
            size_t edge_len = strlen(edge_str);
            ep += edge_len + 1;
            uint64_t child_off = read_uleb128(&ep, trie_end);
            if (strncmp(s, edge_str, edge_len) == 0) {
                s += edge_len;
                next_node = trie_start + child_off;
                break;
            }
        }
        if (!next_node) return NULL;
        node = next_node;
    }
    return NULL;
}

typedef void (*mod_init_fn_t)(int argc, char **argv, char **envp, char **apple, const void *vars);

int macho_run_initializers(MachOImage *img, int argc, char **argv, char **envp, char **apple) {
    if (!img) return -1;
    oh2_log_info("Running %u __mod_init_func initializers at %p...",
                 img->mod_init_count, (void *)img->mod_init_funcs);

    img->mod_init_executed = 0;
    for (uint32_t i = 0; i < img->mod_init_count; ++i) {
        mod_init_fn_t fn = (mod_init_fn_t)img->mod_init_funcs[i];
        if (!fn) continue;
        img->current_mod_init_idx = i;
        img->current_mod_init_fn  = (void *)fn;
        fn(argc, argv, envp, apple, NULL);
        img->mod_init_executed++;
    }
    img->current_mod_init_fn = NULL;

    oh2_log_info("Completed __mod_init_func initializers: executed=%u/%u",
                 img->mod_init_executed, img->mod_init_count);
    return 0;
}

typedef int (*macho_main_fn_t)(int argc, char **argv, char **envp, char **apple);

int macho_call_main(MachOImage *img, int argc, char **argv, char **envp, char **apple) {
    if (!img || !img->entry_point) return -1;
    oh2_log_info("Jumping to LC_MAIN entryoff=0x%llx va=%p",
                 (unsigned long long)img->entry_offset, img->entry_point);
    macho_main_fn_t main_fn = (macho_main_fn_t)img->entry_point;
    int rc = main_fn(argc, argv, envp, apple);
    oh2_log_info("LC_MAIN returned exit_code=%d", rc);
    return rc;
}

void macho_unload(MachOImage *img) {
    if (!img) return;
    if (img->base_addr && img->mapped_span > 0) {
        munmap(img->base_addr, img->mapped_span);
        img->base_addr = NULL;
    }
    if (img->fd >= 0) {
        close(img->fd);
        img->fd = -1;
    }
    if (g_active_image == img) {
        g_active_image = NULL;
    }
}
