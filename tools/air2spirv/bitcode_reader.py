#!/usr/bin/env python3
"""
AIR 2.0 LLVM Bitcode (0x0b17c0de / BC\\xc0\\xde) & MTLB Container Reader.
Decodes Apple Metal 2.0 / AIR 2.0 bitcode streams directly into structured Python IR.
"""

from __future__ import annotations

import hashlib
import struct
from typing import Any

CHAR6_TABLE = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._"


class BitReader:
    __slots__ = ("data", "nbytes", "byte_pos", "cur_word", "bits_in_word")

    def __init__(self, data: bytes):
        self.data = data
        self.nbytes = len(data)
        self.byte_pos = 0
        self.cur_word = 0
        self.bits_in_word = 0

    def bit_pos(self) -> int:
        return self.byte_pos * 8 - self.bits_in_word

    def at_end(self) -> bool:
        return self.byte_pos >= self.nbytes and self.bits_in_word == 0

    def read(self, nbits: int) -> int:
        if nbits == 0:
            return 0
        while self.bits_in_word < nbits:
            if self.byte_pos >= self.nbytes:
                raise EOFError("Unexpected EOF in LLVM bitstream")
            if self.byte_pos + 4 <= self.nbytes:
                w = int.from_bytes(self.data[self.byte_pos : self.byte_pos + 4], "little")
                self.cur_word |= w << self.bits_in_word
                self.byte_pos += 4
                self.bits_in_word += 32
            else:
                self.cur_word |= self.data[self.byte_pos] << self.bits_in_word
                self.byte_pos += 1
                self.bits_in_word += 8
        val = self.cur_word & ((1 << nbits) - 1)
        self.cur_word >>= nbits
        self.bits_in_word -= nbits
        return val

    def read_vbr(self, width: int) -> int:
        val = 0
        shift = 0
        mask = (1 << (width - 1)) - 1
        cont = 1 << (width - 1)
        while True:
            p = self.read(width)
            val |= (p & mask) << shift
            if not (p & cont):
                return val
            shift += width - 1

    def align32(self) -> None:
        rem = (self.byte_pos * 8 - self.bits_in_word) & 31
        if rem:
            self.read(32 - rem)

    def read_bytes(self, nbytes: int) -> bytes:
        self.align32()
        actual_byte_pos = self.byte_pos - (self.bits_in_word // 8)
        out = self.data[actual_byte_pos : actual_byte_pos + nbytes]
        pad = (4 - (nbytes & 3)) & 3
        self.byte_pos = actual_byte_pos + nbytes + pad
        self.cur_word = 0
        self.bits_in_word = 0
        return out


def decode_vbr_signed(v: int) -> int:
    return -(v >> 1) if (v & 1) else (v >> 1)


def rec_str(ops: list[Any]) -> str:
    if ops and isinstance(ops[0], bytes):
        return ops[0].decode("utf-8", errors="replace")
    return bytes(ops).decode("utf-8", errors="replace")


def parse_md_strings(num_strings: int, strings_offset: int, blob: bytes) -> list[str]:
    br = BitReader(blob[:strings_offset])
    lengths = [br.read_vbr(6) for _ in range(num_strings)]
    pos = strings_offset
    out = []
    for ln in lengths:
        out.append(blob[pos : pos + ln].decode("utf-8", errors="replace"))
        pos += ln
    return out


def parse_bitstream(bc: bytes) -> list[tuple[int, int, int, Any]]:
    if bc[:4] != b"BC\xc0\xde":
        raise ValueError(f"Invalid LLVM bitcode magic: {bc[:4]!r}")
    br = BitReader(bc[4:])
    blockinfo_abbrevs: dict[int | None, list[list[tuple[int, int]]]] = {}
    stack: list[tuple[int, int, list[list[tuple[int, int]]]]] = []
    cur_abbrevs: list[list[tuple[int, int]]] = []
    cur_aw = 2
    cur_bid = -1
    blockinfo_target: int | None = None

    def read_abbrev_def() -> list[tuple[int, int]]:
        num_ops = br.read_vbr(5)
        ops = []
        for _ in range(num_ops):
            if br.read(1):
                ops.append((0, br.read_vbr(8)))  # LIT
            else:
                enc = br.read(3)
                if enc in (1, 2):
                    ops.append((enc, br.read_vbr(5)))
                else:
                    ops.append((enc, 0))
        return ops

    def decode_op(kind: int, val: int) -> int:
        if kind == 0:
            return val
        elif kind == 1:
            return br.read(val)
        elif kind == 2:
            return br.read_vbr(val)
        elif kind == 4:
            return ord(CHAR6_TABLE[br.read(6)])
        raise ValueError(f"Unsupported abbrev operand encoding {kind}")

    events: list[tuple[int, int, int, Any]] = []
    while not br.at_end():
        aid = br.read(cur_aw)
        if aid == 0:
            br.align32()
            events.append((0, cur_bid, 0, None))
            if not stack:
                break
            cur_bid, cur_aw, cur_abbrevs = stack.pop()
        elif aid == 1:
            sub_id = br.read_vbr(8)
            sub_aw = br.read_vbr(4)
            br.align32()
            _sub_len = br.read(32)
            events.append((1, sub_id, 0, None))
            stack.append((cur_bid, cur_aw, cur_abbrevs))
            cur_bid = sub_id
            cur_aw = sub_aw
            cur_abbrevs = list(blockinfo_abbrevs.get(sub_id, []))
            if sub_id == 0:
                blockinfo_target = None
        elif aid == 2:
            ops = read_abbrev_def()
            if cur_bid == 0:
                blockinfo_abbrevs.setdefault(blockinfo_target, []).append(ops)
            else:
                cur_abbrevs.append(ops)
        elif aid == 3:
            code = br.read_vbr(6)
            n = br.read_vbr(6)
            ops_u = [br.read_vbr(6) for _ in range(n)]
            if cur_bid == 0 and code == 1:
                blockinfo_target = ops_u[0]
            else:
                events.append((2, cur_bid, code, ops_u))
        else:
            abbrev = cur_abbrevs[aid - 4]
            rec: list[Any] = []
            i = 0
            n = len(abbrev)
            while i < n:
                k, v = abbrev[i]
                if k == 3:
                    ek, ev = abbrev[i + 1]
                    cnt = br.read_vbr(6)
                    for _ in range(cnt):
                        rec.append(decode_op(ek, ev))
                    break
                elif k == 5:
                    ln = br.read_vbr(6)
                    rec.append(br.read_bytes(ln))
                    break
                else:
                    rec.append(decode_op(k, v))
                i += 1
            events.append((2, cur_bid, rec[0], rec[1:]))
    return events


def parse_fmetal_code_header(hdr: bytes) -> dict[str, Any]:
    """Decode UE4 FMetalCodeHeader trailing metadata (frequency, size, CRC, workgroup local size)."""
    info = {
        "header_size": len(hdr),
        "frequency": 0,
        "source_len": 0,
        "source_crc": 0,
        "local_size_x": 0,
        "local_size_y": 0,
        "local_size_z": 0,
    }
    if len(hdr) >= 32:
        freq, src_len, src_crc, lx, ly, lz, _flags, _pad = struct.unpack_from("<8I", hdr, len(hdr) - 32)
        info["frequency"] = freq
        info["source_len"] = src_len
        info["source_crc"] = src_crc
        info["local_size_x"] = lx
        info["local_size_y"] = ly
        info["local_size_z"] = lz
    return info


def parse_mtlb_container(mtlb: bytes) -> dict[str, Any]:
    """Decode Apple MTLB container header, function tags, public VATT/VATY metadata, and bitcode slice."""
    if len(mtlb) < 88 or mtlb[:4] != b"MTLB":
        raise ValueError("Invalid MTLB header")
    ver_u16 = struct.unpack_from("<4H", mtlb, 4)
    total_sz = struct.unpack_from("<Q", mtlb, 16)[0]
    fn_off, fn_sz, pub_off, pub_sz, priv_off, priv_sz, bc_off, bc_sz = struct.unpack_from("<8Q", mtlb, 24)

    tags: dict[str, bytes] = {}
    if fn_off + 8 <= len(mtlb):
        p = fn_off + 8
        end = min(len(mtlb), fn_off + fn_sz)
        while p + 4 <= end:
            tag = mtlb[p : p + 4]
            p += 4
            if tag == b"ENDT" or p + 2 > end:
                break
            tlen = struct.unpack_from("<H", mtlb, p)[0]
            p += 2
            tags[tag.decode("ascii", errors="replace")] = mtlb[p : p + tlen]
            p += tlen

    bc_slice = mtlb[bc_off : bc_off + bc_sz]
    wrapper_magic = struct.unpack_from("<I", bc_slice, 0)[0] if len(bc_slice) >= 20 else 0
    if wrapper_magic == 0x0B17C0DE:
        _, _, inner_off, inner_sz, _ = struct.unpack_from("<5I", bc_slice, 0)
        raw_bc = bc_slice[inner_off : inner_off + inner_sz]
    else:
        raw_bc = bc_slice

    entry_name = "main"
    if "NAME" in tags:
        entry_name = tags["NAME"].split(b"\x00", 1)[0].decode("utf-8", errors="replace") or "main"

    mtlb_sha256 = hashlib.sha256(mtlb).digest()
    shader_id = struct.unpack_from("<Q", mtlb_sha256, 0)[0]

    return {
        "mtlb_ver": ver_u16,
        "total_size": total_sz,
        "tags": tags,
        "entry_name": entry_name,
        "wrapper_magic": wrapper_magic,
        "raw_bc": raw_bc,
        "mtlb_sha256": mtlb_sha256,
        "shader_id": shader_id,
    }


class IRModule:
    """Decoded AIR 2.0 LLVM Bitcode Module."""

    def __init__(self, raw_bc: bytes):
        self.events = parse_bitstream(raw_bc)
        self.types: list[tuple[Any, ...]] = []
        self.values: list[tuple[int, str, Any]] = []
        self.func_records: list[dict[str, Any]] = []
        self.md_nodes: list[Any] = []
        self.named_md: dict[str, list[Any]] = {}
        self.vst: dict[int, str] = {}
        self.functions: list[dict[str, Any]] = []
        self.type_layouts: list[tuple[int, int, tuple[int, ...]]] = []
        self._decode()
        self._compute_layouts()

    def _get_val(self, ops: list[int], idx: int, cur_id: int) -> tuple[int, int]:
        rel = ops[idx]
        return (cur_id - rel) & 0xFFFFFFFF, idx + 1

    def _get_val_type_pair(
        self, ops: list[int], idx: int, cur_id: int, vals: list[tuple[int, str, Any]]
    ) -> tuple[int, int, int]:
        vid, idx = self._get_val(ops, idx, cur_id)
        if vid < len(vals):
            return vid, vals[vid][0], idx
        else:
            ty = ops[idx]
            return vid, ty, idx + 1

    def _decode_constants(self, ev_slice: list[tuple[int, int, int, Any]], vals: list[tuple[int, str, Any]]) -> None:
        cur_ty = 0
        for ev_type, _bid, code, ops in ev_slice:
            if ev_type != 2:
                continue
            if code == 1:  # SETTYPE
                cur_ty = ops[0]
            elif code == 2:  # NULL
                vals.append((cur_ty, "null", 0))
            elif code == 3:  # UNDEF
                vals.append((cur_ty, "undef", None))
            elif code == 4:  # INTEGER
                vals.append((cur_ty, "int", decode_vbr_signed(ops[0])))
            elif code == 6:  # FLOAT
                vals.append((cur_ty, "float", ops[0]))
            elif code == 7:  # AGGREGATE
                vals.append((cur_ty, "agg", list(ops)))
            elif code == 22:  # DATA
                vals.append((cur_ty, "data", list(ops)))
            else:
                raise ValueError(f"Unknown constant record code {code}")

    def _decode(self) -> None:
        pending_struct_name: str | None = None
        for ev_type, bid, code, ops in self.events:
            if ev_type == 2 and bid == 17:
                if code == 1:
                    pass
                elif code == 2:
                    self.types.append(("void",))
                elif code == 3:
                    self.types.append(("f32",))
                elif code == 5:
                    self.types.append(("label",))
                elif code == 6:
                    self.types.append(("opaque", pending_struct_name))
                    pending_struct_name = None
                elif code == 7:
                    self.types.append(("int", ops[0]))
                elif code == 8:
                    self.types.append(("ptr", ops[0], ops[1] if len(ops) > 1 else 0))
                elif code == 10:
                    self.types.append(("f16",))
                elif code == 11:
                    self.types.append(("array", ops[0], ops[1]))
                elif code == 12:
                    self.types.append(("vec", ops[0], ops[1]))
                elif code == 16:
                    self.types.append(("metadata",))
                elif code == 18:
                    self.types.append(("struct", None, bool(ops[0]), tuple(ops[1:])))
                elif code == 19:
                    pending_struct_name = rec_str(ops)
                elif code == 20:
                    self.types.append(("struct", pending_struct_name, bool(ops[0]), tuple(ops[1:])))
                    pending_struct_name = None
                elif code == 21:
                    self.types.append(("func", bool(ops[0]), ops[1], tuple(ops[2:])))
                else:
                    raise ValueError(f"Unknown type code {code}")
            elif ev_type == 2 and bid == 14:
                if code == 1:
                    self.vst[ops[0]] = rec_str(ops[1:])
                elif code == 3:
                    self.vst[ops[0]] = rec_str(ops[2:])

        ptr_map: dict[tuple[int, int], int] = {}
        for tid, t in enumerate(self.types):
            if t[0] == "ptr":
                ptr_map[(t[1], t[2])] = tid

        i = 0
        n = len(self.events)
        defined_funcs: list[dict[str, Any]] = []
        pending_md_name: str | None = None
        while i < n:
            ev_type, bid, code, ops = self.events[i]
            if ev_type == 2 and bid == 8:
                if code == 7:  # GLOBALVAR
                    val_ty = ops[0]
                    is_exp = (ops[1] & 2) != 0
                    addrspace = ops[1] >> 2 if is_exp else 0
                    ptr_ty = ptr_map.get((val_ty, addrspace), val_ty) if is_exp else val_ty
                    vid = len(self.values)
                    self.values.append((ptr_ty, "global", {"val_id": vid, "val_ty": val_ty, "addrspace": addrspace}))
                elif code == 8:  # FUNCTION
                    fty_id = ops[0]
                    fty = self.types[fty_id]
                    if fty[0] == "ptr":
                        ptr_ty = fty_id
                        fty_id = fty[1]
                    else:
                        ptr_ty = ptr_map.get((fty_id, 0), fty_id)
                    is_proto = bool(ops[2])
                    vid = len(self.values)
                    finfo = {"val_id": vid, "func_ty": fty_id, "ptr_ty": ptr_ty, "is_proto": is_proto}
                    self.values.append((ptr_ty, "func", finfo))
                    self.func_records.append(finfo)
                    if not is_proto:
                        defined_funcs.append(finfo)
                i += 1
            elif ev_type == 1 and bid == 11:
                j = i + 1
                depth = 1
                while j < n and depth > 0:
                    if self.events[j][0] == 1:
                        depth += 1
                    elif self.events[j][0] == 0:
                        depth -= 1
                    j += 1
                self._decode_constants(self.events[i + 1 : j - 1], self.values)
                i = j
            elif ev_type == 1 and bid == 15:
                j = i + 1
                depth = 1
                while j < n and depth > 0:
                    if self.events[j][0] == 1:
                        depth += 1
                    elif self.events[j][0] == 0:
                        depth -= 1
                    j += 1
                for mev_type, _mbid, mcode, mops in self.events[i + 1 : j - 1]:
                    if mev_type != 2:
                        continue
                    if mcode == 35:
                        self.md_nodes.extend(parse_md_strings(mops[0], mops[1], mops[2]))
                    elif mcode == 2:
                        self.md_nodes.append(("val", mops[0], mops[1]))
                    elif mcode == 3:
                        self.md_nodes.append(tuple(self.md_nodes[x - 1] if x > 0 else None for x in mops))
                    elif mcode == 4:
                        pending_md_name = rec_str(mops)
                    elif mcode == 10 and pending_md_name is not None:
                        self.named_md[pending_md_name] = [self.md_nodes[x] for x in mops]
                i = j
            elif ev_type == 1 and bid == 12:
                j = i + 1
                depth = 1
                while j < n and depth > 0:
                    if self.events[j][0] == 1:
                        depth += 1
                    elif self.events[j][0] == 0:
                        depth -= 1
                    j += 1
                finfo = defined_funcs[len(self.functions)]
                self._decode_function(finfo, self.events[i + 1 : j - 1], ptr_map)
                i = j
            else:
                i += 1

    def _decode_function(
        self, finfo: dict[str, Any], fevents: list[tuple[int, int, int, Any]], ptr_map: dict[tuple[int, int], int]
    ) -> None:
        vals = list(self.values)
        fty = self.types[finfo["func_ty"]]
        param_types = fty[3]
        args = []
        for idx, pty in enumerate(param_types):
            vid = len(vals)
            vals.append((pty, "arg", idx))
            args.append(vid)

        insts: list[tuple[Any, ...]] = []
        num_bbs = 1
        i = 0
        n = len(fevents)
        while i < n:
            ev_type, bid, code, ops = fevents[i]
            if ev_type == 1 and bid == 11:
                j = i + 1
                depth = 1
                while j < n and depth > 0:
                    if fevents[j][0] == 1:
                        depth += 1
                    elif fevents[j][0] == 0:
                        depth -= 1
                    j += 1
                self._decode_constants(fevents[i + 1 : j - 1], vals)
                i = j
                continue
            elif ev_type == 1 and bid in (15, 16, 14):
                j = i + 1
                depth = 1
                while j < n and depth > 0:
                    if fevents[j][0] == 1:
                        depth += 1
                    elif fevents[j][0] == 0:
                        depth -= 1
                    j += 1
                i = j
                continue
            elif ev_type != 2 or bid != 12:
                i += 1
                continue

            cur_id = len(vals)
            if code == 1:  # DECLAREBLOCKS
                num_bbs = ops[0]
            elif code == 2:  # INST_BINOP
                lhs, ty, idx = self._get_val_type_pair(ops, 0, cur_id, vals)
                rhs, idx = self._get_val(ops, idx, cur_id)
                opc = ops[idx]
                flags = ops[idx + 1] if idx + 1 < len(ops) else 0
                vals.append((ty, "inst", len(insts)))
                insts.append(("binop", cur_id, ty, opc, lhs, rhs, flags))
            elif code == 3:  # INST_CAST
                val, _, idx = self._get_val_type_pair(ops, 0, cur_id, vals)
                dest_ty = ops[idx]
                cast_opc = ops[idx + 1]
                vals.append((dest_ty, "inst", len(insts)))
                insts.append(("cast", cur_id, dest_ty, cast_opc, val))
            elif code == 6:  # INST_EXTRACTELT
                vec, vec_ty, idx = self._get_val_type_pair(ops, 0, cur_id, vals)
                elt_idx, _, idx = self._get_val_type_pair(ops, idx, cur_id, vals)
                elem_ty = self.types[vec_ty][2]
                vals.append((elem_ty, "inst", len(insts)))
                insts.append(("extractelt", cur_id, elem_ty, vec, elt_idx))
            elif code == 7:  # INST_INSERTELT
                vec, vec_ty, idx = self._get_val_type_pair(ops, 0, cur_id, vals)
                elt, idx = self._get_val(ops, idx, cur_id)
                elt_idx, _, idx = self._get_val_type_pair(ops, idx, cur_id, vals)
                vals.append((vec_ty, "inst", len(insts)))
                insts.append(("insertelt", cur_id, vec_ty, vec, elt, elt_idx))
            elif code == 8:  # INST_SHUFFLEVEC
                v1, vec_ty, idx = self._get_val_type_pair(ops, 0, cur_id, vals)
                v2, idx = self._get_val(ops, idx, cur_id)
                mask, mask_ty, idx = self._get_val_type_pair(ops, idx, cur_id, vals)
                elem_ty = self.types[vec_ty][2]
                mask_len = self.types[mask_ty][1]
                res_ty = next(tid for tid, t in enumerate(self.types) if t == ("vec", mask_len, elem_ty))
                vals.append((res_ty, "inst", len(insts)))
                insts.append(("shufflevec", cur_id, res_ty, v1, v2, mask))
            elif code == 10:  # INST_RET
                if len(ops) == 0:
                    insts.append(("ret", None, None))
                else:
                    v, ty, _ = self._get_val_type_pair(ops, 0, cur_id, vals)
                    insts.append(("ret", ty, v))
            elif code == 11:  # INST_BR
                if len(ops) == 1:
                    insts.append(("br", ops[0]))
                else:
                    cond, _ = self._get_val(ops, 2, cur_id)
                    insts.append(("br_cond", cond, ops[0], ops[1]))
            elif code == 12:  # INST_SWITCH
                cond_ty = ops[0]
                cond, _ = self._get_val(ops, 1, cur_id)
                def_bb = ops[2]
                cases = []
                idx = 3
                while idx + 1 < len(ops):
                    cases.append((ops[idx], ops[idx + 1]))
                    idx += 2
                insts.append(("switch", cond_ty, cond, def_bb, cases))
            elif code == 15:  # INST_UNREACHABLE
                insts.append(("unreachable",))
            elif code == 16:  # INST_PHI
                ty = ops[0]
                pairs = []
                idx = 1
                while idx + 1 < len(ops):
                    srel = decode_vbr_signed(ops[idx])
                    vid = (cur_id - srel) & 0xFFFFFFFF
                    bb = ops[idx + 1]
                    pairs.append((vid, bb))
                    idx += 2
                vals.append((ty, "inst", len(insts)))
                insts.append(("phi", cur_id, ty, pairs))
            elif code == 19:  # INST_ALLOCA
                alloc_ty = ops[0]
                align_fl = ops[3]
                in_alloca_or_exp = (align_fl & (1 << 6)) != 0
                if in_alloca_or_exp:
                    ptr_ty = ptr_map.get((alloc_ty, 0), alloc_ty)
                    pointee_ty = alloc_ty
                else:
                    ptr_ty = alloc_ty
                    pointee_ty = self.types[ptr_ty][1] if self.types[ptr_ty][0] == "ptr" else alloc_ty
                vals.append((ptr_ty, "inst", len(insts)))
                insts.append(("alloca", cur_id, ptr_ty, pointee_ty))
            elif code == 20:  # INST_LOAD
                ptr, ptr_ty, idx = self._get_val_type_pair(ops, 0, cur_id, vals)
                if idx + 2 < len(ops):
                    res_ty = ops[idx]
                else:
                    res_ty = self.types[ptr_ty][1]
                vals.append((res_ty, "inst", len(insts)))
                insts.append(("load", cur_id, res_ty, ptr))
            elif code == 27:  # INST_INSERTVAL
                agg, agg_ty, idx = self._get_val_type_pair(ops, 0, cur_id, vals)
                elt, _, idx = self._get_val_type_pair(ops, idx, cur_id, vals)
                indices = tuple(ops[idx:])
                vals.append((agg_ty, "inst", len(insts)))
                insts.append(("insertval", cur_id, agg_ty, agg, elt, indices))
            elif code == 28:  # INST_CMP2
                lhs, lhs_ty, idx = self._get_val_type_pair(ops, 0, cur_id, vals)
                rhs, idx = self._get_val(ops, idx, cur_id)
                pred = ops[idx]
                lt = self.types[lhs_ty]
                if lt[0] == "vec":
                    n_el = lt[1]
                    i1_ty = next(tid for tid, t in enumerate(self.types) if t == ("int", 1))
                    res_ty = next(tid for tid, t in enumerate(self.types) if t == ("vec", n_el, i1_ty))
                else:
                    res_ty = next(tid for tid, t in enumerate(self.types) if t == ("int", 1))
                vals.append((res_ty, "inst", len(insts)))
                insts.append(("cmp2", cur_id, res_ty, pred, lhs, rhs))
            elif code == 29:  # INST_VSELECT
                tval, val_ty, idx = self._get_val_type_pair(ops, 0, cur_id, vals)
                fval, idx = self._get_val(ops, idx, cur_id)
                cond, _, idx = self._get_val_type_pair(ops, idx, cur_id, vals)
                vals.append((val_ty, "inst", len(insts)))
                insts.append(("select", cur_id, val_ty, cond, tval, fval))
            elif code == 34:  # INST_CALL
                ccinfo = ops[1]
                idx = 2
                if ccinfo & (1 << 17):
                    idx += 1
                if ccinfo & (1 << 15):
                    fn_ty_id = ops[idx]
                    idx += 1
                else:
                    fn_ty_id = None
                fn_id, fn_ptr_ty, idx = self._get_val_type_pair(ops, idx, cur_id, vals)
                if fn_ty_id is None:
                    fn_ty_id = self.types[fn_ptr_ty][1]
                fn_ty = self.types[fn_ty_id]
                ret_ty = fn_ty[2]
                arg_tys = fn_ty[3]
                call_args = []
                for aty in arg_tys:
                    if self.types[aty][0] == "metadata":
                        idx += 1
                    else:
                        avid, idx = self._get_val(ops, idx, cur_id)
                        call_args.append(avid)
                if self.types[ret_ty][0] != "void":
                    vals.append((ret_ty, "inst", len(insts)))
                    insts.append(("call", cur_id, ret_ty, fn_id, call_args))
                else:
                    insts.append(("call", None, ret_ty, fn_id, call_args))
            elif code == 43:  # INST_GEP
                base_ty = ops[1]
                ptr, ptr_ty, idx = self._get_val_type_pair(ops, 2, cur_id, vals)
                addrspace = self.types[ptr_ty][2]
                cur_t = base_ty
                gep_indices = []
                first = True
                while idx < len(ops):
                    ivid, _ity, idx = self._get_val_type_pair(ops, idx, cur_id, vals)
                    gep_indices.append(ivid)
                    if not first:
                        ct = self.types[cur_t]
                        if ct[0] in ("array", "vec"):
                            cur_t = ct[2]
                        elif ct[0] == "struct":
                            field_idx = vals[ivid][2]
                            cur_t = ct[3][field_idx]
                    first = False
                res_ptr_ty = ptr_map.get((cur_t, addrspace))
                if res_ptr_ty is None:
                    self.types.append(("ptr", cur_t, addrspace))
                    res_ptr_ty = len(self.types) - 1
                    ptr_map[(cur_t, addrspace)] = res_ptr_ty
                vals.append((res_ptr_ty, "inst", len(insts)))
                insts.append(("gep", cur_id, res_ptr_ty, base_ty, ptr, gep_indices))
            elif code == 44:  # INST_STORE
                ptr, _ptr_ty, idx = self._get_val_type_pair(ops, 0, cur_id, vals)
                val, _, idx = self._get_val_type_pair(ops, idx, cur_id, vals)
                insts.append(("store", ptr, val))
            else:
                raise ValueError(f"Unknown function record code {code}")
            i += 1

        # Partition instructions into basic blocks
        bbs: list[list[tuple[Any, ...]]] = []
        cur_bb: list[tuple[Any, ...]] = []
        for ins in insts:
            cur_bb.append(ins)
            if ins[0] in ("ret", "br", "br_cond", "switch", "unreachable"):
                bbs.append(cur_bb)
                cur_bb = []
        if cur_bb:
            bbs.append(cur_bb)

        self.functions.append(
            {
                "info": finfo,
                "name": self.vst.get(finfo["val_id"], f"fn_{finfo['val_id']}"),
                "args": args,
                "num_bbs": len(bbs),
                "vals": vals,
                "insts": insts,
                "bbs": bbs,
            }
        )

    def _compute_layouts(self) -> None:
        """
        Compute (size_bytes, align_bytes, member_offsets) for every type in self.types
        according to target datalayout:
        e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64
        -v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-...
        """
        memo: dict[int, tuple[int, int, tuple[int, ...]]] = {}

        def get_layout(tid: int) -> tuple[int, int, tuple[int, ...]]:
            if tid in memo:
                return memo[tid]
            t = self.types[tid]
            kind = t[0]
            if kind in ("void", "label", "metadata", "opaque", "func"):
                res = (0, 1, ())
            elif kind == "f32":
                res = (4, 4, ())
            elif kind == "f16":
                res = (2, 2, ())
            elif kind == "int":
                bits = t[1]
                if bits <= 8:
                    res = (1, 1, ())
                elif bits <= 16:
                    res = (2, 2, ())
                elif bits <= 32:
                    res = (4, 4, ())
                else:
                    res = (8, 8, ())
            elif kind == "ptr":
                res = (8, 8, ())
            elif kind == "vec":
                n_el, el_ty = t[1], t[2]
                el_sz, _el_al, _ = get_layout(el_ty)
                tot_bits = n_el * el_sz * 8
                # In AIR datalayout: v24:32:32 (3xi8 -> 4B), v48:64:64 (3xf16 -> 8B), v96:128:128 (3xf32 -> 16B)
                if n_el == 3:
                    alloc_sz = 4 * el_sz
                    align = alloc_sz
                else:
                    alloc_sz = n_el * el_sz
                    align = alloc_sz
                res = (alloc_sz, max(1, align), ())
            elif kind == "array":
                n_el, el_ty = t[1], t[2]
                el_sz, el_al, _ = get_layout(el_ty)
                stride = (el_sz + el_al - 1) & ~(el_al - 1)
                res = (n_el * stride, el_al, ())
            elif kind == "struct":
                _name, is_packed, members = t[1], t[2], t[3]
                off = 0
                max_al = 1
                offsets = []
                for m_ty in members:
                    m_sz, m_al, _ = get_layout(m_ty)
                    if not is_packed:
                        off = (off + m_al - 1) & ~(m_al - 1)
                        if m_al > max_al:
                            max_al = m_al
                    offsets.append(off)
                    off += m_sz
                if not is_packed:
                    off = (off + max_al - 1) & ~(max_al - 1)
                res = (off, max_al, tuple(offsets))
            else:
                res = (4, 4, ())
            memo[tid] = res
            return res

        self.type_layouts = [get_layout(tid) for tid in range(len(self.types))]
