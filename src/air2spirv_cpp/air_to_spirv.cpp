#include "air_to_spirv.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace oh2::air2spirv {

namespace {

static constexpr uint32_t SPV_MAGIC       = 0x07230203u;
static constexpr uint32_t SPV_VERSION_1_3 = 0x00010300u;
static constexpr uint32_t SPV_GENERATOR   = 0x00000000u;

enum SpvOp : uint32_t {
    OP_EXT_INST_IMPORT = 11, OP_EXT_INST = 12, OP_MEMORY_MODEL = 14, OP_ENTRY_POINT = 15,
    OP_EXECUTION_MODE = 16, OP_CAPABILITY = 17, OP_TYPE_VOID = 19, OP_TYPE_BOOL = 20,
    OP_TYPE_INT = 21, OP_TYPE_FLOAT = 22, OP_TYPE_VECTOR = 23, OP_TYPE_IMAGE = 25,
    OP_TYPE_SAMPLER = 26, OP_TYPE_SAMPLED_IMAGE = 27, OP_TYPE_ARRAY = 28,
    OP_TYPE_RUNTIME_ARRAY = 29, OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32,
    OP_TYPE_FUNCTION = 33, OP_CONSTANT_TRUE = 41, OP_CONSTANT_FALSE = 42, OP_CONSTANT = 43,
    OP_CONSTANT_COMPOSITE = 44, OP_CONSTANT_NULL = 46, OP_FUNCTION = 54, OP_FUNCTION_END = 56,
    OP_VARIABLE = 59, OP_LOAD = 61, OP_STORE = 62, OP_ACCESS_CHAIN = 65,
    OP_DECORATE = 71, OP_MEMBER_DECORATE = 72, OP_VECTOR_EXTRACT_DYNAMIC = 77,
    OP_VECTOR_INSERT_DYNAMIC = 78, OP_VECTOR_SHUFFLE = 79, OP_COMPOSITE_CONSTRUCT = 80,
    OP_COMPOSITE_EXTRACT = 81, OP_COMPOSITE_INSERT = 82, OP_SAMPLED_IMAGE = 86,
    OP_IMAGE_SAMPLE_IMPLICIT_LOD = 87, OP_IMAGE_SAMPLE_EXPLICIT_LOD = 88, OP_IMAGE_FETCH = 95,
    OP_IMAGE_GATHER = 96, OP_IMAGE_QUERY_SIZE_LOD = 103, OP_CONVERT_F_TO_U = 109,
    OP_CONVERT_F_TO_S = 110, OP_CONVERT_S_TO_F = 111, OP_CONVERT_U_TO_F = 112,
    OP_BITCAST = 124, OP_SNEGATE = 126, OP_IADD = 128, OP_FADD = 129, OP_ISUB = 130,
    OP_FSUB = 131, OP_IMUL = 132, OP_FMUL = 133, OP_UDIV = 134, OP_SDIV = 135, OP_FDIV = 136,
    OP_UMOD = 137, OP_SREM = 138, OP_FREM = 140, OP_DOT = 148, OP_ANY = 154,
    OP_LOGICAL_EQUAL = 164, OP_LOGICAL_NOT_EQUAL = 165, OP_LOGICAL_OR = 166,
    OP_LOGICAL_AND = 167, OP_SELECT = 169, OP_IEQUAL = 170, OP_INOT_EQUAL = 171,
    OP_UGREATER_THAN = 172, OP_SGREATER_THAN = 173, OP_UGREATER_THAN_EQUAL = 174,
    OP_SGREATER_THAN_EQUAL = 175, OP_ULESS_THAN = 176, OP_SLESS_THAN = 177,
    OP_ULESS_THAN_EQUAL = 178, OP_SLESS_THAN_EQUAL = 179, OP_FORD_EQUAL = 180,
    OP_FUNORD_EQUAL = 181, OP_FORD_NOT_EQUAL = 182, OP_FUNORD_NOT_EQUAL = 183,
    OP_FORD_LESS_THAN = 184, OP_FUNORD_LESS_THAN = 185, OP_FORD_GREATER_THAN = 186,
    OP_FUNORD_GREATER_THAN = 187, OP_FORD_LESS_THAN_EQUAL = 188,
    OP_FUNORD_LESS_THAN_EQUAL = 189, OP_FORD_GREATER_THAN_EQUAL = 190,
    OP_FUNORD_GREATER_THAN_EQUAL = 191, OP_SHIFT_RIGHT_LOGICAL = 194,
    OP_SHIFT_RIGHT_ARITHMETIC = 195, OP_SHIFT_LEFT_LOGICAL = 196, OP_BITWISE_OR = 197,
    OP_BITWISE_XOR = 198, OP_BITWISE_AND = 199, OP_BIT_REVERSE = 204, OP_DPDX = 207,
    OP_DPDY = 208, OP_CONTROL_BARRIER = 224, OP_LOOP_MERGE = 246, OP_SELECTION_MERGE = 247,
    OP_LABEL = 248, OP_BRANCH = 249, OP_BRANCH_CONDITIONAL = 250, OP_KILL = 252, OP_RETURN = 253
};

static constexpr uint32_t SC_UNIFORM_CONSTANT = 0;
static constexpr uint32_t SC_INPUT            = 1;
static constexpr uint32_t SC_UNIFORM          = 2;
static constexpr uint32_t SC_OUTPUT           = 3;
static constexpr uint32_t SC_WORKGROUP        = 4;
static constexpr uint32_t SC_FUNCTION         = 7;
static constexpr uint32_t SC_STORAGE_BUFFER   = 12;

static constexpr uint32_t DEC_RELAXED_PRECISION = 0;
static constexpr uint32_t DEC_BLOCK             = 2;
static constexpr uint32_t DEC_ARRAY_STRIDE      = 6;
static constexpr uint32_t DEC_BUILTIN           = 11;
static constexpr uint32_t DEC_NOPERSPECTIVE     = 13;
static constexpr uint32_t DEC_FLAT              = 14;
static constexpr uint32_t DEC_NON_WRITABLE      = 24;
static constexpr uint32_t DEC_LOCATION          = 30;
static constexpr uint32_t DEC_BINDING           = 33;
static constexpr uint32_t DEC_DESCRIPTOR_SET    = 34;
static constexpr uint32_t DEC_OFFSET            = 35;

static constexpr uint32_t BUILTIN_POSITION             = 0;
static constexpr uint32_t BUILTIN_FRAG_COORD           = 15;
static constexpr uint32_t BUILTIN_FRONT_FACING         = 17;
static constexpr uint32_t BUILTIN_FRAG_DEPTH           = 22;
static constexpr uint32_t BUILTIN_WORKGROUP_ID         = 26;
static constexpr uint32_t BUILTIN_LOCAL_INVOCATION_ID  = 27;
static constexpr uint32_t BUILTIN_GLOBAL_INVOCATION_ID = 28;
static constexpr uint32_t BUILTIN_VERTEX_INDEX         = 42;
static constexpr uint32_t BUILTIN_INSTANCE_INDEX       = 43;

static constexpr uint32_t CAP_SHADER      = 1;
static constexpr uint32_t CAP_IMAGE_QUERY = 50;

enum GlslStd450 : uint32_t {
    GLSL_TRUNC = 3, GLSL_FABS = 4, GLSL_FSIGN = 6, GLSL_FLOOR = 8, GLSL_FRACT = 10,
    GLSL_SIN = 13, GLSL_COS = 14, GLSL_ATAN2 = 25, GLSL_POW = 26, GLSL_EXP = 27,
    GLSL_LOG = 28, GLSL_EXP2 = 29, GLSL_LOG2 = 30, GLSL_SQRT = 31, GLSL_INVERSE_SQRT = 32,
    GLSL_FMIN = 37, GLSL_UMIN = 38, GLSL_FMAX = 40, GLSL_FCLAMP = 43, GLSL_FMIX = 46,
    GLSL_FMA = 50, GLSL_PACK_UNORM_2X16 = 57, GLSL_PACK_HALF_2X16 = 58
};

static inline uint32_t encode_word(uint32_t opcode, uint32_t wc) {
    return ((wc & 0xFFFFu) << 16) | (opcode & 0xFFFFu);
}

static inline void append_inst(std::vector<uint32_t> &dst, uint32_t op, std::initializer_list<uint32_t> ops) {
    dst.push_back(encode_word(op, uint32_t(1 + ops.size())));
    dst.insert(dst.end(), ops.begin(), ops.end());
}

static inline void append_inst_vec(std::vector<uint32_t> &dst, uint32_t op, const std::vector<uint32_t> &ops) {
    dst.push_back(encode_word(op, uint32_t(1 + ops.size())));
    dst.insert(dst.end(), ops.begin(), ops.end());
}

static void append_str_words(std::vector<uint32_t> &dst, const std::string &s) {
    size_t n = s.size() + 1;
    size_t nw = (n + 3) / 4;
    for (size_t i = 0; i < nw; ++i) {
        uint32_t w = 0;
        for (size_t b = 0; b < 4; ++b) {
            size_t idx = i * 4 + b;
            uint8_t ch = (idx < s.size()) ? uint8_t(s[idx]) : 0;
            w |= (uint32_t(ch) << (b * 8));
        }
        dst.push_back(w);
    }
}

static uint32_t half_bits_to_float32_bits(uint32_t hbits) {
    uint32_t h = hbits & 0xFFFFu;
    uint32_t sign = (h & 0x8000u) << 16;
    uint32_t exp  = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x03FFu;
    if (exp == 0) {
        if (mant == 0) return sign;
        /* subnormal */
        int e = -14;
        while ((mant & 0x0400u) == 0) {
            mant <<= 1;
            e -= 1;
        }
        mant &= 0x03FFu;
        return sign | (uint32_t(e + 127) << 23) | (mant << 13);
    } else if (exp == 31) {
        if (mant == 0) return sign | 0x7F800000u; /* Inf */
        return 0x7FC00000u; /* canonical qNaN */
    }
    return sign | ((exp + 112u) << 23) | (mant << 13);
}

static int lookup_user_semantic_loc(const std::string &sem, int fallback) {
    static const std::unordered_map<std::string, int> kMap = {
        {"user(TEXCOORD0)", 0}, {"user(TEXCOORD1)", 1}, {"user(TEXCOORD2)", 2},
        {"user(TEXCOORD3)", 3}, {"user(TEXCOORD4)", 4}, {"user(TEXCOORD5)", 5},
        {"user(TEXCOORD6)", 6}, {"user(TEXCOORD7)", 7}, {"user(TEXCOORD8)", 8},
        {"user(TEXCOORD10)", 9}, {"user(TEXCOORD11)", 10},
        {"user(PARTICLE_SUBUVS)", 11}, {"user(PARTICLE_POSITION)", 12},
        {"user(TANGENTX)", 13}, {"user(COLOR0)", 13},
        {"user(TANGENTZ)", 14}, {"user(COLOR1)", 14},
        {"user(FRINGE_COORDS)", 15}, {"user(ORIGINAL_POSITION)", 15},
    };
    auto it = kMap.find(sem);
    return (it != kMap.end()) ? it->second : fallback;
}

static bool starts_with(const std::string &s, const char *p) {
    size_t n = std::strlen(p);
    return s.size() >= n && s.compare(0, n, p) == 0;
}

static bool ends_with(const std::string &s, const char *p) {
    size_t n = std::strlen(p);
    return s.size() >= n && s.compare(s.size() - n, n, p) == 0;
}

struct SpvVal {
    bool is_struct = false;
    uint32_t id = 0;
    std::vector<uint32_t> fields;
};

struct Reg2MemVar {
    bool is_struct = false;
    uint32_t fvar = 0;
    uint32_t spv_ty = 0;
    std::vector<std::pair<uint32_t, uint32_t>> field_vars; /* (fvar, spv_ty) */
};

enum class PtrBaseKind : uint8_t { Ubo, Ssbo, Wg, Alloca };
struct PtrBaseInfo {
    PtrBaseKind kind = PtrBaseKind::Alloca;
    uint32_t var_id = 0;
    uint32_t cap = 0;
};

struct TexArgInfo {
    uint32_t var_id = 0;
    uint32_t img_ty = 0;
    uint32_t sampled_ty = 0;
    uint32_t dim = 1;
    uint32_t ms = 0;
};

enum class ArgInitKind : uint8_t { Const, LoadInput, FbFetch };
struct ArgInit {
    ArgInitKind kind = ArgInitKind::Const;
    uint32_t a = 0, b = 0, c = 0, d = 0, e = 0;
};

struct OutputVar {
    uint32_t var_id = 0;
    uint32_t spv_ty = 0;
    uint32_t ir_ty = 0;
};

class SpirvBuilder {
public:
    SpirvBuilder(const IRModule &mod, const FMetalCodeHeader &fhdr, const MtlbInfo &mi)
        : mod_(mod), fhdr_(fhdr), mi_(mi), fn_(mod.functions[0]) {
        glsl450_id_ = alloc_id();
        main_fn_id_ = alloc_id();
        capabilities_.insert(CAP_SHADER);
    }

    bool build(std::vector<uint32_t> &out_words, ShaderReflection &out_refl);

private:
    uint32_t alloc_id() { return next_id_++; }

    void emit_dec(uint32_t tid, uint32_t dec) { append_inst(decorations_, OP_DECORATE, {tid, dec}); }
    void emit_dec(uint32_t tid, uint32_t dec, uint32_t lit) { append_inst(decorations_, OP_DECORATE, {tid, dec, lit}); }
    void emit_member_dec(uint32_t st, uint32_t mem, uint32_t dec) { append_inst(decorations_, OP_MEMBER_DECORATE, {st, mem, dec}); }
    void emit_member_dec(uint32_t st, uint32_t mem, uint32_t dec, uint32_t lit) { append_inst(decorations_, OP_MEMBER_DECORATE, {st, mem, dec, lit}); }

    uint32_t get_type(const std::vector<uint32_t> &key, uint32_t op, const std::vector<uint32_t> &ops_after_id) {
        auto it = type_cache_.find(key);
        if (it != type_cache_.end()) return it->second;
        uint32_t tid = alloc_id();
        type_cache_[key] = tid;
        std::vector<uint32_t> full;
        full.reserve(1 + ops_after_id.size());
        full.push_back(tid);
        full.insert(full.end(), ops_after_id.begin(), ops_after_id.end());
        append_inst_vec(type_const_globals_, op, full);
        return tid;
    }

    uint32_t t_void() { return get_type({1}, OP_TYPE_VOID, {}); }
    uint32_t t_bool() { return get_type({2}, OP_TYPE_BOOL, {}); }
    uint32_t t_u32()  { return get_type({3, 32, 0}, OP_TYPE_INT, {32, 0}); }
    uint32_t t_s32()  { return get_type({3, 32, 1}, OP_TYPE_INT, {32, 1}); }
    uint32_t t_f32()  { return get_type({4, 32}, OP_TYPE_FLOAT, {32}); }
    uint32_t t_vec(uint32_t el, uint32_t cnt) {
        if (cnt == 1) return el;
        return get_type({5, el, cnt}, OP_TYPE_VECTOR, {el, cnt});
    }
    uint32_t t_ptr(uint32_t sc, uint32_t pt) { return get_type({6, sc, pt}, OP_TYPE_POINTER, {sc, pt}); }
    uint32_t t_array(uint32_t el, uint32_t len, int stride = -1) {
        uint32_t lcid = c_u32(len);
        std::vector<uint32_t> key = {7, el, len, uint32_t(stride + 1)};
        auto it = type_cache_.find(key);
        if (it != type_cache_.end()) return it->second;
        uint32_t tid = alloc_id();
        type_cache_[key] = tid;
        append_inst(type_const_globals_, OP_TYPE_ARRAY, {tid, el, lcid});
        if (stride >= 0) emit_dec(tid, DEC_ARRAY_STRIDE, uint32_t(stride));
        return tid;
    }
    uint32_t t_runtime_array(uint32_t el, uint32_t stride) {
        std::vector<uint32_t> key = {8, el, stride};
        auto it = type_cache_.find(key);
        if (it != type_cache_.end()) return it->second;
        uint32_t tid = alloc_id();
        type_cache_[key] = tid;
        append_inst(type_const_globals_, OP_TYPE_RUNTIME_ARRAY, {tid, el});
        emit_dec(tid, DEC_ARRAY_STRIDE, stride);
        return tid;
    }
    uint32_t t_ubo_block(uint32_t num_vec4s) {
        uint32_t n = std::max(1u, num_vec4s);
        uint32_t arr = t_array(t_vec(t_f32(), 4), n, 16);
        std::vector<uint32_t> key = {9, n};
        auto it = type_cache_.find(key);
        if (it != type_cache_.end()) return it->second;
        uint32_t tid = alloc_id();
        type_cache_[key] = tid;
        append_inst(type_const_globals_, OP_TYPE_STRUCT, {tid, arr});
        emit_dec(tid, DEC_BLOCK);
        emit_member_dec(tid, 0, DEC_OFFSET, 0);
        return tid;
    }
    uint32_t t_ssbo_block(bool readonly) {
        uint32_t rta = t_runtime_array(t_u32(), 4);
        std::vector<uint32_t> key = {10, readonly ? 1u : 0u};
        auto it = type_cache_.find(key);
        if (it != type_cache_.end()) return it->second;
        uint32_t tid = alloc_id();
        type_cache_[key] = tid;
        append_inst(type_const_globals_, OP_TYPE_STRUCT, {tid, rta});
        emit_dec(tid, DEC_BLOCK);
        emit_member_dec(tid, 0, DEC_OFFSET, 0);
        if (readonly) emit_member_dec(tid, 0, DEC_NON_WRITABLE);
        return tid;
    }
    uint32_t t_sampler() { return get_type({11}, OP_TYPE_SAMPLER, {}); }
    uint32_t t_image(uint32_t sty, uint32_t dim, uint32_t depth, uint32_t arrayed, uint32_t ms) {
        return get_type({12, sty, dim, depth, arrayed, ms}, OP_TYPE_IMAGE, {sty, dim, depth, arrayed, ms, 1, 0});
    }
    uint32_t t_sampled_image(uint32_t ity) { return get_type({13, ity}, OP_TYPE_SAMPLED_IMAGE, {ity}); }
    uint32_t t_func_void() {
        uint32_t v = t_void();
        return get_type({14, v}, OP_TYPE_FUNCTION, {v});
    }

    uint32_t c_bool(bool val) {
        uint32_t bty = t_bool();
        std::vector<uint32_t> k = {1, val ? 1u : 0u};
        auto it = const_cache_.find(k);
        if (it != const_cache_.end()) return it->second;
        uint32_t cid = alloc_id();
        const_cache_[k] = cid;
        append_inst(type_const_globals_, val ? OP_CONSTANT_TRUE : OP_CONSTANT_FALSE, {bty, cid});
        return cid;
    }
    uint32_t c_u32(uint32_t val) {
        uint32_t uty = t_u32();
        std::vector<uint32_t> k = {2, val};
        auto it = const_cache_.find(k);
        if (it != const_cache_.end()) {
            known_u32_[it->second] = val;
            return it->second;
        }
        uint32_t cid = alloc_id();
        const_cache_[k] = cid;
        append_inst(type_const_globals_, OP_CONSTANT, {uty, cid, val});
        known_u32_[cid] = val;
        return cid;
    }
    uint32_t c_s32(uint32_t val) {
        uint32_t sty = t_s32();
        std::vector<uint32_t> k = {3, val};
        auto it = const_cache_.find(k);
        if (it != const_cache_.end()) return it->second;
        uint32_t cid = alloc_id();
        const_cache_[k] = cid;
        append_inst(type_const_globals_, OP_CONSTANT, {sty, cid, val});
        return cid;
    }
    uint32_t c_f32_bits(uint32_t bits) {
        uint32_t fty = t_f32();
        std::vector<uint32_t> k = {4, bits};
        auto it = const_cache_.find(k);
        if (it != const_cache_.end()) return it->second;
        uint32_t cid = alloc_id();
        const_cache_[k] = cid;
        append_inst(type_const_globals_, OP_CONSTANT, {fty, cid, bits});
        return cid;
    }
    uint32_t c_composite(uint32_t vty, const std::vector<uint32_t> &els) {
        std::vector<uint32_t> k;
        k.reserve(2 + els.size());
        k.push_back(5);
        k.push_back(vty);
        k.insert(k.end(), els.begin(), els.end());
        auto it = const_cache_.find(k);
        if (it != const_cache_.end()) return it->second;
        uint32_t cid = alloc_id();
        const_cache_[k] = cid;
        std::vector<uint32_t> ops;
        ops.reserve(2 + els.size());
        ops.push_back(vty);
        ops.push_back(cid);
        ops.insert(ops.end(), els.begin(), els.end());
        append_inst_vec(type_const_globals_, OP_CONSTANT_COMPOSITE, ops);
        return cid;
    }
    uint32_t c_null(uint32_t spv_ty) {
        std::vector<uint32_t> k = {6, spv_ty};
        auto it = const_cache_.find(k);
        if (it != const_cache_.end()) return it->second;
        uint32_t cid = alloc_id();
        const_cache_[k] = cid;
        append_inst(type_const_globals_, OP_CONSTANT_NULL, {spv_ty, cid});
        return cid;
    }

    uint32_t map_ir_type_to_spv(uint32_t ir_tid) {
        const IRType &t = mod_.types[ir_tid];
        switch (t.kind) {
            case IRTypeKind::Void: return t_void();
            case IRTypeKind::F16:
                entry_flags_ |= (1u << 3);
                return t_f32();
            case IRTypeKind::F32: return t_f32();
            case IRTypeKind::Int: return (t.bits_or_len == 1) ? t_bool() : t_u32();
            case IRTypeKind::Vec: return t_vec(map_ir_type_to_spv(t.elem_or_ret), t.bits_or_len);
            default: return t_u32();
        }
    }

    bool is_float_ir_type(uint32_t ir_tid) const {
        const IRType &t = mod_.types[ir_tid];
        if (t.kind == IRTypeKind::F32 || t.kind == IRTypeKind::F16) return true;
        if (t.kind == IRTypeKind::Vec) {
            const IRType &et = mod_.types[t.elem_or_ret];
            return et.kind == IRTypeKind::F32 || et.kind == IRTypeKind::F16;
        }
        return false;
    }

    bool is_bool_ir_type(uint32_t ir_tid) const {
        const IRType &t = mod_.types[ir_tid];
        if (t.kind == IRTypeKind::Int && t.bits_or_len == 1) return true;
        if (t.kind == IRTypeKind::Vec) {
            const IRType &et = mod_.types[t.elem_or_ret];
            return et.kind == IRTypeKind::Int && et.bits_or_len == 1;
        }
        return false;
    }

    SpvVal zero_for_ir_type(uint32_t ir_tid) {
        const IRType &t = mod_.types[ir_tid];
        SpvVal r;
        if (t.kind == IRTypeKind::Struct) {
            r.is_struct = true;
            for (uint32_t mt : t.members) r.fields.push_back(c_null(map_ir_type_to_spv(mt)));
            return r;
        }
        r.id = c_null(map_ir_type_to_spv(ir_tid));
        return r;
    }

    SpvVal const_from_ir_val(uint32_t vid, const std::vector<IRValue> &vals) {
        const IRValue &v = vals[vid];
        const IRType &t = mod_.types[v.ty];
        SpvVal r;
        if (t.kind == IRTypeKind::Struct) {
            r.is_struct = true;
            if (v.kind == IRValKind::Null || v.kind == IRValKind::Undef) {
                for (uint32_t mt : t.members) r.fields.push_back(c_null(map_ir_type_to_spv(mt)));
            } else if (v.kind == IRValKind::Agg) {
                for (uint64_t sub : v.list) r.fields.push_back(const_from_ir_val(uint32_t(sub), vals).id);
            }
            return r;
        }
        if (v.kind == IRValKind::Null || v.kind == IRValKind::Undef) {
            if (t.kind == IRTypeKind::Int && t.bits_or_len > 1) r.id = c_u32(0);
            else if (t.kind == IRTypeKind::F32 || t.kind == IRTypeKind::F16) r.id = c_f32_bits(0);
            else r.id = c_null(map_ir_type_to_spv(v.ty));
            return r;
        }
        if (v.kind == IRValKind::Int) {
            if (t.kind == IRTypeKind::Int && t.bits_or_len == 1) r.id = c_bool((v.i64 & 1) != 0);
            else if (t.kind == IRTypeKind::Int && t.bits_or_len < 32) r.id = c_u32(uint32_t(v.i64) & ((1u << t.bits_or_len) - 1u));
            else r.id = c_u32(uint32_t(v.i64));
            return r;
        }
        if (v.kind == IRValKind::Float) {
            if (t.kind == IRTypeKind::F16) {
                entry_flags_ |= (1u << 3);
                r.id = c_f32_bits(half_bits_to_float32_bits(uint32_t(v.i64)));
            } else {
                r.id = c_f32_bits(uint32_t(v.i64));
            }
            return r;
        }
        if (v.kind == IRValKind::Agg) {
            uint32_t spv_ty = map_ir_type_to_spv(v.ty);
            std::vector<uint32_t> els;
            els.reserve(v.list.size());
            for (uint64_t sub : v.list) els.push_back(const_from_ir_val(uint32_t(sub), vals).id);
            r.id = c_composite(spv_ty, els);
            return r;
        }
        if (v.kind == IRValKind::Data) {
            uint32_t spv_ty = map_ir_type_to_spv(v.ty);
            const IRType &et = mod_.types[t.elem_or_ret];
            std::vector<uint32_t> els;
            els.reserve(v.list.size());
            for (uint64_t raw_el : v.list) {
                if (et.kind == IRTypeKind::F16) {
                    entry_flags_ |= (1u << 3);
                    els.push_back(c_f32_bits(half_bits_to_float32_bits(uint32_t(raw_el))));
                } else if (et.kind == IRTypeKind::F32) {
                    els.push_back(c_f32_bits(uint32_t(raw_el)));
                } else if (et.kind == IRTypeKind::Int && et.bits_or_len == 1) {
                    els.push_back(c_bool((raw_el & 1) != 0));
                } else {
                    els.push_back(c_u32(uint32_t(raw_el)));
                }
            }
            r.id = c_composite(spv_ty, els);
            return r;
        }
        r.id = c_null(map_ir_type_to_spv(v.ty));
        return r;
    }

    int32_t parse_int_md(int32_t md_idx) const {
        if (md_idx < 0 || size_t(md_idx) >= mod_.md_nodes.size()) return 0;
        const MDNode &n = mod_.md_nodes[size_t(md_idx)];
        if (n.kind == MDNodeKind::Val && n.val_id < mod_.values.size()) {
            return int32_t(mod_.values[n.val_id].i64);
        }
        return 0;
    }

    const MDNode *get_md(int32_t idx) const {
        if (idx < 0 || size_t(idx) >= mod_.md_nodes.size()) return nullptr;
        return &mod_.md_nodes[size_t(idx)];
    }

    void setup_stage_and_io();

    uint32_t emit_intrinsic_call(const std::string &name, uint32_t res_spv_ty, uint32_t ret_ir_ty,
                                 const std::vector<uint32_t> &call_args,
                                 const std::function<SpvVal(uint32_t)> &get_val,
                                 const std::function<uint32_t(uint32_t, uint32_t, std::initializer_list<uint32_t>)> &emit_op,
                                 const std::function<uint32_t(uint32_t, uint32_t, const std::vector<uint32_t> &)> &emit_op_v,
                                 const std::function<uint32_t(uint32_t, uint32_t)> &emit_u32_shr,
                                 std::vector<uint32_t> &code);

    const IRModule &mod_;
    const FMetalCodeHeader &fhdr_;
    const MtlbInfo &mi_;
    const IRFunction &fn_;

    uint32_t next_id_ = 1;
    uint32_t glsl450_id_ = 0;
    uint32_t main_fn_id_ = 0;
    std::set<uint32_t> capabilities_;
    std::vector<uint32_t> decorations_;
    std::vector<uint32_t> type_const_globals_;
    std::vector<uint32_t> entry_interface_vars_;

    std::map<std::vector<uint32_t>, uint32_t> type_cache_;
    std::map<std::vector<uint32_t>, uint32_t> const_cache_;
    std::unordered_map<uint32_t, uint32_t> known_u32_;

    uint32_t stage_ = 0;
    uint32_t exec_model_ = 0;
    uint32_t desc_set_ = 0;
    std::vector<BindingRefl> bindings_refl_;
    std::vector<VertexAttrRefl> vattrs_refl_;
    uint32_t buffer_mask_ = 0;
    uint32_t texture_mask_ = 0;
    uint16_t sampler_mask_ = 0;
    uint16_t frag_out_mask_ = 0;
    uint16_t entry_flags_ = 0;
    uint16_t lx_ = 0, ly_ = 0, lz_ = 0;

    std::vector<OutputVar> output_vars_;
    std::unordered_map<uint32_t, ArgInit> arg_init_kind_;
    std::unordered_map<uint32_t, PtrBaseInfo> ptr_base_info_;
    std::unordered_map<uint32_t, TexArgInfo> tex_arg_info_;
    std::unordered_map<uint32_t, uint32_t> samp_arg_info_;
};

void SpirvBuilder::setup_stage_and_io() {
    std::string stage_key;
    if (mod_.named_md.count("air.vertex")) stage_key = "air.vertex";
    else if (mod_.named_md.count("air.fragment")) stage_key = "air.fragment";
    else if (mod_.named_md.count("air.kernel")) stage_key = "air.kernel";

    if (stage_key == "air.vertex") {
        stage_ = 0; exec_model_ = 0; desc_set_ = 0;
    } else if (stage_key == "air.fragment") {
        stage_ = 1; exec_model_ = 4; desc_set_ = 1;
    } else if (stage_key == "air.kernel") {
        stage_ = 2; exec_model_ = 5; desc_set_ = 0;
        lx_ = uint16_t(fhdr_.local_size_x ? fhdr_.local_size_x : 64);
        ly_ = uint16_t(fhdr_.local_size_y ? fhdr_.local_size_y : 1);
        lz_ = uint16_t(fhdr_.local_size_z ? fhdr_.local_size_z : 1);
    } else {
        uint32_t freq = fhdr_.frequency;
        stage_ = (freq == 0) ? 0 : ((freq == 5) ? 2 : 1);
        exec_model_ = (freq == 0) ? 0 : ((freq == 5) ? 5 : 4);
        desc_set_ = (stage_ == 1) ? 1 : 0;
        if (stage_ == 2) { lx_ = 64; ly_ = 1; lz_ = 1; }
    }

    const MDNode *entry_md = nullptr;
    if (!stage_key.empty()) {
        auto it = mod_.named_md.find(stage_key);
        if (it != mod_.named_md.end() && !it->second.empty()) entry_md = get_md(it->second[0]);
    }
    const MDNode *out_mds = (entry_md && entry_md->elems.size() >= 2) ? get_md(entry_md->elems[1]) : nullptr;
    const MDNode *in_mds  = (entry_md && entry_md->elems.size() >= 3) ? get_md(entry_md->elems[2]) : nullptr;

    /* 1. Stage outputs */
    uint32_t ret_ir_ty = mod_.types[fn_.info.func_ty].elem_or_ret;
    const IRType &rt = mod_.types[ret_ir_ty];
    std::vector<uint32_t> member_ir_tys;
    if (rt.kind == IRTypeKind::Struct) member_ir_tys = rt.members;
    else if (rt.kind != IRTypeKind::Void) member_ir_tys.push_back(ret_ir_ty);

    std::unordered_set<int> used_out_locs;
    for (size_t idx = 0; idx < member_ir_tys.size(); ++idx) {
        uint32_t m_ir_ty = member_ir_tys[idx];
        uint32_t spv_ty = map_ir_type_to_spv(m_ir_ty);
        uint32_t ptr_ty = t_ptr(SC_OUTPUT, spv_ty);
        uint32_t var_id = alloc_id();
        append_inst(type_const_globals_, OP_VARIABLE, {ptr_ty, var_id, SC_OUTPUT});
        entry_interface_vars_.push_back(var_id);

        const MDNode *omd = (out_mds && idx < out_mds->elems.size()) ? get_md(out_mds->elems[idx]) : nullptr;
        std::string kind, sem;
        bool has_nopersp = false;
        if (omd) {
            for (int32_t e : omd->elems) {
                const MDNode *en = get_md(e);
                if (!en || en->kind != MDNodeKind::String) continue;
                if (kind.empty() && starts_with(en->str, "air.")) kind = en->str;
                if (sem.empty() && starts_with(en->str, "user(")) sem = en->str;
                if (en->str == "air.no_perspective") has_nopersp = true;
            }
        }
        if (kind == "air.position") {
            emit_dec(var_id, DEC_BUILTIN, BUILTIN_POSITION);
        } else if (kind == "air.depth") {
            emit_dec(var_id, DEC_BUILTIN, BUILTIN_FRAG_DEPTH);
            entry_flags_ |= (1u << 4);
        } else if (kind == "air.render_target") {
            int rt_loc = (omd && omd->elems.size() > 1) ? parse_int_md(omd->elems[1]) : int(idx);
            while (used_out_locs.count(rt_loc)) rt_loc++;
            used_out_locs.insert(rt_loc);
            emit_dec(var_id, DEC_LOCATION, uint32_t(rt_loc));
            frag_out_mask_ |= uint16_t(1u << rt_loc);
            const IRType &mt = mod_.types[m_ir_ty];
            if (mt.kind == IRTypeKind::F16 ||
                (mt.kind == IRTypeKind::Vec && mod_.types[mt.elem_or_ret].kind == IRTypeKind::F16)) {
                emit_dec(var_id, DEC_RELAXED_PRECISION);
            }
        } else {
            int loc = lookup_user_semantic_loc(sem, int(idx));
            while (used_out_locs.count(loc)) loc = (loc + 1) & 15;
            used_out_locs.insert(loc);
            emit_dec(var_id, DEC_LOCATION, uint32_t(loc));
            if (!is_float_ir_type(m_ir_ty)) emit_dec(var_id, DEC_FLAT);
            else if (has_nopersp) emit_dec(var_id, DEC_NOPERSPECTIVE);
        }
        output_vars_.push_back({var_id, spv_ty, m_ir_ty});
    }

    /* 2. Stage inputs & resource descriptors */
    std::unordered_set<int> used_in_locs;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> builtin_in_cache;
    auto get_builtin_input = [&](uint32_t bi, uint32_t spv_ty) -> std::pair<uint32_t, uint32_t> {
        auto it = builtin_in_cache.find(bi);
        if (it != builtin_in_cache.end()) return it->second;
        uint32_t pty = t_ptr(SC_INPUT, spv_ty);
        uint32_t vid = alloc_id();
        append_inst(type_const_globals_, OP_VARIABLE, {pty, vid, SC_INPUT});
        entry_interface_vars_.push_back(vid);
        emit_dec(vid, DEC_BUILTIN, bi);
        builtin_in_cache[bi] = {vid, spv_ty};
        return {vid, spv_ty};
    };

    for (size_t arg_idx = 0; arg_idx < fn_.args.size(); ++arg_idx) {
        uint32_t arg_vid = fn_.args[arg_idx];
        uint32_t arg_ir_ty = fn_.vals[arg_vid].ty;
        const MDNode *amd = (in_mds && arg_idx < in_mds->elems.size()) ? get_md(in_mds->elems[arg_idx]) : nullptr;

        std::string kind, sem;
        bool has_nopersp = false, is_rw = false;
        if (amd) {
            for (int32_t e : amd->elems) {
                const MDNode *en = get_md(e);
                if (!en || en->kind != MDNodeKind::String) continue;
                if (kind.empty() && starts_with(en->str, "air.")) kind = en->str;
                if (sem.empty() && starts_with(en->str, "user(")) sem = en->str;
                if (en->str == "air.no_perspective") has_nopersp = true;
                if (en->str == "air.read_write") is_rw = true;
            }
        }

        if (kind == "air.vertex_input") {
            int loc = int(arg_idx);
            std::string arg_name = "in_attr_" + std::to_string(arg_idx);
            std::string arg_tname = "float4";
            if (amd) {
                for (size_t i = 0; i + 1 < amd->elems.size(); ++i) {
                    const MDNode *en = get_md(amd->elems[i]);
                    if (!en || en->kind != MDNodeKind::String) continue;
                    if (en->str == "air.location_index") loc = parse_int_md(amd->elems[i + 1]);
                    else if (en->str == "air.arg_name") {
                        const MDNode *vn = get_md(amd->elems[i + 1]);
                        if (vn && vn->kind == MDNodeKind::String) arg_name = vn->str;
                    } else if (en->str == "air.arg_type_name") {
                        const MDNode *vn = get_md(amd->elems[i + 1]);
                        if (vn && vn->kind == MDNodeKind::String) arg_tname = vn->str;
                    }
                }
            }
            while (used_in_locs.count(loc)) loc++;
            used_in_locs.insert(loc);
            uint32_t spv_ty = map_ir_type_to_spv(arg_ir_ty);
            uint32_t pty = t_ptr(SC_INPUT, spv_ty);
            uint32_t vid = alloc_id();
            append_inst(type_const_globals_, OP_VARIABLE, {pty, vid, SC_INPUT});
            entry_interface_vars_.push_back(vid);
            emit_dec(vid, DEC_LOCATION, uint32_t(loc));
            arg_init_kind_[arg_vid] = {ArgInitKind::LoadInput, vid, spv_ty, 0, 0, 0};

            uint8_t comp_ty = 0;
            if (arg_tname.find("half") != std::string::npos) comp_ty = 1;
            else if (arg_tname.find("uint") != std::string::npos) comp_ty = 3;
            else if (arg_tname.find("int") != std::string::npos) comp_ty = 2;
            const IRType &irt = mod_.types[arg_ir_ty];
            uint8_t n_comp = (irt.kind == IRTypeKind::Vec) ? uint8_t(irt.bits_or_len) : 1;
            vattrs_refl_.push_back({uint8_t(loc), comp_ty, n_comp, 0, arg_name});
        } else if (kind == "air.fragment_input") {
            int loc = lookup_user_semantic_loc(sem, int(arg_idx));
            while (used_in_locs.count(loc)) loc = (loc + 1) & 15;
            used_in_locs.insert(loc);
            uint32_t spv_ty = map_ir_type_to_spv(arg_ir_ty);
            uint32_t pty = t_ptr(SC_INPUT, spv_ty);
            uint32_t vid = alloc_id();
            append_inst(type_const_globals_, OP_VARIABLE, {pty, vid, SC_INPUT});
            entry_interface_vars_.push_back(vid);
            emit_dec(vid, DEC_LOCATION, uint32_t(loc));
            if (!is_float_ir_type(arg_ir_ty)) emit_dec(vid, DEC_FLAT);
            else if (has_nopersp) emit_dec(vid, DEC_NOPERSPECTIVE);
            arg_init_kind_[arg_vid] = {ArgInitKind::LoadInput, vid, spv_ty, 0, 0, 0};
        } else if (kind == "air.position") {
            auto [vid, bty] = get_builtin_input(BUILTIN_FRAG_COORD, t_vec(t_f32(), 4));
            arg_init_kind_[arg_vid] = {ArgInitKind::LoadInput, vid, bty, 0, 0, 0};
        } else if (kind == "air.front_facing") {
            auto [vid, bty] = get_builtin_input(BUILTIN_FRONT_FACING, t_bool());
            arg_init_kind_[arg_vid] = {ArgInitKind::LoadInput, vid, bty, 0, 0, 0};
        } else if (kind == "air.render_target") {
            uint32_t spv_ty = map_ir_type_to_spv(arg_ir_ty);
            auto [fc_vid, fc_ty] = get_builtin_input(BUILTIN_FRAG_COORD, t_vec(t_f32(), 4));
            const uint32_t fb_slot = 30;
            uint32_t sampled_ty = t_f32();
            uint32_t img_ty = t_image(sampled_ty, 1, 0, 0, 0);
            uint32_t pty = t_ptr(SC_UNIFORM_CONSTANT, img_ty);
            uint32_t tex_var_id = alloc_id();
            append_inst(type_const_globals_, OP_VARIABLE, {pty, tex_var_id, SC_UNIFORM_CONSTANT});
            emit_dec(tex_var_id, DEC_DESCRIPTOR_SET, desc_set_);
            emit_dec(tex_var_id, DEC_BINDING, 32 + fb_slot);
            texture_mask_ |= (1u << fb_slot);
            entry_flags_ |= (1u << 5); /* SPIRV_ENTRY_FLAG_FB_FETCH */

            /* Detect whether this shader reads RGB channels (not just .a SceneDepth) */
            std::unordered_set<uint32_t> fb_tainted = {arg_vid};
            bool fb_reads_rgb = false;
            for (const IRInst &ins : fn_.insts) {
                if (ins.op == IROp::Call) {
                    auto itn = mod_.vst.find(ins.a);
                    const std::string cname = (itn != mod_.vst.end()) ? itn->second : "";
                    if (starts_with(cname, "air.convert.f.") && !ins.args.empty() && fb_tainted.count(ins.args[0])) {
                        if (ins.cid >= 0) fb_tainted.insert(uint32_t(ins.cid));
                    } else {
                        for (uint32_t a : ins.args) if (fb_tainted.count(a)) { fb_reads_rgb = true; break; }
                    }
                } else if (ins.op == IROp::ExtractElt && fb_tainted.count(ins.a)) {
                    const IRValue &iv = fn_.vals[ins.b];
                    if ((iv.kind != IRValKind::Int && iv.kind != IRValKind::Null) || iv.i64 != 3) fb_reads_rgb = true;
                } else if (ins.op == IROp::ShuffleVec && (fb_tainted.count(ins.a) || fb_tainted.count(ins.b))) {
                    const IRValue &mv = fn_.vals[ins.c];
                    if (mv.kind == IRValKind::Data) {
                        for (uint64_t m : mv.list) if ((m & 3u) != 3u) { fb_reads_rgb = true; break; }
                    } else {
                        fb_reads_rgb = true;
                    }
                } else {
                    auto chk = [&](uint32_t v) { if (fb_tainted.count(v)) fb_reads_rgb = true; };
                    switch (ins.op) {
                        case IROp::Binop: case IROp::Cmp2: case IROp::Store: chk(ins.a); chk(ins.b); break;
                        case IROp::Cast: case IROp::Load: case IROp::BrCond: case IROp::Switch: chk(ins.a); break;
                        case IROp::Ret: if (ins.cid >= 0) chk(ins.a); break;
                        case IROp::InsertElt: case IROp::Select: chk(ins.a); chk(ins.b); chk(ins.c); break;
                        case IROp::InsertVal: chk(ins.a); chk(ins.b); break;
                        case IROp::Phi: for (auto &pr : ins.pairs) chk(pr.first); break;
                        case IROp::Gep: chk(ins.a); for (uint32_t x : ins.args) chk(x); break;
                        default: break;
                    }
                }
            }
            if (fb_reads_rgb) entry_flags_ |= (1u << 6); /* SPIRV_ENTRY_FLAG_FB_FETCH_RGB */

            bindings_refl_.push_back({uint8_t(desc_set_), uint8_t(32 + fb_slot), 2, uint8_t(fb_slot), 1, 2, 0, 1, 0});
            arg_init_kind_[arg_vid] = {ArgInitKind::FbFetch, fc_vid, fc_ty, tex_var_id, img_ty, spv_ty};
        } else if (kind == "air.vertex_id") {
            auto [vid, bty] = get_builtin_input(BUILTIN_VERTEX_INDEX, t_u32());
            arg_init_kind_[arg_vid] = {ArgInitKind::LoadInput, vid, bty, 0, 0, 0};
        } else if (kind == "air.instance_id") {
            auto [vid, bty] = get_builtin_input(BUILTIN_INSTANCE_INDEX, t_u32());
            arg_init_kind_[arg_vid] = {ArgInitKind::LoadInput, vid, bty, 0, 0, 0};
        } else if (kind == "air.thread_position_in_grid") {
            auto [vid, bty] = get_builtin_input(BUILTIN_GLOBAL_INVOCATION_ID, t_vec(t_u32(), 3));
            arg_init_kind_[arg_vid] = {ArgInitKind::LoadInput, vid, bty, 0, 0, 0};
        } else if (kind == "air.thread_position_in_threadgroup") {
            auto [vid, bty] = get_builtin_input(BUILTIN_LOCAL_INVOCATION_ID, t_vec(t_u32(), 3));
            arg_init_kind_[arg_vid] = {ArgInitKind::LoadInput, vid, bty, 0, 0, 0};
        } else if (kind == "air.threadgroup_position_in_grid") {
            auto [vid, bty] = get_builtin_input(BUILTIN_WORKGROUP_ID, t_vec(t_u32(), 3));
            arg_init_kind_[arg_vid] = {ArgInitKind::LoadInput, vid, bty, 0, 0, 0};
        } else if (kind == "air.buffer") {
            uint32_t slot = 0, buf_size = 0;
            if (amd) {
                for (size_t i = 0; i + 1 < amd->elems.size(); ++i) {
                    const MDNode *en = get_md(amd->elems[i]);
                    if (!en || en->kind != MDNodeKind::String) continue;
                    if (en->str == "air.location_index") slot = uint32_t(parse_int_md(amd->elems[i + 1]));
                    else if (en->str == "air.buffer_size") buf_size = uint32_t(parse_int_md(amd->elems[i + 1]));
                }
            }
            const IRType &irt = mod_.types[arg_ir_ty];
            if (irt.kind == IRTypeKind::Ptr && irt.bits_or_len == 1) is_rw = true;

            uint8_t desc_type;
            if (!is_rw && buf_size > 0) {
                uint32_t num_v4 = std::max(1u, (buf_size + 15u) / 16u);
                uint32_t blk_ty = t_ubo_block(num_v4);
                uint32_t pty = t_ptr(SC_UNIFORM, blk_ty);
                uint32_t vid = alloc_id();
                append_inst(type_const_globals_, OP_VARIABLE, {pty, vid, SC_UNIFORM});
                emit_dec(vid, DEC_DESCRIPTOR_SET, desc_set_);
                emit_dec(vid, DEC_BINDING, slot);
                ptr_base_info_[arg_vid] = {PtrBaseKind::Ubo, vid, num_v4};
                desc_type = 6;
            } else {
                uint32_t blk_ty = t_ssbo_block(!is_rw);
                uint32_t pty = t_ptr(SC_STORAGE_BUFFER, blk_ty);
                uint32_t vid = alloc_id();
                append_inst(type_const_globals_, OP_VARIABLE, {pty, vid, SC_STORAGE_BUFFER});
                emit_dec(vid, DEC_DESCRIPTOR_SET, desc_set_);
                emit_dec(vid, DEC_BINDING, slot);
                ptr_base_info_[arg_vid] = {PtrBaseKind::Ssbo, vid, 0};
                desc_type = 7;
            }
            arg_init_kind_[arg_vid] = {ArgInitKind::Const, c_u32(0), 0, 0, 0, 0};
            buffer_mask_ |= (1u << slot);
            bindings_refl_.push_back({uint8_t(desc_set_), uint8_t(slot), desc_type, uint8_t(slot), 0, 0, 0, uint8_t(is_rw ? 3 : 1), buf_size});
        } else if (kind == "air.texture") {
            uint32_t slot = 0;
            std::string tname = "texture2d<float, sample>";
            if (amd) {
                for (size_t i = 0; i + 1 < amd->elems.size(); ++i) {
                    const MDNode *en = get_md(amd->elems[i]);
                    if (!en || en->kind != MDNodeKind::String) continue;
                    if (en->str == "air.location_index") slot = uint32_t(parse_int_md(amd->elems[i + 1]));
                    else if (en->str == "air.arg_type_name") {
                        const MDNode *vn = get_md(amd->elems[i + 1]);
                        if (vn && vn->kind == MDNodeKind::String) tname = vn->str;
                    }
                }
            }
            uint32_t sampled_ty = (tname.find("uint") != std::string::npos) ? t_u32() : t_f32();
            uint32_t spv_dim = 1, arrayed = 0, ms = 0;
            uint8_t refl_dim = 2;
            if (tname.find("texturecube") != std::string::npos) { spv_dim = 3; refl_dim = 4; }
            else if (tname.find("texture3d") != std::string::npos) { spv_dim = 2; refl_dim = 3; }
            else if (tname.find("texture2d_array") != std::string::npos) { spv_dim = 1; arrayed = 1; refl_dim = 5; }
            else if (tname.find("texture2d_ms") != std::string::npos) { spv_dim = 1; ms = 1; refl_dim = 6; }

            uint32_t img_ty = t_image(sampled_ty, spv_dim, 0, arrayed, ms);
            uint32_t pty = t_ptr(SC_UNIFORM_CONSTANT, img_ty);
            uint32_t vid = alloc_id();
            append_inst(type_const_globals_, OP_VARIABLE, {pty, vid, SC_UNIFORM_CONSTANT});
            emit_dec(vid, DEC_DESCRIPTOR_SET, desc_set_);
            emit_dec(vid, DEC_BINDING, 32 + slot);
            tex_arg_info_[arg_vid] = {vid, img_ty, sampled_ty, spv_dim, ms};
            arg_init_kind_[arg_vid] = {ArgInitKind::Const, c_u32(0), 0, 0, 0, 0};
            texture_mask_ |= (1u << slot);
            uint8_t is_depth = (tname.find("depth") != std::string::npos) ? 1 : 0;
            bindings_refl_.push_back({uint8_t(desc_set_), uint8_t(32 + slot), 2, uint8_t(slot), 1, refl_dim, is_depth, 1, 0});
        } else if (kind == "air.sampler") {
            uint32_t slot = 0;
            if (amd) {
                for (size_t i = 0; i + 1 < amd->elems.size(); ++i) {
                    const MDNode *en = get_md(amd->elems[i]);
                    if (en && en->kind == MDNodeKind::String && en->str == "air.location_index") {
                        slot = uint32_t(parse_int_md(amd->elems[i + 1]));
                    }
                }
            }
            uint32_t sty = t_sampler();
            uint32_t pty = t_ptr(SC_UNIFORM_CONSTANT, sty);
            uint32_t vid = alloc_id();
            append_inst(type_const_globals_, OP_VARIABLE, {pty, vid, SC_UNIFORM_CONSTANT});
            emit_dec(vid, DEC_DESCRIPTOR_SET, desc_set_);
            emit_dec(vid, DEC_BINDING, 64 + slot);
            samp_arg_info_[arg_vid] = vid;
            arg_init_kind_[arg_vid] = {ArgInitKind::Const, c_u32(0), 0, 0, 0, 0};
            sampler_mask_ |= uint16_t(1u << slot);
            bindings_refl_.push_back({uint8_t(desc_set_), uint8_t(64 + slot), 0, uint8_t(slot), 2, 0, 0, 1, 0});
        } else {
            uint32_t spv_ty = map_ir_type_to_spv(arg_ir_ty);
            arg_init_kind_[arg_vid] = {ArgInitKind::Const, c_null(spv_ty), 0, 0, 0, 0};
        }
    }

    /* 3. Module-scope Workgroup (addrspace 3) globals */
    for (uint32_t vid = 0; vid < mod_.values.size(); ++vid) {
        const IRValue &gv = mod_.values[vid];
        if (gv.kind == IRValKind::Global && gv.extra_u32 == 3) {
            uint32_t byte_sz = std::max(4u, mod_.type_layouts[gv.extra_ty].size);
            uint32_t num_words = (byte_sz + 3u) / 4u;
            uint32_t arr_ty = t_array(t_u32(), num_words);
            uint32_t pty = t_ptr(SC_WORKGROUP, arr_ty);
            uint32_t g_var_id = alloc_id();
            append_inst(type_const_globals_, OP_VARIABLE, {pty, g_var_id, SC_WORKGROUP});
            ptr_base_info_[vid] = {PtrBaseKind::Wg, g_var_id, num_words};
            entry_flags_ |= (1u << 1);
        }
    }
}

bool SpirvBuilder::build(std::vector<uint32_t> &out_words, ShaderReflection &out_refl) {
    setup_stage_and_io();

    const auto &vals = fn_.vals;
    const auto &bbs = fn_.bbs;
    size_t num_bbs = bbs.size();

    /* 1. Alloca offsets & pointer base propagation */
    std::unordered_map<uint32_t, uint32_t> alloca_offsets;
    uint32_t total_alloca_bytes = 0;
    for (const IRInst &ins : fn_.insts) {
        if (ins.op == IROp::Alloca) {
            uint32_t sz = std::max(4u, mod_.type_layouts[ins.sub_op].size);
            uint32_t sz_aligned = (sz + 15u) & ~15u;
            alloca_offsets[uint32_t(ins.cid)] = total_alloca_bytes;
            total_alloca_bytes += sz_aligned;
        }
    }

    uint32_t alloca_var_id = 0;
    uint32_t alloca_words = (total_alloca_bytes > 0) ? std::max(1u, (total_alloca_bytes + 3u) / 4u) : 0;
    std::vector<uint32_t> entry_local_vars;
    if (alloca_words > 0) {
        uint32_t arr_ty = t_array(t_f32(), alloca_words);
        uint32_t pty = t_ptr(SC_FUNCTION, arr_ty);
        alloca_var_id = alloc_id();
        append_inst(entry_local_vars, OP_VARIABLE, {pty, alloca_var_id, SC_FUNCTION});
        for (const auto &kv : alloca_offsets) {
            ptr_base_info_[kv.first] = {PtrBaseKind::Alloca, alloca_var_id, alloca_words};
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (const IRInst &ins : fn_.insts) {
            if (ins.cid < 0) continue;
            uint32_t cid = uint32_t(ins.cid);
            if (ptr_base_info_.count(cid)) continue;
            if (ins.op == IROp::Gep || ins.op == IROp::Cast) {
                auto it = ptr_base_info_.find(ins.a);
                if (it != ptr_base_info_.end()) { ptr_base_info_[cid] = it->second; changed = true; }
            } else if (ins.op == IROp::Phi) {
                for (const auto &pr : ins.pairs) {
                    auto it = ptr_base_info_.find(pr.first);
                    if (it != ptr_base_info_.end()) { ptr_base_info_[cid] = it->second; changed = true; break; }
                }
            } else if (ins.op == IROp::Select) {
                auto it = ptr_base_info_.find(ins.b);
                if (it != ptr_base_info_.end()) { ptr_base_info_[cid] = it->second; changed = true; }
                else {
                    auto it2 = ptr_base_info_.find(ins.c);
                    if (it2 != ptr_base_info_.end()) { ptr_base_info_[cid] = it2->second; changed = true; }
                }
            }
        }
    }

    /* 2. Identify cross-block SSA values & phi nodes */
    std::unordered_map<uint32_t, size_t> def_bb_of_val;
    std::set<uint32_t> cross_block_vals;
    std::unordered_map<size_t, std::vector<uint32_t>> phi_insts_by_bb;

    for (size_t bi = 0; bi < num_bbs; ++bi) {
        for (uint32_t ii : bbs[bi]) {
            const IRInst &ins = fn_.insts[ii];
            if (ins.cid >= 0 && ins.op != IROp::Ret) {
                def_bb_of_val[uint32_t(ins.cid)] = bi;
            }
            if (ins.op == IROp::Phi) {
                cross_block_vals.insert(uint32_t(ins.cid));
                phi_insts_by_bb[bi].push_back(ii);
            }
        }
    }

    auto check_used = [&](uint32_t vid, size_t cur_bb) {
        auto it = def_bb_of_val.find(vid);
        if (it != def_bb_of_val.end() && it->second != cur_bb) cross_block_vals.insert(vid);
    };

    for (size_t bi = 0; bi < num_bbs; ++bi) {
        for (uint32_t ii : bbs[bi]) {
            const IRInst &ins = fn_.insts[ii];
            switch (ins.op) {
                case IROp::Binop: case IROp::ExtractElt: case IROp::ShuffleVec: case IROp::Store:
                case IROp::InsertVal: case IROp::Cmp2:
                    check_used(ins.a, bi); check_used(ins.b, bi); break;
                case IROp::Cast: case IROp::BrCond: case IROp::Switch: case IROp::Load:
                    check_used(ins.a, bi); break;
                case IROp::Ret:
                    if (ins.cid >= 0) check_used(ins.a, bi); break;
                case IROp::InsertElt: case IROp::Select:
                    check_used(ins.a, bi); check_used(ins.b, bi); check_used(ins.c, bi); break;
                case IROp::Phi:
                    for (const auto &pr : ins.pairs) {
                        if (def_bb_of_val.count(pr.first)) cross_block_vals.insert(pr.first);
                    }
                    break;
                case IROp::Call:
                    for (uint32_t a : ins.args) check_used(a, bi); break;
                case IROp::Gep:
                    check_used(ins.a, bi);
                    for (uint32_t iv : ins.args) check_used(iv, bi);
                    break;
                default: break;
            }
        }
    }

    std::unordered_map<uint32_t, Reg2MemVar> reg2mem_vars;
    for (uint32_t vid : cross_block_vals) {
        uint32_t ir_ty = vals[vid].ty;
        const IRType &t = mod_.types[ir_ty];
        Reg2MemVar rv;
        if (t.kind == IRTypeKind::Struct) {
            rv.is_struct = true;
            for (uint32_t mty : t.members) {
                uint32_t sty = map_ir_type_to_spv(mty);
                uint32_t pty = t_ptr(SC_FUNCTION, sty);
                uint32_t fvar = alloc_id();
                append_inst(entry_local_vars, OP_VARIABLE, {pty, fvar, SC_FUNCTION});
                rv.field_vars.emplace_back(fvar, sty);
            }
        } else {
            rv.spv_ty = map_ir_type_to_spv(ir_ty);
            uint32_t pty = t_ptr(SC_FUNCTION, rv.spv_ty);
            rv.fvar = alloc_id();
            append_inst(entry_local_vars, OP_VARIABLE, {pty, rv.fvar, SC_FUNCTION});
        }
        reg2mem_vars[vid] = std::move(rv);
    }

    uint32_t var_next_bb = 0;
    if (num_bbs > 1) {
        uint32_t pty_u32 = t_ptr(SC_FUNCTION, t_u32());
        var_next_bb = alloc_id();
        append_inst(entry_local_vars, OP_VARIABLE, {pty_u32, var_next_bb, SC_FUNCTION});
    }

    std::vector<uint32_t> entry_init_code;
    std::unordered_map<uint32_t, SpvVal> global_ssa_map;

    for (uint32_t vid = 0; vid < vals.size(); ++vid) {
        const IRValue &v = vals[vid];
        if (v.kind == IRValKind::Null || v.kind == IRValKind::Undef || v.kind == IRValKind::Int ||
            v.kind == IRValKind::Float || v.kind == IRValKind::Agg || v.kind == IRValKind::Data) {
            global_ssa_map[vid] = const_from_ir_val(vid, vals);
        } else if (v.kind == IRValKind::Global) {
            global_ssa_map[vid] = {false, c_u32(0), {}};
        } else if (v.kind == IRValKind::Arg) {
            auto it = arg_init_kind_.find(vid);
            ArgInit ak = (it != arg_init_kind_.end()) ? it->second : ArgInit{ArgInitKind::Const, c_u32(0), 0, 0, 0, 0};
            if (ak.kind == ArgInitKind::Const) {
                global_ssa_map[vid] = {false, ak.a, {}};
            } else if (ak.kind == ArgInitKind::LoadInput) {
                uint32_t ld_id = alloc_id();
                append_inst(entry_init_code, OP_LOAD, {ak.b, ld_id, ak.a});
                global_ssa_map[vid] = {false, ld_id, {}};
            } else if (ak.kind == ArgInitKind::FbFetch) {
                uint32_t fc_val = alloc_id();
                append_inst(entry_init_code, OP_LOAD, {ak.b, fc_val, ak.a});
                uint32_t v2f_ty = t_vec(t_f32(), 2);
                uint32_t fc_xy = alloc_id();
                append_inst(entry_init_code, OP_VECTOR_SHUFFLE, {v2f_ty, fc_xy, fc_val, fc_val, 0, 1});
                uint32_t v2u_ty = t_vec(t_u32(), 2);
                uint32_t coord_u2 = alloc_id();
                append_inst(entry_init_code, OP_CONVERT_F_TO_U, {v2u_ty, coord_u2, fc_xy});
                uint32_t img_val = alloc_id();
                append_inst(entry_init_code, OP_LOAD, {ak.d, img_val, ak.c});
                uint32_t v4f_ty = t_vec(t_f32(), 4);
                uint32_t lod0 = c_u32(0);
                uint32_t fetched_v4 = alloc_id();
                append_inst(entry_init_code, OP_IMAGE_FETCH, {v4f_ty, fetched_v4, img_val, coord_u2, 0x02u, lod0});
                if (ak.e == v4f_ty) {
                    global_ssa_map[vid] = {false, fetched_v4, {}};
                } else {
                    const IRType &irt = mod_.types[v.ty];
                    if (irt.kind == IRTypeKind::Vec) {
                        uint32_t shuf_id = alloc_id();
                        std::vector<uint32_t> sops = {ak.e, shuf_id, fetched_v4, fetched_v4};
                        for (uint32_t k = 0; k < irt.bits_or_len; ++k) sops.push_back(k);
                        append_inst_vec(entry_init_code, OP_VECTOR_SHUFFLE, sops);
                        global_ssa_map[vid] = {false, shuf_id, {}};
                    } else {
                        uint32_t ext_id = alloc_id();
                        append_inst(entry_init_code, OP_COMPOSITE_EXTRACT, {ak.e, ext_id, fetched_v4, 0});
                        global_ssa_map[vid] = {false, ext_id, {}};
                    }
                }
            }
        }
    }

    for (const auto &kv : reg2mem_vars) {
        SpvVal zv = zero_for_ir_type(vals[kv.first].ty);
        if (kv.second.is_struct) {
            for (size_t i = 0; i < kv.second.field_vars.size(); ++i) {
                append_inst(entry_init_code, OP_STORE, {kv.second.field_vars[i].first, zv.fields[i]});
            }
        } else {
            append_inst(entry_init_code, OP_STORE, {kv.second.fvar, zv.id});
        }
    }

    auto emit_bb_instructions = [&](size_t b_idx, bool &ended_with_kill) -> std::vector<uint32_t> {
        std::vector<uint32_t> code;
        std::unordered_map<uint32_t, SpvVal> local_ssa = global_ssa_map;
        bool terminated = false;

        std::function<SpvVal(uint32_t)> get_val = [&](uint32_t vid) -> SpvVal {
            auto it = local_ssa.find(vid);
            if (it != local_ssa.end()) return it->second;
            auto rit = reg2mem_vars.find(vid);
            if (rit != reg2mem_vars.end()) {
                SpvVal r;
                if (rit->second.is_struct) {
                    r.is_struct = true;
                    for (const auto &fv : rit->second.field_vars) {
                        uint32_t lid = alloc_id();
                        append_inst(code, OP_LOAD, {fv.second, lid, fv.first});
                        r.fields.push_back(lid);
                    }
                } else {
                    uint32_t lid = alloc_id();
                    append_inst(code, OP_LOAD, {rit->second.spv_ty, lid, rit->second.fvar});
                    r.id = lid;
                }
                local_ssa[vid] = r;
                return r;
            }
            SpvVal z = zero_for_ir_type(vals[vid].ty);
            local_ssa[vid] = z;
            return z;
        };

        auto set_val = [&](uint32_t vid, const SpvVal &val) {
            local_ssa[vid] = val;
            auto rit = reg2mem_vars.find(vid);
            if (rit != reg2mem_vars.end()) {
                if (rit->second.is_struct) {
                    for (size_t i = 0; i < rit->second.field_vars.size(); ++i) {
                        append_inst(code, OP_STORE, {rit->second.field_vars[i].first, val.fields[i]});
                    }
                } else {
                    append_inst(code, OP_STORE, {rit->second.fvar, val.id});
                }
            }
        };

        auto pit = phi_insts_by_bb.find(b_idx);
        if (pit != phi_insts_by_bb.end()) {
            for (uint32_t pii : pit->second) (void)get_val(uint32_t(fn_.insts[pii].cid));
        }

        auto emit_op = [&](uint32_t op, uint32_t res_ty, std::initializer_list<uint32_t> ops) -> uint32_t {
            uint32_t rid = alloc_id();
            code.push_back(encode_word(op, uint32_t(3 + ops.size())));
            code.push_back(res_ty);
            code.push_back(rid);
            code.insert(code.end(), ops.begin(), ops.end());
            return rid;
        };
        auto emit_op_v = [&](uint32_t op, uint32_t res_ty, const std::vector<uint32_t> &ops) -> uint32_t {
            uint32_t rid = alloc_id();
            code.push_back(encode_word(op, uint32_t(3 + ops.size())));
            code.push_back(res_ty);
            code.push_back(rid);
            code.insert(code.end(), ops.begin(), ops.end());
            return rid;
        };

        auto emit_u32_add = [&](uint32_t a, uint32_t b) -> uint32_t {
            auto ia = known_u32_.find(a), ib = known_u32_.find(b);
            if (ia != known_u32_.end() && ib != known_u32_.end()) return c_u32(ia->second + ib->second);
            if (ia != known_u32_.end() && ia->second == 0) return b;
            if (ib != known_u32_.end() && ib->second == 0) return a;
            return emit_op(OP_IADD, t_u32(), {a, b});
        };
        auto emit_u32_mul = [&](uint32_t a, uint32_t b) -> uint32_t {
            auto ia = known_u32_.find(a), ib = known_u32_.find(b);
            if (ia != known_u32_.end() && ib != known_u32_.end()) return c_u32(ia->second * ib->second);
            if ((ia != known_u32_.end() && ia->second == 0) || (ib != known_u32_.end() && ib->second == 0)) return c_u32(0);
            if (ia != known_u32_.end() && ia->second == 1) return b;
            if (ib != known_u32_.end() && ib->second == 1) return a;
            return emit_op(OP_IMUL, t_u32(), {a, b});
        };
        auto emit_u32_shr = [&](uint32_t a, uint32_t shift) -> uint32_t {
            if (shift == 0) return a;
            auto ia = known_u32_.find(a);
            if (ia != known_u32_.end()) return c_u32(ia->second >> shift);
            return emit_op(OP_SHIFT_RIGHT_LOGICAL, t_u32(), {a, c_u32(shift)});
        };
        auto emit_u32_and = [&](uint32_t a, uint32_t mask) -> uint32_t {
            auto ia = known_u32_.find(a);
            if (ia != known_u32_.end()) return c_u32(ia->second & mask);
            return emit_op(OP_BITWISE_AND, t_u32(), {a, c_u32(mask)});
        };

        auto emit_phi_copies_for_edge = [&](size_t target_bb) -> std::map<uint32_t, SpvVal> {
            std::map<uint32_t, SpvVal> copies;
            auto it = phi_insts_by_bb.find(target_bb);
            if (it == phi_insts_by_bb.end()) return copies;
            for (uint32_t pii : it->second) {
                const IRInst &pins = fn_.insts[pii];
                int32_t chosen = -1;
                for (const auto &pr : pins.pairs) {
                    if (pr.second == b_idx) { chosen = int32_t(pr.first); break; }
                }
                copies[uint32_t(pins.cid)] = (chosen >= 0) ? get_val(uint32_t(chosen)) : zero_for_ir_type(pins.ty);
            }
            return copies;
        };

        auto commit_phi_copies = [&](const std::map<uint32_t, SpvVal> &copies) {
            for (const auto &kv : copies) {
                const Reg2MemVar &rv = reg2mem_vars[kv.first];
                if (rv.is_struct) {
                    for (size_t i = 0; i < rv.field_vars.size(); ++i) {
                        append_inst(code, OP_STORE, {rv.field_vars[i].first, kv.second.fields[i]});
                    }
                } else {
                    append_inst(code, OP_STORE, {rv.fvar, kv.second.id});
                }
            }
        };

        for (uint32_t ii : bbs[b_idx]) {
            if (terminated) break;
            const IRInst &ins = fn_.insts[ii];

            switch (ins.op) {
                case IROp::Phi: break;
                case IROp::Alloca: {
                    uint32_t cid = uint32_t(ins.cid);
                    auto it = alloca_offsets.find(cid);
                    set_val(cid, {false, c_u32(it != alloca_offsets.end() ? it->second : 0u), {}});
                    break;
                }
                case IROp::Gep: {
                    uint32_t off_id = get_val(ins.a).id;
                    uint32_t cur_t = ins.sub_op;
                    for (size_t idx_pos = 0; idx_pos < ins.args.size(); ++idx_pos) {
                        uint32_t ivid = ins.args[idx_pos];
                        uint32_t ival_id = get_val(ivid).id;
                        if (idx_pos == 0) {
                            uint32_t ty_sz = mod_.type_layouts[cur_t].size;
                            off_id = emit_u32_add(off_id, emit_u32_mul(ival_id, c_u32(ty_sz)));
                        } else {
                            const IRType &ct = mod_.types[cur_t];
                            if (ct.kind == IRTypeKind::Struct) {
                                uint32_t f_idx = uint32_t(vals[ivid].i64);
                                uint32_t f_off = mod_.type_layouts[cur_t].member_offsets[f_idx];
                                cur_t = ct.members[f_idx];
                                if (f_off != 0) off_id = emit_u32_add(off_id, c_u32(f_off));
                            } else if (ct.kind == IRTypeKind::Array || ct.kind == IRTypeKind::Vec) {
                                uint32_t el_t = ct.elem_or_ret;
                                uint32_t el_sz = mod_.type_layouts[el_t].size;
                                cur_t = el_t;
                                off_id = emit_u32_add(off_id, emit_u32_mul(ival_id, c_u32(el_sz)));
                            }
                        }
                    }
                    set_val(uint32_t(ins.cid), {false, off_id, {}});
                    break;
                }
                case IROp::Load: {
                    uint32_t cid = uint32_t(ins.cid);
                    uint32_t byte_off_id = get_val(ins.a).id;
                    uint32_t res_spv_ty = map_ir_type_to_spv(ins.ty);
                    auto bit = ptr_base_info_.find(ins.a);
                    if (bit == ptr_base_info_.end()) {
                        set_val(cid, {false, c_null(res_spv_ty), {}});
                        break;
                    }
                    const PtrBaseInfo &binfo = bit->second;
                    const IRType &rt = mod_.types[ins.ty];
                    uint32_t n_comp = (rt.kind == IRTypeKind::Vec) ? rt.bits_or_len : 1;
                    uint32_t el_ir_ty = (rt.kind == IRTypeKind::Vec) ? rt.elem_or_ret : ins.ty;
                    bool el_is_float = is_float_ir_type(el_ir_ty);
                    bool el_is_bool = is_bool_ir_type(el_ir_ty);
                    uint32_t word_idx_id = emit_u32_shr(byte_off_id, 2);

                    auto kw = known_u32_.find(word_idx_id);
                    if (binfo.kind == PtrBaseKind::Ubo && n_comp == 4 && kw != known_u32_.end() && (kw->second & 3u) == 0) {
                        uint32_t v4_idx = c_u32(kw->second >> 2);
                        uint32_t v4f_ty = t_vec(t_f32(), 4);
                        uint32_t p_v4f = t_ptr(SC_UNIFORM, v4f_ty);
                        uint32_t ac_id = emit_op(OP_ACCESS_CHAIN, p_v4f, {binfo.var_id, c_u32(0), v4_idx});
                        uint32_t ld_id = emit_op(OP_LOAD, v4f_ty, {ac_id});
                        if (!el_is_float) ld_id = emit_op(OP_BITCAST, res_spv_ty, {ld_id});
                        set_val(cid, {false, ld_id, {}});
                        break;
                    }

                    std::vector<uint32_t> comp_ids;
                    comp_ids.reserve(n_comp);
                    for (uint32_t k = 0; k < n_comp; ++k) {
                        uint32_t w_id = emit_u32_add(word_idx_id, c_u32(k));
                        uint32_t v_id = 0;
                        if (binfo.kind == PtrBaseKind::Ubo) {
                            uint32_t v4_idx = emit_u32_shr(w_id, 2);
                            uint32_t c_idx = emit_u32_and(w_id, 3);
                            uint32_t ac_id = emit_op(OP_ACCESS_CHAIN, t_ptr(SC_UNIFORM, t_f32()), {binfo.var_id, c_u32(0), v4_idx, c_idx});
                            v_id = emit_op(OP_LOAD, t_f32(), {ac_id});
                            if (el_is_bool) {
                                uint32_t u_id = emit_op(OP_BITCAST, t_u32(), {v_id});
                                v_id = emit_op(OP_INOT_EQUAL, t_bool(), {u_id, c_u32(0)});
                            } else if (!el_is_float) {
                                v_id = emit_op(OP_BITCAST, t_u32(), {v_id});
                            }
                        } else if (binfo.kind == PtrBaseKind::Ssbo) {
                            uint32_t ac_id = emit_op(OP_ACCESS_CHAIN, t_ptr(SC_STORAGE_BUFFER, t_u32()), {binfo.var_id, c_u32(0), w_id});
                            v_id = emit_op(OP_LOAD, t_u32(), {ac_id});
                            if (el_is_float) v_id = emit_op(OP_BITCAST, t_f32(), {v_id});
                            else if (el_is_bool) v_id = emit_op(OP_INOT_EQUAL, t_bool(), {v_id, c_u32(0)});
                        } else if (binfo.kind == PtrBaseKind::Wg) {
                            uint32_t ac_id = emit_op(OP_ACCESS_CHAIN, t_ptr(SC_WORKGROUP, t_u32()), {binfo.var_id, w_id});
                            v_id = emit_op(OP_LOAD, t_u32(), {ac_id});
                            if (el_is_float) v_id = emit_op(OP_BITCAST, t_f32(), {v_id});
                            else if (el_is_bool) v_id = emit_op(OP_INOT_EQUAL, t_bool(), {v_id, c_u32(0)});
                        } else {
                            uint32_t ac_id = emit_op(OP_ACCESS_CHAIN, t_ptr(SC_FUNCTION, t_f32()), {binfo.var_id, w_id});
                            v_id = emit_op(OP_LOAD, t_f32(), {ac_id});
                            if (el_is_bool) {
                                uint32_t u_id = emit_op(OP_BITCAST, t_u32(), {v_id});
                                v_id = emit_op(OP_INOT_EQUAL, t_bool(), {u_id, c_u32(0)});
                            } else if (!el_is_float) {
                                v_id = emit_op(OP_BITCAST, t_u32(), {v_id});
                            }
                        }
                        comp_ids.push_back(v_id);
                    }
                    uint32_t out_id = (n_comp == 1) ? comp_ids[0] : emit_op_v(OP_COMPOSITE_CONSTRUCT, res_spv_ty, comp_ids);
                    set_val(cid, {false, out_id, {}});
                    break;
                }
                case IROp::Store: {
                    uint32_t byte_off_id = get_val(ins.a).id;
                    uint32_t sval = get_val(ins.b).id;
                    auto bit = ptr_base_info_.find(ins.a);
                    if (bit == ptr_base_info_.end()) break;
                    const PtrBaseInfo &binfo = bit->second;
                    uint32_t val_ir_ty = vals[ins.b].ty;
                    const IRType &vt = mod_.types[val_ir_ty];
                    uint32_t n_comp = (vt.kind == IRTypeKind::Vec) ? vt.bits_or_len : 1;
                    uint32_t el_ir_ty = (vt.kind == IRTypeKind::Vec) ? vt.elem_or_ret : val_ir_ty;
                    bool el_is_float = is_float_ir_type(el_ir_ty);
                    uint32_t el_spv_ty = map_ir_type_to_spv(el_ir_ty);
                    uint32_t word_idx_id = emit_u32_shr(byte_off_id, 2);

                    for (uint32_t k = 0; k < n_comp; ++k) {
                        uint32_t comp_id = (n_comp == 1) ? sval : emit_op(OP_COMPOSITE_EXTRACT, el_spv_ty, {sval, k});
                        uint32_t w_id = emit_u32_add(word_idx_id, c_u32(k));
                        if (binfo.kind == PtrBaseKind::Alloca) {
                            if (!el_is_float) comp_id = emit_op(OP_BITCAST, t_f32(), {comp_id});
                            uint32_t ac_id = emit_op(OP_ACCESS_CHAIN, t_ptr(SC_FUNCTION, t_f32()), {binfo.var_id, w_id});
                            append_inst(code, OP_STORE, {ac_id, comp_id});
                        } else if (binfo.kind == PtrBaseKind::Ssbo) {
                            if (el_is_float) comp_id = emit_op(OP_BITCAST, t_u32(), {comp_id});
                            uint32_t ac_id = emit_op(OP_ACCESS_CHAIN, t_ptr(SC_STORAGE_BUFFER, t_u32()), {binfo.var_id, c_u32(0), w_id});
                            append_inst(code, OP_STORE, {ac_id, comp_id});
                        } else if (binfo.kind == PtrBaseKind::Wg) {
                            if (el_is_float) comp_id = emit_op(OP_BITCAST, t_u32(), {comp_id});
                            uint32_t ac_id = emit_op(OP_ACCESS_CHAIN, t_ptr(SC_WORKGROUP, t_u32()), {binfo.var_id, w_id});
                            append_inst(code, OP_STORE, {ac_id, comp_id});
                        }
                    }
                    break;
                }
                case IROp::Binop: {
                    uint32_t lhs = get_val(ins.a).id, rhs = get_val(ins.b).id;
                    uint32_t res_ty = map_ir_type_to_spv(ins.ty);
                    bool is_fl = is_float_ir_type(ins.ty);
                    bool is_bl = is_bool_ir_type(ins.ty);
                    uint32_t res = lhs;
                    switch (ins.sub_op) {
                        case 0: res = emit_op(is_fl ? OP_FADD : OP_IADD, res_ty, {lhs, rhs}); break;
                        case 1: res = emit_op(is_fl ? OP_FSUB : OP_ISUB, res_ty, {lhs, rhs}); break;
                        case 2: res = emit_op(is_fl ? OP_FMUL : OP_IMUL, res_ty, {lhs, rhs}); break;
                        case 3: res = emit_op(OP_UDIV, res_ty, {lhs, rhs}); break;
                        case 4: res = emit_op(is_fl ? OP_FDIV : OP_SDIV, res_ty, {lhs, rhs}); break;
                        case 5: res = emit_op(OP_UMOD, res_ty, {lhs, rhs}); break;
                        case 6: res = emit_op(is_fl ? OP_FREM : OP_SREM, res_ty, {lhs, rhs}); break;
                        case 7: res = emit_op(OP_SHIFT_LEFT_LOGICAL, res_ty, {lhs, rhs}); break;
                        case 8: res = emit_op(OP_SHIFT_RIGHT_LOGICAL, res_ty, {lhs, rhs}); break;
                        case 9: res = emit_op(OP_SHIFT_RIGHT_ARITHMETIC, res_ty, {lhs, rhs}); break;
                        case 10: res = emit_op(is_bl ? OP_LOGICAL_AND : OP_BITWISE_AND, res_ty, {lhs, rhs}); break;
                        case 11: res = emit_op(is_bl ? OP_LOGICAL_OR : OP_BITWISE_OR, res_ty, {lhs, rhs}); break;
                        case 12: res = emit_op(is_bl ? OP_LOGICAL_NOT_EQUAL : OP_BITWISE_XOR, res_ty, {lhs, rhs}); break;
                        default: break;
                    }
                    set_val(uint32_t(ins.cid), {false, res, {}});
                    break;
                }
                case IROp::Cast: {
                    uint32_t cid = uint32_t(ins.cid);
                    uint32_t src_ir_ty = vals[ins.a].ty;
                    uint32_t src_val = get_val(ins.a).id;
                    const IRType &dst_t = mod_.types[ins.ty];
                    const IRType &src_t = mod_.types[src_ir_ty];
                    uint32_t dst_spv_ty = map_ir_type_to_spv(ins.ty);
                    uint32_t out_id = src_val;

                    if (dst_t.kind == IRTypeKind::Ptr || src_t.kind == IRTypeKind::Ptr ||
                        ins.sub_op == 7 || ins.sub_op == 8) {
                        out_id = src_val;
                    } else if (ins.sub_op <= 2) { /* trunc, zext, sext */
                        bool src_b = is_bool_ir_type(src_ir_ty), dst_b = is_bool_ir_type(ins.ty);
                        if (src_b && !dst_b) {
                            uint32_t n_c = (dst_t.kind == IRTypeKind::Vec) ? dst_t.bits_or_len : 1;
                            uint32_t t_lit = c_u32(ins.sub_op == 2 ? 0xFFFFFFFFu : 1u);
                            uint32_t one = (n_c == 1) ? t_lit : c_composite(dst_spv_ty, std::vector<uint32_t>(n_c, t_lit));
                            uint32_t zero = (n_c == 1) ? c_u32(0) : c_null(dst_spv_ty);
                            out_id = emit_op(OP_SELECT, dst_spv_ty, {src_val, one, zero});
                        } else if (dst_b && !src_b) {
                            uint32_t n_c = (dst_t.kind == IRTypeKind::Vec) ? dst_t.bits_or_len : 1;
                            uint32_t zero = (n_c == 1) ? c_u32(0) : c_null(map_ir_type_to_spv(src_ir_ty));
                            out_id = emit_op(OP_INOT_EQUAL, dst_spv_ty, {src_val, zero});
                        } else if (ins.sub_op == 0 && dst_t.kind == IRTypeKind::Int && dst_t.bits_or_len < 32) {
                            out_id = emit_u32_and(src_val, (1u << dst_t.bits_or_len) - 1u);
                        }
                    } else if (ins.sub_op == 11) { /* bitcast */
                        uint32_t src_spv_ty = map_ir_type_to_spv(src_ir_ty);
                        if (src_spv_ty == dst_spv_ty) {
                            out_id = src_val;
                        } else if (src_t.kind == IRTypeKind::Vec && dst_t.kind != IRTypeKind::Vec) {
                            const IRType &set = mod_.types[src_t.elem_or_ret];
                            if (src_t.bits_or_len == 2 && (set.kind == IRTypeKind::F16 || set.kind == IRTypeKind::F32)) {
                                out_id = emit_op(OP_EXT_INST, t_u32(), {glsl450_id_, GLSL_PACK_HALF_2X16, src_val});
                            } else {
                                uint32_t c0 = emit_op(OP_COMPOSITE_EXTRACT, t_u32(), {src_val, 0});
                                uint32_t c1 = emit_op(OP_COMPOSITE_EXTRACT, t_u32(), {src_val, 1});
                                uint32_t c0_m = emit_u32_and(c0, 0xFFFFu);
                                uint32_t c1_s = emit_op(OP_SHIFT_LEFT_LOGICAL, t_u32(), {c1, c_u32(16)});
                                out_id = emit_op(OP_BITWISE_OR, t_u32(), {c0_m, c1_s});
                            }
                        } else {
                            out_id = emit_op(OP_BITCAST, dst_spv_ty, {src_val});
                        }
                    }
                    set_val(cid, {false, out_id, {}});
                    break;
                }
                case IROp::ExtractElt: {
                    uint32_t vec_val = get_val(ins.a).id, idx_val = get_val(ins.b).id;
                    uint32_t elem_spv_ty = map_ir_type_to_spv(ins.ty);
                    uint32_t vec_len = mod_.types[vals[ins.a].ty].bits_or_len;
                    auto ki = known_u32_.find(idx_val);
                    uint32_t out_id = (ki != known_u32_.end())
                        ? emit_op(OP_COMPOSITE_EXTRACT, elem_spv_ty, {vec_val, ki->second % vec_len})
                        : emit_op(OP_VECTOR_EXTRACT_DYNAMIC, elem_spv_ty, {vec_val, idx_val});
                    set_val(uint32_t(ins.cid), {false, out_id, {}});
                    break;
                }
                case IROp::InsertElt: {
                    uint32_t vec_val = get_val(ins.a).id, elt_val = get_val(ins.b).id, idx_val = get_val(ins.c).id;
                    uint32_t vec_spv_ty = map_ir_type_to_spv(ins.ty);
                    uint32_t vec_len = mod_.types[ins.ty].bits_or_len;
                    auto ki = known_u32_.find(idx_val);
                    uint32_t out_id = (ki != known_u32_.end())
                        ? emit_op(OP_COMPOSITE_INSERT, vec_spv_ty, {elt_val, vec_val, ki->second % vec_len})
                        : emit_op(OP_VECTOR_INSERT_DYNAMIC, vec_spv_ty, {vec_val, elt_val, idx_val});
                    set_val(uint32_t(ins.cid), {false, out_id, {}});
                    break;
                }
                case IROp::ShuffleVec: {
                    uint32_t v1_val = get_val(ins.a).id, v2_val = get_val(ins.b).id;
                    uint32_t res_spv_ty = map_ir_type_to_spv(ins.ty);
                    const IRValue &mv = vals[ins.c];
                    uint32_t mask_len = mod_.types[mv.ty].bits_or_len;
                    uint32_t in_vec_len = mod_.types[vals[ins.a].ty].bits_or_len;
                    uint32_t max_idx = 2u * in_vec_len - 1u;
                    std::vector<uint32_t> ops = {v1_val, v2_val};
                    if (mv.kind == IRValKind::Null) {
                        for (uint32_t k = 0; k < mask_len; ++k) ops.push_back(0);
                    } else if (mv.kind == IRValKind::Undef) {
                        for (uint32_t k = 0; k < mask_len; ++k) ops.push_back(0xFFFFFFFFu);
                    } else if (mv.kind == IRValKind::Data) {
                        for (uint64_t x : mv.list) ops.push_back((x <= max_idx) ? uint32_t(x) : 0xFFFFFFFFu);
                    } else if (mv.kind == IRValKind::Agg) {
                        for (uint64_t svid : mv.list) {
                            const IRValue &sv = vals[size_t(svid)];
                            if (sv.kind == IRValKind::Undef) ops.push_back(0xFFFFFFFFu);
                            else if (sv.kind == IRValKind::Null) ops.push_back(0);
                            else ops.push_back((sv.i64 >= 0 && uint64_t(sv.i64) <= max_idx) ? uint32_t(sv.i64) : 0xFFFFFFFFu);
                        }
                    } else {
                        for (uint32_t k = 0; k < mask_len; ++k) ops.push_back(0);
                    }
                    set_val(uint32_t(ins.cid), {false, emit_op_v(OP_VECTOR_SHUFFLE, res_spv_ty, ops), {}});
                    break;
                }
                case IROp::InsertVal: {
                    SpvVal agg = get_val(ins.a);
                    uint32_t elt = get_val(ins.b).id;
                    if (!ins.args.empty() && ins.args[0] < agg.fields.size()) agg.fields[ins.args[0]] = elt;
                    set_val(uint32_t(ins.cid), agg);
                    break;
                }
                case IROp::Cmp2: {
                    uint32_t lhs = get_val(ins.a).id, rhs = get_val(ins.b).id;
                    uint32_t res_spv_ty = map_ir_type_to_spv(ins.ty);
                    bool lb = is_bool_ir_type(vals[ins.a].ty);
                    uint32_t sop = OP_IEQUAL;
                    switch (ins.sub_op) {
                        case 1: sop = OP_FORD_EQUAL; break;
                        case 2: sop = OP_FORD_GREATER_THAN; break;
                        case 3: sop = OP_FORD_GREATER_THAN_EQUAL; break;
                        case 4: sop = OP_FORD_LESS_THAN; break;
                        case 5: sop = OP_FORD_LESS_THAN_EQUAL; break;
                        case 6: sop = OP_FORD_NOT_EQUAL; break;
                        case 9: sop = OP_FUNORD_EQUAL; break;
                        case 10: sop = OP_FUNORD_GREATER_THAN; break;
                        case 11: sop = OP_FUNORD_GREATER_THAN_EQUAL; break;
                        case 12: sop = OP_FUNORD_LESS_THAN; break;
                        case 13: sop = OP_FUNORD_LESS_THAN_EQUAL; break;
                        case 14: sop = OP_FUNORD_NOT_EQUAL; break;
                        case 32: sop = lb ? OP_LOGICAL_EQUAL : OP_IEQUAL; break;
                        case 33: sop = lb ? OP_LOGICAL_NOT_EQUAL : OP_INOT_EQUAL; break;
                        case 34: sop = OP_UGREATER_THAN; break;
                        case 35: sop = OP_UGREATER_THAN_EQUAL; break;
                        case 36: sop = OP_ULESS_THAN; break;
                        case 37: sop = OP_ULESS_THAN_EQUAL; break;
                        case 38: sop = OP_SGREATER_THAN; break;
                        case 39: sop = OP_SGREATER_THAN_EQUAL; break;
                        case 40: sop = OP_SLESS_THAN; break;
                        case 41: sop = OP_SLESS_THAN_EQUAL; break;
                        default: break;
                    }
                    set_val(uint32_t(ins.cid), {false, emit_op(sop, res_spv_ty, {lhs, rhs}), {}});
                    break;
                }
                case IROp::Select: {
                    uint32_t cond = get_val(ins.a).id, tv = get_val(ins.b).id, fv = get_val(ins.c).id;
                    uint32_t res_spv_ty = map_ir_type_to_spv(ins.ty);
                    const IRType &vt = mod_.types[ins.ty];
                    const IRType &ct = mod_.types[vals[ins.a].ty];
                    if (vt.kind == IRTypeKind::Vec && ct.kind != IRTypeKind::Vec) {
                        uint32_t vb_ty = t_vec(t_bool(), vt.bits_or_len);
                        cond = emit_op_v(OP_COMPOSITE_CONSTRUCT, vb_ty, std::vector<uint32_t>(vt.bits_or_len, cond));
                    }
                    set_val(uint32_t(ins.cid), {false, emit_op(OP_SELECT, res_spv_ty, {cond, tv, fv}), {}});
                    break;
                }
                case IROp::Call: {
                    auto itn = mod_.vst.find(ins.a);
                    const std::string cname = (itn != mod_.vst.end()) ? itn->second : "";
                    if (starts_with(cname, "llvm.lifetime.")) break;
                    if (cname == "air.discard_fragment") {
                        entry_flags_ |= (1u << 2);
                        append_inst(code, OP_KILL, {});
                        terminated = true;
                        break;
                    }
                    if (cname == "air.wg.barrier") {
                        entry_flags_ |= (1u << 1);
                        append_inst(code, OP_CONTROL_BARRIER, {c_u32(2), c_u32(2), c_u32(0x108u)});
                        break;
                    }
                    uint32_t res_spv_ty = map_ir_type_to_spv(ins.ty);
                    uint32_t res_id = emit_intrinsic_call(cname, res_spv_ty, ins.ty, ins.args,
                                                          get_val, emit_op, emit_op_v, emit_u32_shr, code);
                    if (ins.cid >= 0 && res_id != 0) set_val(uint32_t(ins.cid), {false, res_id, {}});
                    break;
                }
                case IROp::Ret: {
                    if (ins.cid >= 0 && !output_vars_.empty()) {
                        SpvVal rval = get_val(ins.a);
                        if (rval.is_struct) {
                            for (size_t i = 0; i < output_vars_.size() && i < rval.fields.size(); ++i) {
                                append_inst(code, OP_STORE, {output_vars_[i].var_id, rval.fields[i]});
                            }
                        } else {
                            append_inst(code, OP_STORE, {output_vars_[0].var_id, rval.id});
                        }
                    }
                    if (num_bbs == 1) append_inst(code, OP_RETURN, {});
                    else append_inst(code, OP_STORE, {var_next_bb, c_u32(0xFFFFFFFFu)});
                    terminated = true;
                    break;
                }
                case IROp::Br: {
                    auto copies = emit_phi_copies_for_edge(ins.a);
                    commit_phi_copies(copies);
                    if (num_bbs > 1) append_inst(code, OP_STORE, {var_next_bb, c_u32(ins.a)});
                    terminated = true;
                    break;
                }
                case IROp::BrCond: {
                    uint32_t cond_val = get_val(ins.a).id;
                    auto t_copies = emit_phi_copies_for_edge(ins.b);
                    auto f_copies = emit_phi_copies_for_edge(ins.c);
                    std::set<uint32_t> all_pcids;
                    for (const auto &kv : t_copies) all_pcids.insert(kv.first);
                    for (const auto &kv : f_copies) all_pcids.insert(kv.first);
                    for (uint32_t pcid : all_pcids) {
                        uint32_t p_ir_ty = vals[pcid].ty;
                        SpvVal tv = t_copies.count(pcid) ? t_copies[pcid] : get_val(pcid);
                        SpvVal fv = f_copies.count(pcid) ? f_copies[pcid] : get_val(pcid);
                        const Reg2MemVar &rv = reg2mem_vars[pcid];
                        if (rv.is_struct) {
                            for (size_t i = 0; i < rv.field_vars.size(); ++i) {
                                uint32_t sid = emit_op(OP_SELECT, rv.field_vars[i].second, {cond_val, tv.fields[i], fv.fields[i]});
                                append_inst(code, OP_STORE, {rv.field_vars[i].first, sid});
                            }
                        } else {
                            const IRType &pt = mod_.types[p_ir_ty];
                            uint32_t c_use = cond_val;
                            if (pt.kind == IRTypeKind::Vec) {
                                c_use = emit_op_v(OP_COMPOSITE_CONSTRUCT, t_vec(t_bool(), pt.bits_or_len),
                                                  std::vector<uint32_t>(pt.bits_or_len, cond_val));
                            }
                            uint32_t sid = emit_op(OP_SELECT, rv.spv_ty, {c_use, tv.id, fv.id});
                            append_inst(code, OP_STORE, {rv.fvar, sid});
                        }
                    }
                    if (num_bbs > 1) {
                        uint32_t nid = emit_op(OP_SELECT, t_u32(), {cond_val, c_u32(ins.b), c_u32(ins.c)});
                        append_inst(code, OP_STORE, {var_next_bb, nid});
                    }
                    terminated = true;
                    break;
                }
                case IROp::Switch: {
                    uint32_t cond_val = get_val(ins.a).id;
                    uint32_t cur_tgt = c_u32(ins.b);
                    auto def_copies = emit_phi_copies_for_edge(ins.b);
                    struct CaseCopies { uint32_t lit; uint32_t bb; std::map<uint32_t, SpvVal> copies; };
                    std::vector<CaseCopies> case_list;
                    std::set<uint32_t> all_pcids;
                    for (const auto &kv : def_copies) all_pcids.insert(kv.first);
                    for (const auto &pr : ins.pairs) {
                        CaseCopies cc{pr.first, pr.second, emit_phi_copies_for_edge(pr.second)};
                        for (const auto &kv : cc.copies) all_pcids.insert(kv.first);
                        case_list.push_back(std::move(cc));
                    }
                    std::map<uint32_t, SpvVal> cur_phi_vals;
                    for (uint32_t pcid : all_pcids) {
                        cur_phi_vals[pcid] = def_copies.count(pcid) ? def_copies[pcid] : get_val(pcid);
                    }
                    for (const auto &cc : case_list) {
                        uint32_t cmp_id = emit_op(OP_IEQUAL, t_bool(), {cond_val, c_u32(cc.lit)});
                        cur_tgt = emit_op(OP_SELECT, t_u32(), {cmp_id, c_u32(cc.bb), cur_tgt});
                        for (uint32_t pcid : all_pcids) {
                            uint32_t p_ir_ty = vals[pcid].ty;
                            const IRType &pt = mod_.types[p_ir_ty];
                            SpvVal cv = cc.copies.count(pcid) ? cc.copies.at(pcid) : get_val(pcid);
                            SpvVal pv = cur_phi_vals[pcid];
                            if (cv.is_struct) {
                                SpvVal nv; nv.is_struct = true;
                                for (size_t i = 0; i < pt.members.size(); ++i) {
                                    nv.fields.push_back(emit_op(OP_SELECT, map_ir_type_to_spv(pt.members[i]),
                                                                {cmp_id, cv.fields[i], pv.fields[i]}));
                                }
                                cur_phi_vals[pcid] = std::move(nv);
                            } else {
                                uint32_t sty = map_ir_type_to_spv(p_ir_ty);
                                uint32_t c_use = cmp_id;
                                if (pt.kind == IRTypeKind::Vec) {
                                    c_use = emit_op_v(OP_COMPOSITE_CONSTRUCT, t_vec(t_bool(), pt.bits_or_len),
                                                      std::vector<uint32_t>(pt.bits_or_len, cmp_id));
                                }
                                cur_phi_vals[pcid] = {false, emit_op(OP_SELECT, sty, {c_use, cv.id, pv.id}), {}};
                            }
                        }
                    }
                    commit_phi_copies(cur_phi_vals);
                    if (num_bbs > 1) append_inst(code, OP_STORE, {var_next_bb, cur_tgt});
                    terminated = true;
                    break;
                }
                case IROp::Unreachable: {
                    if (num_bbs == 1) append_inst(code, OP_RETURN, {});
                    else append_inst(code, OP_STORE, {var_next_bb, c_u32(0xFFFFFFFFu)});
                    terminated = true;
                    break;
                }
            }
        }
        ended_with_kill = (!code.empty() && code.back() == encode_word(OP_KILL, 1));
        return code;
    };

    /* 3. RPO order & backedge detection */
    std::vector<std::vector<size_t>> succs(num_bbs);
    for (size_t bi = 0; bi < num_bbs; ++bi) {
        if (bbs[bi].empty()) continue;
        const IRInst &term = fn_.insts[bbs[bi].back()];
        if (term.op == IROp::Br) succs[bi].push_back(term.a);
        else if (term.op == IROp::BrCond) { succs[bi].push_back(term.b); succs[bi].push_back(term.c); }
        else if (term.op == IROp::Switch) {
            succs[bi].push_back(term.b);
            for (const auto &pr : term.pairs) succs[bi].push_back(pr.second);
        }
    }

    std::vector<uint8_t> visited(num_bbs, 0);
    std::vector<size_t> postorder;
    if (num_bbs > 0) {
        std::vector<std::pair<size_t, size_t>> st_dfs = {{0, 0}};
        visited[0] = 1;
        while (!st_dfs.empty()) {
            size_t curr = st_dfs.back().first;
            size_t &si = st_dfs.back().second;
            if (si < succs[curr].size()) {
                size_t nxt = succs[curr][si++];
                if (nxt < num_bbs && !visited[nxt]) {
                    visited[nxt] = 1;
                    st_dfs.emplace_back(nxt, 0);
                }
            } else {
                postorder.push_back(curr);
                st_dfs.pop_back();
            }
        }
    }
    std::vector<size_t> rpo(postorder.rbegin(), postorder.rend());
    std::unordered_map<size_t, size_t> rpo_pos;
    for (size_t i = 0; i < rpo.size(); ++i) rpo_pos[rpo[i]] = i;
    bool has_backedge = false;
    for (size_t u : rpo) {
        for (size_t v : succs[u]) {
            auto it = rpo_pos.find(v);
            if (it != rpo_pos.end() && it->second <= rpo_pos[u]) { has_backedge = true; break; }
        }
        if (has_backedge) break;
    }

    /* 4. Assemble Function Body */
    std::vector<uint32_t> fn_words;
    append_inst(fn_words, OP_FUNCTION, {t_void(), main_fn_id_, 0, t_func_void()});
    uint32_t entry_label = alloc_id();
    append_inst(fn_words, OP_LABEL, {entry_label});

    if (num_bbs == 1) {
        bool ended_kill = false;
        auto bb0 = emit_bb_instructions(0, ended_kill);
        fn_words.insert(fn_words.end(), entry_local_vars.begin(), entry_local_vars.end());
        fn_words.insert(fn_words.end(), entry_init_code.begin(), entry_init_code.end());
        fn_words.insert(fn_words.end(), bb0.begin(), bb0.end());
        if (!ended_kill && (bb0.empty() || bb0.back() != encode_word(OP_RETURN, 1))) {
            append_inst(fn_words, OP_RETURN, {});
        }
    } else if (!has_backedge) {
        bool ended_kill = false;
        auto bb0 = emit_bb_instructions(0, ended_kill);
        fn_words.insert(fn_words.end(), entry_local_vars.begin(), entry_local_vars.end());
        fn_words.insert(fn_words.end(), entry_init_code.begin(), entry_init_code.end());
        fn_words.insert(fn_words.end(), bb0.begin(), bb0.end());
        if (!ended_kill) {
            uint32_t next_hdr = alloc_id();
            append_inst(fn_words, OP_BRANCH, {next_hdr});
            for (size_t i = 1; i < rpo.size(); ++i) {
                size_t b_id = rpo[i];
                uint32_t hdr = next_hdr, body = alloc_id(), after = alloc_id();
                next_hdr = after;
                append_inst(fn_words, OP_LABEL, {hdr});
                uint32_t cur_bb = alloc_id();
                append_inst(fn_words, OP_LOAD, {t_u32(), cur_bb, var_next_bb});
                uint32_t cond = alloc_id();
                append_inst(fn_words, OP_IEQUAL, {t_bool(), cond, cur_bb, c_u32(uint32_t(b_id))});
                append_inst(fn_words, OP_SELECTION_MERGE, {after, 0});
                append_inst(fn_words, OP_BRANCH_CONDITIONAL, {cond, body, after});
                append_inst(fn_words, OP_LABEL, {body});
                bool b_kill = false;
                auto b_code = emit_bb_instructions(b_id, b_kill);
                fn_words.insert(fn_words.end(), b_code.begin(), b_code.end());
                if (!b_kill) append_inst(fn_words, OP_BRANCH, {after});
            }
            append_inst(fn_words, OP_LABEL, {next_hdr});
            append_inst(fn_words, OP_RETURN, {});
        }
    } else {
        fn_words.insert(fn_words.end(), entry_local_vars.begin(), entry_local_vars.end());
        fn_words.insert(fn_words.end(), entry_init_code.begin(), entry_init_code.end());
        append_inst(fn_words, OP_STORE, {var_next_bb, c_u32(0)});

        uint32_t loop_hdr = alloc_id(), loop_exit = alloc_id(), loop_cont = alloc_id(), first_sese = alloc_id();
        append_inst(fn_words, OP_BRANCH, {loop_hdr});
        append_inst(fn_words, OP_LABEL, {loop_hdr});
        uint32_t cur_top = alloc_id();
        append_inst(fn_words, OP_LOAD, {t_u32(), cur_top, var_next_bb});
        uint32_t is_done = alloc_id();
        append_inst(fn_words, OP_IEQUAL, {t_bool(), is_done, cur_top, c_u32(0xFFFFFFFFu)});
        append_inst(fn_words, OP_LOOP_MERGE, {loop_exit, loop_cont, 0});
        append_inst(fn_words, OP_BRANCH_CONDITIONAL, {is_done, loop_exit, first_sese});

        uint32_t next_hdr = first_sese;
        for (size_t b_id : rpo) {
            uint32_t hdr = next_hdr, body = alloc_id(), after = alloc_id();
            next_hdr = after;
            append_inst(fn_words, OP_LABEL, {hdr});
            uint32_t cur_bb = alloc_id();
            append_inst(fn_words, OP_LOAD, {t_u32(), cur_bb, var_next_bb});
            uint32_t cond = alloc_id();
            append_inst(fn_words, OP_IEQUAL, {t_bool(), cond, cur_bb, c_u32(uint32_t(b_id))});
            append_inst(fn_words, OP_SELECTION_MERGE, {after, 0});
            append_inst(fn_words, OP_BRANCH_CONDITIONAL, {cond, body, after});
            append_inst(fn_words, OP_LABEL, {body});
            bool b_kill = false;
            auto b_code = emit_bb_instructions(b_id, b_kill);
            fn_words.insert(fn_words.end(), b_code.begin(), b_code.end());
            if (!b_kill) append_inst(fn_words, OP_BRANCH, {after});
        }
        append_inst(fn_words, OP_LABEL, {next_hdr});
        append_inst(fn_words, OP_BRANCH, {loop_cont});
        append_inst(fn_words, OP_LABEL, {loop_cont});
        append_inst(fn_words, OP_BRANCH, {loop_hdr});
        append_inst(fn_words, OP_LABEL, {loop_exit});
        append_inst(fn_words, OP_RETURN, {});
    }
    append_inst(fn_words, OP_FUNCTION_END, {});

    /* 5. Assemble Full Module */
    out_words.clear();
    out_words.reserve(64 + decorations_.size() + type_const_globals_.size() + fn_words.size());
    out_words.push_back(SPV_MAGIC);
    out_words.push_back(SPV_VERSION_1_3);
    out_words.push_back(SPV_GENERATOR);
    out_words.push_back(next_id_);
    out_words.push_back(0);
    for (uint32_t cap : capabilities_) append_inst(out_words, OP_CAPABILITY, {cap});
    {
        std::vector<uint32_t> ops = {glsl450_id_};
        append_str_words(ops, "GLSL.std.450");
        append_inst_vec(out_words, OP_EXT_INST_IMPORT, ops);
    }
    append_inst(out_words, OP_MEMORY_MODEL, {0, 1});
    {
        std::vector<uint32_t> ops = {exec_model_, main_fn_id_};
        append_str_words(ops, "main");
        ops.insert(ops.end(), entry_interface_vars_.begin(), entry_interface_vars_.end());
        append_inst_vec(out_words, OP_ENTRY_POINT, ops);
    }
    if (exec_model_ == 4) {
        append_inst(out_words, OP_EXECUTION_MODE, {main_fn_id_, 7});
        if (entry_flags_ & (1u << 4)) append_inst(out_words, OP_EXECUTION_MODE, {main_fn_id_, 12});
    } else if (exec_model_ == 5) {
        append_inst(out_words, OP_EXECUTION_MODE, {main_fn_id_, 17, lx_, ly_, lz_});
    }
    out_words.insert(out_words.end(), decorations_.begin(), decorations_.end());
    out_words.insert(out_words.end(), type_const_globals_.begin(), type_const_globals_.end());
    out_words.insert(out_words.end(), fn_words.begin(), fn_words.end());

    out_refl.shader_id = mi_.shader_id;
    out_refl.mtlb_sha256 = mi_.mtlb_sha256;
    out_refl.entry_name = mi_.entry_name;
    out_refl.stage = stage_;
    out_refl.bindings = std::move(bindings_refl_);
    out_refl.vertex_attrs = std::move(vattrs_refl_);
    out_refl.buffer_mask = buffer_mask_;
    out_refl.texture_mask = texture_mask_;
    out_refl.sampler_mask = sampler_mask_;
    out_refl.fragment_output_mask = frag_out_mask_;
    out_refl.flags = entry_flags_;
    out_refl.local_size_x = lx_;
    out_refl.local_size_y = ly_;
    out_refl.local_size_z = lz_;
    return true;
}

uint32_t SpirvBuilder::emit_intrinsic_call(
    const std::string &name, uint32_t res_spv_ty, uint32_t ret_ir_ty,
    const std::vector<uint32_t> &call_args,
    const std::function<SpvVal(uint32_t)> &get_val,
    const std::function<uint32_t(uint32_t, uint32_t, std::initializer_list<uint32_t>)> &emit_op,
    const std::function<uint32_t(uint32_t, uint32_t, const std::vector<uint32_t> &)> &emit_op_v,
    const std::function<uint32_t(uint32_t, uint32_t)> &emit_u32_shr,
    std::vector<uint32_t> &code) {

    /* 1. Float <-> Half and Int/Bool <-> Float conversions (scalar & vector) */
    if (starts_with(name, "air.convert.")) {
        uint32_t arg0 = get_val(call_args[0]).id;
        if (starts_with(name, "air.convert.f.") && name.size() > 13) {
            if (name.find(".f.", 13) != std::string::npos) {
                return arg0;
            }
            if (name.find(".u.", 13) != std::string::npos) {
                if (ends_with(name, "i1")) {
                    const IRType &rt = mod_.types[ret_ir_ty];
                    uint32_t n_c = (rt.kind == IRTypeKind::Vec) ? rt.bits_or_len : 1;
                    uint32_t one_f = c_f32_bits(0x3F800000u);
                    uint32_t zero_f = c_f32_bits(0);
                    uint32_t one = (n_c == 1) ? one_f : c_composite(res_spv_ty, std::vector<uint32_t>(n_c, one_f));
                    uint32_t zero = (n_c == 1) ? zero_f : c_null(res_spv_ty);
                    return emit_op(OP_SELECT, res_spv_ty, {arg0, one, zero});
                }
                return emit_op(OP_CONVERT_U_TO_F, res_spv_ty, {arg0});
            }
            if (name.find(".s.", 13) != std::string::npos) {
                if (ends_with(name, "i1")) {
                    const IRType &rt = mod_.types[ret_ir_ty];
                    uint32_t n_c = (rt.kind == IRTypeKind::Vec) ? rt.bits_or_len : 1;
                    uint32_t neg_one_f = c_f32_bits(0xBF800000u);
                    uint32_t zero_f = c_f32_bits(0);
                    uint32_t neg_one = (n_c == 1) ? neg_one_f : c_composite(res_spv_ty, std::vector<uint32_t>(n_c, neg_one_f));
                    uint32_t zero = (n_c == 1) ? zero_f : c_null(res_spv_ty);
                    return emit_op(OP_SELECT, res_spv_ty, {arg0, neg_one, zero});
                }
                return emit_op(OP_CONVERT_S_TO_F, res_spv_ty, {arg0});
            }
        }
        if (starts_with(name, "air.convert.u.")) return emit_op(OP_CONVERT_F_TO_U, res_spv_ty, {arg0});
        if (starts_with(name, "air.convert.s.")) {
            const IRType &rt = mod_.types[ret_ir_ty];
            uint32_t s_ty = (rt.kind == IRTypeKind::Vec) ? t_vec(t_s32(), rt.bits_or_len) : t_s32();
            uint32_t s_id = emit_op(OP_CONVERT_F_TO_S, s_ty, {arg0});
            return emit_op(OP_BITCAST, res_spv_ty, {s_id});
        }
    }

    /* 3. Texture Sampling, Gathering, Fetching, Queries */
    if (name.find("texture") != std::string::npos && !call_args.empty()) {
        auto tit = tex_arg_info_.find(call_args[0]);
        if (tit == tex_arg_info_.end()) return c_null(res_spv_ty);
        const TexArgInfo &ti = tit->second;
        uint32_t tex_id = emit_op(OP_LOAD, ti.img_ty, {ti.var_id});

        if (name == "air.get_width_texture_2d" || name == "air.get_height_texture_2d") {
            capabilities_.insert(CAP_IMAGE_QUERY);
            uint32_t lod_id = (call_args.size() > 1) ? get_val(call_args[1]).id : c_u32(0);
            uint32_t sz_id = emit_op(OP_IMAGE_QUERY_SIZE_LOD, t_vec(t_u32(), 2), {tex_id, lod_id});
            uint32_t comp = (name.find("width") != std::string::npos) ? 0u : 1u;
            return emit_op(OP_COMPOSITE_EXTRACT, t_u32(), {sz_id, comp});
        }

        if (starts_with(name, "air.read_texture_")) {
            uint32_t coord_id = get_val(call_args[1]).id;
            uint32_t lod_or_samp = (call_args.size() > 2) ? get_val(call_args[2]).id : c_u32(0);
            uint32_t fetch_vty = t_vec(ti.sampled_ty, 4);
            uint32_t res_v = emit_op(OP_IMAGE_FETCH, fetch_vty, {tex_id, coord_id, ti.ms ? 0x40u : 0x02u, lod_or_samp});
            if (fetch_vty != res_spv_ty) res_v = emit_op(OP_BITCAST, res_spv_ty, {res_v});
            return res_v;
        }

        auto sit = samp_arg_info_.find(call_args[1]);
        if (sit == samp_arg_info_.end()) return c_null(res_spv_ty);
        uint32_t samp_id = emit_op(OP_LOAD, t_sampler(), {sit->second});
        uint32_t si_id = emit_op(OP_SAMPLED_IMAGE, t_sampled_image(ti.img_ty), {tex_id, samp_id});
        uint32_t coord_id = get_val(call_args[2]).id;
        uint32_t sample_vty = t_vec(ti.sampled_ty, 4);
        const auto &vtbl = fn_.vals;

        auto const_bool_true = [&](uint32_t vid) -> bool {
            return vtbl[vid].kind == IRValKind::Int && vtbl[vid].i64 != 0;
        };
        auto const_float_is_zero = [&](uint32_t vid) -> bool {
            if (vtbl[vid].kind == IRValKind::Null) return true;
            if (vtbl[vid].kind == IRValKind::Float) return (uint32_t(vtbl[vid].i64) & 0x7FFFFFFFu) == 0;
            return false;
        };
        auto const_ivec_offset = [&](uint32_t vid, uint32_t dims) -> uint32_t {
            const IRValue &v = vtbl[vid];
            if (v.kind == IRValKind::Null) return 0;
            std::vector<uint32_t> raw_els;
            if (v.kind == IRValKind::Data && v.list.size() >= dims) {
                for (uint32_t i = 0; i < dims; ++i) raw_els.push_back(uint32_t(v.list[i]));
            } else if (v.kind == IRValKind::Agg && v.list.size() >= dims) {
                for (uint32_t i = 0; i < dims; ++i) {
                    const IRValue &sv = vtbl[size_t(v.list[i])];
                    if (sv.kind == IRValKind::Null) raw_els.push_back(0);
                    else if (sv.kind == IRValKind::Int) raw_els.push_back(uint32_t(sv.i64));
                    else return 0;
                }
            } else return 0;
            bool all_zero = true;
            for (uint32_t x : raw_els) if (x != 0) all_zero = false;
            if (all_zero) return 0;
            std::vector<uint32_t> cels;
            for (uint32_t x : raw_els) cels.push_back(c_s32(x));
            return c_composite(t_vec(t_s32(), dims), cels);
        };

        uint32_t res_v = 0;
        if (starts_with(name, "air.gather_texture_")) {
            uint32_t comp_vid = (call_args.size() >= 6) ? call_args[5] : call_args.back();
            uint32_t comp_id = get_val(comp_vid).id;
            uint32_t const_off = (call_args.size() >= 5 && const_bool_true(call_args[3])) ? const_ivec_offset(call_args[4], 2) : 0;
            res_v = const_off
                ? emit_op(OP_IMAGE_GATHER, sample_vty, {si_id, coord_id, comp_id, 0x08u, const_off})
                : emit_op(OP_IMAGE_GATHER, sample_vty, {si_id, coord_id, comp_id});
        } else if (starts_with(name, "air.sample_texture_2d_grad")) {
            uint32_t ddx = get_val(call_args[3]).id, ddy = get_val(call_args[4]).id;
            uint32_t const_off = (call_args.size() >= 7 && const_bool_true(call_args[5])) ? const_ivec_offset(call_args[6], 2) : 0;
            res_v = const_off
                ? emit_op(OP_IMAGE_SAMPLE_EXPLICIT_LOD, sample_vty, {si_id, coord_id, 0x0Cu, ddx, ddy, const_off})
                : emit_op(OP_IMAGE_SAMPLE_EXPLICIT_LOD, sample_vty, {si_id, coord_id, 0x04u, ddx, ddy});
        } else if (starts_with(name, "air.sample_texture_2d_array")) {
            uint32_t layer_f = emit_op(OP_CONVERT_S_TO_F, t_f32(), {get_val(call_args[3]).id});
            uint32_t cx = emit_op(OP_COMPOSITE_EXTRACT, t_f32(), {coord_id, 0});
            uint32_t cy = emit_op(OP_COMPOSITE_EXTRACT, t_f32(), {coord_id, 1});
            uint32_t c3 = emit_op(OP_COMPOSITE_CONSTRUCT, t_vec(t_f32(), 3), {cx, cy, layer_f});
            uint32_t const_off = (call_args.size() >= 6 && const_bool_true(call_args[4])) ? const_ivec_offset(call_args[5], 2) : 0;
            bool use_exp = (exec_model_ != 4) || (call_args.size() >= 8 && const_bool_true(call_args[6]));
            if (use_exp) {
                uint32_t lod_f = (call_args.size() >= 8) ? get_val(call_args[7]).id : c_f32_bits(0);
                res_v = const_off
                    ? emit_op(OP_IMAGE_SAMPLE_EXPLICIT_LOD, sample_vty, {si_id, c3, 0x0Au, lod_f, const_off})
                    : emit_op(OP_IMAGE_SAMPLE_EXPLICIT_LOD, sample_vty, {si_id, c3, 0x02u, lod_f});
            } else {
                bool has_bias = (call_args.size() >= 8 && !const_float_is_zero(call_args[7]));
                uint32_t mask = (has_bias ? 0x01u : 0u) | (const_off ? 0x08u : 0u);
                std::vector<uint32_t> ops = {si_id, c3};
                if (mask) {
                    ops.push_back(mask);
                    if (has_bias) ops.push_back(get_val(call_args[7]).id);
                    if (const_off) ops.push_back(const_off);
                }
                res_v = emit_op_v(OP_IMAGE_SAMPLE_IMPLICIT_LOD, sample_vty, ops);
            }
        } else if (starts_with(name, "air.sample_texture_cube")) {
            bool use_exp = (exec_model_ != 4) || (call_args.size() >= 5 && const_bool_true(call_args[3]));
            if (use_exp) {
                uint32_t lod_f = (call_args.size() >= 5) ? get_val(call_args[4]).id : c_f32_bits(0);
                res_v = emit_op(OP_IMAGE_SAMPLE_EXPLICIT_LOD, sample_vty, {si_id, coord_id, 0x02u, lod_f});
            } else if (call_args.size() >= 5 && !const_float_is_zero(call_args[4])) {
                res_v = emit_op(OP_IMAGE_SAMPLE_IMPLICIT_LOD, sample_vty, {si_id, coord_id, 0x01u, get_val(call_args[4]).id});
            } else {
                res_v = emit_op(OP_IMAGE_SAMPLE_IMPLICIT_LOD, sample_vty, {si_id, coord_id});
            }
        } else {
            uint32_t off_dims = (name.find("_3d") != std::string::npos) ? 3u : 2u;
            uint32_t const_off = (call_args.size() >= 5 && const_bool_true(call_args[3])) ? const_ivec_offset(call_args[4], off_dims) : 0;
            bool use_exp = (exec_model_ != 4) || (call_args.size() >= 7 && const_bool_true(call_args[5]));
            if (use_exp) {
                uint32_t lod_f = (call_args.size() >= 7) ? get_val(call_args[6]).id : c_f32_bits(0);
                res_v = const_off
                    ? emit_op(OP_IMAGE_SAMPLE_EXPLICIT_LOD, sample_vty, {si_id, coord_id, 0x0Au, lod_f, const_off})
                    : emit_op(OP_IMAGE_SAMPLE_EXPLICIT_LOD, sample_vty, {si_id, coord_id, 0x02u, lod_f});
            } else {
                bool has_bias = (call_args.size() >= 7 && !const_float_is_zero(call_args[6]));
                uint32_t mask = (has_bias ? 0x01u : 0u) | (const_off ? 0x08u : 0u);
                std::vector<uint32_t> ops = {si_id, coord_id};
                if (mask) {
                    ops.push_back(mask);
                    if (has_bias) ops.push_back(get_val(call_args[6]).id);
                    if (const_off) ops.push_back(const_off);
                }
                res_v = emit_op_v(OP_IMAGE_SAMPLE_IMPLICIT_LOD, sample_vty, ops);
            }
        }
        if (sample_vty != res_spv_ty) res_v = emit_op(OP_BITCAST, res_spv_ty, {res_v});
        return res_v;
    }

    /* 4. Sincos */
    if (name.find("sincos") != std::string::npos && call_args.size() >= 2) {
        uint32_t x_id = get_val(call_args[0]).id;
        uint32_t ptr_vid = call_args[1];
        uint32_t byte_off_id = get_val(ptr_vid).id;
        uint32_t sin_id = emit_op(OP_EXT_INST, res_spv_ty, {glsl450_id_, GLSL_SIN, x_id});
        uint32_t cos_id = emit_op(OP_EXT_INST, res_spv_ty, {glsl450_id_, GLSL_COS, x_id});
        auto bit = ptr_base_info_.find(ptr_vid);
        if (bit != ptr_base_info_.end() && bit->second.kind == PtrBaseKind::Alloca) {
            const IRType &rt = mod_.types[ret_ir_ty];
            uint32_t n_comp = (rt.kind == IRTypeKind::Vec) ? rt.bits_or_len : 1;
            uint32_t w_idx = emit_u32_shr(byte_off_id, 2);
            uint32_t p_f32 = t_ptr(SC_FUNCTION, t_f32());
            for (uint32_t k = 0; k < n_comp; ++k) {
                uint32_t c_el = (n_comp == 1) ? cos_id : emit_op(OP_COMPOSITE_EXTRACT, t_f32(), {cos_id, k});
                uint32_t w_k = (k > 0) ? emit_op(OP_IADD, t_u32(), {w_idx, c_u32(k)}) : w_idx;
                uint32_t ac_id = emit_op(OP_ACCESS_CHAIN, p_f32, {bit->second.var_id, w_k});
                append_inst(code, OP_STORE, {ac_id, c_el});
            }
        }
        return sin_id;
    }

    /* 5. Standard math & vector intrinsics */
    std::vector<uint32_t> av;
    av.reserve(call_args.size());
    for (uint32_t a : call_args) av.push_back(get_val(a).id);

    if (starts_with(name, "air.dot.")) return emit_op(OP_DOT, res_spv_ty, {av[0], av[1]});
    if (starts_with(name, "air.dfdx.")) return emit_op(OP_DPDX, res_spv_ty, {av[0]});
    if (starts_with(name, "air.dfdy.")) return emit_op(OP_DPDY, res_spv_ty, {av[0]});
    if (starts_with(name, "air.any.")) return emit_op(OP_ANY, res_spv_ty, {av[0]});
    if (starts_with(name, "air.reverse_bits.")) return emit_op(OP_BIT_REVERSE, res_spv_ty, {av[0]});
    if (starts_with(name, "air.fast_fmod.")) return emit_op(OP_FREM, res_spv_ty, {av[0], av[1]});
    if (starts_with(name, "air.pack.unorm2x16")) return emit_op(OP_EXT_INST, res_spv_ty, {glsl450_id_, GLSL_PACK_UNORM_2X16, av[0]});
    if (starts_with(name, "air.fma.")) return emit_op(OP_EXT_INST, res_spv_ty, {glsl450_id_, GLSL_FMA, av[0], av[1], av[2]});
    if (starts_with(name, "air.mix.")) return emit_op(OP_EXT_INST, res_spv_ty, {glsl450_id_, GLSL_FMIX, av[0], av[1], av[2]});
    if (starts_with(name, "air.clamp.") || starts_with(name, "air.fast_clamp."))
        return emit_op(OP_EXT_INST, res_spv_ty, {glsl450_id_, GLSL_FCLAMP, av[0], av[1], av[2]});
    if (starts_with(name, "air.saturate.") || starts_with(name, "air.fast_saturate.")) {
        const IRType &rt = mod_.types[ret_ir_ty];
        uint32_t n_c = (rt.kind == IRTypeKind::Vec) ? rt.bits_or_len : 1;
        uint32_t zf = c_f32_bits(0), of = c_f32_bits(0x3F800000u);
        uint32_t c0 = (n_c == 1) ? zf : c_null(res_spv_ty);
        uint32_t c1 = (n_c == 1) ? of : c_composite(res_spv_ty, std::vector<uint32_t>(n_c, of));
        return emit_op(OP_EXT_INST, res_spv_ty, {glsl450_id_, GLSL_FCLAMP, av[0], c0, c1});
    }
    if (starts_with(name, "air.fmin.") || starts_with(name, "air.fast_fmin.") || starts_with(name, "_Z11_target_minff"))
        return emit_op(OP_EXT_INST, res_spv_ty, {glsl450_id_, GLSL_FMIN, av[0], av[1]});
    if (starts_with(name, "air.fmax.") || starts_with(name, "air.fast_fmax."))
        return emit_op(OP_EXT_INST, res_spv_ty, {glsl450_id_, GLSL_FMAX, av[0], av[1]});
    if (starts_with(name, "air.min.u."))
        return emit_op(OP_EXT_INST, res_spv_ty, {glsl450_id_, GLSL_UMIN, av[0], av[1]});
    if (starts_with(name, "air.max.s.")) {
        const IRType &rt = mod_.types[ret_ir_ty];
        uint32_t b_ty = (rt.kind == IRTypeKind::Vec) ? t_vec(t_bool(), rt.bits_or_len) : t_bool();
        uint32_t cmp = emit_op(OP_SGREATER_THAN, b_ty, {av[0], av[1]});
        return emit_op(OP_SELECT, res_spv_ty, {cmp, av[0], av[1]});
    }
    if (starts_with(name, "air.abs.s.")) {
        const IRType &rt = mod_.types[ret_ir_ty];
        uint32_t b_ty = (rt.kind == IRTypeKind::Vec) ? t_vec(t_bool(), rt.bits_or_len) : t_bool();
        uint32_t neg = emit_op(OP_SNEGATE, res_spv_ty, {av[0]});
        uint32_t cmp = emit_op(OP_SLESS_THAN, b_ty, {av[0], c_null(res_spv_ty)});
        return emit_op(OP_SELECT, res_spv_ty, {cmp, neg, av[0]});
    }
    if (starts_with(name, "air.fabs.") || starts_with(name, "air.fast_fabs."))
        return emit_op(OP_EXT_INST, res_spv_ty, {glsl450_id_, GLSL_FABS, av[0]});
    if (starts_with(name, "air.sign."))
        return emit_op(OP_EXT_INST, res_spv_ty, {glsl450_id_, GLSL_FSIGN, av[0]});
    if (starts_with(name, "air.floor.") || starts_with(name, "air.fast_floor.") || starts_with(name, "_Z13_target_floorf"))
        return emit_op(OP_EXT_INST, res_spv_ty, {glsl450_id_, GLSL_FLOOR, av[0]});
    if (starts_with(name, "air.fast_trunc."))
        return emit_op(OP_EXT_INST, res_spv_ty, {glsl450_id_, GLSL_TRUNC, av[0]});
    if (starts_with(name, "air.fract.") || starts_with(name, "air.fast_fract.") ||
        starts_with(name, "___metal_fract_") || starts_with(name, "_Z18_target_fast_fractf") ||
        starts_with(name, "_ZN11_fract_impl"))
        return emit_op(OP_EXT_INST, res_spv_ty, {glsl450_id_, GLSL_FRACT, av[0]});
    if (starts_with(name, "air.sqrt.") || starts_with(name, "air.fast_sqrt."))
        return emit_op(OP_EXT_INST, res_spv_ty, {glsl450_id_, GLSL_SQRT, av[0]});
    if (starts_with(name, "air.rsqrt.") || starts_with(name, "air.fast_rsqrt."))
        return emit_op(OP_EXT_INST, res_spv_ty, {glsl450_id_, GLSL_INVERSE_SQRT, av[0]});
    if (starts_with(name, "air.sin.") || starts_with(name, "air.fast_sin."))
        return emit_op(OP_EXT_INST, res_spv_ty, {glsl450_id_, GLSL_SIN, av[0]});
    if (starts_with(name, "air.cos.") || starts_with(name, "air.fast_cos."))
        return emit_op(OP_EXT_INST, res_spv_ty, {glsl450_id_, GLSL_COS, av[0]});
    if (starts_with(name, "air.atan2.") || starts_with(name, "air.fast_atan2."))
        return emit_op(OP_EXT_INST, res_spv_ty, {glsl450_id_, GLSL_ATAN2, av[0], av[1]});
    if (starts_with(name, "air.exp2.") || starts_with(name, "air.fast_exp2."))
        return emit_op(OP_EXT_INST, res_spv_ty, {glsl450_id_, GLSL_EXP2, av[0]});
    if (starts_with(name, "air.fast_exp."))
        return emit_op(OP_EXT_INST, res_spv_ty, {glsl450_id_, GLSL_EXP, av[0]});
    if (starts_with(name, "air.log2.") || starts_with(name, "air.fast_log2."))
        return emit_op(OP_EXT_INST, res_spv_ty, {glsl450_id_, GLSL_LOG2, av[0]});
    if (starts_with(name, "air.fast_log."))
        return emit_op(OP_EXT_INST, res_spv_ty, {glsl450_id_, GLSL_LOG, av[0]});
    if (starts_with(name, "air.pow.") || starts_with(name, "air.fast_pow."))
        return emit_op(OP_EXT_INST, res_spv_ty, {glsl450_id_, GLSL_POW, av[0], av[1]});

    return c_null(res_spv_ty);
}

} // namespace

bool translate_air_shader(const uint8_t *raw_entry, size_t len, uint32_t default_freq,
                          std::vector<uint32_t> &out_spirv, ShaderReflection &out_refl,
                          std::string &err) {
    size_t mtlb_idx = std::string::npos;
    for (size_t i = 0; i + 4 <= len; ++i) {
        if (raw_entry[i] == 'M' && raw_entry[i + 1] == 'T' && raw_entry[i + 2] == 'L' && raw_entry[i + 3] == 'B') {
            mtlb_idx = i;
            break;
        }
    }
    if (mtlb_idx == std::string::npos) {
        err = "No MTLB magic found in shader entry";
        return false;
    }
    FMetalCodeHeader fhdr = parse_fmetal_code_header(raw_entry, mtlb_idx);
    if (fhdr.frequency != 0 && fhdr.frequency != 3 && fhdr.frequency != 5) {
        fhdr.frequency = default_freq;
    }
    MtlbInfo mi;
    if (!parse_mtlb_container(raw_entry + mtlb_idx, len - mtlb_idx, mi, err)) return false;

    IRModule mod;
    if (!mod.decode(mi.raw_bc.data(), mi.raw_bc.size(), err)) return false;
    if (mod.functions.empty()) {
        err = "IRModule has no defined functions";
        return false;
    }

    SpirvBuilder builder(mod, fhdr, mi);
    return builder.build(out_spirv, out_refl);
}

} // namespace oh2::air2spirv
