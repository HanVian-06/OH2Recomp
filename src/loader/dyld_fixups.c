#include "macho_loader.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define REBASE_OPCODE_MASK                              0xF0U
#define REBASE_IMMEDIATE_MASK                           0x0FU
#define REBASE_OPCODE_DONE                              0x00U
#define REBASE_OPCODE_SET_TYPE_IMM                      0x10U
#define REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB       0x20U
#define REBASE_OPCODE_ADD_ADDR_ULEB                     0x30U
#define REBASE_OPCODE_ADD_ADDR_IMM_SCALED               0x40U
#define REBASE_OPCODE_DO_REBASE_IMM_TIMES               0x50U
#define REBASE_OPCODE_DO_REBASE_ULEB_TIMES              0x60U
#define REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB           0x70U
#define REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB 0x80U

#define BIND_OPCODE_MASK                                0xF0U
#define BIND_IMMEDIATE_MASK                             0x0FU
#define BIND_OPCODE_DONE                                0x00U
#define BIND_OPCODE_SET_DYLIB_ORDINAL_IMM               0x10U
#define BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB              0x20U
#define BIND_OPCODE_SET_DYLIB_SPECIAL_IMM               0x30U
#define BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM       0x40U
#define BIND_OPCODE_SET_TYPE_IMM                        0x50U
#define BIND_OPCODE_SET_ADDEND_SLEB                     0x60U
#define BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB         0x70U
#define BIND_OPCODE_ADD_ADDR_ULEB                       0x80U
#define BIND_OPCODE_DO_BIND                             0x90U
#define BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB               0xA0U
#define BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED         0xB0U
#define BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB    0xC0U

#define BIND_SYMBOL_FLAGS_WEAK_IMPORT                   0x01U

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

static int64_t read_sleb128(const uint8_t **pp, const uint8_t *end) {
    int64_t result = 0;
    int shift = 0;
    const uint8_t *p = *pp;
    uint8_t byte = 0;
    while (p < end) {
        byte = *p++;
        result |= (int64_t)(byte & 0x7fU) << shift;
        shift += 7;
        if ((byte & 0x80U) == 0) break;
    }
    if ((shift < 64) && (byte & 0x40U)) {
        result |= -(1LL << shift);
    }
    *pp = p;
    return result;
}

static int dyld_apply_rebases(MachOImage *img) {
    if (!img || !img->linkedit_file_base || img->dyld_info.rebase_size == 0) {
        return 0;
    }

    const uint8_t *start = img->linkedit_file_base + img->dyld_info.rebase_off;
    const uint8_t *end   = start + img->dyld_info.rebase_size;
    const uint8_t *p     = start;

    uint32_t seg_idx = 0;
    uint64_t seg_off = 0;
    uint8_t  type    = 1;

    while (p < end) {
        uint8_t byte   = *p++;
        uint8_t opcode = byte & REBASE_OPCODE_MASK;
        uint8_t imm    = byte & REBASE_IMMEDIATE_MASK;

        switch (opcode) {
            case REBASE_OPCODE_DONE:
                p = end;
                break;

            case REBASE_OPCODE_SET_TYPE_IMM:
                type = imm;
                (void)type;
                break;

            case REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                seg_idx = imm;
                seg_off = read_uleb128(&p, end);
                break;

            case REBASE_OPCODE_ADD_ADDR_ULEB:
                seg_off += read_uleb128(&p, end);
                break;

            case REBASE_OPCODE_ADD_ADDR_IMM_SCALED:
                seg_off += (uint64_t)imm * sizeof(uint64_t);
                break;

            case REBASE_OPCODE_DO_REBASE_IMM_TIMES:
                for (uint8_t i = 0; i < imm; ++i) {
                    if (seg_idx >= img->num_segments || !img->segments[seg_idx].mapped_addr) return -1;
                    uint64_t *slot = (uint64_t *)((uintptr_t)img->segments[seg_idx].mapped_addr + (uintptr_t)seg_off);
                    *slot = (uint64_t)((int64_t)(*slot) + (int64_t)img->slide);
                    img->rebase_count++;
                    seg_off += sizeof(uint64_t);
                }
                break;

            case REBASE_OPCODE_DO_REBASE_ULEB_TIMES: {
                uint64_t count = read_uleb128(&p, end);
                for (uint64_t i = 0; i < count; ++i) {
                    if (seg_idx >= img->num_segments || !img->segments[seg_idx].mapped_addr) return -1;
                    uint64_t *slot = (uint64_t *)((uintptr_t)img->segments[seg_idx].mapped_addr + (uintptr_t)seg_off);
                    *slot = (uint64_t)((int64_t)(*slot) + (int64_t)img->slide);
                    img->rebase_count++;
                    seg_off += sizeof(uint64_t);
                }
                break;
            }

            case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB: {
                if (seg_idx >= img->num_segments || !img->segments[seg_idx].mapped_addr) return -1;
                uint64_t *slot = (uint64_t *)((uintptr_t)img->segments[seg_idx].mapped_addr + (uintptr_t)seg_off);
                *slot = (uint64_t)((int64_t)(*slot) + (int64_t)img->slide);
                img->rebase_count++;
                seg_off += sizeof(uint64_t) + read_uleb128(&p, end);
                break;
            }

            case REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB: {
                uint64_t count = read_uleb128(&p, end);
                uint64_t skip  = read_uleb128(&p, end);
                for (uint64_t i = 0; i < count; ++i) {
                    if (seg_idx >= img->num_segments || !img->segments[seg_idx].mapped_addr) return -1;
                    uint64_t *slot = (uint64_t *)((uintptr_t)img->segments[seg_idx].mapped_addr + (uintptr_t)seg_off);
                    *slot = (uint64_t)((int64_t)(*slot) + (int64_t)img->slide);
                    img->rebase_count++;
                    seg_off += sizeof(uint64_t) + skip;
                }
                break;
            }

            default:
                oh2_log_error("Unsupported rebase opcode 0x%02x", byte);
                return -1;
        }
    }

    return 0;
}

static int do_single_bind(MachOImage *img,
                          uint32_t seg_idx,
                          uint64_t seg_off,
                          int ordinal,
                          const char *sym_name,
                          uint8_t sym_flags,
                          int64_t addend,
                          bool is_lazy) {
    if (seg_idx >= img->num_segments || !img->segments[seg_idx].mapped_addr || !sym_name) {
        oh2_log_error("Invalid bind state: seg_idx=%u sym_name=%s", seg_idx, sym_name ? sym_name : "NULL");
        return -1;
    }

    const char *dylib_path = NULL;
    bool is_weak_dylib = false;
    if (ordinal >= 1 && (uint32_t)ordinal <= img->num_dylibs) {
        dylib_path    = img->dylibs[ordinal];
        is_weak_dylib = img->dylib_weak[ordinal];
    }

    void *resolved = darwin_resolve_symbol(dylib_path, sym_name);
    if (!resolved) {
        bool weak_ok = ((sym_flags & BIND_SYMBOL_FLAGS_WEAK_IMPORT) != 0) || is_weak_dylib;
        if (!weak_ok) {
            img->missing_symbol_count++;
            oh2_log_error("MISSING SYMBOL: '%s' (dylib='%s' ord=%d)",
                          sym_name, dylib_path ? dylib_path : "?", ordinal);
        }
    }

    uint64_t *slot = (uint64_t *)((uintptr_t)img->segments[seg_idx].mapped_addr + (uintptr_t)seg_off);
    *slot = resolved ? (uint64_t)((int64_t)(uintptr_t)resolved + addend) : 0ULL;

    if (is_lazy) {
        img->lazy_bind_count++;
    } else {
        img->bind_count++;
        if (addend == 16) {
            img->bind_addend16_count++;
        }
    }
    return 0;
}

static int dyld_apply_binds(MachOImage *img, uint32_t off, uint32_t size, bool is_lazy) {
    if (!img || !img->linkedit_file_base || size == 0) {
        return 0;
    }

    const uint8_t *start = img->linkedit_file_base + off;
    const uint8_t *end   = start + size;
    const uint8_t *p     = start;

    uint32_t    seg_idx   = 0;
    uint64_t    seg_off   = 0;
    int         ordinal   = 0;
    const char *sym_name  = NULL;
    uint8_t     sym_flags = 0;
    uint8_t     type      = 1;
    int64_t     addend    = 0;

    while (p < end) {
        uint8_t byte   = *p++;
        uint8_t opcode = byte & BIND_OPCODE_MASK;
        uint8_t imm    = byte & BIND_IMMEDIATE_MASK;

        switch (opcode) {
            case BIND_OPCODE_DONE:
                if (!is_lazy) {
                    p = end;
                } else {
                    /* Lazy bind contains one DONE per record + trailing 0x00 padding */
                    addend = 0;
                }
                break;

            case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
                ordinal = (int)imm;
                break;

            case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
                ordinal = (int)read_uleb128(&p, end);
                break;

            case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
                ordinal = (imm == 0) ? 0 : (int)(int8_t)(0xF0U | imm);
                break;

            case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
                sym_flags = imm;
                sym_name  = (const char *)p;
                while (p < end && *p != '\0') p++;
                if (p < end) p++;
                break;

            case BIND_OPCODE_SET_TYPE_IMM:
                type = imm;
                (void)type;
                break;

            case BIND_OPCODE_SET_ADDEND_SLEB:
                /* Persist addend across DO_BIND* until next SET_ADDEND_SLEB */
                addend = read_sleb128(&p, end);
                break;

            case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                seg_idx = imm;
                seg_off = read_uleb128(&p, end);
                break;

            case BIND_OPCODE_ADD_ADDR_ULEB:
                seg_off = (uint64_t)((int64_t)seg_off + (int64_t)read_uleb128(&p, end));
                break;

            case BIND_OPCODE_DO_BIND:
                if (do_single_bind(img, seg_idx, seg_off, ordinal, sym_name, sym_flags, addend, is_lazy) != 0)
                    return -1;
                seg_off += sizeof(uint64_t);
                break;

            case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
                if (do_single_bind(img, seg_idx, seg_off, ordinal, sym_name, sym_flags, addend, is_lazy) != 0)
                    return -1;
                seg_off += sizeof(uint64_t) + read_uleb128(&p, end);
                break;

            case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
                if (do_single_bind(img, seg_idx, seg_off, ordinal, sym_name, sym_flags, addend, is_lazy) != 0)
                    return -1;
                seg_off += sizeof(uint64_t) + (uint64_t)imm * sizeof(uint64_t);
                break;

            case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB: {
                uint64_t count = read_uleb128(&p, end);
                uint64_t skip  = read_uleb128(&p, end);
                for (uint64_t i = 0; i < count; ++i) {
                    if (do_single_bind(img, seg_idx, seg_off, ordinal, sym_name, sym_flags, addend, is_lazy) != 0)
                        return -1;
                    seg_off += sizeof(uint64_t) + skip;
                }
                break;
            }

            default:
                oh2_log_error("Unsupported bind opcode 0x%02x (is_lazy=%d)", byte, (int)is_lazy);
                return -1;
        }
    }

    return 0;
}

static int dyld_apply_weak_binds(MachOImage *img) {
    if (!img || !img->linkedit_file_base || img->dyld_info.weak_bind_size == 0) {
        return 0;
    }

    const uint8_t *start = img->linkedit_file_base + img->dyld_info.weak_bind_off;
    const uint8_t *end   = start + img->dyld_info.weak_bind_size;
    const uint8_t *p     = start;

    uint32_t    seg_idx   = 0;
    uint64_t    seg_off   = 0;
    const char *sym_name  = NULL;
    uint8_t     sym_flags = 0;
    uint8_t     type      = 1;
    int64_t     addend    = 0;

    while (p < end) {
        uint8_t byte   = *p++;
        uint8_t opcode = byte & BIND_OPCODE_MASK;
        uint8_t imm    = byte & BIND_IMMEDIATE_MASK;

        switch (opcode) {
            case BIND_OPCODE_DONE:
                p = end;
                break;

            case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
                sym_flags = imm;
                (void)sym_flags;
                sym_name = (const char *)p;
                while (p < end && *p != '\0') p++;
                if (p < end) p++;
                break;

            case BIND_OPCODE_SET_TYPE_IMM:
                type = imm;
                (void)type;
                break;

            case BIND_OPCODE_SET_ADDEND_SLEB:
                addend = read_sleb128(&p, end);
                break;

            case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                seg_idx = imm;
                seg_off = read_uleb128(&p, end);
                break;

            case BIND_OPCODE_ADD_ADDR_ULEB:
                seg_off = (uint64_t)((int64_t)seg_off + (int64_t)read_uleb128(&p, end));
                break;

            case BIND_OPCODE_DO_BIND: {
                if (seg_idx >= img->num_segments || !img->segments[seg_idx].mapped_addr) return -1;
                uint64_t *slot = (uint64_t *)((uintptr_t)img->segments[seg_idx].mapped_addr + (uintptr_t)seg_off);
                if (*slot == 0 && sym_name) {
                    void *exp = macho_lookup_export(img, sym_name);
                    if (exp) *slot = (uint64_t)((int64_t)(uintptr_t)exp + addend);
                }
                img->weak_bind_count++;
                seg_off += sizeof(uint64_t);
                break;
            }

            case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB: {
                if (seg_idx >= img->num_segments || !img->segments[seg_idx].mapped_addr) return -1;
                uint64_t *slot = (uint64_t *)((uintptr_t)img->segments[seg_idx].mapped_addr + (uintptr_t)seg_off);
                if (*slot == 0 && sym_name) {
                    void *exp = macho_lookup_export(img, sym_name);
                    if (exp) *slot = (uint64_t)((int64_t)(uintptr_t)exp + addend);
                }
                img->weak_bind_count++;
                seg_off += sizeof(uint64_t) + read_uleb128(&p, end);
                break;
            }

            case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED: {
                if (seg_idx >= img->num_segments || !img->segments[seg_idx].mapped_addr) return -1;
                uint64_t *slot = (uint64_t *)((uintptr_t)img->segments[seg_idx].mapped_addr + (uintptr_t)seg_off);
                if (*slot == 0 && sym_name) {
                    void *exp = macho_lookup_export(img, sym_name);
                    if (exp) *slot = (uint64_t)((int64_t)(uintptr_t)exp + addend);
                }
                img->weak_bind_count++;
                seg_off += sizeof(uint64_t) + (uint64_t)imm * sizeof(uint64_t);
                break;
            }

            case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB: {
                uint64_t count = read_uleb128(&p, end);
                uint64_t skip  = read_uleb128(&p, end);
                for (uint64_t i = 0; i < count; ++i) {
                    if (seg_idx >= img->num_segments || !img->segments[seg_idx].mapped_addr) return -1;
                    uint64_t *slot = (uint64_t *)((uintptr_t)img->segments[seg_idx].mapped_addr + (uintptr_t)seg_off);
                    if (*slot == 0 && sym_name) {
                        void *exp = macho_lookup_export(img, sym_name);
                        if (exp) *slot = (uint64_t)((int64_t)(uintptr_t)exp + addend);
                    }
                    img->weak_bind_count++;
                    seg_off += sizeof(uint64_t) + skip;
                }
                break;
            }

            default:
                oh2_log_error("Unsupported weak_bind opcode 0x%02x", byte);
                return -1;
        }
    }

    return 0;
}

int dyld_apply_all_fixups(MachOImage *img) {
    if (!img) return -1;

    /* 1. REBASE (must run first so lazy_bind/weak_bind/mod_init_func pointers are slid) */
    if (dyld_apply_rebases(img) != 0) return -1;

    /* 2. BIND (preserves addend across DO_BIND* for all 219 +16 C++ RTTI vtables) */
    if (dyld_apply_binds(img, img->dyld_info.bind_off, img->dyld_info.bind_size, false) != 0) return -1;

    /* 3. LAZY_BIND (bound eagerly at load time across all 539 records) */
    if (dyld_apply_binds(img, img->dyld_info.lazy_bind_off, img->dyld_info.lazy_bind_size, true) != 0) return -1;

    /* 4. WEAK_BIND (preserves OH2's 672 rebased internal weak exports) */
    if (dyld_apply_weak_binds(img) != 0) return -1;

    oh2_log_info("LC_DYLD_INFO_ONLY fixups applied: rebase=%u bind=%u (addend16=%u) lazy_bind=%u weak_bind=%u missing_symbols=%u",
                 img->rebase_count,
                 img->bind_count,
                 img->bind_addend16_count,
                 img->lazy_bind_count,
                 img->weak_bind_count,
                 img->missing_symbol_count);

    if (img->missing_symbol_count > 0) {
        oh2_log_error("Fixup validation failed: missing_symbols=%u (expected 0)", img->missing_symbol_count);
        return -1;
    }

    return 0;
}
