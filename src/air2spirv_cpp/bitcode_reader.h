#ifndef OH2_AIR2SPIRV_BITCODE_READER_H_
#define OH2_AIR2SPIRV_BITCODE_READER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace oh2::air2spirv {

struct FMetalCodeHeader {
    uint32_t header_size = 0;
    uint32_t frequency = 0;
    uint32_t source_len = 0;
    uint32_t source_crc = 0;
    uint32_t local_size_x = 0;
    uint32_t local_size_y = 0;
    uint32_t local_size_z = 0;
};

struct MtlbInfo {
    uint16_t mtlb_ver[4]{};
    uint64_t total_size = 0;
    std::string entry_name = "main";
    uint32_t wrapper_magic = 0;
    std::vector<uint8_t> raw_bc;
    std::array<uint8_t, 32> mtlb_sha256{};
    uint64_t shader_id = 0;
};

enum class IRTypeKind : uint8_t {
    Void, F32, F16, Label, Opaque, Int, Ptr, Array, Vec, Metadata, Struct, Func
};

struct IRType {
    IRTypeKind kind = IRTypeKind::Void;
    uint32_t bits_or_len = 0;   /* int: bits, vec/array: count, ptr: addrspace */
    uint32_t elem_or_ret = 0;   /* ptr: pointee, vec/array: elem_ty, func: ret_ty */
    bool flag = false;          /* struct: is_packed, func: is_vararg */
    std::string name;           /* struct/opaque name */
    std::vector<uint32_t> members; /* struct: member tys, func: param tys */
};

struct IRTypeLayout {
    uint32_t size = 0;
    uint32_t align = 1;
    std::vector<uint32_t> member_offsets;
};

enum class IRValKind : uint8_t {
    Null, Undef, Int, Float, Agg, Data, Global, Func, Arg, Inst
};

struct IRValue {
    uint32_t ty = 0;
    IRValKind kind = IRValKind::Null;
    int64_t i64 = 0;            /* Int signed value, Float raw bits, Arg idx, Inst idx */
    uint32_t extra_ty = 0;      /* Global: val_ty, Func: func_ty */
    uint32_t extra_u32 = 0;     /* Global: addrspace, Func: ptr_ty */
    bool is_proto = false;      /* Func: is_proto */
    std::vector<uint64_t> list; /* Agg: sub value ids, Data: raw elements */
};

enum class MDNodeKind : uint8_t {
    Null, String, Val, Tuple
};

struct MDNode {
    MDNodeKind kind = MDNodeKind::Null;
    std::string str;
    uint32_t val_ty = 0;
    uint32_t val_id = 0;
    std::vector<int32_t> elems; /* indices into IRModule::md_nodes, or -1 for null */
};

enum class IROp : uint8_t {
    Binop, Cast, ExtractElt, InsertElt, ShuffleVec, Ret, Br, BrCond, Switch,
    Unreachable, Phi, Alloca, Load, InsertVal, Cmp2, Select, Call, Gep, Store
};

struct IRInst {
    IROp op = IROp::Unreachable;
    int32_t cid = -1;           /* result value id, or -1 if void */
    uint32_t ty = 0;            /* result/cond/agg/base type */
    uint32_t sub_op = 0;        /* binop opc, cast opc, cmp pred, base_ty (gep), ret_ty (call) */
    uint32_t flags = 0;
    uint32_t a = 0;             /* lhs, val, vec, ptr, cond, fn_id, agg, target_bb */
    uint32_t b = 0;             /* rhs, elt, elt_idx, t_bb, def_bb, val_vid (store) */
    uint32_t c = 0;             /* elt_idx (insertelt), mask, f_bb, fval (select) */
    std::vector<uint32_t> args; /* call_args, gep_indices, insertval indices */
    std::vector<std::pair<uint32_t, uint32_t>> pairs; /* phi (vid, bb) or switch (lit, bb) */
};

struct IRFuncRecord {
    uint32_t val_id = 0;
    uint32_t func_ty = 0;
    uint32_t ptr_ty = 0;
    bool is_proto = false;
};

struct IRFunction {
    IRFuncRecord info;
    std::string name;
    std::vector<uint32_t> args;
    std::vector<IRValue> vals;
    std::vector<IRInst> insts;
    std::vector<std::vector<uint32_t>> bbs; /* indices into insts */
};

struct IRModule {
    std::vector<IRType> types;
    std::vector<IRValue> values;
    std::vector<IRFuncRecord> func_records;
    std::vector<MDNode> md_nodes;
    std::unordered_map<std::string, std::vector<int32_t>> named_md;
    std::unordered_map<uint32_t, std::string> vst;
    std::vector<IRFunction> functions;
    std::vector<IRTypeLayout> type_layouts;

    bool decode(const uint8_t *bc, size_t len, std::string &err);
};

FMetalCodeHeader parse_fmetal_code_header(const uint8_t *hdr, size_t len);
bool parse_mtlb_container(const uint8_t *mtlb, size_t len, MtlbInfo &out, std::string &err);

} // namespace oh2::air2spirv

#endif /* OH2_AIR2SPIRV_BITCODE_READER_H_ */
