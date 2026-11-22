#!/usr/bin/env python3
"""
AIR 2.0 LLVM Bitcode -> Vulkan 1.1 SPIR-V 1.3 (0x00010300) Translator & Reflection Extractor.
"""

from __future__ import annotations

import math
import struct
from typing import Any

from .bitcode_reader import IRModule, parse_fmetal_code_header, parse_mtlb_container

# SPIR-V Constants & Opcodes
SPV_MAGIC = 0x07230203
SPV_VERSION_1_3 = 0x00010300
SPV_GENERATOR = 0x00000000

OP_UNDEF = 1
OP_EXT_INST_IMPORT = 11
OP_EXT_INST = 12
OP_MEMORY_MODEL = 14
OP_ENTRY_POINT = 15
OP_EXECUTION_MODE = 16
OP_CAPABILITY = 17
OP_TYPE_VOID = 19
OP_TYPE_BOOL = 20
OP_TYPE_INT = 21
OP_TYPE_FLOAT = 22
OP_TYPE_VECTOR = 23
OP_TYPE_IMAGE = 25
OP_TYPE_SAMPLER = 26
OP_TYPE_SAMPLED_IMAGE = 27
OP_TYPE_ARRAY = 28
OP_TYPE_RUNTIME_ARRAY = 29
OP_TYPE_STRUCT = 30
OP_TYPE_POINTER = 32
OP_TYPE_FUNCTION = 33
OP_CONSTANT_TRUE = 41
OP_CONSTANT_FALSE = 42
OP_CONSTANT = 43
OP_CONSTANT_COMPOSITE = 44
OP_CONSTANT_NULL = 46
OP_FUNCTION = 54
OP_FUNCTION_END = 56
OP_VARIABLE = 59
OP_LOAD = 61
OP_STORE = 62
OP_ACCESS_CHAIN = 65
OP_DECORATE = 71
OP_MEMBER_DECORATE = 72
OP_VECTOR_EXTRACT_DYNAMIC = 77
OP_VECTOR_INSERT_DYNAMIC = 78
OP_VECTOR_SHUFFLE = 79
OP_COMPOSITE_CONSTRUCT = 80
OP_COMPOSITE_EXTRACT = 81
OP_COMPOSITE_INSERT = 82
OP_COPY_OBJECT = 83
OP_SAMPLED_IMAGE = 86
OP_IMAGE_SAMPLE_IMPLICIT_LOD = 87
OP_IMAGE_SAMPLE_EXPLICIT_LOD = 88
OP_IMAGE_FETCH = 95
OP_IMAGE_GATHER = 96
OP_IMAGE_QUERY_SIZE_LOD = 103
OP_CONVERT_F_TO_U = 109
OP_CONVERT_F_TO_S = 110
OP_CONVERT_S_TO_F = 111
OP_CONVERT_U_TO_F = 112
OP_BITCAST = 124
OP_SNEGATE = 126
OP_FNEGATE = 127
OP_IADD = 128
OP_FADD = 129
OP_ISUB = 130
OP_FSUB = 131
OP_IMUL = 132
OP_FMUL = 133
OP_UDIV = 134
OP_SDIV = 135
OP_FDIV = 136
OP_UMOD = 137
OP_SREM = 138
OP_FREM = 140
OP_DOT = 148
OP_ANY = 154
OP_LOGICAL_EQUAL = 164
OP_LOGICAL_NOT_EQUAL = 165
OP_LOGICAL_OR = 166
OP_LOGICAL_AND = 167
OP_SELECT = 169
OP_IEQUAL = 170
OP_INOT_EQUAL = 171
OP_UGREATER_THAN = 172
OP_SGREATER_THAN = 173
OP_UGREATER_THAN_EQUAL = 174
OP_SGREATER_THAN_EQUAL = 175
OP_ULESS_THAN = 176
OP_SLESS_THAN = 177
OP_ULESS_THAN_EQUAL = 178
OP_SLESS_THAN_EQUAL = 179
OP_FORD_EQUAL = 180
OP_FUNORD_EQUAL = 181
OP_FORD_NOT_EQUAL = 182
OP_FUNORD_NOT_EQUAL = 183
OP_FORD_LESS_THAN = 184
OP_FUNORD_LESS_THAN = 185
OP_FORD_GREATER_THAN = 186
OP_FUNORD_GREATER_THAN = 187
OP_FORD_LESS_THAN_EQUAL = 188
OP_FUNORD_LESS_THAN_EQUAL = 189
OP_FORD_GREATER_THAN_EQUAL = 190
OP_FUNORD_GREATER_THAN_EQUAL = 191
OP_SHIFT_RIGHT_LOGICAL = 194
OP_SHIFT_RIGHT_ARITHMETIC = 195
OP_SHIFT_LEFT_LOGICAL = 196
OP_BITWISE_OR = 197
OP_BITWISE_XOR = 198
OP_BITWISE_AND = 199
OP_BIT_REVERSE = 204
OP_DPDX = 207
OP_DPDY = 208
OP_CONTROL_BARRIER = 224
OP_LOOP_MERGE = 246
OP_SELECTION_MERGE = 247
OP_LABEL = 248
OP_BRANCH = 249
OP_BRANCH_CONDITIONAL = 250
OP_KILL = 252
OP_RETURN = 253

# Storage Classes
SC_UNIFORM_CONSTANT = 0
SC_INPUT = 1
SC_UNIFORM = 2
SC_OUTPUT = 3
SC_WORKGROUP = 4
SC_FUNCTION = 7
SC_STORAGE_BUFFER = 12

# Decorations
DEC_RELAXED_PRECISION = 0
DEC_BLOCK = 2
DEC_ARRAY_STRIDE = 6
DEC_BUILTIN = 11
DEC_NOPERSPECTIVE = 13
DEC_FLAT = 14
DEC_NON_WRITABLE = 24
DEC_LOCATION = 30
DEC_BINDING = 33
DEC_DESCRIPTOR_SET = 34
DEC_OFFSET = 35

# BuiltIns
BUILTIN_POSITION = 0
BUILTIN_FRAG_COORD = 15
BUILTIN_FRONT_FACING = 17
BUILTIN_FRAG_DEPTH = 22
BUILTIN_WORKGROUP_ID = 26
BUILTIN_LOCAL_INVOCATION_ID = 27
BUILTIN_GLOBAL_INVOCATION_ID = 28
BUILTIN_VERTEX_INDEX = 42
BUILTIN_INSTANCE_INDEX = 43

# Capabilities
CAP_SHADER = 1
CAP_IMAGE_QUERY = 50

# GLSL.std.450 Extended Instructions
GLSL_TRUNC = 3
GLSL_FABS = 4
GLSL_FSIGN = 6
GLSL_FLOOR = 8
GLSL_FRACT = 10
GLSL_SIN = 13
GLSL_COS = 14
GLSL_ATAN2 = 25
GLSL_POW = 26
GLSL_EXP = 27
GLSL_LOG = 28
GLSL_EXP2 = 29
GLSL_LOG2 = 30
GLSL_SQRT = 31
GLSL_INVERSE_SQRT = 32
GLSL_FMIN = 37
GLSL_UMIN = 38
GLSL_FMAX = 40
GLSL_FCLAMP = 43
GLSL_FMIX = 46
GLSL_FMA = 50
GLSL_PACK_UNORM_2X16 = 57
GLSL_PACK_HALF_2X16 = 58

# Fixed semantic -> Location table (0..15)
USER_SEMANTIC_LOCATIONS = {
    "user(TEXCOORD0)": 0,
    "user(TEXCOORD1)": 1,
    "user(TEXCOORD2)": 2,
    "user(TEXCOORD3)": 3,
    "user(TEXCOORD4)": 4,
    "user(TEXCOORD5)": 5,
    "user(TEXCOORD6)": 6,
    "user(TEXCOORD7)": 7,
    "user(TEXCOORD8)": 8,
    "user(TEXCOORD10)": 9,
    "user(PARTICLE_POSITION)": 9,
    "user(TEXCOORD11)": 10,
    "user(PARTICLE_SUBUVS)": 10,
    "user(COLOR0)": 11,
    "user(FRINGE_COORDS)": 11,
    "user(COLOR1)": 12,
    "user(ORIGINAL_POSITION)": 12,
    "user(TANGENTX)": 13,
    "user(TANGENTZ)": 14,
}


def encode_word(opcode: int, word_count: int) -> int:
    return ((word_count & 0xFFFF) << 16) | (opcode & 0xFFFF)


def encode_inst(opcode: int, operands: list[int]) -> list[int]:
    return [encode_word(opcode, 1 + len(operands)), *operands]


def encode_str(s: str) -> list[int]:
    raw = s.encode("utf-8") + b"\x00"
    pad = (4 - (len(raw) & 3)) & 3
    raw += b"\x00" * pad
    return list(struct.unpack(f"<{len(raw)//4}I", raw))


def half_bits_to_float32_bits(hbits: int) -> int:
    h = hbits & 0xFFFF
    fval = struct.unpack("<e", struct.pack("<H", h))[0]
    if math.isnan(fval):
        return 0x7FC00000
    if math.isinf(fval):
        return 0xFF800000 if fval < 0 else 0x7F800000
    return struct.unpack("<I", struct.pack("<f", fval))[0]


def float32_to_bits(fval: float) -> int:
    return struct.unpack("<I", struct.pack("<f", fval))[0]


class SpirvBuilder:
    def __init__(self, mod: IRModule, fmetal_hdr: dict[str, Any], mtlb_info: dict[str, Any]):
        self.mod = mod
        self.fmetal_hdr = fmetal_hdr
        self.mtlb_info = mtlb_info
        self.fn = mod.functions[0]

        self.next_id = 1
        self.capabilities: set[int] = {CAP_SHADER}
        self.decorations: list[int] = []
        self.type_const_globals: list[int] = []
        self.entry_interface_vars: list[int] = []

        self.type_cache: dict[tuple[Any, ...], int] = {}
        self.const_cache: dict[tuple[Any, ...], int] = {}
        self.undef_cache: dict[int, int] = {}

        # Reflection results
        self.stage = 0  # 0=VS, 1=PS, 2=CS
        self.exec_model = 0  # 0=Vertex, 4=Fragment, 5=GLCompute
        self.desc_set = 0  # Set 0 = VS/CS, Set 1 = PS
        self.bindings_reflection: list[dict[str, int]] = []
        self.vertex_attrs_reflection: list[dict[str, Any]] = []
        self.buffer_mask = 0
        self.texture_mask = 0
        self.sampler_mask = 0
        self.fragment_output_mask = 0
        self.entry_flags = 0
        self.local_size = (0, 0, 0)

        self.glsl450_id = self.alloc_id()
        self.main_fn_id = self.alloc_id()

    def alloc_id(self) -> int:
        r = self.next_id
        self.next_id += 1
        return r

    def emit_dec(self, target_id: int, dec: int, literals: list[int] | None = None) -> None:
        ops = [target_id, dec]
        if literals:
            ops.extend(literals)
        self.decorations.extend(encode_inst(OP_DECORATE, ops))

    def emit_member_dec(self, struct_ty_id: int, member_idx: int, dec: int, literals: list[int] | None = None) -> None:
        ops = [struct_ty_id, member_idx, dec]
        if literals:
            ops.extend(literals)
        self.decorations.extend(encode_inst(OP_MEMBER_DECORATE, ops))

    # --- SPIR-V Type & Constant Management ---
    def t_void(self) -> int:
        k = ("void",)
        if k not in self.type_cache:
            tid = self.alloc_id()
            self.type_cache[k] = tid
            self.type_const_globals.extend(encode_inst(OP_TYPE_VOID, [tid]))
        return self.type_cache[k]

    def t_bool(self) -> int:
        k = ("bool",)
        if k not in self.type_cache:
            tid = self.alloc_id()
            self.type_cache[k] = tid
            self.type_const_globals.extend(encode_inst(OP_TYPE_BOOL, [tid]))
        return self.type_cache[k]

    def t_u32(self) -> int:
        k = ("int", 32, 0)
        if k not in self.type_cache:
            tid = self.alloc_id()
            self.type_cache[k] = tid
            self.type_const_globals.extend(encode_inst(OP_TYPE_INT, [tid, 32, 0]))
        return self.type_cache[k]

    def t_s32(self) -> int:
        k = ("int", 32, 1)
        if k not in self.type_cache:
            tid = self.alloc_id()
            self.type_cache[k] = tid
            self.type_const_globals.extend(encode_inst(OP_TYPE_INT, [tid, 32, 1]))
        return self.type_cache[k]

    def t_f32(self) -> int:
        k = ("f32",)
        if k not in self.type_cache:
            tid = self.alloc_id()
            self.type_cache[k] = tid
            self.type_const_globals.extend(encode_inst(OP_TYPE_FLOAT, [tid, 32]))
        return self.type_cache[k]

    def t_vec(self, elem_ty: int, count: int) -> int:
        if count == 1:
            return elem_ty
        k = ("vec", elem_ty, count)
        if k not in self.type_cache:
            tid = self.alloc_id()
            self.type_cache[k] = tid
            self.type_const_globals.extend(encode_inst(OP_TYPE_VECTOR, [tid, elem_ty, count]))
        return self.type_cache[k]

    def t_ptr(self, sc: int, pointee_ty: int) -> int:
        k = ("ptr", sc, pointee_ty)
        if k not in self.type_cache:
            tid = self.alloc_id()
            self.type_cache[k] = tid
            self.type_const_globals.extend(encode_inst(OP_TYPE_POINTER, [tid, sc, pointee_ty]))
        return self.type_cache[k]

    def t_array(self, elem_ty: int, length: int, stride: int | None = None) -> int:
        len_cid = self.c_u32(length)
        k = ("array", elem_ty, length, stride)
        if k not in self.type_cache:
            tid = self.alloc_id()
            self.type_cache[k] = tid
            self.type_const_globals.extend(encode_inst(OP_TYPE_ARRAY, [tid, elem_ty, len_cid]))
            if stride is not None:
                self.emit_dec(tid, DEC_ARRAY_STRIDE, [stride])
        return self.type_cache[k]

    def t_runtime_array(self, elem_ty: int, stride: int) -> int:
        k = ("rta", elem_ty, stride)
        if k not in self.type_cache:
            tid = self.alloc_id()
            self.type_cache[k] = tid
            self.type_const_globals.extend(encode_inst(OP_TYPE_RUNTIME_ARRAY, [tid, elem_ty]))
            self.emit_dec(tid, DEC_ARRAY_STRIDE, [stride])
        return self.type_cache[k]

    def t_ubo_block(self, num_vec4s: int) -> int:
        arr_ty = self.t_array(self.t_vec(self.t_f32(), 4), max(1, num_vec4s), stride=16)
        k = ("ubo_block", num_vec4s)
        if k not in self.type_cache:
            tid = self.alloc_id()
            self.type_cache[k] = tid
            self.type_const_globals.extend(encode_inst(OP_TYPE_STRUCT, [tid, arr_ty]))
            self.emit_dec(tid, DEC_BLOCK)
            self.emit_member_dec(tid, 0, DEC_OFFSET, [0])
        return self.type_cache[k]

    def t_ssbo_block(self, readonly: bool) -> int:
        rta_ty = self.t_runtime_array(self.t_u32(), stride=4)
        k = ("ssbo_block", readonly)
        if k not in self.type_cache:
            tid = self.alloc_id()
            self.type_cache[k] = tid
            self.type_const_globals.extend(encode_inst(OP_TYPE_STRUCT, [tid, rta_ty]))
            self.emit_dec(tid, DEC_BLOCK)
            self.emit_member_dec(tid, 0, DEC_OFFSET, [0])
            if readonly:
                self.emit_member_dec(tid, 0, DEC_NON_WRITABLE)
        return self.type_cache[k]

    def t_sampler(self) -> int:
        k = ("sampler",)
        if k not in self.type_cache:
            tid = self.alloc_id()
            self.type_cache[k] = tid
            self.type_const_globals.extend(encode_inst(OP_TYPE_SAMPLER, [tid]))
        return self.type_cache[k]

    def t_image(self, sampled_ty: int, dim: int, depth: int, arrayed: int, ms: int) -> int:
        # SPIR-V Dim: 1D=0, 2D=1, 3D=2, Cube=3
        k = ("image", sampled_ty, dim, depth, arrayed, ms)
        if k not in self.type_cache:
            tid = self.alloc_id()
            self.type_cache[k] = tid
            self.type_const_globals.extend(
                encode_inst(OP_TYPE_IMAGE, [tid, sampled_ty, dim, depth, arrayed, ms, 1, 0])
            )
        return self.type_cache[k]

    def t_sampled_image(self, img_ty: int) -> int:
        k = ("sampled_image", img_ty)
        if k not in self.type_cache:
            tid = self.alloc_id()
            self.type_cache[k] = tid
            self.type_const_globals.extend(encode_inst(OP_TYPE_SAMPLED_IMAGE, [tid, img_ty]))
        return self.type_cache[k]

    def t_func_void(self) -> int:
        v = self.t_void()
        k = ("func", v)
        if k not in self.type_cache:
            tid = self.alloc_id()
            self.type_cache[k] = tid
            self.type_const_globals.extend(encode_inst(OP_TYPE_FUNCTION, [tid, v]))
        return self.type_cache[k]

    def c_bool(self, val: bool) -> int:
        bty = self.t_bool()
        k = ("bool", bool(val))
        if k not in self.const_cache:
            cid = self.alloc_id()
            self.const_cache[k] = cid
            opc = OP_CONSTANT_TRUE if val else OP_CONSTANT_FALSE
            self.type_const_globals.extend(encode_inst(opc, [bty, cid]))
        return self.const_cache[k]

    def c_u32(self, val: int) -> int:
        uty = self.t_u32()
        uval = val & 0xFFFFFFFF
        k = ("u32", uval)
        if k not in self.const_cache:
            cid = self.alloc_id()
            self.const_cache[k] = cid
            self.type_const_globals.extend(encode_inst(OP_CONSTANT, [uty, cid, uval]))
        return self.const_cache[k]

    def c_f32_bits(self, bits: int) -> int:
        fty = self.t_f32()
        ubits = bits & 0xFFFFFFFF
        k = ("f32", ubits)
        if k not in self.const_cache:
            cid = self.alloc_id()
            self.const_cache[k] = cid
            self.type_const_globals.extend(encode_inst(OP_CONSTANT, [fty, cid, ubits]))
        return self.const_cache[k]

    def c_composite(self, vec_ty: int, elem_cids: tuple[int, ...]) -> int:
        k = ("comp", vec_ty, elem_cids)
        if k not in self.const_cache:
            cid = self.alloc_id()
            self.const_cache[k] = cid
            self.type_const_globals.extend(encode_inst(OP_CONSTANT_COMPOSITE, [vec_ty, cid, *elem_cids]))
        return self.const_cache[k]

    def c_null(self, spv_ty: int) -> int:
        k = ("null", spv_ty)
        if k not in self.const_cache:
            cid = self.alloc_id()
            self.const_cache[k] = cid
            self.type_const_globals.extend(encode_inst(OP_CONSTANT_NULL, [spv_ty, cid]))
        return self.const_cache[k]

    def map_ir_type_to_spv(self, ir_tid: int) -> int:
        t = self.mod.types[ir_tid]
        kind = t[0]
        if kind == "void":
            return self.t_void()
        elif kind in ("f32", "f16"):
            if kind == "f16":
                self.entry_flags |= (1 << 3)  # SPIRV_ENTRY_FLAG_RELAXED_PRECISION
            return self.t_f32()
        elif kind == "int":
            if t[1] == 1:
                return self.t_bool()
            return self.t_u32()
        elif kind == "vec":
            n_el, el_ir_ty = t[1], t[2]
            return self.t_vec(self.map_ir_type_to_spv(el_ir_ty), n_el)
        elif kind == "ptr":
            # Pointer values in SSA (offsets) are represented by uint32 byte offsets
            return self.t_u32()
        return self.t_u32()

    def is_float_ir_type(self, ir_tid: int) -> bool:
        t = self.mod.types[ir_tid]
        if t[0] in ("f32", "f16"):
            return True
        if t[0] == "vec":
            return self.mod.types[t[2]][0] in ("f32", "f16")
        return False

    def is_bool_ir_type(self, ir_tid: int) -> bool:
        t = self.mod.types[ir_tid]
        if t[0] == "int" and t[1] == 1:
            return True
        if t[0] == "vec":
            et = self.mod.types[t[2]]
            return et[0] == "int" and et[1] == 1
        return False

    def zero_for_ir_type(self, ir_tid: int) -> Any:
        t = self.mod.types[ir_tid]
        if t[0] == "struct":
            return [self.zero_for_ir_type(mt) for mt in t[3]]
        spv_ty = self.map_ir_type_to_spv(ir_tid)
        return self.c_null(spv_ty)

    def const_from_ir_val(self, vid: int, vals: list[tuple[int, str, Any]]) -> Any:
        ir_tid, vkind, vdata = vals[vid]
        t = self.mod.types[ir_tid]
        if t[0] == "struct":
            if vkind in ("null", "undef"):
                return [self.zero_for_ir_type(mt) for mt in t[3]]
            elif vkind == "agg":
                return [self.const_from_ir_val(sub_id, vals) for sub_id in vdata]
        if vkind in ("null", "undef"):
            if t[0] == "int" and t[1] > 1:
                return self.c_u32(0)
            if t[0] in ("f32", "f16"):
                return self.c_f32_bits(0)
            return self.c_null(self.map_ir_type_to_spv(ir_tid))
        elif vkind == "int":
            if t[0] == "int" and t[1] == 1:
                return self.c_bool(bool(vdata & 1))
            if t[0] == "int" and t[1] < 32:
                return self.c_u32(vdata & ((1 << t[1]) - 1))
            return self.c_u32(vdata)
        elif vkind == "float":
            if t[0] == "f16":
                self.entry_flags |= (1 << 3)
                return self.c_f32_bits(half_bits_to_float32_bits(vdata))
            return self.c_f32_bits(vdata)
        elif vkind == "agg":
            spv_ty = self.map_ir_type_to_spv(ir_tid)
            el_cids = tuple(self.const_from_ir_val(sub_id, vals) for sub_id in vdata)
            return self.c_composite(spv_ty, el_cids)
        elif vkind == "data":
            spv_ty = self.map_ir_type_to_spv(ir_tid)
            el_ir_ty = t[2]
            el_t = self.mod.types[el_ir_ty]
            el_cids_l = []
            for raw_el in vdata:
                if el_t[0] == "f16":
                    self.entry_flags |= (1 << 3)
                    el_cids_l.append(self.c_f32_bits(half_bits_to_float32_bits(raw_el)))
                elif el_t[0] == "f32":
                    el_cids_l.append(self.c_f32_bits(raw_el))
                elif el_t[0] == "int" and el_t[1] == 1:
                    el_cids_l.append(self.c_bool(bool(raw_el & 1)))
                else:
                    el_cids_l.append(self.c_u32(raw_el))
            return self.c_composite(spv_ty, tuple(el_cids_l))
        return self.c_null(self.map_ir_type_to_spv(ir_tid))

    def _parse_int_md(self, md_item: Any) -> int:
        if isinstance(md_item, tuple) and len(md_item) == 3 and md_item[0] == "val":
            vid = md_item[2]
            if vid < len(self.mod.values):
                return int(self.mod.values[vid][2] or 0)
        return 0

    def _setup_stage_and_io(self) -> None:
        stage_key = next((k for k in ("air.vertex", "air.fragment", "air.kernel") if k in self.mod.named_md), None)
        if stage_key == "air.vertex":
            self.stage = 0
            self.exec_model = 0
            self.desc_set = 0
        elif stage_key == "air.fragment":
            self.stage = 1
            self.exec_model = 4
            self.desc_set = 1
        elif stage_key == "air.kernel":
            self.stage = 2
            self.exec_model = 5
            self.desc_set = 0
            lx = self.fmetal_hdr.get("local_size_x", 0) or 64
            ly = self.fmetal_hdr.get("local_size_y", 0) or 1
            lz = self.fmetal_hdr.get("local_size_z", 0) or 1
            self.local_size = (lx, ly, lz)
        else:
            freq = self.fmetal_hdr.get("frequency", 3)
            self.stage = 0 if freq == 0 else (2 if freq == 5 else 1)
            self.exec_model = 0 if freq == 0 else (5 if freq == 5 else 4)
            self.desc_set = 1 if self.stage == 1 else 0
            if self.stage == 2:
                self.local_size = (64, 1, 1)

        entry_md = self.mod.named_md[stage_key][0] if (stage_key and self.mod.named_md.get(stage_key)) else ()
        out_mds = entry_md[1] if (len(entry_md) >= 2 and entry_md[1]) else ()
        in_mds = entry_md[2] if (len(entry_md) >= 3 and entry_md[2]) else ()

        # 1. Setup stage outputs
        ret_ir_ty = self.mod.types[self.fn["info"]["func_ty"]][2]
        rt = self.mod.types[ret_ir_ty]
        member_ir_tys = rt[3] if rt[0] == "struct" else (() if rt[0] == "void" else (ret_ir_ty,))

        self.output_vars: list[tuple[int, int, int]] = []  # (var_id, spv_ty, ir_ty)
        used_out_locs: set[int] = set()

        for idx, m_ir_ty in enumerate(member_ir_tys):
            spv_ty = self.map_ir_type_to_spv(m_ir_ty)
            ptr_ty = self.t_ptr(SC_OUTPUT, spv_ty)
            var_id = self.alloc_id()
            self.type_const_globals.extend(encode_inst(OP_VARIABLE, [ptr_ty, var_id, SC_OUTPUT]))
            self.entry_interface_vars.append(var_id)

            omd = out_mds[idx] if idx < len(out_mds) else ()
            kind = next((x for x in omd if isinstance(x, str) and x.startswith("air.")), "")
            if kind == "air.position":
                self.emit_dec(var_id, DEC_BUILTIN, [BUILTIN_POSITION])
            elif kind == "air.depth":
                self.emit_dec(var_id, DEC_BUILTIN, [BUILTIN_FRAG_DEPTH])
                self.entry_flags |= (1 << 4)  # SPIRV_ENTRY_FLAG_FRAG_DEPTH
            elif kind == "air.render_target":
                rt_loc = self._parse_int_md(omd[1]) if len(omd) > 1 else idx
                while rt_loc in used_out_locs:
                    rt_loc += 1
                used_out_locs.add(rt_loc)
                self.emit_dec(var_id, DEC_LOCATION, [rt_loc])
                self.fragment_output_mask |= (1 << rt_loc)
                if self.mod.types[m_ir_ty][0] == "f16" or (
                    self.mod.types[m_ir_ty][0] == "vec" and self.mod.types[self.mod.types[m_ir_ty][2]][0] == "f16"
                ):
                    self.emit_dec(var_id, DEC_RELAXED_PRECISION)
            else:
                # air.vertex_output
                sem = next((x for x in omd if isinstance(x, str) and x.startswith("user(")), "")
                loc = USER_SEMANTIC_LOCATIONS.get(sem, idx)
                while loc in used_out_locs:
                    loc = (loc + 1) & 15
                used_out_locs.add(loc)
                self.emit_dec(var_id, DEC_LOCATION, [loc])
                if not self.is_float_ir_type(m_ir_ty):
                    self.emit_dec(var_id, DEC_FLAT)
                elif "air.no_perspective" in omd:
                    self.emit_dec(var_id, DEC_NOPERSPECTIVE)

            self.output_vars.append((var_id, spv_ty, m_ir_ty))

        # 2. Setup stage inputs & resource descriptors for function arguments
        self.arg_init_kind: dict[int, tuple[Any, ...]] = {}
        self.ptr_base_info: dict[int, tuple[str, int, int]] = {}
        self.tex_arg_info: dict[int, tuple[int, int, int, int, int]] = {}  # arg_vid -> (var_id, img_ty, sampled_ty, dim, ms)
        self.samp_arg_info: dict[int, int] = {}  # arg_vid -> var_id

        used_in_locs: set[int] = set()
        builtin_in_cache: dict[int, tuple[int, int]] = {}

        def get_builtin_input(bi: int, spv_ty: int) -> tuple[int, int]:
            if bi not in builtin_in_cache:
                pty = self.t_ptr(SC_INPUT, spv_ty)
                vid = self.alloc_id()
                self.type_const_globals.extend(encode_inst(OP_VARIABLE, [pty, vid, SC_INPUT]))
                self.entry_interface_vars.append(vid)
                self.emit_dec(vid, DEC_BUILTIN, [bi])
                builtin_in_cache[bi] = (vid, spv_ty)
            return builtin_in_cache[bi]

        for arg_idx, arg_vid in enumerate(self.fn["args"]):
            arg_ir_ty = self.fn["vals"][arg_vid][0]
            amd = in_mds[arg_idx] if arg_idx < len(in_mds) else ()
            kind = next((x for x in amd if isinstance(x, str) and x.startswith("air.")), "")

            if kind == "air.vertex_input":
                loc = arg_idx
                arg_name = f"in_attr_{arg_idx}"
                arg_tname = "float4"
                for i, x in enumerate(amd):
                    if x == "air.location_index" and i + 1 < len(amd):
                        loc = self._parse_int_md(amd[i + 1])
                    elif x == "air.arg_name" and i + 1 < len(amd) and isinstance(amd[i + 1], str):
                        arg_name = amd[i + 1]
                    elif x == "air.arg_type_name" and i + 1 < len(amd) and isinstance(amd[i + 1], str):
                        arg_tname = amd[i + 1]
                while loc in used_in_locs:
                    loc += 1
                used_in_locs.add(loc)
                spv_ty = self.map_ir_type_to_spv(arg_ir_ty)
                pty = self.t_ptr(SC_INPUT, spv_ty)
                vid = self.alloc_id()
                self.type_const_globals.extend(encode_inst(OP_VARIABLE, [pty, vid, SC_INPUT]))
                self.entry_interface_vars.append(vid)
                self.emit_dec(vid, DEC_LOCATION, [loc])
                self.arg_init_kind[arg_vid] = ("load_input", vid, spv_ty)

                # Reflection
                comp_ty = 0
                if "half" in arg_tname:
                    comp_ty = 1
                elif "uint" in arg_tname:
                    comp_ty = 3
                elif "int" in arg_tname:
                    comp_ty = 2
                ir_t = self.mod.types[arg_ir_ty]
                n_comp = ir_t[1] if ir_t[0] == "vec" else 1
                self.vertex_attrs_reflection.append(
                    {
                        "location": loc,
                        "component_type": comp_ty,
                        "components": n_comp,
                        "flags": 0,
                        "name": arg_name,
                    }
                )

            elif kind == "air.fragment_input":
                sem = next((x for x in amd if isinstance(x, str) and x.startswith("user(")), "")
                loc = USER_SEMANTIC_LOCATIONS.get(sem, arg_idx)
                while loc in used_in_locs:
                    loc = (loc + 1) & 15
                used_in_locs.add(loc)
                spv_ty = self.map_ir_type_to_spv(arg_ir_ty)
                pty = self.t_ptr(SC_INPUT, spv_ty)
                vid = self.alloc_id()
                self.type_const_globals.extend(encode_inst(OP_VARIABLE, [pty, vid, SC_INPUT]))
                self.entry_interface_vars.append(vid)
                self.emit_dec(vid, DEC_LOCATION, [loc])
                if not self.is_float_ir_type(arg_ir_ty):
                    self.emit_dec(var_id if False else vid, DEC_FLAT)
                elif "air.no_perspective" in amd:
                    self.emit_dec(vid, DEC_NOPERSPECTIVE)
                self.arg_init_kind[arg_vid] = ("load_input", vid, spv_ty)

            elif kind == "air.position":
                vid, b_ty = get_builtin_input(BUILTIN_FRAG_COORD, self.t_vec(self.t_f32(), 4))
                self.arg_init_kind[arg_vid] = ("load_input", vid, b_ty)

            elif kind == "air.front_facing":
                vid, b_ty = get_builtin_input(BUILTIN_FRONT_FACING, self.t_bool())
                self.arg_init_kind[arg_vid] = ("load_input", vid, b_ty)

            elif kind == "air.render_target":
                # Framebuffer fetch input (gl_LastFragData): provide zero vector constant
                spv_ty = self.map_ir_type_to_spv(arg_ir_ty)
                self.arg_init_kind[arg_vid] = ("const", self.c_null(spv_ty))

            elif kind == "air.vertex_id":
                vid, b_ty = get_builtin_input(BUILTIN_VERTEX_INDEX, self.t_u32())
                self.arg_init_kind[arg_vid] = ("load_input", vid, b_ty)

            elif kind == "air.instance_id":
                vid, b_ty = get_builtin_input(BUILTIN_INSTANCE_INDEX, self.t_u32())
                self.arg_init_kind[arg_vid] = ("load_input", vid, b_ty)

            elif kind == "air.thread_position_in_grid":
                vid, b_ty = get_builtin_input(BUILTIN_GLOBAL_INVOCATION_ID, self.t_vec(self.t_u32(), 3))
                self.arg_init_kind[arg_vid] = ("load_input", vid, b_ty)

            elif kind == "air.thread_position_in_threadgroup":
                vid, b_ty = get_builtin_input(BUILTIN_LOCAL_INVOCATION_ID, self.t_vec(self.t_u32(), 3))
                self.arg_init_kind[arg_vid] = ("load_input", vid, b_ty)

            elif kind == "air.threadgroup_position_in_grid":
                vid, b_ty = get_builtin_input(BUILTIN_WORKGROUP_ID, self.t_vec(self.t_u32(), 3))
                self.arg_init_kind[arg_vid] = ("load_input", vid, b_ty)

            elif kind == "air.buffer":
                slot = 0
                buf_size = 0
                is_rw = "air.read_write" in amd
                for i, x in enumerate(amd):
                    if x == "air.location_index" and i + 1 < len(amd):
                        slot = self._parse_int_md(amd[i + 1])
                    elif x == "air.buffer_size" and i + 1 < len(amd):
                        buf_size = self._parse_int_md(amd[i + 1])
                ir_t = self.mod.types[arg_ir_ty]
                if ir_t[0] == "ptr" and ir_t[2] == 1:
                    is_rw = True

                if not is_rw and buf_size > 0:
                    num_vec4s = max(1, (buf_size + 15) // 16)
                    blk_ty = self.t_ubo_block(num_vec4s)
                    pty = self.t_ptr(SC_UNIFORM, blk_ty)
                    vid = self.alloc_id()
                    self.type_const_globals.extend(encode_inst(OP_VARIABLE, [pty, vid, SC_UNIFORM]))
                    self.emit_dec(vid, DEC_DESCRIPTOR_SET, [self.desc_set])
                    self.emit_dec(vid, DEC_BINDING, [slot])
                    self.ptr_base_info[arg_vid] = ("ubo", vid, num_vec4s)
                    desc_type = 6  # VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                else:
                    blk_ty = self.t_ssbo_block(readonly=not is_rw)
                    pty = self.t_ptr(SC_STORAGE_BUFFER, blk_ty)
                    vid = self.alloc_id()
                    self.type_const_globals.extend(encode_inst(OP_VARIABLE, [pty, vid, SC_STORAGE_BUFFER]))
                    self.emit_dec(vid, DEC_DESCRIPTOR_SET, [self.desc_set])
                    self.emit_dec(vid, DEC_BINDING, [slot])
                    self.ptr_base_info[arg_vid] = ("ssbo", vid, 0)
                    desc_type = 7  # VK_DESCRIPTOR_TYPE_STORAGE_BUFFER

                self.arg_init_kind[arg_vid] = ("const", self.c_u32(0))
                self.buffer_mask |= (1 << slot)
                self.bindings_reflection.append(
                    {
                        "set": self.desc_set,
                        "binding": slot,
                        "descriptor_type": desc_type,
                        "metal_slot": slot,
                        "metal_kind": 0,  # SPIRV_METAL_KIND_BUFFER
                        "image_dim": 0,
                        "is_depth": 0,
                        "flags": 3 if is_rw else 1,
                        "buffer_size_bytes": buf_size,
                    }
                )

            elif kind == "air.texture":
                slot = 0
                tname = "texture2d<float, sample>"
                for i, x in enumerate(amd):
                    if x == "air.location_index" and i + 1 < len(amd):
                        slot = self._parse_int_md(amd[i + 1])
                    elif x == "air.arg_type_name" and i + 1 < len(amd) and isinstance(amd[i + 1], str):
                        tname = amd[i + 1]

                sampled_ty = self.t_u32() if "uint" in tname else self.t_f32()
                spv_dim = 1  # 2D
                refl_dim = 2  # SPIRV_IMAGE_DIM_2D
                arrayed = 0
                ms = 0
                if "texturecube" in tname:
                    spv_dim = 3  # Cube
                    refl_dim = 4  # SPIRV_IMAGE_DIM_CUBE
                elif "texture3d" in tname:
                    spv_dim = 2  # 3D
                    refl_dim = 3  # SPIRV_IMAGE_DIM_3D
                elif "texture2d_array" in tname:
                    spv_dim = 1
                    arrayed = 1
                    refl_dim = 5  # SPIRV_IMAGE_DIM_2D_ARRAY
                elif "texture2d_ms" in tname:
                    spv_dim = 1
                    ms = 1
                    refl_dim = 6  # SPIRV_IMAGE_DIM_2D_MS

                img_ty = self.t_image(sampled_ty, spv_dim, 0, arrayed, ms)
                pty = self.t_ptr(SC_UNIFORM_CONSTANT, img_ty)
                vid = self.alloc_id()
                self.type_const_globals.extend(encode_inst(OP_VARIABLE, [pty, vid, SC_UNIFORM_CONSTANT]))
                self.emit_dec(vid, DEC_DESCRIPTOR_SET, [self.desc_set])
                self.emit_dec(vid, DEC_BINDING, [32 + slot])
                self.tex_arg_info[arg_vid] = (vid, img_ty, sampled_ty, spv_dim, ms)
                self.arg_init_kind[arg_vid] = ("const", self.c_u32(0))
                self.texture_mask |= (1 << slot)
                self.bindings_reflection.append(
                    {
                        "set": self.desc_set,
                        "binding": 32 + slot,
                        "descriptor_type": 2,  # VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                        "metal_slot": slot,
                        "metal_kind": 1,  # SPIRV_METAL_KIND_TEXTURE
                        "image_dim": refl_dim,
                        "is_depth": 1 if "depth" in tname else 0,
                        "flags": 1,
                        "buffer_size_bytes": 0,
                    }
                )

            elif kind == "air.sampler":
                slot = 0
                for i, x in enumerate(amd):
                    if x == "air.location_index" and i + 1 < len(amd):
                        slot = self._parse_int_md(amd[i + 1])
                samp_ty = self.t_sampler()
                pty = self.t_ptr(SC_UNIFORM_CONSTANT, samp_ty)
                vid = self.alloc_id()
                self.type_const_globals.extend(encode_inst(OP_VARIABLE, [pty, vid, SC_UNIFORM_CONSTANT]))
                self.emit_dec(vid, DEC_DESCRIPTOR_SET, [self.desc_set])
                self.emit_dec(vid, DEC_BINDING, [64 + slot])
                self.samp_arg_info[arg_vid] = vid
                self.arg_init_kind[arg_vid] = ("const", self.c_u32(0))
                self.sampler_mask |= (1 << slot)
                self.bindings_reflection.append(
                    {
                        "set": self.desc_set,
                        "binding": 64 + slot,
                        "descriptor_type": 0,  # VK_DESCRIPTOR_TYPE_SAMPLER
                        "metal_slot": slot,
                        "metal_kind": 2,  # SPIRV_METAL_KIND_SAMPLER
                        "image_dim": 0,
                        "is_depth": 0,
                        "flags": 1,
                        "buffer_size_bytes": 0,
                    }
                )

            else:
                spv_ty = self.map_ir_type_to_spv(arg_ir_ty)
                self.arg_init_kind[arg_vid] = ("const", self.c_null(spv_ty))

        # 3. Setup module-scope Workgroup (addrspace 3) globals
        for vid, (_pty, vkind, vdata) in enumerate(self.mod.values):
            if vkind == "global" and vdata["addrspace"] == 3:
                val_ty = vdata["val_ty"]
                byte_sz = max(4, self.mod.type_layouts[val_ty][0])
                num_words = (byte_sz + 3) // 4
                arr_ty = self.t_array(self.t_u32(), num_words)
                pty = self.t_ptr(SC_WORKGROUP, arr_ty)
                g_var_id = self.alloc_id()
                self.type_const_globals.extend(encode_inst(OP_VARIABLE, [pty, g_var_id, SC_WORKGROUP]))
                self.ptr_base_info[vid] = ("wg", g_var_id, num_words)
                self.entry_flags |= (1 << 1)  # SPIRV_ENTRY_FLAG_USES_WORKGROUP

    def build(self) -> tuple[bytes, dict[str, Any]]:
        self._setup_stage_and_io()

        fn = self.fn
        vals = fn["vals"]
        bbs: list[list[tuple[Any, ...]]] = fn["bbs"]
        num_bbs = len(bbs)

        # Track known u32 constants for compile-time offset folding
        self.known_u32: dict[int, int] = {}
        for (ck, cval), cid in self.const_cache.items():
            if ck == "u32":
                self.known_u32[cid] = cval

        orig_c_u32 = self.c_u32

        def tracked_c_u32(v: int) -> int:
            cid = orig_c_u32(v)
            self.known_u32[cid] = v & 0xFFFFFFFF
            return cid

        self.c_u32 = tracked_c_u32  # type: ignore[method-assign]

        # 1. Assign alloca offsets and propagate pointer bases across all instructions
        alloca_offsets: dict[int, int] = {}
        total_alloca_bytes = 0
        for ins in fn["insts"]:
            if ins[0] == "alloca":
                _, cid, _ptr_ty, pointee_ty = ins
                sz = max(4, self.mod.type_layouts[pointee_ty][0])
                sz_aligned = (sz + 15) & ~15
                alloca_offsets[cid] = total_alloca_bytes
                total_alloca_bytes += sz_aligned

        alloca_var_id = 0
        alloca_words = max(1, (total_alloca_bytes + 3) // 4) if total_alloca_bytes > 0 else 0
        entry_local_vars: list[int] = []
        if alloca_words > 0:
            arr_ty = self.t_array(self.t_f32(), alloca_words)
            pty = self.t_ptr(SC_FUNCTION, arr_ty)
            alloca_var_id = self.alloc_id()
            entry_local_vars.extend(encode_inst(OP_VARIABLE, [pty, alloca_var_id, SC_FUNCTION]))
            for cid in alloca_offsets:
                self.ptr_base_info[cid] = ("alloca", alloca_var_id, alloca_words)

        # Propagate ptr_base_info through gep, cast, phi, select (fixed point)
        changed = True
        while changed:
            changed = False
            for ins in fn["insts"]:
                op = ins[0]
                if op == "gep":
                    _, cid, _res_pty, _bty, src_ptr, _indices = ins
                    if cid not in self.ptr_base_info and src_ptr in self.ptr_base_info:
                        self.ptr_base_info[cid] = self.ptr_base_info[src_ptr]
                        changed = True
                elif op == "cast":
                    _, cid, _dst_ty, _copc, src_val = ins
                    if cid not in self.ptr_base_info and src_val in self.ptr_base_info:
                        self.ptr_base_info[cid] = self.ptr_base_info[src_val]
                        changed = True
                elif op == "phi":
                    _, cid, _ty, pairs = ins
                    if cid not in self.ptr_base_info:
                        for pvid, _pbb in pairs:
                            if pvid in self.ptr_base_info:
                                self.ptr_base_info[cid] = self.ptr_base_info[pvid]
                                changed = True
                                break
                elif op == "select":
                    _, cid, _ty, _cond, tv, fv = ins
                    if cid not in self.ptr_base_info:
                        if tv in self.ptr_base_info:
                            self.ptr_base_info[cid] = self.ptr_base_info[tv]
                            changed = True
                        elif fv in self.ptr_base_info:
                            self.ptr_base_info[cid] = self.ptr_base_info[fv]
                            changed = True

        # 2. Identify cross-block SSA values & phi nodes for reg2mem promotion
        def_bb_of_val: dict[int, int] = {}
        cross_block_vals: set[int] = set()
        phi_by_bb: dict[int, list[tuple[int, int, list[tuple[int, int]]]]] = {}

        for b_idx, bb in enumerate(bbs):
            for ins in bb:
                op = ins[0]
                if op in ("binop", "cast", "extractelt", "insertelt", "shufflevec", "phi", "alloca", "load", "insertval", "cmp2", "select", "gep"):
                    def_bb_of_val[ins[1]] = b_idx
                elif op == "call" and ins[1] is not None:
                    def_bb_of_val[ins[1]] = b_idx
                if op == "phi":
                    _, cid, ty, pairs = ins
                    cross_block_vals.add(cid)
                    phi_by_bb.setdefault(b_idx, []).append((cid, ty, pairs))

        def check_used(vid: int, cur_bb_idx: int) -> None:
            if vid in def_bb_of_val and def_bb_of_val[vid] != cur_bb_idx:
                cross_block_vals.add(vid)

        for b_idx, bb in enumerate(bbs):
            for ins in bb:
                op = ins[0]
                if op == "binop":
                    check_used(ins[4], b_idx)
                    check_used(ins[5], b_idx)
                elif op == "cast":
                    check_used(ins[4], b_idx)
                elif op == "extractelt":
                    check_used(ins[3], b_idx)
                    check_used(ins[4], b_idx)
                elif op == "insertelt":
                    check_used(ins[3], b_idx)
                    check_used(ins[4], b_idx)
                    check_used(ins[5], b_idx)
                elif op == "shufflevec":
                    check_used(ins[3], b_idx)
                    check_used(ins[4], b_idx)
                elif op == "ret" and ins[2] is not None:
                    check_used(ins[2], b_idx)
                elif op == "br_cond":
                    check_used(ins[1], b_idx)
                elif op == "switch":
                    check_used(ins[2], b_idx)
                elif op == "phi":
                    for pvid, pbb in ins[3]:
                        if pvid in def_bb_of_val:
                            cross_block_vals.add(pvid)
                elif op == "load":
                    check_used(ins[3], b_idx)
                elif op == "store":
                    check_used(ins[1], b_idx)
                    check_used(ins[2], b_idx)
                elif op == "insertval":
                    check_used(ins[3], b_idx)
                    check_used(ins[4], b_idx)
                elif op == "cmp2":
                    check_used(ins[4], b_idx)
                    check_used(ins[5], b_idx)
                elif op == "select":
                    check_used(ins[3], b_idx)
                    check_used(ins[4], b_idx)
                    check_used(ins[5], b_idx)
                elif op == "call":
                    for avid in ins[4]:
                        check_used(avid, b_idx)
                elif op == "gep":
                    check_used(ins[4], b_idx)
                    for ivid in ins[5]:
                        check_used(ivid, b_idx)

        # Create entry-block Function variables for cross_block_vals
        reg2mem_vars: dict[int, Any] = {}  # vid -> var_id (or list of var_ids for struct)
        for vid in sorted(cross_block_vals):
            ir_ty = vals[vid][0]
            t = self.mod.types[ir_ty]
            if t[0] == "struct":
                vlist = []
                for m_ty in t[3]:
                    spv_ty = self.map_ir_type_to_spv(m_ty)
                    pty = self.t_ptr(SC_FUNCTION, spv_ty)
                    fvar = self.alloc_id()
                    entry_local_vars.extend(encode_inst(OP_VARIABLE, [pty, fvar, SC_FUNCTION]))
                    vlist.append((fvar, spv_ty))
                reg2mem_vars[vid] = vlist
            else:
                spv_ty = self.map_ir_type_to_spv(ir_ty)
                pty = self.t_ptr(SC_FUNCTION, spv_ty)
                fvar = self.alloc_id()
                entry_local_vars.extend(encode_inst(OP_VARIABLE, [pty, fvar, SC_FUNCTION]))
                reg2mem_vars[vid] = (fvar, spv_ty)

        # If multi-BB, create %var_next_bb in Function storage class
        var_next_bb = 0
        if num_bbs > 1:
            pty_u32 = self.t_ptr(SC_FUNCTION, self.t_u32())
            var_next_bb = self.alloc_id()
            entry_local_vars.extend(encode_inst(OP_VARIABLE, [pty_u32, var_next_bb, SC_FUNCTION]))

        # Pre-populate module/function constants & initial argument loads in %entry
        entry_init_code: list[int] = []
        global_ssa_map: dict[int, Any] = {}

        for vid, (ir_ty, vkind, _vdata) in enumerate(vals):
            if vkind in ("null", "undef", "int", "float", "agg", "data"):
                global_ssa_map[vid] = self.const_from_ir_val(vid, vals)
            elif vkind == "global":
                global_ssa_map[vid] = self.c_u32(0)
            elif vkind == "arg":
                akind = self.arg_init_kind.get(vid, ("const", self.c_u32(0)))
                if akind[0] == "const":
                    global_ssa_map[vid] = akind[1]
                elif akind[0] == "load_input":
                    in_var_id, in_spv_ty = akind[1], akind[2]
                    ld_id = self.alloc_id()
                    entry_init_code.extend(encode_inst(OP_LOAD, [in_spv_ty, ld_id, in_var_id]))
                    global_ssa_map[vid] = ld_id

        # Initialize all reg2mem_vars in entry block so they are always initialized
        for vid, vinfo in reg2mem_vars.items():
            ir_ty = vals[vid][0]
            zval = self.zero_for_ir_type(ir_ty)
            if isinstance(vinfo, list):
                for (fvar, _sty), zel in zip(vinfo, zval):
                    entry_init_code.extend(encode_inst(OP_STORE, [fvar, zel]))
            else:
                fvar, _sty = vinfo
                entry_init_code.extend(encode_inst(OP_STORE, [fvar, zval]))

        # Helper to translate a single basic block's instructions
        def emit_bb_instructions(b_idx: int, bb: list[tuple[Any, ...]]) -> tuple[list[int], bool]:
            code: list[int] = []
            local_ssa: dict[int, Any] = dict(global_ssa_map)
            terminated_by_kill_or_ret = False

            def get_val(vid: int) -> Any:
                if vid in local_ssa:
                    return local_ssa[vid]
                if vid in reg2mem_vars:
                    vinfo = reg2mem_vars[vid]
                    if isinstance(vinfo, list):
                        res_l = []
                        for fvar, sty in vinfo:
                            lid = self.alloc_id()
                            code.extend(encode_inst(OP_LOAD, [sty, lid, fvar]))
                            res_l.append(lid)
                        local_ssa[vid] = res_l
                        return res_l
                    else:
                        fvar, sty = vinfo
                        lid = self.alloc_id()
                        code.extend(encode_inst(OP_LOAD, [sty, lid, fvar]))
                        local_ssa[vid] = lid
                        return lid
                z = self.zero_for_ir_type(vals[vid][0])
                local_ssa[vid] = z
                return z

            def set_val(vid: int, val: Any) -> None:
                local_ssa[vid] = val
                if vid in reg2mem_vars:
                    vinfo = reg2mem_vars[vid]
                    if isinstance(vinfo, list):
                        for (fvar, _sty), vel in zip(vinfo, val):
                            code.extend(encode_inst(OP_STORE, [fvar, vel]))
                    else:
                        fvar, _sty = vinfo
                        code.extend(encode_inst(OP_STORE, [fvar, val]))

            # Load phis defined at top of b_idx into local_ssa
            for cid, _ty, _pairs in phi_by_bb.get(b_idx, ()):
                get_val(cid)

            def emit_op(opcode: int, res_ty: int, operands: list[int]) -> int:
                rid = self.alloc_id()
                code.extend(encode_inst(opcode, [res_ty, rid, *operands]))
                return rid

            def emit_u32_add(a: int, b: int) -> int:
                if a in self.known_u32 and b in self.known_u32:
                    return self.c_u32(self.known_u32[a] + self.known_u32[b])
                if a in self.known_u32 and self.known_u32[a] == 0:
                    return b
                if b in self.known_u32 and self.known_u32[b] == 0:
                    return a
                return emit_op(OP_IADD, self.t_u32(), [a, b])

            def emit_u32_mul(a: int, b: int) -> int:
                if a in self.known_u32 and b in self.known_u32:
                    return self.c_u32(self.known_u32[a] * self.known_u32[b])
                if a in self.known_u32 and self.known_u32[a] == 0:
                    return self.c_u32(0)
                if b in self.known_u32 and self.known_u32[b] == 0:
                    return self.c_u32(0)
                if a in self.known_u32 and self.known_u32[a] == 1:
                    return b
                if b in self.known_u32 and self.known_u32[b] == 1:
                    return a
                return emit_op(OP_IMUL, self.t_u32(), [a, b])

            def emit_u32_shr(a: int, shift: int) -> int:
                if shift == 0:
                    return a
                if a in self.known_u32:
                    return self.c_u32(self.known_u32[a] >> shift)
                return emit_op(OP_SHIFT_RIGHT_LOGICAL, self.t_u32(), [a, self.c_u32(shift)])

            def emit_u32_and(a: int, mask: int) -> int:
                if a in self.known_u32:
                    return self.c_u32(self.known_u32[a] & mask)
                return emit_op(OP_BITWISE_AND, self.t_u32(), [a, self.c_u32(mask)])

            def emit_phi_copies_for_edge(target_bb: int) -> dict[int, Any]:
                # Compute all incoming values first (parallel copy semantics)
                copies: dict[int, Any] = {}
                for phi_cid, phi_ty, pairs in phi_by_bb.get(target_bb, ()):
                    chosen_vid = None
                    for pvid, pbb in pairs:
                        if pbb == b_idx:
                            chosen_vid = pvid
                            break
                    if chosen_vid is not None:
                        copies[phi_cid] = get_val(chosen_vid)
                    else:
                        copies[phi_cid] = self.zero_for_ir_type(phi_ty)
                return copies

            def commit_phi_copies(copies: dict[int, Any]) -> None:
                for phi_cid, val in copies.items():
                    vinfo = reg2mem_vars[phi_cid]
                    if isinstance(vinfo, list):
                        for (fvar, _sty), vel in zip(vinfo, val):
                            code.extend(encode_inst(OP_STORE, [fvar, vel]))
                    else:
                        fvar, _sty = vinfo
                        code.extend(encode_inst(OP_STORE, [fvar, val]))

            for ins in bb:
                if terminated_by_kill_or_ret:
                    break
                op = ins[0]

                if op == "phi":
                    continue

                elif op == "alloca":
                    _, cid, _ptr_ty, _pointee_ty = ins
                    set_val(cid, self.c_u32(alloca_offsets.get(cid, 0)))

                elif op == "gep":
                    _, cid, _res_pty, base_ty, src_ptr, indices = ins
                    off_id = get_val(src_ptr)
                    cur_t = base_ty
                    for idx_pos, ivid in enumerate(indices):
                        ival_id = get_val(ivid)
                        if idx_pos == 0:
                            ty_sz = self.mod.type_layouts[cur_t][0]
                            off_id = emit_u32_add(off_id, emit_u32_mul(ival_id, self.c_u32(ty_sz)))
                        else:
                            ct = self.mod.types[cur_t]
                            if ct[0] == "struct":
                                f_idx = int(vals[ivid][2] or 0)
                                f_off = self.mod.type_layouts[cur_t][2][f_idx]
                                cur_t = ct[3][f_idx]
                                if f_off != 0:
                                    off_id = emit_u32_add(off_id, self.c_u32(f_off))
                            elif ct[0] in ("array", "vec"):
                                el_t = ct[2]
                                el_sz = self.mod.type_layouts[el_t][0]
                                cur_t = el_t
                                off_id = emit_u32_add(off_id, emit_u32_mul(ival_id, self.c_u32(el_sz)))
                    set_val(cid, off_id)

                elif op == "load":
                    _, cid, res_ir_ty, ptr_vid = ins
                    byte_off_id = get_val(ptr_vid)
                    binfo = self.ptr_base_info.get(ptr_vid)
                    res_spv_ty = self.map_ir_type_to_spv(res_ir_ty)
                    if binfo is None:
                        set_val(cid, self.c_null(res_spv_ty))
                        continue
                    bkind, bvar_id, _bcap = binfo
                    rt = self.mod.types[res_ir_ty]
                    n_comp = rt[1] if rt[0] == "vec" else 1
                    el_ir_ty = rt[2] if rt[0] == "vec" else res_ir_ty
                    el_is_float = self.is_float_ir_type(el_ir_ty)
                    el_is_bool = self.is_bool_ir_type(el_ir_ty)
                    el_spv_ty = self.map_ir_type_to_spv(el_ir_ty)
                    word_idx_id = emit_u32_shr(byte_off_id, 2)

                    # Fast path: aligned vec4 load from UBO
                    if (
                        bkind == "ubo"
                        and n_comp == 4
                        and word_idx_id in self.known_u32
                        and (self.known_u32[word_idx_id] & 3) == 0
                    ):
                        v4_idx = self.c_u32(self.known_u32[word_idx_id] >> 2)
                        v4f_ty = self.t_vec(self.t_f32(), 4)
                        p_v4f = self.t_ptr(SC_UNIFORM, v4f_ty)
                        ac_id = emit_op(OP_ACCESS_CHAIN, p_v4f, [bvar_id, self.c_u32(0), v4_idx])
                        ld_id = emit_op(OP_LOAD, v4f_ty, [ac_id])
                        if not el_is_float:
                            ld_id = emit_op(OP_BITCAST, res_spv_ty, [ld_id])
                        set_val(cid, ld_id)
                        continue

                    comp_ids = []
                    for k in range(n_comp):
                        w_id = emit_u32_add(word_idx_id, self.c_u32(k))
                        if bkind == "ubo":
                            v4_idx = emit_u32_shr(w_id, 2)
                            c_idx = emit_u32_and(w_id, 3)
                            p_f32 = self.t_ptr(SC_UNIFORM, self.t_f32())
                            ac_id = emit_op(OP_ACCESS_CHAIN, p_f32, [bvar_id, self.c_u32(0), v4_idx, c_idx])
                            v_id = emit_op(OP_LOAD, self.t_f32(), [ac_id])
                            if el_is_bool:
                                u_id = emit_op(OP_BITCAST, self.t_u32(), [v_id])
                                v_id = emit_op(OP_INOT_EQUAL, self.t_bool(), [u_id, self.c_u32(0)])
                            elif not el_is_float:
                                v_id = emit_op(OP_BITCAST, self.t_u32(), [v_id])
                        elif bkind == "ssbo":
                            p_u32 = self.t_ptr(SC_STORAGE_BUFFER, self.t_u32())
                            ac_id = emit_op(OP_ACCESS_CHAIN, p_u32, [bvar_id, self.c_u32(0), w_id])
                            v_id = emit_op(OP_LOAD, self.t_u32(), [ac_id])
                            if el_is_float:
                                v_id = emit_op(OP_BITCAST, self.t_f32(), [v_id])
                            elif el_is_bool:
                                v_id = emit_op(OP_INOT_EQUAL, self.t_bool(), [v_id, self.c_u32(0)])
                        elif bkind == "wg":
                            p_u32 = self.t_ptr(SC_WORKGROUP, self.t_u32())
                            ac_id = emit_op(OP_ACCESS_CHAIN, p_u32, [bvar_id, w_id])
                            v_id = emit_op(OP_LOAD, self.t_u32(), [ac_id])
                            if el_is_float:
                                v_id = emit_op(OP_BITCAST, self.t_f32(), [v_id])
                            elif el_is_bool:
                                v_id = emit_op(OP_INOT_EQUAL, self.t_bool(), [v_id, self.c_u32(0)])
                        else:  # alloca
                            p_f32 = self.t_ptr(SC_FUNCTION, self.t_f32())
                            ac_id = emit_op(OP_ACCESS_CHAIN, p_f32, [bvar_id, w_id])
                            v_id = emit_op(OP_LOAD, self.t_f32(), [ac_id])
                            if el_is_bool:
                                u_id = emit_op(OP_BITCAST, self.t_u32(), [v_id])
                                v_id = emit_op(OP_INOT_EQUAL, self.t_bool(), [u_id, self.c_u32(0)])
                            elif not el_is_float:
                                v_id = emit_op(OP_BITCAST, self.t_u32(), [v_id])
                        comp_ids.append(v_id)

                    if n_comp == 1:
                        set_val(cid, comp_ids[0])
                    else:
                        set_val(cid, emit_op(OP_COMPOSITE_CONSTRUCT, res_spv_ty, comp_ids))

                elif op == "store":
                    _, ptr_vid, val_vid = ins
                    byte_off_id = get_val(ptr_vid)
                    sval = get_val(val_vid)
                    binfo = self.ptr_base_info.get(ptr_vid)
                    if binfo is None:
                        continue
                    bkind, bvar_id, _bcap = binfo
                    val_ir_ty = vals[val_vid][0]
                    vt = self.mod.types[val_ir_ty]
                    n_comp = vt[1] if vt[0] == "vec" else 1
                    el_ir_ty = vt[2] if vt[0] == "vec" else val_ir_ty
                    el_is_float = self.is_float_ir_type(el_ir_ty)
                    el_spv_ty = self.map_ir_type_to_spv(el_ir_ty)
                    word_idx_id = emit_u32_shr(byte_off_id, 2)

                    for k in range(n_comp):
                        comp_id = sval if n_comp == 1 else emit_op(OP_COMPOSITE_EXTRACT, el_spv_ty, [sval, k])
                        w_id = emit_u32_add(word_idx_id, self.c_u32(k))
                        if bkind == "alloca":
                            if not el_is_float:
                                comp_id = emit_op(OP_BITCAST, self.t_f32(), [comp_id])
                            p_f32 = self.t_ptr(SC_FUNCTION, self.t_f32())
                            ac_id = emit_op(OP_ACCESS_CHAIN, p_f32, [bvar_id, w_id])
                            code.extend(encode_inst(OP_STORE, [ac_id, comp_id]))
                        elif bkind == "ssbo":
                            if el_is_float:
                                comp_id = emit_op(OP_BITCAST, self.t_u32(), [comp_id])
                            p_u32 = self.t_ptr(SC_STORAGE_BUFFER, self.t_u32())
                            ac_id = emit_op(OP_ACCESS_CHAIN, p_u32, [bvar_id, self.c_u32(0), w_id])
                            code.extend(encode_inst(OP_STORE, [ac_id, comp_id]))
                        elif bkind == "wg":
                            if el_is_float:
                                comp_id = emit_op(OP_BITCAST, self.t_u32(), [comp_id])
                            p_u32 = self.t_ptr(SC_WORKGROUP, self.t_u32())
                            ac_id = emit_op(OP_ACCESS_CHAIN, p_u32, [bvar_id, w_id])
                            code.extend(encode_inst(OP_STORE, [ac_id, comp_id]))

                elif op == "binop":
                    _, cid, ir_ty, b_opc, lhs_vid, rhs_vid, _fl = ins
                    lhs = get_val(lhs_vid)
                    rhs = get_val(rhs_vid)
                    res_ty = self.map_ir_type_to_spv(ir_ty)
                    is_fl = self.is_float_ir_type(ir_ty)
                    is_bl = self.is_bool_ir_type(ir_ty)
                    if b_opc == 0:
                        spv_op = OP_FADD if is_fl else OP_IADD
                        res = emit_op(spv_op, res_ty, [lhs, rhs])
                    elif b_opc == 1:
                        spv_op = OP_FSUB if is_fl else OP_ISUB
                        res = emit_op(spv_op, res_ty, [lhs, rhs])
                    elif b_opc == 2:
                        spv_op = OP_FMUL if is_fl else OP_IMUL
                        res = emit_op(spv_op, res_ty, [lhs, rhs])
                    elif b_opc == 3:
                        res = emit_op(OP_UDIV, res_ty, [lhs, rhs])
                    elif b_opc == 4:
                        spv_op = OP_FDIV if is_fl else OP_SDIV
                        res = emit_op(spv_op, res_ty, [lhs, rhs])
                    elif b_opc == 5:
                        res = emit_op(OP_UMOD, res_ty, [lhs, rhs])
                    elif b_opc == 6:
                        spv_op = OP_FREM if is_fl else OP_SREM
                        res = emit_op(spv_op, res_ty, [lhs, rhs])
                    elif b_opc == 7:
                        res = emit_op(OP_SHIFT_LEFT_LOGICAL, res_ty, [lhs, rhs])
                    elif b_opc == 8:
                        res = emit_op(OP_SHIFT_RIGHT_LOGICAL, res_ty, [lhs, rhs])
                    elif b_opc == 9:
                        res = emit_op(OP_SHIFT_RIGHT_ARITHMETIC, res_ty, [lhs, rhs])
                    elif b_opc == 10:
                        spv_op = OP_LOGICAL_AND if is_bl else OP_BITWISE_AND
                        res = emit_op(spv_op, res_ty, [lhs, rhs])
                    elif b_opc == 11:
                        spv_op = OP_LOGICAL_OR if is_bl else OP_BITWISE_OR
                        res = emit_op(spv_op, res_ty, [lhs, rhs])
                    elif b_opc == 12:
                        spv_op = OP_LOGICAL_NOT_EQUAL if is_bl else OP_BITWISE_XOR
                        res = emit_op(spv_op, res_ty, [lhs, rhs])
                    else:
                        res = lhs
                    set_val(cid, res)

                elif op == "cast":
                    _, cid, dst_ir_ty, cast_opc, src_vid = ins
                    src_ir_ty = vals[src_vid][0]
                    src_val = get_val(src_vid)
                    dst_t = self.mod.types[dst_ir_ty]
                    src_t = self.mod.types[src_ir_ty]
                    dst_spv_ty = self.map_ir_type_to_spv(dst_ir_ty)

                    if dst_t[0] == "ptr" or src_t[0] == "ptr":
                        # Pointer bitcast preserves byte offset
                        set_val(cid, src_val)
                    elif cast_opc in (7, 8):  # fptrunc, fpext (both f16 and f32 map to OpTypeFloat 32)
                        set_val(cid, src_val)
                    elif cast_opc in (0, 1, 2):  # trunc, zext, sext
                        src_is_b = self.is_bool_ir_type(src_ir_ty)
                        dst_is_b = self.is_bool_ir_type(dst_ir_ty)
                        if src_is_b and not dst_is_b:
                            n_c = dst_t[1] if dst_t[0] == "vec" else 1
                            one = self.c_u32(1) if n_c == 1 else self.c_composite(dst_spv_ty, (self.c_u32(1),) * n_c)
                            zero = self.c_u32(0) if n_c == 1 else self.c_null(dst_spv_ty)
                            set_val(cid, emit_op(OP_SELECT, dst_spv_ty, [src_val, one, zero]))
                        elif dst_is_b and not src_is_b:
                            n_c = dst_t[1] if dst_t[0] == "vec" else 1
                            zero = self.c_u32(0) if n_c == 1 else self.c_null(self.map_ir_type_to_spv(src_ir_ty))
                            set_val(cid, emit_op(OP_INOT_EQUAL, dst_spv_ty, [src_val, zero]))
                        elif cast_opc == 0 and dst_t[0] == "int" and dst_t[1] < 32:
                            mask = (1 << dst_t[1]) - 1
                            set_val(cid, emit_u32_and(src_val, mask))
                        else:
                            set_val(cid, src_val)
                    elif cast_opc == 11:  # bitcast
                        src_spv_ty = self.map_ir_type_to_spv(src_ir_ty)
                        if src_spv_ty == dst_spv_ty:
                            set_val(cid, src_val)
                        elif src_t[0] == "vec" and dst_t[0] != "vec":
                            if src_t[1] == 2 and self.mod.types[src_t[2]][0] in ("f16", "f32"):
                                set_val(
                                    cid,
                                    emit_op(OP_EXT_INST, self.t_u32(), [self.glsl450_id, GLSL_PACK_HALF_2X16, src_val]),
                                )
                            else:
                                c0 = emit_op(OP_COMPOSITE_EXTRACT, self.t_u32(), [src_val, 0])
                                c1 = emit_op(OP_COMPOSITE_EXTRACT, self.t_u32(), [src_val, 1])
                                c0_m = emit_u32_and(c0, 0xFFFF)
                                c1_s = emit_op(OP_SHIFT_LEFT_LOGICAL, self.t_u32(), [c1, self.c_u32(16)])
                                set_val(cid, emit_op(OP_BITWISE_OR, self.t_u32(), [c0_m, c1_s]))
                        else:
                            set_val(cid, emit_op(OP_BITCAST, dst_spv_ty, [src_val]))
                    else:
                        set_val(cid, src_val)

                elif op == "extractelt":
                    _, cid, elem_ir_ty, vec_vid, idx_vid = ins
                    vec_val = get_val(vec_vid)
                    idx_val = get_val(idx_vid)
                    elem_spv_ty = self.map_ir_type_to_spv(elem_ir_ty)
                    vec_len = self.mod.types[vals[vec_vid][0]][1]
                    if idx_val in self.known_u32:
                        c_idx = self.known_u32[idx_val] % vec_len
                        set_val(cid, emit_op(OP_COMPOSITE_EXTRACT, elem_spv_ty, [vec_val, c_idx]))
                    else:
                        set_val(cid, emit_op(OP_VECTOR_EXTRACT_DYNAMIC, elem_spv_ty, [vec_val, idx_val]))

                elif op == "insertelt":
                    _, cid, vec_ir_ty, vec_vid, elt_vid, idx_vid = ins
                    vec_val = get_val(vec_vid)
                    elt_val = get_val(elt_vid)
                    idx_val = get_val(idx_vid)
                    vec_spv_ty = self.map_ir_type_to_spv(vec_ir_ty)
                    vec_len = self.mod.types[vec_ir_ty][1]
                    if idx_val in self.known_u32:
                        c_idx = self.known_u32[idx_val] % vec_len
                        set_val(cid, emit_op(OP_COMPOSITE_INSERT, vec_spv_ty, [elt_val, vec_val, c_idx]))
                    else:
                        set_val(cid, emit_op(OP_VECTOR_INSERT_DYNAMIC, vec_spv_ty, [vec_val, elt_val, idx_val]))

                elif op == "shufflevec":
                    _, cid, res_ir_ty, v1_vid, v2_vid, mask_vid = ins
                    v1_val = get_val(v1_vid)
                    v2_val = get_val(v2_vid)
                    res_spv_ty = self.map_ir_type_to_spv(res_ir_ty)
                    mask_t, mkind, mdata = vals[mask_vid]
                    mask_len = self.mod.types[mask_t][1]
                    in_vec_len = self.mod.types[vals[v1_vid][0]][1]
                    max_idx = 2 * in_vec_len - 1
                    mask_lits: list[int] = []
                    if mkind == "null":
                        mask_lits = [0] * mask_len
                    elif mkind == "undef":
                        mask_lits = [0xFFFFFFFF] * mask_len
                    elif mkind == "data":
                        mask_lits = [int(x) if 0 <= int(x) <= max_idx else 0xFFFFFFFF for x in mdata]
                    elif mkind == "agg":
                        for sub_vid in mdata:
                            _st, skind, sdata = vals[sub_vid]
                            if skind == "undef":
                                mask_lits.append(0xFFFFFFFF)
                            elif skind == "null":
                                mask_lits.append(0)
                            else:
                                iv = int(sdata or 0)
                                mask_lits.append(iv if 0 <= iv <= max_idx else 0xFFFFFFFF)
                    else:
                        mask_lits = [0] * mask_len
                    set_val(cid, emit_op(OP_VECTOR_SHUFFLE, res_spv_ty, [v1_val, v2_val, *mask_lits]))

                elif op == "insertval":
                    _, cid, _agg_ty, agg_vid, elt_vid, indices = ins
                    agg_list = list(get_val(agg_vid))
                    elt_val = get_val(elt_vid)
                    agg_list[indices[0]] = elt_val
                    set_val(cid, agg_list)

                elif op == "cmp2":
                    _, cid, res_ir_ty, pred, lhs_vid, rhs_vid = ins
                    lhs = get_val(lhs_vid)
                    rhs = get_val(rhs_vid)
                    res_spv_ty = self.map_ir_type_to_spv(res_ir_ty)
                    lhs_is_bool = self.is_bool_ir_type(vals[lhs_vid][0])
                    cmp_map = {
                        1: OP_FORD_EQUAL,
                        2: OP_FORD_GREATER_THAN,
                        3: OP_FORD_GREATER_THAN_EQUAL,
                        4: OP_FORD_LESS_THAN,
                        5: OP_FORD_LESS_THAN_EQUAL,
                        6: OP_FORD_NOT_EQUAL,
                        9: OP_FUNORD_EQUAL,
                        10: OP_FUNORD_GREATER_THAN,
                        11: OP_FUNORD_GREATER_THAN_EQUAL,
                        12: OP_FUNORD_LESS_THAN,
                        13: OP_FUNORD_LESS_THAN_EQUAL,
                        14: OP_FUNORD_NOT_EQUAL,
                        32: OP_LOGICAL_EQUAL if lhs_is_bool else OP_IEQUAL,
                        33: OP_LOGICAL_NOT_EQUAL if lhs_is_bool else OP_INOT_EQUAL,
                        34: OP_UGREATER_THAN,
                        35: OP_UGREATER_THAN_EQUAL,
                        36: OP_ULESS_THAN,
                        37: OP_ULESS_THAN_EQUAL,
                        38: OP_SGREATER_THAN,
                        39: OP_SGREATER_THAN_EQUAL,
                        40: OP_SLESS_THAN,
                        41: OP_SLESS_THAN_EQUAL,
                    }
                    spv_op = cmp_map.get(pred, OP_IEQUAL)
                    set_val(cid, emit_op(spv_op, res_spv_ty, [lhs, rhs]))

                elif op == "select":
                    _, cid, val_ir_ty, cond_vid, t_vid, f_vid = ins
                    cond_val = get_val(cond_vid)
                    t_val = get_val(t_vid)
                    f_val = get_val(f_vid)
                    res_spv_ty = self.map_ir_type_to_spv(val_ir_ty)
                    vt = self.mod.types[val_ir_ty]
                    ct = self.mod.types[vals[cond_vid][0]]
                    if vt[0] == "vec" and ct[0] != "vec":
                        n_el = vt[1]
                        vb_ty = self.t_vec(self.t_bool(), n_el)
                        cond_val = emit_op(OP_COMPOSITE_CONSTRUCT, vb_ty, [cond_val] * n_el)
                    set_val(cid, emit_op(OP_SELECT, res_spv_ty, [cond_val, t_val, f_val]))

                elif op == "call":
                    _, cid, ret_ir_ty, fn_vid, call_args = ins
                    callee_name = self.mod.vst.get(fn_vid, "")
                    if callee_name.startswith("llvm.lifetime."):
                        continue
                    if callee_name == "air.discard_fragment":
                        self.entry_flags |= (1 << 2)  # SPIRV_ENTRY_FLAG_USES_KILL
                        code.extend(encode_inst(OP_KILL, []))
                        terminated_by_kill_or_ret = True
                        break
                    if callee_name == "air.wg.barrier":
                        self.entry_flags |= (1 << 1)
                        code.extend(
                            encode_inst(OP_CONTROL_BARRIER, [self.c_u32(2), self.c_u32(2), self.c_u32(0x108)])
                        )
                        continue

                    res_spv_ty = self.map_ir_type_to_spv(ret_ir_ty)
                    res_id = self._emit_intrinsic_call(
                        callee_name, res_spv_ty, ret_ir_ty, call_args, get_val, emit_op, emit_u32_shr, code
                    )
                    if cid is not None and res_id is not None:
                        set_val(cid, res_id)

                elif op == "ret":
                    _, ret_ir_ty, ret_vid = ins
                    if ret_vid is not None and self.output_vars:
                        rval = get_val(ret_vid)
                        if isinstance(rval, list):
                            for (out_var, _osty, _oirty), mel in zip(self.output_vars, rval):
                                code.extend(encode_inst(OP_STORE, [out_var, mel]))
                        else:
                            out_var, _osty, _oirty = self.output_vars[0]
                            code.extend(encode_inst(OP_STORE, [out_var, rval]))
                    if num_bbs == 1:
                        code.extend(encode_inst(OP_RETURN, []))
                    else:
                        code.extend(encode_inst(OP_STORE, [var_next_bb, self.c_u32(0xFFFFFFFF)]))
                    terminated_by_kill_or_ret = True
                    break

                elif op == "br":
                    _, target_bb = ins
                    copies = emit_phi_copies_for_edge(target_bb)
                    commit_phi_copies(copies)
                    if num_bbs > 1:
                        code.extend(encode_inst(OP_STORE, [var_next_bb, self.c_u32(target_bb)]))
                    break

                elif op == "br_cond":
                    _, cond_vid, t_bb, f_bb = ins
                    cond_val = get_val(cond_vid)
                    t_copies = emit_phi_copies_for_edge(t_bb)
                    f_copies = emit_phi_copies_for_edge(f_bb)
                    all_phi_cids = sorted(set(t_copies.keys()) | set(f_copies.keys()))
                    for pcid in all_phi_cids:
                        p_ir_ty = vals[pcid][0]
                        tv = t_copies.get(pcid, get_val(pcid))
                        fv = f_copies.get(pcid, get_val(pcid))
                        vinfo = reg2mem_vars[pcid]
                        if isinstance(vinfo, list):
                            for (fvar, sty), t_el, f_el in zip(vinfo, tv, fv):
                                sel_id = emit_op(OP_SELECT, sty, [cond_val, t_el, f_el])
                                code.extend(encode_inst(OP_STORE, [fvar, sel_id]))
                        else:
                            fvar, sty = vinfo
                            pt = self.mod.types[p_ir_ty]
                            c_use = cond_val
                            if pt[0] == "vec":
                                vb_ty = self.t_vec(self.t_bool(), pt[1])
                                c_use = emit_op(OP_COMPOSITE_CONSTRUCT, vb_ty, [cond_val] * pt[1])
                            sel_id = emit_op(OP_SELECT, sty, [c_use, tv, fv])
                            code.extend(encode_inst(OP_STORE, [fvar, sel_id]))
                    if num_bbs > 1:
                        next_id = emit_op(OP_SELECT, self.t_u32(), [cond_val, self.c_u32(t_bb), self.c_u32(f_bb)])
                        code.extend(encode_inst(OP_STORE, [var_next_bb, next_id]))
                    break

                elif op == "switch":
                    _, _cond_ty, cond_vid, def_bb, cases = ins
                    cond_val = get_val(cond_vid)
                    # Build select chain for target BB and any phi copies
                    cur_tgt = self.c_u32(def_bb)
                    def_copies = emit_phi_copies_for_edge(def_bb)
                    case_copies_list = [(c_val, c_bb, emit_phi_copies_for_edge(c_bb)) for c_val, c_bb in cases]
                    all_phi_cids = set(def_copies.keys())
                    for _, _, cc in case_copies_list:
                        all_phi_cids |= set(cc.keys())
                    cur_phi_vals: dict[int, Any] = {
                        pcid: def_copies.get(pcid, get_val(pcid)) for pcid in all_phi_cids
                    }
                    for c_lit, c_bb, c_copies in case_copies_list:
                        cmp_id = emit_op(OP_IEQUAL, self.t_bool(), [cond_val, self.c_u32(c_lit)])
                        cur_tgt = emit_op(OP_SELECT, self.t_u32(), [cmp_id, self.c_u32(c_bb), cur_tgt])
                        for pcid in all_phi_cids:
                            p_ir_ty = vals[pcid][0]
                            pt = self.mod.types[p_ir_ty]
                            cv = c_copies.get(pcid, get_val(pcid))
                            pv = cur_phi_vals[pcid]
                            if isinstance(cv, list):
                                new_l = []
                                for m_ty, c_el, p_el in zip(pt[3], cv, pv):
                                    sty = self.map_ir_type_to_spv(m_ty)
                                    new_l.append(emit_op(OP_SELECT, sty, [cmp_id, c_el, p_el]))
                                cur_phi_vals[pcid] = new_l
                            else:
                                sty = self.map_ir_type_to_spv(p_ir_ty)
                                c_use = cmp_id
                                if pt[0] == "vec":
                                    vb_ty = self.t_vec(self.t_bool(), pt[1])
                                    c_use = emit_op(OP_COMPOSITE_CONSTRUCT, vb_ty, [cmp_id] * pt[1])
                                cur_phi_vals[pcid] = emit_op(OP_SELECT, sty, [c_use, cv, pv])
                    commit_phi_copies(cur_phi_vals)
                    if num_bbs > 1:
                        code.extend(encode_inst(OP_STORE, [var_next_bb, cur_tgt]))
                    break

                elif op == "unreachable":
                    if num_bbs == 1:
                        code.extend(encode_inst(OP_RETURN, []))
                    else:
                        code.extend(encode_inst(OP_STORE, [var_next_bb, self.c_u32(0xFFFFFFFF)]))
                    terminated_by_kill_or_ret = True
                    break

            return code, (code[-1] == encode_word(OP_KILL, 1) if code else False)

        # 3. Order basic blocks (RPO / topological sort) and check for back-edges
        succs: list[list[int]] = [[] for _ in range(num_bbs)]
        for b_idx, bb in enumerate(bbs):
            if not bb:
                continue
            term = bb[-1]
            if term[0] == "br":
                succs[b_idx].append(term[1])
            elif term[0] == "br_cond":
                succs[b_idx].extend([term[2], term[3]])
            elif term[0] == "switch":
                succs[b_idx].append(term[3])
                succs[b_idx].extend(c_bb for _, c_bb in term[4])

        visited: set[int] = set()
        postorder: list[int] = []

        def dfs(u: int) -> None:
            stack_dfs = [(u, 0)]
            visited.add(u)
            while stack_dfs:
                curr, s_idx = stack_dfs[-1]
                if s_idx < len(succs[curr]):
                    stack_dfs[-1] = (curr, s_idx + 1)
                    nxt = succs[curr][s_idx]
                    if 0 <= nxt < num_bbs and nxt not in visited:
                        visited.add(nxt)
                        stack_dfs.append((nxt, 0))
                else:
                    postorder.append(curr)
                    stack_dfs.pop()

        if num_bbs > 0:
            dfs(0)
        rpo = list(reversed(postorder))
        rpo_pos = {bb_id: pos for pos, bb_id in enumerate(rpo)}
        has_backedge = any(
            rpo_pos.get(v, 0) <= rpo_pos[u] for u in rpo for v in succs[u] if v in rpo_pos
        )

        # 4. Assemble Function Body
        fn_words: list[int] = []
        fn_words.extend(encode_inst(OP_FUNCTION, [self.t_void(), self.main_fn_id, 0, self.t_func_void()]))
        entry_label_id = self.alloc_id()
        fn_words.extend(encode_inst(OP_LABEL, [entry_label_id]))

        if num_bbs == 1:
            bb0_code, ended_with_kill = emit_bb_instructions(0, bbs[0])
            fn_words.extend(entry_local_vars)
            fn_words.extend(entry_init_code)
            fn_words.extend(bb0_code)
            if not ended_with_kill and (not bb0_code or bb0_code[-1] != encode_word(OP_RETURN, 1)):
                fn_words.extend(encode_inst(OP_RETURN, []))
        elif not has_backedge:
            # Topological SESE chain (acyclic DAG)
            bb0_code, ended_with_kill = emit_bb_instructions(0, bbs[0])
            fn_words.extend(entry_local_vars)
            fn_words.extend(entry_init_code)
            fn_words.extend(bb0_code)

            if ended_with_kill:
                pass
            else:
                next_hdr_id = self.alloc_id()
                fn_words.extend(encode_inst(OP_BRANCH, [next_hdr_id]))
                for b_id in rpo[1:]:
                    hdr_id = next_hdr_id
                    body_id = self.alloc_id()
                    after_id = self.alloc_id()
                    next_hdr_id = after_id

                    fn_words.extend(encode_inst(OP_LABEL, [hdr_id]))
                    cur_bb_id = self.alloc_id()
                    fn_words.extend(encode_inst(OP_LOAD, [self.t_u32(), cur_bb_id, var_next_bb]))
                    cond_id = self.alloc_id()
                    fn_words.extend(encode_inst(OP_IEQUAL, [self.t_bool(), cond_id, cur_bb_id, self.c_u32(b_id)]))
                    fn_words.extend(encode_inst(OP_SELECTION_MERGE, [after_id, 0]))
                    fn_words.extend(encode_inst(OP_BRANCH_CONDITIONAL, [cond_id, body_id, after_id]))

                    fn_words.extend(encode_inst(OP_LABEL, [body_id]))
                    b_code, b_kill = emit_bb_instructions(b_id, bbs[b_id])
                    fn_words.extend(b_code)
                    if not b_kill:
                        fn_words.extend(encode_inst(OP_BRANCH, [after_id]))

                fn_words.extend(encode_inst(OP_LABEL, [next_hdr_id]))
                fn_words.extend(encode_inst(OP_RETURN, []))
        else:
            # Single-header OpLoopMerge + RPO SESE chain
            fn_words.extend(entry_local_vars)
            fn_words.extend(entry_init_code)
            fn_words.extend(encode_inst(OP_STORE, [var_next_bb, self.c_u32(0)]))

            loop_hdr_id = self.alloc_id()
            loop_exit_id = self.alloc_id()
            loop_cont_id = self.alloc_id()
            first_sese_id = self.alloc_id()

            fn_words.extend(encode_inst(OP_BRANCH, [loop_hdr_id]))
            fn_words.extend(encode_inst(OP_LABEL, [loop_hdr_id]))
            cur_top_id = self.alloc_id()
            fn_words.extend(encode_inst(OP_LOAD, [self.t_u32(), cur_top_id, var_next_bb]))
            is_done_id = self.alloc_id()
            fn_words.extend(encode_inst(OP_IEQUAL, [self.t_bool(), is_done_id, cur_top_id, self.c_u32(0xFFFFFFFF)]))
            fn_words.extend(encode_inst(OP_LOOP_MERGE, [loop_exit_id, loop_cont_id, 0]))
            fn_words.extend(encode_inst(OP_BRANCH_CONDITIONAL, [is_done_id, loop_exit_id, first_sese_id]))

            next_hdr_id = first_sese_id
            for b_id in rpo:
                hdr_id = next_hdr_id
                body_id = self.alloc_id()
                after_id = self.alloc_id()
                next_hdr_id = after_id

                fn_words.extend(encode_inst(OP_LABEL, [hdr_id]))
                cur_bb_id = self.alloc_id()
                fn_words.extend(encode_inst(OP_LOAD, [self.t_u32(), cur_bb_id, var_next_bb]))
                cond_id = self.alloc_id()
                fn_words.extend(encode_inst(OP_IEQUAL, [self.t_bool(), cond_id, cur_bb_id, self.c_u32(b_id)]))
                fn_words.extend(encode_inst(OP_SELECTION_MERGE, [after_id, 0]))
                fn_words.extend(encode_inst(OP_BRANCH_CONDITIONAL, [cond_id, body_id, after_id]))

                fn_words.extend(encode_inst(OP_LABEL, [body_id]))
                b_code, b_kill = emit_bb_instructions(b_id, bbs[b_id])
                fn_words.extend(b_code)
                if not b_kill:
                    fn_words.extend(encode_inst(OP_BRANCH, [after_id]))

            fn_words.extend(encode_inst(OP_LABEL, [next_hdr_id]))
            fn_words.extend(encode_inst(OP_BRANCH, [loop_cont_id]))
            fn_words.extend(encode_inst(OP_LABEL, [loop_cont_id]))
            fn_words.extend(encode_inst(OP_BRANCH, [loop_hdr_id]))
            fn_words.extend(encode_inst(OP_LABEL, [loop_exit_id]))
            fn_words.extend(encode_inst(OP_RETURN, []))

        fn_words.extend(encode_inst(OP_FUNCTION_END, []))

        # 5. Assemble Full Module in Strict SPIR-V Logical Layout Order
        mod_words: list[int] = [
            SPV_MAGIC,
            SPV_VERSION_1_3,
            SPV_GENERATOR,
            self.next_id,
            0,
        ]
        for cap in sorted(self.capabilities):
            mod_words.extend(encode_inst(OP_CAPABILITY, [cap]))
        mod_words.extend(encode_inst(OP_EXT_INST_IMPORT, [self.glsl450_id, *encode_str("GLSL.std.450")]))
        mod_words.extend(encode_inst(OP_MEMORY_MODEL, [0, 1]))  # Logical, GLSL450
        mod_words.extend(
            encode_inst(
                OP_ENTRY_POINT,
                [self.exec_model, self.main_fn_id, *encode_str("main"), *self.entry_interface_vars],
            )
        )
        if self.exec_model == 4:  # Fragment
            mod_words.extend(encode_inst(OP_EXECUTION_MODE, [self.main_fn_id, 7]))  # OriginUpperLeft
            if self.entry_flags & (1 << 4):
                mod_words.extend(encode_inst(OP_EXECUTION_MODE, [self.main_fn_id, 12]))  # DepthReplacing
        elif self.exec_model == 5:  # GLCompute
            lx, ly, lz = self.local_size
            mod_words.extend(encode_inst(OP_EXECUTION_MODE, [self.main_fn_id, 17, lx, ly, lz]))  # LocalSize

        mod_words.extend(self.decorations)
        mod_words.extend(self.type_const_globals)
        mod_words.extend(fn_words)

        spirv_bytes = struct.pack(f"<{len(mod_words)}I", *mod_words)
        reflection = {
            "shader_id": self.mtlb_info["shader_id"],
            "mtlb_sha256": self.mtlb_info["mtlb_sha256"],
            "entry_name": self.mtlb_info["entry_name"],
            "stage": self.stage,
            "bindings": self.bindings_reflection,
            "vertex_attrs": self.vertex_attrs_reflection,
            "buffer_mask": self.buffer_mask,
            "texture_mask": self.texture_mask,
            "sampler_mask": self.sampler_mask,
            "fragment_output_mask": self.fragment_output_mask,
            "flags": self.entry_flags,
            "local_size": self.local_size,
        }
        return spirv_bytes, reflection

    def _emit_intrinsic_call(
        self,
        name: str,
        res_spv_ty: int,
        ret_ir_ty: int,
        call_args: list[int],
        get_val: Any,
        emit_op: Any,
        emit_u32_shr: Any,
        code: list[int],
    ) -> int | None:
        # 1. Float <-> Half conversions (pass-through since both use OpTypeFloat 32)
        if name.startswith("air.convert.f.") and ".f." in name[13:]:
            return get_val(call_args[0])

        # 2. Int <-> Float conversions
        if name.startswith("air.convert."):
            arg0 = get_val(call_args[0])
            if ".f.u." in name or ".f16.u." in name or ".f32.u." in name:
                if name.endswith("i1"):
                    rt = self.mod.types[ret_ir_ty]
                    n_c = rt[1] if rt[0] == "vec" else 1
                    one_f = self.c_f32_bits(0x3F800000)
                    zero_f = self.c_f32_bits(0)
                    one = one_f if n_c == 1 else self.c_composite(res_spv_ty, (one_f,) * n_c)
                    zero = zero_f if n_c == 1 else self.c_null(res_spv_ty)
                    return emit_op(OP_SELECT, res_spv_ty, [arg0, one, zero])
                return emit_op(OP_CONVERT_U_TO_F, res_spv_ty, [arg0])
            if ".f.s." in name or ".f16.s." in name or ".f32.s." in name:
                return emit_op(OP_CONVERT_S_TO_F, res_spv_ty, [arg0])
            if name.startswith("air.convert.u."):
                return emit_op(OP_CONVERT_F_TO_U, res_spv_ty, [arg0])
            if name.startswith("air.convert.s."):
                rt = self.mod.types[ret_ir_ty]
                s_ty = self.t_vec(self.t_s32(), rt[1]) if rt[0] == "vec" else self.t_s32()
                s_id = emit_op(OP_CONVERT_F_TO_S, s_ty, [arg0])
                return emit_op(OP_BITCAST, res_spv_ty, [s_id])

        # 3. Texture Sampling, Gathering, Fetching, Queries
        if "texture" in name:
            tex_arg_vid = call_args[0]
            if tex_arg_vid not in self.tex_arg_info:
                return self.c_null(res_spv_ty)
            t_var_id, img_ty, sampled_ty, _spv_dim, ms = self.tex_arg_info[tex_arg_vid]
            tex_id = emit_op(OP_LOAD, img_ty, [t_var_id])

            if name == "air.get_width_texture_2d" or name == "air.get_height_texture_2d":
                self.capabilities.add(CAP_IMAGE_QUERY)
                lod_id = get_val(call_args[1]) if len(call_args) > 1 else self.c_u32(0)
                v2u_ty = self.t_vec(self.t_u32(), 2)
                sz_id = emit_op(OP_IMAGE_QUERY_SIZE_LOD, v2u_ty, [tex_id, lod_id])
                comp = 0 if "width" in name else 1
                return emit_op(OP_COMPOSITE_EXTRACT, self.t_u32(), [sz_id, comp])

            if name.startswith("air.read_texture_"):
                coord_id = get_val(call_args[1])
                lod_or_samp_id = get_val(call_args[2]) if len(call_args) > 2 else self.c_u32(0)
                fetch_vec_ty = self.t_vec(sampled_ty, 4)
                if ms:
                    # Sample operand = 0x40
                    res_v = emit_op(OP_IMAGE_FETCH, fetch_vec_ty, [tex_id, coord_id, 0x40, lod_or_samp_id])
                else:
                    # Lod operand = 0x02
                    res_v = emit_op(OP_IMAGE_FETCH, fetch_vec_ty, [tex_id, coord_id, 0x02, lod_or_samp_id])
                if fetch_vec_ty != res_spv_ty:
                    res_v = emit_op(OP_BITCAST, res_spv_ty, [res_v])
                return res_v

            # Sampled texture operations (sample / gather)
            samp_arg_vid = call_args[1]
            s_var_id = self.samp_arg_info.get(samp_arg_vid)
            if s_var_id is None:
                return self.c_null(res_spv_ty)
            samp_id = emit_op(OP_LOAD, self.t_sampler(), [s_var_id])
            si_ty = self.t_sampled_image(img_ty)
            si_id = emit_op(OP_SAMPLED_IMAGE, si_ty, [tex_id, samp_id])
            coord_id = get_val(call_args[2])
            sample_vec_ty = self.t_vec(sampled_ty, 4)

            if name.startswith("air.gather_texture_"):
                comp_id = get_val(call_args[-1])
                res_v = emit_op(OP_IMAGE_GATHER, sample_vec_ty, [si_id, coord_id, comp_id])
            elif name.startswith("air.sample_texture_2d_grad"):
                ddx_id = get_val(call_args[3])
                ddy_id = get_val(call_args[4])
                # Grad operand = 0x04
                res_v = emit_op(OP_IMAGE_SAMPLE_EXPLICIT_LOD, sample_vec_ty, [si_id, coord_id, 0x04, ddx_id, ddy_id])
            elif name.startswith("air.sample_texture_2d_array"):
                arr_idx_id = get_val(call_args[3])
                layer_f = emit_op(OP_CONVERT_S_TO_F, self.t_f32(), [arr_idx_id])
                cx = emit_op(OP_COMPOSITE_EXTRACT, self.t_f32(), [coord_id, 0])
                cy = emit_op(OP_COMPOSITE_EXTRACT, self.t_f32(), [coord_id, 1])
                c3_id = emit_op(OP_COMPOSITE_CONSTRUCT, self.t_vec(self.t_f32(), 3), [cx, cy, layer_f])
                if self.exec_model == 4:
                    res_v = emit_op(OP_IMAGE_SAMPLE_IMPLICIT_LOD, sample_vec_ty, [si_id, c3_id])
                else:
                    res_v = emit_op(
                        OP_IMAGE_SAMPLE_EXPLICIT_LOD, sample_vec_ty, [si_id, c3_id, 0x02, self.c_f32_bits(0)]
                    )
            else:
                # 2D, Cube, 3D sample
                if self.exec_model == 4:
                    res_v = emit_op(OP_IMAGE_SAMPLE_IMPLICIT_LOD, sample_vec_ty, [si_id, coord_id])
                else:
                    lod_f = get_val(call_args[-2]) if len(call_args) >= 2 else self.c_f32_bits(0)
                    res_v = emit_op(OP_IMAGE_SAMPLE_EXPLICIT_LOD, sample_vec_ty, [si_id, coord_id, 0x02, lod_f])

            if sample_vec_ty != res_spv_ty:
                res_v = emit_op(OP_BITCAST, res_spv_ty, [res_v])
            return res_v

        # 4. Sincos with pointer out-parameter
        if "sincos" in name:
            x_id = get_val(call_args[0])
            ptr_vid = call_args[1]
            byte_off_id = get_val(ptr_vid)
            sin_id = emit_op(OP_EXT_INST, res_spv_ty, [self.glsl450_id, GLSL_SIN, x_id])
            cos_id = emit_op(OP_EXT_INST, res_spv_ty, [self.glsl450_id, GLSL_COS, x_id])
            binfo = self.ptr_base_info.get(ptr_vid)
            if binfo and binfo[0] == "alloca":
                bvar_id = binfo[1]
                rt = self.mod.types[ret_ir_ty]
                n_comp = rt[1] if rt[0] == "vec" else 1
                w_idx = emit_u32_shr(byte_off_id, 2)
                p_f32 = self.t_ptr(SC_FUNCTION, self.t_f32())
                for k in range(n_comp):
                    c_el = cos_id if n_comp == 1 else emit_op(OP_COMPOSITE_EXTRACT, self.t_f32(), [cos_id, k])
                    w_k = emit_op(OP_IADD, self.t_u32(), [w_idx, self.c_u32(k)]) if k > 0 else w_idx
                    ac_id = emit_op(OP_ACCESS_CHAIN, p_f32, [bvar_id, w_k])
                    code.extend(encode_inst(OP_STORE, [ac_id, c_el]))
            return sin_id

        # 5. Standard math & vector intrinsics
        args_v = [get_val(a) for a in call_args]
        if name.startswith("air.dot."):
            return emit_op(OP_DOT, res_spv_ty, [args_v[0], args_v[1]])
        if name.startswith("air.dfdx."):
            return emit_op(OP_DPDX, res_spv_ty, [args_v[0]])
        if name.startswith("air.dfdy."):
            return emit_op(OP_DPDY, res_spv_ty, [args_v[0]])
        if name.startswith("air.any."):
            return emit_op(OP_ANY, res_spv_ty, [args_v[0]])
        if name.startswith("air.reverse_bits."):
            return emit_op(OP_BIT_REVERSE, res_spv_ty, [args_v[0]])
        if name.startswith("air.fast_fmod."):
            return emit_op(OP_FREM, res_spv_ty, [args_v[0], args_v[1]])
        if name.startswith("air.pack.unorm2x16"):
            return emit_op(OP_EXT_INST, res_spv_ty, [self.glsl450_id, GLSL_PACK_UNORM_2X16, args_v[0]])

        if name.startswith("air.fma."):
            return emit_op(OP_EXT_INST, res_spv_ty, [self.glsl450_id, GLSL_FMA, args_v[0], args_v[1], args_v[2]])
        if name.startswith("air.mix."):
            return emit_op(OP_EXT_INST, res_spv_ty, [self.glsl450_id, GLSL_FMIX, args_v[0], args_v[1], args_v[2]])
        if name.startswith(("air.clamp.", "air.fast_clamp.")):
            return emit_op(OP_EXT_INST, res_spv_ty, [self.glsl450_id, GLSL_FCLAMP, args_v[0], args_v[1], args_v[2]])
        if name.startswith(("air.saturate.", "air.fast_saturate.")):
            rt = self.mod.types[ret_ir_ty]
            n_c = rt[1] if rt[0] == "vec" else 1
            zero_f = self.c_f32_bits(0)
            one_f = self.c_f32_bits(0x3F800000)
            c0 = zero_f if n_c == 1 else self.c_null(res_spv_ty)
            c1 = one_f if n_c == 1 else self.c_composite(res_spv_ty, (one_f,) * n_c)
            return emit_op(OP_EXT_INST, res_spv_ty, [self.glsl450_id, GLSL_FCLAMP, args_v[0], c0, c1])
        if name.startswith(("air.fmin.", "air.fast_fmin.", "_Z11_target_minff")):
            return emit_op(OP_EXT_INST, res_spv_ty, [self.glsl450_id, GLSL_FMIN, args_v[0], args_v[1]])
        if name.startswith(("air.fmax.", "air.fast_fmax.")):
            return emit_op(OP_EXT_INST, res_spv_ty, [self.glsl450_id, GLSL_FMAX, args_v[0], args_v[1]])
        if name.startswith("air.min.u."):
            return emit_op(OP_EXT_INST, res_spv_ty, [self.glsl450_id, GLSL_UMIN, args_v[0], args_v[1]])
        if name.startswith("air.max.s."):
            rt = self.mod.types[ret_ir_ty]
            b_ty = self.t_vec(self.t_bool(), rt[1]) if rt[0] == "vec" else self.t_bool()
            cmp_id = emit_op(OP_SGREATER_THAN, b_ty, [args_v[0], args_v[1]])
            return emit_op(OP_SELECT, res_spv_ty, [cmp_id, args_v[0], args_v[1]])
        if name.startswith("air.abs.s."):
            rt = self.mod.types[ret_ir_ty]
            b_ty = self.t_vec(self.t_bool(), rt[1]) if rt[0] == "vec" else self.t_bool()
            z_id = self.c_null(res_spv_ty)
            neg_id = emit_op(OP_SNEGATE, res_spv_ty, [args_v[0]])
            cmp_id = emit_op(OP_SLESS_THAN, b_ty, [args_v[0], z_id])
            return emit_op(OP_SELECT, res_spv_ty, [cmp_id, neg_id, args_v[0]])
        if name.startswith(("air.fabs.", "air.fast_fabs.")):
            return emit_op(OP_EXT_INST, res_spv_ty, [self.glsl450_id, GLSL_FABS, args_v[0]])
        if name.startswith("air.sign."):
            return emit_op(OP_EXT_INST, res_spv_ty, [self.glsl450_id, GLSL_FSIGN, args_v[0]])
        if name.startswith(("air.floor.", "air.fast_floor.", "_Z13_target_floorf")):
            return emit_op(OP_EXT_INST, res_spv_ty, [self.glsl450_id, GLSL_FLOOR, args_v[0]])
        if name.startswith("air.fast_trunc."):
            return emit_op(OP_EXT_INST, res_spv_ty, [self.glsl450_id, GLSL_TRUNC, args_v[0]])
        if name.startswith(("air.fract.", "air.fast_fract.", "___metal_fract_", "_Z18_target_fast_fractf", "_ZN11_fract_impl")):
            return emit_op(OP_EXT_INST, res_spv_ty, [self.glsl450_id, GLSL_FRACT, args_v[0]])
        if name.startswith(("air.sqrt.", "air.fast_sqrt.")):
            return emit_op(OP_EXT_INST, res_spv_ty, [self.glsl450_id, GLSL_SQRT, args_v[0]])
        if name.startswith(("air.rsqrt.", "air.fast_rsqrt.")):
            return emit_op(OP_EXT_INST, res_spv_ty, [self.glsl450_id, GLSL_INVERSE_SQRT, args_v[0]])
        if name.startswith(("air.sin.", "air.fast_sin.")):
            return emit_op(OP_EXT_INST, res_spv_ty, [self.glsl450_id, GLSL_SIN, args_v[0]])
        if name.startswith(("air.cos.", "air.fast_cos.")):
            return emit_op(OP_EXT_INST, res_spv_ty, [self.glsl450_id, GLSL_COS, args_v[0]])
        if name.startswith(("air.atan2.", "air.fast_atan2.")):
            return emit_op(OP_EXT_INST, res_spv_ty, [self.glsl450_id, GLSL_ATAN2, args_v[0], args_v[1]])
        if name.startswith(("air.exp2.", "air.fast_exp2.")):
            return emit_op(OP_EXT_INST, res_spv_ty, [self.glsl450_id, GLSL_EXP2, args_v[0]])
        if name.startswith("air.fast_exp."):
            return emit_op(OP_EXT_INST, res_spv_ty, [self.glsl450_id, GLSL_EXP, args_v[0]])
        if name.startswith(("air.log2.", "air.fast_log2.")):
            return emit_op(OP_EXT_INST, res_spv_ty, [self.glsl450_id, GLSL_LOG2, args_v[0]])
        if name.startswith("air.fast_log."):
            return emit_op(OP_EXT_INST, res_spv_ty, [self.glsl450_id, GLSL_LOG, args_v[0]])
        if name.startswith(("air.pow.", "air.fast_pow.")):
            return emit_op(OP_EXT_INST, res_spv_ty, [self.glsl450_id, GLSL_POW, args_v[0], args_v[1]])

        return self.c_null(res_spv_ty)


def translate_air_shader(raw_shader_entry: bytes, default_freq: int = 3) -> tuple[bytes, dict[str, Any]]:
    """
    Decode a raw UE4 .ushaderbytecode entry (FMetalCodeHeader + MTLB + AIR 2.0 LLVM Bitcode)
    and translate it into a Vulkan 1.1 SPIR-V 1.3 binary module + reflection dictionary.
    """
    mtlb_idx = raw_shader_entry.find(b"MTLB")
    if mtlb_idx == -1:
        raise ValueError("No MTLB magic found in shader entry")
    fmetal_hdr = parse_fmetal_code_header(raw_shader_entry[:mtlb_idx])
    if fmetal_hdr["frequency"] not in (0, 3, 5):
        fmetal_hdr["frequency"] = default_freq
    mtlb_info = parse_mtlb_container(raw_shader_entry[mtlb_idx:])
    mod = IRModule(mtlb_info["raw_bc"])
    builder = SpirvBuilder(mod, fmetal_hdr, mtlb_info)
    return builder.build()
