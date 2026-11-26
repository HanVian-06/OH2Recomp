#include "bitcode_reader.h"
#include "sha256.h"

#include <algorithm>
#include <cstring>
#include <functional>

namespace oh2::air2spirv {

namespace {

static constexpr const char kChar6Table[] =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._";

static inline uint16_t rd_u16(const uint8_t *p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}
static inline uint32_t rd_u32(const uint8_t *p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
static inline uint64_t rd_u64(const uint8_t *p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

class BitReader {
public:
    BitReader(const uint8_t *data, size_t nbytes) : data_(data), nbytes_(nbytes) {}

    bool at_end() const { return byte_pos_ >= nbytes_ && bits_in_word_ == 0; }
    bool ok() const { return !eof_err_; }

    uint64_t read(int nbits) {
        if (nbits <= 0 || eof_err_) return 0;
        if (nbits > 32) {
            uint64_t lo = read(32);
            uint64_t hi = read(nbits - 32);
            return lo | (hi << 32);
        }
        while (bits_in_word_ < nbits) {
            if (byte_pos_ >= nbytes_) {
                eof_err_ = true;
                return 0;
            }
            if (bits_in_word_ <= 32 && byte_pos_ + 4 <= nbytes_) {
                uint64_t w = rd_u32(data_ + byte_pos_);
                cur_word_ |= (w << bits_in_word_);
                byte_pos_ += 4;
                bits_in_word_ += 32;
            } else {
                cur_word_ |= (uint64_t(data_[byte_pos_]) << bits_in_word_);
                byte_pos_ += 1;
                bits_in_word_ += 8;
            }
        }
        uint64_t mask = (nbits == 64) ? ~0ULL : ((1ULL << nbits) - 1ULL);
        uint64_t val = cur_word_ & mask;
        cur_word_ >>= nbits;
        bits_in_word_ -= nbits;
        return val;
    }

    uint64_t read_vbr(int width) {
        uint64_t val = 0;
        int shift = 0;
        uint64_t mask = (1ULL << (width - 1)) - 1ULL;
        uint64_t cont = 1ULL << (width - 1);
        while (!eof_err_) {
            uint64_t p = read(width);
            val |= (p & mask) << shift;
            if ((p & cont) == 0) return val;
            shift += width - 1;
        }
        return val;
    }

    void align32() {
        size_t bpos = byte_pos_ * 8 - size_t(bits_in_word_);
        int rem = int(bpos & 31);
        if (rem) read(32 - rem);
    }

    std::vector<uint8_t> read_bytes(size_t nbytes) {
        align32();
        size_t actual_byte_pos = byte_pos_ - size_t(bits_in_word_ / 8);
        std::vector<uint8_t> out;
        if (actual_byte_pos <= nbytes_) {
            size_t avail = std::min(nbytes, nbytes_ - actual_byte_pos);
            out.assign(data_ + actual_byte_pos, data_ + actual_byte_pos + avail);
        }
        size_t pad = (4 - (nbytes & 3)) & 3;
        byte_pos_ = std::min(nbytes_, actual_byte_pos + nbytes + pad);
        cur_word_ = 0;
        bits_in_word_ = 0;
        return out;
    }

private:
    const uint8_t *data_;
    size_t nbytes_;
    size_t byte_pos_ = 0;
    uint64_t cur_word_ = 0;
    int bits_in_word_ = 0;
    bool eof_err_ = false;
};

static inline int64_t decode_vbr_signed(uint64_t v) {
    return (v & 1ULL) ? -int64_t(v >> 1) : int64_t(v >> 1);
}

struct BitEvent {
    uint8_t ev_type = 0; /* 0=ENDBLOCK, 1=ENTER_SUBBLOCK, 2=RECORD */
    int32_t bid = 0;
    uint32_t code = 0;
    std::vector<uint64_t> ops;
    bool has_blob = false;
    std::vector<uint8_t> blob;
};

static std::string rec_str(const BitEvent &ev, size_t start_idx = 0) {
    if (ev.has_blob && start_idx == 0) {
        return std::string(reinterpret_cast<const char *>(ev.blob.data()), ev.blob.size());
    }
    std::string s;
    if (start_idx < ev.ops.size()) {
        s.reserve(ev.ops.size() - start_idx);
        for (size_t i = start_idx; i < ev.ops.size(); ++i) {
            s.push_back(static_cast<char>(ev.ops[i] & 0xFF));
        }
    }
    return s;
}

static std::vector<std::string> parse_md_strings(uint32_t num_strings, uint32_t strings_offset,
                                                 const std::vector<uint8_t> &blob) {
    std::vector<std::string> out;
    out.reserve(num_strings);
    if (strings_offset > blob.size()) return out;
    BitReader br(blob.data(), strings_offset);
    std::vector<uint32_t> lengths(num_strings);
    for (uint32_t i = 0; i < num_strings; ++i) {
        lengths[i] = static_cast<uint32_t>(br.read_vbr(6));
    }
    size_t pos = strings_offset;
    for (uint32_t ln : lengths) {
        if (pos + ln <= blob.size()) {
            out.emplace_back(reinterpret_cast<const char *>(blob.data() + pos), ln);
        } else {
            out.emplace_back();
        }
        pos += ln;
    }
    return out;
}

using AbbrevDef = std::vector<std::pair<int, int>>;

static bool parse_bitstream(const uint8_t *bc, size_t len, std::vector<BitEvent> &events, std::string &err) {
    if (len < 4 || bc[0] != 'B' || bc[1] != 'C' || bc[2] != 0xC0 || bc[3] != 0xDE) {
        err = "Invalid LLVM bitcode magic";
        return false;
    }
    BitReader br(bc + 4, len - 4);
    std::unordered_map<int32_t, std::vector<AbbrevDef>> blockinfo_abbrevs;
    struct StackFrame {
        int32_t bid;
        int aw;
        std::vector<AbbrevDef> abbrevs;
    };
    std::vector<StackFrame> stack;
    std::vector<AbbrevDef> cur_abbrevs;
    int cur_aw = 2;
    int32_t cur_bid = -1;
    int32_t blockinfo_target = -1;

    auto read_abbrev_def = [&]() -> AbbrevDef {
        uint64_t num_ops = br.read_vbr(5);
        AbbrevDef ops;
        ops.reserve(size_t(num_ops));
        for (uint64_t i = 0; i < num_ops; ++i) {
            if (br.read(1)) {
                ops.emplace_back(0, int(br.read_vbr(8)));
            } else {
                int enc = int(br.read(3));
                if (enc == 1 || enc == 2) {
                    ops.emplace_back(enc, int(br.read_vbr(5)));
                } else {
                    ops.emplace_back(enc, 0);
                }
            }
        }
        return ops;
    };

    auto decode_op = [&](int kind, int val) -> uint64_t {
        if (kind == 0) return uint64_t(val);
        if (kind == 1) return br.read(val);
        if (kind == 2) return br.read_vbr(val);
        if (kind == 4) {
            uint64_t idx = br.read(6);
            return uint64_t(uint8_t(kChar6Table[idx & 63]));
        }
        return 0;
    };

    while (!br.at_end() && br.ok()) {
        uint64_t aid = br.read(cur_aw);
        if (!br.ok()) break;
        if (aid == 0) {
            br.align32();
            BitEvent ev;
            ev.ev_type = 0;
            ev.bid = cur_bid;
            events.push_back(std::move(ev));
            if (stack.empty()) break;
            cur_bid = stack.back().bid;
            cur_aw = stack.back().aw;
            cur_abbrevs = std::move(stack.back().abbrevs);
            stack.pop_back();
        } else if (aid == 1) {
            int32_t sub_id = int32_t(br.read_vbr(8));
            int sub_aw = int(br.read_vbr(4));
            br.align32();
            (void)br.read(32); /* sub_len */
            BitEvent ev;
            ev.ev_type = 1;
            ev.bid = sub_id;
            events.push_back(std::move(ev));
            stack.push_back({cur_bid, cur_aw, std::move(cur_abbrevs)});
            cur_bid = sub_id;
            cur_aw = sub_aw;
            auto it = blockinfo_abbrevs.find(sub_id);
            cur_abbrevs = (it != blockinfo_abbrevs.end()) ? it->second : std::vector<AbbrevDef>{};
            if (sub_id == 0) blockinfo_target = -1;
        } else if (aid == 2) {
            AbbrevDef ops = read_abbrev_def();
            if (cur_bid == 0) {
                blockinfo_abbrevs[blockinfo_target].push_back(std::move(ops));
            } else {
                cur_abbrevs.push_back(std::move(ops));
            }
        } else if (aid == 3) {
            uint32_t code = uint32_t(br.read_vbr(6));
            uint64_t n = br.read_vbr(6);
            std::vector<uint64_t> ops_u(static_cast<size_t>(n));
            for (uint64_t i = 0; i < n; ++i) ops_u[size_t(i)] = br.read_vbr(6);
            if (cur_bid == 0 && code == 1) {
                blockinfo_target = ops_u.empty() ? -1 : int32_t(ops_u[0]);
            } else {
                BitEvent ev;
                ev.ev_type = 2;
                ev.bid = cur_bid;
                ev.code = code;
                ev.ops = std::move(ops_u);
                events.push_back(std::move(ev));
            }
        } else {
            size_t aidx = size_t(aid - 4);
            if (aidx >= cur_abbrevs.size()) {
                err = "Invalid abbrev index";
                return false;
            }
            const AbbrevDef &abbrev = cur_abbrevs[aidx];
            BitEvent ev;
            ev.ev_type = 2;
            ev.bid = cur_bid;
            bool got_code = false;
            for (size_t i = 0; i < abbrev.size(); ++i) {
                int k = abbrev[i].first;
                int v = abbrev[i].second;
                if (k == 3) {
                    int ek = abbrev[i + 1].first;
                    int ev_v = abbrev[i + 1].second;
                    uint64_t cnt = br.read_vbr(6);
                    for (uint64_t j = 0; j < cnt; ++j) {
                        uint64_t val = decode_op(ek, ev_v);
                        if (!got_code) { ev.code = uint32_t(val); got_code = true; }
                        else ev.ops.push_back(val);
                    }
                    break;
                } else if (k == 5) {
                    uint64_t ln = br.read_vbr(6);
                    ev.blob = br.read_bytes(size_t(ln));
                    ev.has_blob = true;
                    break;
                } else {
                    uint64_t val = decode_op(k, v);
                    if (!got_code) { ev.code = uint32_t(val); got_code = true; }
                    else ev.ops.push_back(val);
                }
            }
            events.push_back(std::move(ev));
        }
    }
    return true;
}

static inline uint32_t get_val_rel(const std::vector<uint64_t> &ops, size_t &idx, uint32_t cur_id) {
    uint64_t rel = ops[idx++];
    return static_cast<uint32_t>((uint64_t(cur_id) - rel) & 0xFFFFFFFFULL);
}

static inline void get_val_type_pair(const std::vector<uint64_t> &ops, size_t &idx, uint32_t cur_id,
                                     const std::vector<IRValue> &vals, uint32_t &out_vid, uint32_t &out_ty) {
    out_vid = get_val_rel(ops, idx, cur_id);
    if (out_vid < vals.size()) {
        out_ty = vals[out_vid].ty;
    } else {
        out_ty = uint32_t(ops[idx++]);
    }
}

static bool decode_constants(const std::vector<BitEvent> &events, size_t start, size_t end,
                             std::vector<IRValue> &vals, std::string &err) {
    uint32_t cur_ty = 0;
    for (size_t k = start; k < end; ++k) {
        const BitEvent &ev = events[k];
        if (ev.ev_type != 2) continue;
        IRValue v;
        v.ty = cur_ty;
        if (ev.code == 1) {
            cur_ty = uint32_t(ev.ops[0]);
        } else if (ev.code == 2) {
            v.kind = IRValKind::Null;
            vals.push_back(std::move(v));
        } else if (ev.code == 3) {
            v.kind = IRValKind::Undef;
            vals.push_back(std::move(v));
        } else if (ev.code == 4) {
            v.kind = IRValKind::Int;
            v.i64 = decode_vbr_signed(ev.ops[0]);
            vals.push_back(std::move(v));
        } else if (ev.code == 6) {
            v.kind = IRValKind::Float;
            v.i64 = int64_t(ev.ops[0]);
            vals.push_back(std::move(v));
        } else if (ev.code == 7) {
            v.kind = IRValKind::Agg;
            v.list = ev.ops;
            vals.push_back(std::move(v));
        } else if (ev.code == 22) {
            v.kind = IRValKind::Data;
            v.list = ev.ops;
            vals.push_back(std::move(v));
        } else {
            err = "Unknown constant record code " + std::to_string(ev.code);
            return false;
        }
    }
    return true;
}

static uint64_t ptr_key(uint32_t pointee, uint32_t addrspace) {
    return (uint64_t(pointee) << 32) | uint64_t(addrspace);
}

} // namespace

FMetalCodeHeader parse_fmetal_code_header(const uint8_t *hdr, size_t len) {
    FMetalCodeHeader info;
    info.header_size = uint32_t(len);
    if (len >= 32) {
        const uint8_t *p = hdr + (len - 32);
        info.frequency = rd_u32(p + 0);
        info.source_len = rd_u32(p + 4);
        info.source_crc = rd_u32(p + 8);
        info.local_size_x = rd_u32(p + 12);
        info.local_size_y = rd_u32(p + 16);
        info.local_size_z = rd_u32(p + 20);
    }
    return info;
}

bool parse_mtlb_container(const uint8_t *mtlb, size_t len, MtlbInfo &out, std::string &err) {
    if (len < 88 || std::memcmp(mtlb, "MTLB", 4) != 0) {
        err = "Invalid MTLB header";
        return false;
    }
    for (int i = 0; i < 4; ++i) out.mtlb_ver[i] = rd_u16(mtlb + 4 + i * 2);
    out.total_size = rd_u64(mtlb + 16);
    uint64_t fn_off = rd_u64(mtlb + 24);
    uint64_t fn_sz  = rd_u64(mtlb + 32);
    uint64_t bc_off = rd_u64(mtlb + 72);
    uint64_t bc_sz  = rd_u64(mtlb + 80);

    out.entry_name = "main";
    if (fn_off + 8 <= len) {
        size_t p = size_t(fn_off + 8);
        size_t end = std::min(len, size_t(fn_off + fn_sz));
        while (p + 4 <= end) {
            const char *tag = reinterpret_cast<const char *>(mtlb + p);
            p += 4;
            if (std::memcmp(tag, "ENDT", 4) == 0 || p + 2 > end) break;
            uint16_t tlen = rd_u16(mtlb + p);
            p += 2;
            if (p + tlen > end) break;
            if (std::memcmp(tag, "NAME", 4) == 0) {
                size_t slen = 0;
                while (slen < tlen && mtlb[p + slen] != 0) ++slen;
                if (slen > 0) out.entry_name.assign(reinterpret_cast<const char *>(mtlb + p), slen);
            }
            p += tlen;
        }
    }

    if (bc_off > len || bc_off + bc_sz > len) {
        err = "MTLB bitcode slice out of bounds";
        return false;
    }
    const uint8_t *bc_slice = mtlb + bc_off;
    size_t bc_slice_sz = size_t(bc_sz);
    out.wrapper_magic = (bc_slice_sz >= 20) ? rd_u32(bc_slice) : 0;
    if (out.wrapper_magic == 0x0B17C0DEu) {
        uint32_t inner_off = rd_u32(bc_slice + 8);
        uint32_t inner_sz  = rd_u32(bc_slice + 12);
        if (size_t(inner_off) + size_t(inner_sz) > bc_slice_sz) {
            err = "Bitcode wrapper inner slice out of bounds";
            return false;
        }
        out.raw_bc.assign(bc_slice + inner_off, bc_slice + inner_off + inner_sz);
    } else {
        out.raw_bc.assign(bc_slice, bc_slice + bc_slice_sz);
    }

    out.mtlb_sha256 = sha256_bytes(mtlb, len);
    out.shader_id = rd_u64(out.mtlb_sha256.data());
    return true;
}

bool IRModule::decode(const uint8_t *bc, size_t len, std::string &err) {
    std::vector<BitEvent> events;
    if (!parse_bitstream(bc, len, events, err)) return false;

    std::string pending_struct_name;
    bool has_pending_struct = false;
    for (const BitEvent &ev : events) {
        if (ev.ev_type == 2 && ev.bid == 17) {
            IRType t;
            switch (ev.code) {
                case 1: continue;
                case 2: t.kind = IRTypeKind::Void; break;
                case 3: t.kind = IRTypeKind::F32; break;
                case 5: t.kind = IRTypeKind::Label; break;
                case 6:
                    t.kind = IRTypeKind::Opaque;
                    if (has_pending_struct) { t.name = pending_struct_name; has_pending_struct = false; }
                    break;
                case 7:
                    t.kind = IRTypeKind::Int;
                    t.bits_or_len = uint32_t(ev.ops[0]);
                    break;
                case 8:
                    t.kind = IRTypeKind::Ptr;
                    t.elem_or_ret = uint32_t(ev.ops[0]);
                    t.bits_or_len = (ev.ops.size() > 1) ? uint32_t(ev.ops[1]) : 0;
                    break;
                case 10: t.kind = IRTypeKind::F16; break;
                case 11:
                    t.kind = IRTypeKind::Array;
                    t.bits_or_len = uint32_t(ev.ops[0]);
                    t.elem_or_ret = uint32_t(ev.ops[1]);
                    break;
                case 12:
                    t.kind = IRTypeKind::Vec;
                    t.bits_or_len = uint32_t(ev.ops[0]);
                    t.elem_or_ret = uint32_t(ev.ops[1]);
                    break;
                case 16: t.kind = IRTypeKind::Metadata; break;
                case 18:
                    t.kind = IRTypeKind::Struct;
                    t.flag = ev.ops[0] != 0;
                    for (size_t k = 1; k < ev.ops.size(); ++k) t.members.push_back(uint32_t(ev.ops[k]));
                    break;
                case 19:
                    pending_struct_name = rec_str(ev);
                    has_pending_struct = true;
                    continue;
                case 20:
                    t.kind = IRTypeKind::Struct;
                    if (has_pending_struct) { t.name = pending_struct_name; has_pending_struct = false; }
                    t.flag = ev.ops[0] != 0;
                    for (size_t k = 1; k < ev.ops.size(); ++k) t.members.push_back(uint32_t(ev.ops[k]));
                    break;
                case 21:
                    t.kind = IRTypeKind::Func;
                    t.flag = ev.ops[0] != 0;
                    t.elem_or_ret = uint32_t(ev.ops[1]);
                    for (size_t k = 2; k < ev.ops.size(); ++k) t.members.push_back(uint32_t(ev.ops[k]));
                    break;
                default:
                    err = "Unknown type code " + std::to_string(ev.code);
                    return false;
            }
            types.push_back(std::move(t));
        } else if (ev.ev_type == 2 && ev.bid == 14) {
            if (ev.code == 1 && !ev.ops.empty()) {
                vst[uint32_t(ev.ops[0])] = rec_str(ev, 1);
            } else if (ev.code == 3 && ev.ops.size() >= 2) {
                vst[uint32_t(ev.ops[0])] = rec_str(ev, 2);
            }
        }
    }

    std::unordered_map<uint64_t, uint32_t> ptr_map;
    for (uint32_t tid = 0; tid < types.size(); ++tid) {
        if (types[tid].kind == IRTypeKind::Ptr) {
            ptr_map[ptr_key(types[tid].elem_or_ret, types[tid].bits_or_len)] = tid;
        }
    }

    auto find_subblock_end = [&](size_t start) -> size_t {
        size_t j = start + 1;
        int depth = 1;
        while (j < events.size() && depth > 0) {
            if (events[j].ev_type == 1) depth++;
            else if (events[j].ev_type == 0) depth--;
            j++;
        }
        return j;
    };

    std::vector<IRFuncRecord> defined_funcs;
    std::string pending_md_name;
    bool has_pending_md_name = false;

    for (size_t i = 0; i < events.size();) {
        const BitEvent &ev = events[i];
        if (ev.ev_type == 2 && ev.bid == 8) {
            if (ev.code == 7) {
                uint32_t val_ty = uint32_t(ev.ops[0]);
                bool is_exp = (ev.ops[1] & 2ULL) != 0;
                uint32_t addrspace = is_exp ? uint32_t(ev.ops[1] >> 2) : 0;
                uint32_t pty = val_ty;
                if (is_exp) {
                    auto it = ptr_map.find(ptr_key(val_ty, addrspace));
                    if (it != ptr_map.end()) pty = it->second;
                }
                IRValue gv;
                gv.ty = pty;
                gv.kind = IRValKind::Global;
                gv.i64 = int64_t(values.size());
                gv.extra_ty = val_ty;
                gv.extra_u32 = addrspace;
                values.push_back(std::move(gv));
            } else if (ev.code == 8) {
                uint32_t fty_id = uint32_t(ev.ops[0]);
                uint32_t pty = fty_id;
                if (types[fty_id].kind == IRTypeKind::Ptr) {
                    pty = fty_id;
                    fty_id = types[fty_id].elem_or_ret;
                } else {
                    auto it = ptr_map.find(ptr_key(fty_id, 0));
                    if (it != ptr_map.end()) pty = it->second;
                }
                bool is_proto = ev.ops[2] != 0;
                uint32_t vid = uint32_t(values.size());
                IRFuncRecord finfo{vid, fty_id, pty, is_proto};
                IRValue fv;
                fv.ty = pty;
                fv.kind = IRValKind::Func;
                fv.i64 = int64_t(vid);
                fv.extra_ty = fty_id;
                fv.extra_u32 = pty;
                fv.is_proto = is_proto;
                values.push_back(std::move(fv));
                func_records.push_back(finfo);
                if (!is_proto) defined_funcs.push_back(finfo);
            }
            ++i;
        } else if (ev.ev_type == 1 && ev.bid == 11) {
            size_t j = find_subblock_end(i);
            if (!decode_constants(events, i + 1, j - 1, values, err)) return false;
            i = j;
        } else if (ev.ev_type == 1 && ev.bid == 15) {
            size_t j = find_subblock_end(i);
            for (size_t k = i + 1; k < j - 1; ++k) {
                const BitEvent &mev = events[k];
                if (mev.ev_type != 2) continue;
                if (mev.code == 35 && mev.ops.size() >= 2 && mev.has_blob) {
                    auto strs = parse_md_strings(uint32_t(mev.ops[0]), uint32_t(mev.ops[1]), mev.blob);
                    for (auto &s : strs) {
                        MDNode n;
                        n.kind = MDNodeKind::String;
                        n.str = std::move(s);
                        md_nodes.push_back(std::move(n));
                    }
                } else if (mev.code == 2 && mev.ops.size() >= 2) {
                    MDNode n;
                    n.kind = MDNodeKind::Val;
                    n.val_ty = uint32_t(mev.ops[0]);
                    n.val_id = uint32_t(mev.ops[1]);
                    md_nodes.push_back(std::move(n));
                } else if (mev.code == 3) {
                    MDNode n;
                    n.kind = MDNodeKind::Tuple;
                    n.elems.reserve(mev.ops.size());
                    for (uint64_t x : mev.ops) {
                        n.elems.push_back((x > 0) ? int32_t(x - 1) : -1);
                    }
                    md_nodes.push_back(std::move(n));
                } else if (mev.code == 4) {
                    pending_md_name = rec_str(mev);
                    has_pending_md_name = true;
                } else if (mev.code == 10 && has_pending_md_name) {
                    std::vector<int32_t> refs;
                    refs.reserve(mev.ops.size());
                    for (uint64_t x : mev.ops) refs.push_back(int32_t(x));
                    named_md[pending_md_name] = std::move(refs);
                }
            }
            i = j;
        } else if (ev.ev_type == 1 && ev.bid == 12) {
            size_t j = find_subblock_end(i);
            if (functions.size() >= defined_funcs.size()) {
                err = "Function body without record";
                return false;
            }
            const IRFuncRecord &finfo = defined_funcs[functions.size()];
            IRFunction fn;
            fn.info = finfo;
            auto itn = vst.find(finfo.val_id);
            fn.name = (itn != vst.end()) ? itn->second : ("fn_" + std::to_string(finfo.val_id));
            fn.vals = values;

            const auto &param_types = types[finfo.func_ty].members;
            for (size_t pi = 0; pi < param_types.size(); ++pi) {
                uint32_t vid = uint32_t(fn.vals.size());
                IRValue av;
                av.ty = param_types[pi];
                av.kind = IRValKind::Arg;
                av.i64 = int64_t(pi);
                fn.vals.push_back(std::move(av));
                fn.args.push_back(vid);
            }

            auto find_vec_ty = [&](uint32_t n_el, uint32_t el_ty) -> uint32_t {
                for (uint32_t tid = 0; tid < types.size(); ++tid) {
                    if (types[tid].kind == IRTypeKind::Vec &&
                        types[tid].bits_or_len == n_el && types[tid].elem_or_ret == el_ty) {
                        return tid;
                    }
                }
                IRType vt;
                vt.kind = IRTypeKind::Vec;
                vt.bits_or_len = n_el;
                vt.elem_or_ret = el_ty;
                types.push_back(vt);
                return uint32_t(types.size() - 1);
            };
            auto find_i1_ty = [&]() -> uint32_t {
                for (uint32_t tid = 0; tid < types.size(); ++tid) {
                    if (types[tid].kind == IRTypeKind::Int && types[tid].bits_or_len == 1) return tid;
                }
                IRType it1;
                it1.kind = IRTypeKind::Int;
                it1.bits_or_len = 1;
                types.push_back(it1);
                return uint32_t(types.size() - 1);
            };

            for (size_t k = i + 1; k < j - 1;) {
                const BitEvent &fev = events[k];
                if (fev.ev_type == 1 && fev.bid == 11) {
                    size_t endk = find_subblock_end(k);
                    if (!decode_constants(events, k + 1, endk - 1, fn.vals, err)) return false;
                    k = endk;
                    continue;
                } else if (fev.ev_type == 1 && (fev.bid == 15 || fev.bid == 16 || fev.bid == 14)) {
                    k = find_subblock_end(k);
                    continue;
                } else if (fev.ev_type != 2 || fev.bid != 12) {
                    ++k;
                    continue;
                }

                uint32_t cur_id = uint32_t(fn.vals.size());
                const auto &ops = fev.ops;
                size_t idx = 0;
                IRInst ins;
                auto push_val = [&](uint32_t ty) {
                    IRValue iv;
                    iv.ty = ty;
                    iv.kind = IRValKind::Inst;
                    iv.i64 = int64_t(fn.insts.size());
                    fn.vals.push_back(std::move(iv));
                };

                switch (fev.code) {
                    case 1: /* DECLAREBLOCKS */
                        break;
                    case 2: { /* BINOP */
                        uint32_t lhs, ty;
                        get_val_type_pair(ops, idx, cur_id, fn.vals, lhs, ty);
                        uint32_t rhs = get_val_rel(ops, idx, cur_id);
                        uint32_t opc = uint32_t(ops[idx]);
                        uint32_t fl = (idx + 1 < ops.size()) ? uint32_t(ops[idx + 1]) : 0;
                        ins.op = IROp::Binop;
                        ins.cid = int32_t(cur_id);
                        ins.ty = ty;
                        ins.sub_op = opc;
                        ins.flags = fl;
                        ins.a = lhs;
                        ins.b = rhs;
                        push_val(ty);
                        fn.insts.push_back(std::move(ins));
                        break;
                    }
                    case 3: { /* CAST */
                        uint32_t val, dummy;
                        get_val_type_pair(ops, idx, cur_id, fn.vals, val, dummy);
                        uint32_t dest_ty = uint32_t(ops[idx]);
                        uint32_t cast_opc = uint32_t(ops[idx + 1]);
                        ins.op = IROp::Cast;
                        ins.cid = int32_t(cur_id);
                        ins.ty = dest_ty;
                        ins.sub_op = cast_opc;
                        ins.a = val;
                        push_val(dest_ty);
                        fn.insts.push_back(std::move(ins));
                        break;
                    }
                    case 6: { /* EXTRACTELT */
                        uint32_t vec, vec_ty, elt_idx, dummy;
                        get_val_type_pair(ops, idx, cur_id, fn.vals, vec, vec_ty);
                        get_val_type_pair(ops, idx, cur_id, fn.vals, elt_idx, dummy);
                        uint32_t elem_ty = types[vec_ty].elem_or_ret;
                        ins.op = IROp::ExtractElt;
                        ins.cid = int32_t(cur_id);
                        ins.ty = elem_ty;
                        ins.a = vec;
                        ins.b = elt_idx;
                        push_val(elem_ty);
                        fn.insts.push_back(std::move(ins));
                        break;
                    }
                    case 7: { /* INSERTELT */
                        uint32_t vec, vec_ty, elt_idx, dummy;
                        get_val_type_pair(ops, idx, cur_id, fn.vals, vec, vec_ty);
                        uint32_t elt = get_val_rel(ops, idx, cur_id);
                        get_val_type_pair(ops, idx, cur_id, fn.vals, elt_idx, dummy);
                        ins.op = IROp::InsertElt;
                        ins.cid = int32_t(cur_id);
                        ins.ty = vec_ty;
                        ins.a = vec;
                        ins.b = elt;
                        ins.c = elt_idx;
                        push_val(vec_ty);
                        fn.insts.push_back(std::move(ins));
                        break;
                    }
                    case 8: { /* SHUFFLEVEC */
                        uint32_t v1, vec_ty, mask, mask_ty;
                        get_val_type_pair(ops, idx, cur_id, fn.vals, v1, vec_ty);
                        uint32_t v2 = get_val_rel(ops, idx, cur_id);
                        get_val_type_pair(ops, idx, cur_id, fn.vals, mask, mask_ty);
                        uint32_t elem_ty = types[vec_ty].elem_or_ret;
                        uint32_t mask_len = types[mask_ty].bits_or_len;
                        uint32_t res_ty = find_vec_ty(mask_len, elem_ty);
                        ins.op = IROp::ShuffleVec;
                        ins.cid = int32_t(cur_id);
                        ins.ty = res_ty;
                        ins.a = v1;
                        ins.b = v2;
                        ins.c = mask;
                        push_val(res_ty);
                        fn.insts.push_back(std::move(ins));
                        break;
                    }
                    case 10: { /* RET */
                        ins.op = IROp::Ret;
                        if (ops.empty()) {
                            ins.cid = -1;
                        } else {
                            uint32_t v, ty;
                            get_val_type_pair(ops, idx, cur_id, fn.vals, v, ty);
                            ins.cid = 0; /* non-void marker */
                            ins.ty = ty;
                            ins.a = v;
                        }
                        fn.insts.push_back(std::move(ins));
                        break;
                    }
                    case 11: { /* BR */
                        if (ops.size() == 1) {
                            ins.op = IROp::Br;
                            ins.a = uint32_t(ops[0]);
                        } else {
                            size_t cidx = 2;
                            uint32_t cond = get_val_rel(ops, cidx, cur_id);
                            ins.op = IROp::BrCond;
                            ins.a = cond;
                            ins.b = uint32_t(ops[0]);
                            ins.c = uint32_t(ops[1]);
                        }
                        fn.insts.push_back(std::move(ins));
                        break;
                    }
                    case 12: { /* SWITCH */
                        ins.op = IROp::Switch;
                        ins.ty = uint32_t(ops[0]);
                        size_t cidx = 1;
                        ins.a = get_val_rel(ops, cidx, cur_id);
                        ins.b = uint32_t(ops[2]);
                        for (size_t s = 3; s + 1 < ops.size(); s += 2) {
                            ins.pairs.emplace_back(uint32_t(ops[s]), uint32_t(ops[s + 1]));
                        }
                        fn.insts.push_back(std::move(ins));
                        break;
                    }
                    case 15: { /* UNREACHABLE */
                        ins.op = IROp::Unreachable;
                        fn.insts.push_back(std::move(ins));
                        break;
                    }
                    case 16: { /* PHI */
                        ins.op = IROp::Phi;
                        ins.cid = int32_t(cur_id);
                        ins.ty = uint32_t(ops[0]);
                        for (size_t s = 1; s + 1 < ops.size(); s += 2) {
                            int64_t srel = decode_vbr_signed(ops[s]);
                            uint32_t vid = static_cast<uint32_t>((int64_t(cur_id) - srel) & 0xFFFFFFFFLL);
                            ins.pairs.emplace_back(vid, uint32_t(ops[s + 1]));
                        }
                        push_val(ins.ty);
                        fn.insts.push_back(std::move(ins));
                        break;
                    }
                    case 19: { /* ALLOCA */
                        uint32_t alloc_ty = uint32_t(ops[0]);
                        uint64_t align_fl = ops[3];
                        bool in_alloca_or_exp = (align_fl & (1ULL << 6)) != 0;
                        uint32_t pty = alloc_ty;
                        uint32_t pointee_ty = alloc_ty;
                        if (in_alloca_or_exp) {
                            auto it = ptr_map.find(ptr_key(alloc_ty, 0));
                            if (it != ptr_map.end()) pty = it->second;
                            pointee_ty = alloc_ty;
                        } else {
                            pty = alloc_ty;
                            pointee_ty = (types[pty].kind == IRTypeKind::Ptr) ? types[pty].elem_or_ret : alloc_ty;
                        }
                        ins.op = IROp::Alloca;
                        ins.cid = int32_t(cur_id);
                        ins.ty = pty;
                        ins.sub_op = pointee_ty;
                        push_val(pty);
                        fn.insts.push_back(std::move(ins));
                        break;
                    }
                    case 20: { /* LOAD */
                        uint32_t ptr, pty;
                        get_val_type_pair(ops, idx, cur_id, fn.vals, ptr, pty);
                        uint32_t res_ty = (idx + 2 < ops.size()) ? uint32_t(ops[idx]) : types[pty].elem_or_ret;
                        ins.op = IROp::Load;
                        ins.cid = int32_t(cur_id);
                        ins.ty = res_ty;
                        ins.a = ptr;
                        push_val(res_ty);
                        fn.insts.push_back(std::move(ins));
                        break;
                    }
                    case 27: { /* INSERTVAL */
                        uint32_t agg, agg_ty, elt, dummy;
                        get_val_type_pair(ops, idx, cur_id, fn.vals, agg, agg_ty);
                        get_val_type_pair(ops, idx, cur_id, fn.vals, elt, dummy);
                        ins.op = IROp::InsertVal;
                        ins.cid = int32_t(cur_id);
                        ins.ty = agg_ty;
                        ins.a = agg;
                        ins.b = elt;
                        for (size_t s = idx; s < ops.size(); ++s) ins.args.push_back(uint32_t(ops[s]));
                        push_val(agg_ty);
                        fn.insts.push_back(std::move(ins));
                        break;
                    }
                    case 28: { /* CMP2 */
                        uint32_t lhs, lhs_ty;
                        get_val_type_pair(ops, idx, cur_id, fn.vals, lhs, lhs_ty);
                        uint32_t rhs = get_val_rel(ops, idx, cur_id);
                        uint32_t pred = uint32_t(ops[idx]);
                        uint32_t i1_ty = find_i1_ty();
                        uint32_t res_ty = (types[lhs_ty].kind == IRTypeKind::Vec)
                                              ? find_vec_ty(types[lhs_ty].bits_or_len, i1_ty)
                                              : i1_ty;
                        ins.op = IROp::Cmp2;
                        ins.cid = int32_t(cur_id);
                        ins.ty = res_ty;
                        ins.sub_op = pred;
                        ins.a = lhs;
                        ins.b = rhs;
                        push_val(res_ty);
                        fn.insts.push_back(std::move(ins));
                        break;
                    }
                    case 29: { /* VSELECT */
                        uint32_t tval, val_ty, cond, dummy;
                        get_val_type_pair(ops, idx, cur_id, fn.vals, tval, val_ty);
                        uint32_t fval = get_val_rel(ops, idx, cur_id);
                        get_val_type_pair(ops, idx, cur_id, fn.vals, cond, dummy);
                        ins.op = IROp::Select;
                        ins.cid = int32_t(cur_id);
                        ins.ty = val_ty;
                        ins.a = cond;
                        ins.b = tval;
                        ins.c = fval;
                        push_val(val_ty);
                        fn.insts.push_back(std::move(ins));
                        break;
                    }
                    case 34: { /* CALL */
                        uint64_t ccinfo = ops[1];
                        idx = 2;
                        if (ccinfo & (1ULL << 17)) idx += 1;
                        int32_t fn_ty_id = -1;
                        if (ccinfo & (1ULL << 15)) {
                            fn_ty_id = int32_t(ops[idx++]);
                        }
                        uint32_t fn_id, fn_ptr_ty;
                        get_val_type_pair(ops, idx, cur_id, fn.vals, fn_id, fn_ptr_ty);
                        if (fn_ty_id < 0) fn_ty_id = int32_t(types[fn_ptr_ty].elem_or_ret);
                        const IRType &fty = types[size_t(fn_ty_id)];
                        uint32_t ret_ty = fty.elem_or_ret;
                        ins.op = IROp::Call;
                        ins.ty = ret_ty;
                        ins.a = fn_id;
                        for (uint32_t aty : fty.members) {
                            if (types[aty].kind == IRTypeKind::Metadata) {
                                idx += 1;
                            } else {
                                ins.args.push_back(get_val_rel(ops, idx, cur_id));
                            }
                        }
                        if (types[ret_ty].kind != IRTypeKind::Void) {
                            ins.cid = int32_t(cur_id);
                            push_val(ret_ty);
                        } else {
                            ins.cid = -1;
                        }
                        fn.insts.push_back(std::move(ins));
                        break;
                    }
                    case 43: { /* GEP */
                        uint32_t base_ty = uint32_t(ops[1]);
                        idx = 2;
                        uint32_t ptr, pty;
                        get_val_type_pair(ops, idx, cur_id, fn.vals, ptr, pty);
                        uint32_t addrspace = types[pty].bits_or_len;
                        uint32_t cur_t = base_ty;
                        bool first = true;
                        while (idx < ops.size()) {
                            uint32_t ivid, dummy;
                            get_val_type_pair(ops, idx, cur_id, fn.vals, ivid, dummy);
                            ins.args.push_back(ivid);
                            if (!first) {
                                const IRType &ct = types[cur_t];
                                if (ct.kind == IRTypeKind::Array || ct.kind == IRTypeKind::Vec) {
                                    cur_t = ct.elem_or_ret;
                                } else if (ct.kind == IRTypeKind::Struct) {
                                    uint32_t f_idx = uint32_t(fn.vals[ivid].i64);
                                    cur_t = ct.members[f_idx];
                                }
                            }
                            first = false;
                        }
                        uint32_t res_pty;
                        auto it = ptr_map.find(ptr_key(cur_t, addrspace));
                        if (it != ptr_map.end()) {
                            res_pty = it->second;
                        } else {
                            IRType npt;
                            npt.kind = IRTypeKind::Ptr;
                            npt.elem_or_ret = cur_t;
                            npt.bits_or_len = addrspace;
                            types.push_back(npt);
                            res_pty = uint32_t(types.size() - 1);
                            ptr_map[ptr_key(cur_t, addrspace)] = res_pty;
                        }
                        ins.op = IROp::Gep;
                        ins.cid = int32_t(cur_id);
                        ins.ty = res_pty;
                        ins.sub_op = base_ty;
                        ins.a = ptr;
                        push_val(res_pty);
                        fn.insts.push_back(std::move(ins));
                        break;
                    }
                    case 44: { /* STORE */
                        uint32_t ptr, dummy1, val, dummy2;
                        get_val_type_pair(ops, idx, cur_id, fn.vals, ptr, dummy1);
                        get_val_type_pair(ops, idx, cur_id, fn.vals, val, dummy2);
                        ins.op = IROp::Store;
                        ins.a = ptr;
                        ins.b = val;
                        fn.insts.push_back(std::move(ins));
                        break;
                    }
                    default:
                        err = "Unknown function record code " + std::to_string(fev.code);
                        return false;
                }
                ++k;
            }

            std::vector<uint32_t> cur_bb;
            for (uint32_t ii = 0; ii < fn.insts.size(); ++ii) {
                cur_bb.push_back(ii);
                IROp o = fn.insts[ii].op;
                if (o == IROp::Ret || o == IROp::Br || o == IROp::BrCond ||
                    o == IROp::Switch || o == IROp::Unreachable) {
                    fn.bbs.push_back(std::move(cur_bb));
                    cur_bb.clear();
                }
            }
            if (!cur_bb.empty()) fn.bbs.push_back(std::move(cur_bb));

            functions.push_back(std::move(fn));
            i = j;
        } else {
            ++i;
        }
    }

    /* Compute type layouts */
    type_layouts.resize(types.size());
    std::vector<uint8_t> done(types.size(), 0);
    std::function<const IRTypeLayout &(uint32_t)> get_layout = [&](uint32_t tid) -> const IRTypeLayout & {
        if (done[tid]) return type_layouts[tid];
        done[tid] = 1;
        IRTypeLayout res;
        const IRType &t = types[tid];
        switch (t.kind) {
            case IRTypeKind::Void:
            case IRTypeKind::Label:
            case IRTypeKind::Metadata:
            case IRTypeKind::Opaque:
            case IRTypeKind::Func:
                res.size = 0; res.align = 1; break;
            case IRTypeKind::F32:
                res.size = 4; res.align = 4; break;
            case IRTypeKind::F16:
                res.size = 2; res.align = 2; break;
            case IRTypeKind::Int:
                if (t.bits_or_len <= 8) { res.size = 1; res.align = 1; }
                else if (t.bits_or_len <= 16) { res.size = 2; res.align = 2; }
                else if (t.bits_or_len <= 32) { res.size = 4; res.align = 4; }
                else { res.size = 8; res.align = 8; }
                break;
            case IRTypeKind::Ptr:
                res.size = 8; res.align = 8; break;
            case IRTypeKind::Vec: {
                const auto &el = get_layout(t.elem_or_ret);
                uint32_t alloc_sz = (t.bits_or_len == 3) ? (4 * el.size) : (t.bits_or_len * el.size);
                res.size = alloc_sz;
                res.align = std::max(1u, alloc_sz);
                break;
            }
            case IRTypeKind::Array: {
                const auto &el = get_layout(t.elem_or_ret);
                uint32_t stride = (el.size + el.align - 1) & ~(el.align - 1);
                res.size = t.bits_or_len * stride;
                res.align = el.align;
                break;
            }
            case IRTypeKind::Struct: {
                uint32_t off = 0, max_al = 1;
                res.member_offsets.reserve(t.members.size());
                for (uint32_t mty : t.members) {
                    const auto &ml = get_layout(mty);
                    if (!t.flag) {
                        off = (off + ml.align - 1) & ~(ml.align - 1);
                        if (ml.align > max_al) max_al = ml.align;
                    }
                    res.member_offsets.push_back(off);
                    off += ml.size;
                }
                if (!t.flag) off = (off + max_al - 1) & ~(max_al - 1);
                res.size = off;
                res.align = max_al;
                break;
            }
        }
        type_layouts[tid] = std::move(res);
        return type_layouts[tid];
    };
    for (uint32_t tid = 0; tid < types.size(); ++tid) (void)get_layout(tid);
    return true;
}

} // namespace oh2::air2spirv
