#include "macho_loader.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <unordered_map>

extern "C" {
void __register_frame(const void *fde);
void __unw_add_dynamic_eh_frame_section(uintptr_t eh_frame_start);
int  __gxx_personality_v0(int version, int actions, uint64_t exception_class,
                          void *exception_object, void *context);
}

namespace {

struct UnwindInfoHeader {
    uint32_t version;
    uint32_t commonEncodingsArraySectionOffset;
    uint32_t commonEncodingsArrayCount;
    uint32_t personalityArraySectionOffset;
    uint32_t personalityArrayCount;
    uint32_t indexSectionOffset;
    uint32_t indexCount;
};

struct UnwindIndexEntry {
    uint32_t functionOffset;
    uint32_t secondLevelPagesSectionOffset;
    uint32_t lsdaIndexArraySectionOffset;
};

struct UnwindLsdaEntry {
    uint32_t functionOffset;
    uint32_t lsdaOffset;
};

struct CompactFuncEntry {
    uint32_t func_off;
    uint32_t next_off;
    uint32_t encoding;
    uint32_t lsda_off;
};

static uint8_t *g_synth_eh_frame_buf = nullptr;

static void append_u8(std::vector<uint8_t> &buf, uint8_t v) {
    buf.push_back(v);
}

static void append_u32(std::vector<uint8_t> &buf, uint32_t v) {
    uint8_t b[4];
    memcpy(b, &v, 4);
    buf.insert(buf.end(), b, b + 4);
}

static void append_u64(std::vector<uint8_t> &buf, uint64_t v) {
    uint8_t b[8];
    memcpy(b, &v, 8);
    buf.insert(buf.end(), b, b + 8);
}

static void append_uleb128(std::vector<uint8_t> &buf, uint64_t val) {
    do {
        uint8_t byte = (uint8_t)(val & 0x7fU);
        val >>= 7;
        if (val != 0) byte |= 0x80U;
        buf.push_back(byte);
    } while (val != 0);
}

static void emit_cfa_offset(std::vector<uint8_t> &insns, uint8_t reg, uint32_t factored_off) {
    if (reg < 64) {
        append_u8(insns, (uint8_t)(0x80U | reg)); /* DW_CFA_offset */
        append_uleb128(insns, factored_off);
    } else {
        append_u8(insns, 0x05U); /* DW_CFA_offset_extended */
        append_uleb128(insns, reg);
        append_uleb128(insns, factored_off);
    }
}

} // namespace

extern "C" int oh2_unwind_register(MachOImage *img) {
    if (!img) return -1;

    /* 1. Register the 19 existing FDEs in __TEXT,__eh_frame individually via __register_frame */
    const MachOSection *eh_sec = macho_find_section(img, "__TEXT", "__eh_frame");
    uint32_t eh_fdes = 0;
    if (eh_sec && eh_sec->mapped_addr && eh_sec->size >= 8) {
        const uint8_t *p   = (const uint8_t *)eh_sec->mapped_addr;
        const uint8_t *end = p + eh_sec->size;
        while (p + 8 <= end) {
            uint32_t len = 0;
            uint32_t cid = 0;
            memcpy(&len, p, 4);
            if (len == 0) break;
            const uint8_t *next = p + 4 + len;
            if (next > end) break;
            memcpy(&cid, p + 4, 4);
            if (cid != 0) {
                /* In .eh_frame, non-zero CIE pointer identifies an FDE */
                __register_frame(p);
                eh_fdes++;
            }
            p = next;
        }
    }
    img->eh_frame_fde_count = eh_fdes;

    /* 2. Parse __TEXT,__unwind_info and synthesize DWARF .eh_frame for all 3,951 non-DWARF functions */
    const MachOSection *uw_sec = macho_find_section(img, "__TEXT", "__unwind_info");
    if (!uw_sec || !uw_sec->mapped_addr || uw_sec->size < sizeof(UnwindInfoHeader)) {
        oh2_log_error("__TEXT,__unwind_info section missing or invalid");
        return -1;
    }

    const uint8_t *uw_base = (const uint8_t *)uw_sec->mapped_addr;
    const UnwindInfoHeader *hdr = (const UnwindInfoHeader *)uw_base;
    if (hdr->version != 1 || hdr->indexCount < 2) {
        oh2_log_error("Unsupported __unwind_info header: version=%u indexCount=%u",
                      hdr->version, hdr->indexCount);
        return -1;
    }

    const uint32_t *common_encs = (const uint32_t *)(uw_base + hdr->commonEncodingsArraySectionOffset);
    const UnwindIndexEntry *idxs = (const UnwindIndexEntry *)(uw_base + hdr->indexSectionOffset);

    uint32_t lsda_start_off = idxs[0].lsdaIndexArraySectionOffset;
    uint32_t lsda_end_off   = idxs[hdr->indexCount - 1].lsdaIndexArraySectionOffset;
    uint32_t lsda_count     = (lsda_end_off >= lsda_start_off)
                              ? ((lsda_end_off - lsda_start_off) / sizeof(UnwindLsdaEntry))
                              : 0;
    img->lsda_count = lsda_count;

    std::unordered_map<uint32_t, uint32_t> lsda_map;
    lsda_map.reserve(lsda_count + 16);
    const UnwindLsdaEntry *lsda_arr = (const UnwindLsdaEntry *)(uw_base + lsda_start_off);
    for (uint32_t i = 0; i < lsda_count; ++i) {
        lsda_map[lsda_arr[i].functionOffset] = lsda_arr[i].lsdaOffset;
    }

    std::vector<CompactFuncEntry> entries;
    entries.reserve(4096);

    for (uint32_t i = 0; i + 1 < hdr->indexCount; ++i) {
        uint32_t page_func_base = idxs[i].functionOffset;
        uint32_t page_off       = idxs[i].secondLevelPagesSectionOffset;
        if (page_off == 0 || page_off + 8 > uw_sec->size) continue;

        const uint8_t *page = uw_base + page_off;
        uint32_t kind = *(const uint32_t *)page;

        if (kind == 2) { /* UNWIND_SECOND_LEVEL_REGULAR */
            uint16_t entry_off   = *(const uint16_t *)(page + 4);
            uint16_t entry_count = *(const uint16_t *)(page + 6);
            const uint8_t *ep    = page + entry_off;
            for (uint16_t j = 0; j < entry_count; ++j) {
                uint32_t f_off = *(const uint32_t *)(ep + j * 8);
                uint32_t enc   = *(const uint32_t *)(ep + j * 8 + 4);
                auto it = lsda_map.find(f_off);
                uint32_t l_off = (it != lsda_map.end()) ? it->second : 0;
                entries.push_back({f_off, 0, enc, l_off});
            }
        } else if (kind == 3) { /* UNWIND_SECOND_LEVEL_COMPRESSED */
            uint16_t entry_off   = *(const uint16_t *)(page + 4);
            uint16_t entry_count = *(const uint16_t *)(page + 6);
            uint16_t encs_off    = *(const uint16_t *)(page + 8);
            uint16_t encs_count  = *(const uint16_t *)(page + 10);
            const uint32_t *ep   = (const uint32_t *)(page + entry_off);
            const uint32_t *penc = (const uint32_t *)(page + encs_off);

            for (uint16_t j = 0; j < entry_count; ++j) {
                uint32_t v     = ep[j];
                uint32_t f_off = page_func_base + (v & 0x00ffffffU);
                uint32_t e_idx = (v >> 24) & 0xffU;
                uint32_t enc   = 0;
                if (e_idx < hdr->commonEncodingsArrayCount) {
                    enc = common_encs[e_idx];
                } else if (e_idx - hdr->commonEncodingsArrayCount < encs_count) {
                    enc = penc[e_idx - hdr->commonEncodingsArrayCount];
                }
                auto it = lsda_map.find(f_off);
                uint32_t l_off = (it != lsda_map.end()) ? it->second : 0;
                entries.push_back({f_off, 0, enc, l_off});
            }
        }
    }

    uint32_t sentinel_end_off = idxs[hdr->indexCount - 1].functionOffset;
    for (size_t i = 0; i < entries.size(); ++i) {
        entries[i].next_off = (i + 1 < entries.size()) ? entries[i + 1].func_off : sentinel_end_off;
    }

    /* Build synthesized .eh_frame buffer with CIE (aug="zPLR") + 3,951 FDEs + 0 terminator */
    std::vector<uint8_t> eh_buf;
    eh_buf.reserve(256 * 1024);

    /* Emit CIE at offset 0 */
    size_t cie_start = eh_buf.size();
    append_u32(eh_buf, 0); /* placeholder for CIE length */
    append_u32(eh_buf, 0); /* CIE ID = 0 */
    append_u8(eh_buf, 1);  /* version = 1 */
    const char aug[] = "zPLR";
    eh_buf.insert(eh_buf.end(), aug, aug + 5); /* "zPLR\0" */
    append_u8(eh_buf, 1);      /* code_alignment_factor = 1 (ULEB128) */
    append_u8(eh_buf, 0x78U);  /* data_alignment_factor = -8 (SLEB128) */
    append_u8(eh_buf, 30);     /* return_address_register = x30 (LR) */
    append_u8(eh_buf, 11);     /* augmentation data length = 1 + 8 + 1 + 1 = 11 */
    append_u8(eh_buf, 0x00U);  /* P encoding = DW_EH_PE_absptr */
    append_u64(eh_buf, (uint64_t)(uintptr_t)&__gxx_personality_v0);
    append_u8(eh_buf, 0x00U);  /* L encoding = DW_EH_PE_absptr */
    append_u8(eh_buf, 0x00U);  /* R encoding = DW_EH_PE_absptr */
    /* Initial CFA instructions: DW_CFA_def_cfa(reg=31 [SP], off=0) */
    append_u8(eh_buf, 0x0cU);
    append_u8(eh_buf, 31U);
    append_u8(eh_buf, 0U);
    while ((eh_buf.size() - cie_start) % 8 != 0) {
        append_u8(eh_buf, 0x00U); /* DW_CFA_nop */
    }
    uint32_t cie_len = (uint32_t)(eh_buf.size() - cie_start - 4);
    memcpy(&eh_buf[cie_start], &cie_len, 4);

    /* Callee-saved register pairs for UNWIND_ARM64_MODE_FRAME bits 0..8 */
    static const uint8_t kFrameRegPairs[9][2] = {
        {20, 19}, {22, 21}, {24, 23}, {26, 25}, {28, 27},
        {73, 72}, {75, 74}, {77, 76}, {79, 78}
    };

    uint32_t synth_fdes = 0;
    std::vector<uint8_t> cfa_insns;
    cfa_insns.reserve(64);

    for (const auto &fn : entries) {
        uint32_t mode = (fn.encoding >> 24) & 0x0fU;
        if (mode == 3) {
            /* UNWIND_ARM64_MODE_DWARF: already covered by the 19 FDEs in __TEXT,__eh_frame */
            continue;
        }

        uint64_t pc_start = (uint64_t)(uintptr_t)img->base_addr + fn.func_off;
        uint64_t pc_range = (fn.next_off > fn.func_off) ? (fn.next_off - fn.func_off) : 4ULL;
        uint64_t lsda_va  = (fn.lsda_off != 0)
                            ? ((uint64_t)(uintptr_t)img->base_addr + fn.lsda_off)
                            : 0ULL;

        cfa_insns.clear();
        if (mode == 4) {
            /* UNWIND_ARM64_MODE_FRAME: CFA = x29 + 16, x30=[CFA-8], x29=[CFA-16] */
            append_u8(cfa_insns, 0x0cU); /* DW_CFA_def_cfa */
            append_uleb128(cfa_insns, 29U);
            append_uleb128(cfa_insns, 16U);
            emit_cfa_offset(cfa_insns, 30U, 1U); /* x30 at CFA - 1*8 */
            emit_cfa_offset(cfa_insns, 29U, 2U); /* x29 at CFA - 2*8 */

            uint32_t slot = 3;
            for (int bit = 0; bit < 9; ++bit) {
                if (fn.encoding & (1U << bit)) {
                    emit_cfa_offset(cfa_insns, kFrameRegPairs[bit][0], slot++);
                    emit_cfa_offset(cfa_insns, kFrameRegPairs[bit][1], slot++);
                }
            }
        } else if (mode == 2) {
            /* UNWIND_ARM64_MODE_FRAMELESS: stack_size = ((encoding >> 12) & 0xfff) * 16 */
            uint32_t stack_size = ((fn.encoding >> 12) & 0x0fffU) * 16U;
            if (stack_size > 0) {
                append_u8(cfa_insns, 0x0eU); /* DW_CFA_def_cfa_offset */
                append_uleb128(cfa_insns, stack_size);
            }
        }

        size_t fde_start = eh_buf.size();
        append_u32(eh_buf, 0); /* placeholder for FDE length */
        uint32_t cie_ptr = (uint32_t)((fde_start + 4) - cie_start);
        append_u32(eh_buf, cie_ptr);
        append_u64(eh_buf, pc_start);
        append_u64(eh_buf, pc_range);
        append_u8(eh_buf, 8U); /* augmentation data length = 8 (LSDA pointer) */
        append_u64(eh_buf, lsda_va);
        eh_buf.insert(eh_buf.end(), cfa_insns.begin(), cfa_insns.end());

        while ((eh_buf.size() - fde_start) % 8 != 0) {
            append_u8(eh_buf, 0x00U); /* DW_CFA_nop */
        }
        uint32_t fde_len = (uint32_t)(eh_buf.size() - fde_start - 4);
        memcpy(&eh_buf[fde_start], &fde_len, 4);
        synth_fdes++;
    }

    /* 4-byte zero terminator required by __unw_add_dynamic_eh_frame_section */
    append_u32(eh_buf, 0);

    free(g_synth_eh_frame_buf);
    g_synth_eh_frame_buf = (uint8_t *)malloc(eh_buf.size());
    if (!g_synth_eh_frame_buf) {
        oh2_log_error("Failed allocating %zu bytes for synthesized .eh_frame", eh_buf.size());
        return -1;
    }
    memcpy(g_synth_eh_frame_buf, eh_buf.data(), eh_buf.size());
    __unw_add_dynamic_eh_frame_section((uintptr_t)g_synth_eh_frame_buf);

    img->synth_fde_count = synth_fdes;
    oh2_log_info("Unwind bridge registered: eh_frame_fdes=%u compact_unwind_synth_fdes=%u lsdas=%u buf_bytes=%zu personality=%p",
                 img->eh_frame_fde_count,
                 img->synth_fde_count,
                 img->lsda_count,
                 eh_buf.size(),
                 (void *)&__gxx_personality_v0);

    return 0;
}
