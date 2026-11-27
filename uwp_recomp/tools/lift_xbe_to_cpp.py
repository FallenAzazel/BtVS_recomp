#!/usr/bin/env python3
"""
Static x86-32 -> Portable x64 C++ Lifter for BtVS Windows/UWP Static Recomp.

Reads extracted/Default.xbe + decompile/functions.tsv and emits clean,
portable C++17 functions operating on uwp_recomp::X86Context & flat 64 MB RAM.

Usage:
  python uwp_recomp/tools/lift_xbe_to_cpp.py --module console.obj
  python uwp_recomp/tools/lift_xbe_to_cpp.py --limit 100
  python uwp_recomp/tools/lift_xbe_to_cpp.py --all --chunk-size 500
"""

import argparse
import os
import struct
import sys
import time
from collections import Counter

from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_REG, X86_OP_IMM, X86_OP_MEM

REG32 = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"}
REG16 = {"ax": "eax", "cx": "ecx", "dx": "edx", "bx": "ebx", "sp": "esp", "bp": "ebp", "si": "esi", "di": "edi"}
REG8_LO = {"al": "eax", "cl": "ecx", "dl": "edx", "bl": "ebx"}
REG8_HI = {"ah": "eax", "ch": "ecx", "dh": "edx", "bh": "ebx"}

COND_MAP = {
    "je": "ctx.zf", "jz": "ctx.zf",
    "jne": "!ctx.zf", "jnz": "!ctx.zf",
    "jb": "ctx.cf", "jc": "ctx.cf", "jnae": "ctx.cf",
    "jae": "!ctx.cf", "jnc": "!ctx.cf", "jnb": "!ctx.cf",
    "jbe": "(ctx.cf || ctx.zf)", "jna": "(ctx.cf || ctx.zf)",
    "ja": "(!ctx.cf && !ctx.zf)", "jnbe": "(!ctx.cf && !ctx.zf)",
    "jl": "(ctx.sf != ctx.of)", "jnge": "(ctx.sf != ctx.of)",
    "jge": "(ctx.sf == ctx.of)", "jnl": "(ctx.sf == ctx.of)",
    "jle": "(ctx.zf || (ctx.sf != ctx.of))", "jng": "(ctx.zf || (ctx.sf != ctx.of))",
    "jg": "(!ctx.zf && (ctx.sf == ctx.of))", "jnle": "(!ctx.zf && (ctx.sf == ctx.of))",
    "js": "ctx.sf", "jns": "!ctx.sf",
    "jp": "ctx.pf", "jpe": "ctx.pf",
    "jnp": "!ctx.pf", "jpo": "!ctx.pf",
    "jo": "ctx.of", "jno": "!ctx.of",
}

SETCC_MAP = {("set" + k[1:]): v for k, v in COND_MAP.items()}


# SlateEngine VAs overridden by portable x64 host bridges (always dispatch via CallGuestVA)
HOST_OVERRIDE_VAS = {
    0x00038220,  # RenderLog
    0x00038250,  # RenderError
    0x00062650,  # G_ErrorToUser
    0x00063250,  # RunInstaller
    0x00063920,  # ShellSetTitle
    0x00063AE0,  # ShellWarning
    0x000A06D0,  # ScriptLog
    0x00297020,  # Movie_Kill
    0x00297060,  # Movie_KillCurrent
    0x002970A0,  # Movie_Playing
    0x00297A90,  # Movie_Play
    0x002AA870,  # TLocker::Lock
    0x002AA950,  # TLocker::Unlock
    0x002AC5B0,  # TTimerManager::GetTimeStamp
}


def load_xbe(xbe_path: str):
    data = open(xbe_path, "rb").read()
    base = struct.unpack_from("<I", data, 0x104)[0]
    nsec = struct.unpack_from("<I", data, 0x11C)[0]
    sec_hdr = struct.unpack_from("<I", data, 0x120)[0] - base
    secs = []
    for i in range(nsec):
        vaddr, vsize, raddr, rsize = struct.unpack_from("<IIII", data, sec_hdr + i * 56 + 4)
        secs.append((vaddr, vsize, raddr, rsize))
    return data, secs


def va_to_raw(secs, va: int):
    for vaddr, vsize, raddr, rsize in secs:
        if vaddr <= va < vaddr + rsize:
            return raddr + (va - vaddr)
    return None


def load_functions(tsv_path: str):
    funcs = []
    with open(tsv_path, "r", encoding="utf-8", errors="replace") as f:
        next(f)
        for line in f:
            p = line.rstrip("\n").split("\t")
            if len(p) < 10:
                continue
            funcs.append({
                "va": int(p[0], 16),
                "size": int(p[2]),
                "section": p[3],
                "library": p[4],
                "object": p[5],
                "mangled": p[8],
                "demangled": p[9],
            })
    return funcs


class X86Lifter:
    def __init__(self, known_vas: set[int], xbe_data: bytes, secs: list):
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = True
        self.known_vas = known_vas
        self.xbe_data = xbe_data
        self.secs = secs
        self.unhandled = Counter()
        self.total_insns = 0
        self.lifted_insns = 0
        self.switch_tables = 0

    def _scan_jump_table(self, table_va: int, valid_addrs: set[int]) -> list[int]:
        targets = []
        seen = set()
        for i in range(512):
            raw = va_to_raw(self.secs, table_va + i * 4)
            if raw is None or raw + 4 > len(self.xbe_data):
                break
            va = struct.unpack_from("<I", self.xbe_data, raw)[0]
            if va not in valid_addrs:
                break
            if va not in seen:
                seen.add(va)
                targets.append(va)
        return targets

    def reg_name(self, insn, reg_id: int) -> str:
        return insn.reg_name(reg_id) if reg_id else ""

    def mem_addr(self, insn, op) -> str:
        def fmt_addr_reg(reg_id: int) -> str:
            rn = self.reg_name(insn, reg_id)
            if rn in REG32:
                return f"ctx.{rn}"
            if rn in REG16:
                return f"static_cast<uint32_t>(X86Context::lo16(ctx.{REG16[rn]}))"
            return "0u"

        parts = []
        m = op.mem
        if m.base:
            parts.append(fmt_addr_reg(m.base))
        if m.index:
            idx = fmt_addr_reg(m.index)
            parts.append(f"({idx} * {m.scale}u)" if m.scale != 1 else idx)
        if m.disp != 0 or not parts:
            parts.append(f"0x{m.disp & 0xFFFFFFFF:X}u")
        return "(" + " + ".join(parts) + ")"

    def read_op(self, insn, op, bits: int = 0) -> str:
        if bits == 0:
            bits = op.size * 8 if op.size else 32
        if op.type == X86_OP_IMM:
            mask = (1 << bits) - 1
            return f"0x{op.imm & mask:X}u"
        if op.type == X86_OP_REG:
            rn = self.reg_name(insn, op.reg)
            if rn in REG32:
                return f"ctx.{rn}"
            if rn in REG16:
                return f"X86Context::lo16(ctx.{REG16[rn]})"
            if rn in REG8_LO:
                return f"X86Context::lo8(ctx.{REG8_LO[rn]})"
            if rn in REG8_HI:
                return f"X86Context::hi8(ctx.{REG8_HI[rn]})"
            return "0u"
        if op.type == X86_OP_MEM:
            addr = self.mem_addr(insn, op)
            if bits == 8:
                return f"ctx.r8({addr})"
            if bits == 16:
                return f"ctx.r16({addr})"
            return f"ctx.r32({addr})"
        return "0u"

    def write_op(self, insn, op, expr: str, bits: int = 0) -> str:
        if bits == 0:
            bits = op.size * 8 if op.size else 32
        if op.type == X86_OP_REG:
            rn = self.reg_name(insn, op.reg)
            if rn in REG32:
                return f"ctx.{rn} = ({expr});"
            if rn in REG16:
                return f"X86Context::setLo16(ctx.{REG16[rn]}, static_cast<uint16_t>({expr}));"
            if rn in REG8_LO:
                return f"X86Context::setLo8(ctx.{REG8_LO[rn]}, static_cast<uint8_t>({expr}));"
            if rn in REG8_HI:
                return f"X86Context::setHi8(ctx.{REG8_HI[rn]}, static_cast<uint8_t>({expr}));"
            return f"/* unhandled reg write {rn} */"
        if op.type == X86_OP_MEM:
            addr = self.mem_addr(insn, op)
            if bits == 8:
                return f"ctx.w8({addr}, static_cast<uint8_t>({expr}));"
            if bits == 16:
                return f"ctx.w16({addr}, static_cast<uint16_t>({expr}));"
            return f"ctx.w32({addr}, static_cast<uint32_t>({expr}));"
        return "/* unhandled write */"

    def read_fpu_op(self, insn, op) -> str:
        if op.type == X86_OP_REG:
            rn = self.reg_name(insn, op.reg)
            if rn.startswith("st(") and rn.endswith(")"):
                return f"ctx.fpuReg({rn[3:-1]})"
            return "ctx.fpuReg(0)"
        if op.type == X86_OP_MEM:
            addr = self.mem_addr(insn, op)
            if op.size == 8:
                return f"ctx.rf64({addr})"
            return f"static_cast<double>(ctx.rf32({addr}))"
        return "0.0"

    def lift_function(self, func: dict, code: bytes) -> str:
        va0 = func["va"]
        va_end = va0 + func["size"]
        insns = list(self.md.disasm(code, va0))

        # Collect local jump target labels that land on actual decoded instruction addresses
        valid_addrs = {insn.address for insn in insns}
        local_labels = set()
        jump_tables: dict[int, list[int]] = {}
        for idx, insn in enumerate(insns):
            m = insn.mnemonic
            if (m == "jmp" or m in COND_MAP) and insn.operands and insn.operands[0].type == X86_OP_IMM:
                tgt = insn.operands[0].imm & 0xFFFFFFFF
                if tgt in valid_addrs:
                    local_labels.add(tgt)
            elif m == "jmp" and insn.operands and insn.operands[0].type != X86_OP_IMM:
                jt_targets = []
                op0 = insn.operands[0]
                if op0.type == X86_OP_MEM and op0.mem.disp != 0:
                    jt_targets = self._scan_jump_table(op0.mem.disp & 0xFFFFFFFF, valid_addrs)
                if not jt_targets:
                    for back in range(max(0, idx - 6), idx):
                        for bop in insns[back].operands:
                            if bop.type == X86_OP_MEM and bop.mem.disp != 0:
                                cand = self._scan_jump_table(bop.mem.disp & 0xFFFFFFFF, valid_addrs)
                                if len(cand) > len(jt_targets):
                                    jt_targets = cand
                if jt_targets:
                    jump_tables[insn.address] = jt_targets
                    local_labels.update(jt_targets)
                    self.switch_tables += 1

        out = []
        sig = func["demangled"].replace("*/", "* /")
        out.append(f"// {sig} ({func['object']}, {func['size']} bytes)")
        out.append(f"void sub_{va0:08X}(X86Context& ctx) {{")

        # Built-in portable 1 MHz steady_clock overrides for Xbox TTimerManager (replaces 733 MHz rdtsc!)
        timer_overrides = {
            0x002AC510: "    uint32_t clocks = ctx.r32(ctx.esp + 0x4u);\n    ctx.fpuPush(static_cast<double>(clocks) * 0.001);\n    ctx.esp += 8u; return;",
            0x002AC530: "    uint32_t clocks = ctx.r32(ctx.esp + 0x4u);\n    double sec = static_cast<double>(clocks) * 0.000001;\n    if (sec > 0.1) sec = 0.1;\n    ctx.fpuPush(sec);\n    ctx.esp += 8u; return;",
            0x002AC550: "    uint32_t delta = ctx.r32(ctx.esp + 0x4u) - ctx.r32(ctx.esp + 0x8u);\n    ctx.fpuPush(static_cast<double>(delta) * 0.001);\n    ctx.esp += 12u; return;",
            0x002AC580: "    uint32_t delta = ctx.r32(ctx.esp + 0x4u) - ctx.r32(ctx.esp + 0x8u);\n    double sec = static_cast<double>(delta) * 0.000001;\n    if (sec > 0.1) sec = 0.1;\n    ctx.fpuPush(sec);\n    ctx.esp += 12u; return;",
            0x002AC5B0: "    ctx.eax = UwpTimerMicros32();\n    ctx.esp += 4u; return;",
        }
        if va0 in timer_overrides:
            out.append(timer_overrides[va0])
            out.append("}")
            return "\n".join(out)

        for insn in insns:
            self.total_insns += 1
            if insn.address in local_labels:
                out.append(f"L_{insn.address:08X}:;")
            line = self._lift_insn(insn, va0, va_end, local_labels, jump_tables)
            if line is not None:
                self.lifted_insns += 1
                if line:
                    out.append(f"    {line}")
            else:
                self.unhandled[insn.mnemonic] += 1
                out.append(f"    /* TODO 0x{insn.address:08X}: {insn.mnemonic} {insn.op_str} */")

        out.append("}")
        return "\n".join(out)

    def _lift_insn(self, insn, va0: int, va_end: int, local_labels: set[int], jump_tables: dict[int, list[int]]):
        m = insn.mnemonic
        ops = insn.operands

        if m in ("nop", "int3", "wait", "fwait"):
            return ""

        # --- Data Movement ---
        if m == "mov" and len(ops) == 2:
            bits = ops[0].size * 8 or 32
            return self.write_op(insn, ops[0], self.read_op(insn, ops[1], bits), bits)

        if m == "movzx" and len(ops) == 2:
            sbits = ops[1].size * 8 or 8
            dbits = ops[0].size * 8 or 32
            return self.write_op(insn, ops[0], f"static_cast<uint32_t>({self.read_op(insn, ops[1], sbits)})", dbits)

        if m == "movsx" and len(ops) == 2:
            sbits = ops[1].size * 8 or 8
            dbits = ops[0].size * 8 or 32
            stype = "int8_t" if sbits == 8 else "int16_t"
            return self.write_op(insn, ops[0], f"static_cast<uint32_t>(static_cast<int32_t>(static_cast<{stype}>({self.read_op(insn, ops[1], sbits)})))", dbits)

        if m == "lea" and len(ops) == 2 and ops[1].type == X86_OP_MEM:
            return self.write_op(insn, ops[0], self.mem_addr(insn, ops[1]), 32)

        if m == "push" and len(ops) == 1:
            return f"ctx.push32({self.read_op(insn, ops[0], 32)});"

        if m == "pop" and len(ops) == 1:
            return self.write_op(insn, ops[0], "ctx.pop32()", 32)

        if m == "cdq":
            return "ctx.edx = (static_cast<int32_t>(ctx.eax) < 0) ? 0xFFFFFFFFu : 0u;"

        if m == "leave":
            return "ctx.esp = ctx.ebp; ctx.ebp = ctx.pop32();"

        if m == "clc": return "ctx.cf = false;"
        if m == "stc": return "ctx.cf = true;"
        if m == "cld": return "ctx.df = false;"
        if m == "std": return "ctx.df = true;"

        if m == "xchg" and len(ops) == 2:
            bits = ops[0].size * 8 or 32
            a = self.read_op(insn, ops[0], bits)
            b = self.read_op(insn, ops[1], bits)
            w0 = self.write_op(insn, ops[0], "tb", bits)
            w1 = self.write_op(insn, ops[1], "ta", bits)
            return f"{{ uint32_t ta = {a}, tb = {b}; {w0} {w1} }}"

        if m in ("rol", "ror") and len(ops) == 2:
            bits = ops[0].size * 8 or 32
            a = self.read_op(insn, ops[0], bits)
            c = self.read_op(insn, ops[1], 8)
            w = self.write_op(insn, ops[0], "r", bits)
            if m == "rol":
                return f"{{ uint32_t c = ({c}) & {bits - 1}u; uint32_t a = {a}; uint32_t r = c ? ((a << c) | (a >> ({bits}u - c))) : a; {w} }}"
            return f"{{ uint32_t c = ({c}) & {bits - 1}u; uint32_t a = {a}; uint32_t r = c ? ((a >> c) | (a << ({bits}u - c))) : a; {w} }}"

        if m == "movsb":
            return "{ uint32_t d = ctx.df ? 0xFFFFFFFFu : 1u; ctx.w8(ctx.edi, ctx.r8(ctx.esi)); ctx.esi += d; ctx.edi += d; }"
        if m == "movsd":
            return "{ uint32_t d = ctx.df ? 0xFFFFFFFCu : 4u; ctx.w32(ctx.edi, ctx.r32(ctx.esi)); ctx.esi += d; ctx.edi += d; }"
        if m == "stosb":
            return "{ uint32_t d = ctx.df ? 0xFFFFFFFFu : 1u; ctx.w8(ctx.edi, X86Context::lo8(ctx.eax)); ctx.edi += d; }"
        if m == "stosd":
            return "{ uint32_t d = ctx.df ? 0xFFFFFFFCu : 4u; ctx.w32(ctx.edi, ctx.eax); ctx.edi += d; }"
        if m == "lodsb":
            return "{ uint32_t d = ctx.df ? 0xFFFFFFFFu : 1u; X86Context::setLo8(ctx.eax, ctx.r8(ctx.esi)); ctx.esi += d; }"

        if m == "sahf":
            return "ctx.sahf();"

        if m == "rep movsd":
            return "{ uint32_t d = ctx.df ? 0xFFFFFFFCu : 4u; while (ctx.ecx) { ctx.w32(ctx.edi, ctx.r32(ctx.esi)); ctx.esi += d; ctx.edi += d; ctx.ecx--; } }"
        if m == "rep movsb":
            return "{ uint32_t d = ctx.df ? 0xFFFFFFFFu : 1u; while (ctx.ecx) { ctx.w8(ctx.edi, ctx.r8(ctx.esi)); ctx.esi += d; ctx.edi += d; ctx.ecx--; } }"
        if m == "rep stosd":
            return "{ uint32_t d = ctx.df ? 0xFFFFFFFCu : 4u; while (ctx.ecx) { ctx.w32(ctx.edi, ctx.eax); ctx.edi += d; ctx.ecx--; } }"
        if m == "rep stosb":
            return "{ uint32_t d = ctx.df ? 0xFFFFFFFFu : 1u; while (ctx.ecx) { ctx.w8(ctx.edi, X86Context::lo8(ctx.eax)); ctx.edi += d; ctx.ecx--; } }"

        # --- Integer ALU ---
        if m in ("add", "sub", "and", "or", "xor") and len(ops) == 2:
            bits = ops[0].size * 8 or 32
            a = self.read_op(insn, ops[0], bits)
            b = self.read_op(insn, ops[1], bits)
            op_sym = {"add": "+", "sub": "-", "and": "&", "or": "|", "xor": "^"}[m]
            w = self.write_op(insn, ops[0], "r", bits)
            if m == "add":
                return f"{{ uint32_t a = {a}, b = {b}, r = a + b; {w} ctx.setFlagsAdd(a, b, r, {bits}); }}"
            if m == "sub":
                return f"{{ uint32_t a = {a}, b = {b}, r = a - b; {w} ctx.setFlagsSub(a, b, r, {bits}); }}"
            return f"{{ uint32_t r = ({a}) {op_sym} ({b}); {w} ctx.setFlagsLogic(r, {bits}); }}"

        if m in ("adc", "sbb") and len(ops) == 2:
            bits = ops[0].size * 8 or 32
            a = self.read_op(insn, ops[0], bits)
            b = self.read_op(insn, ops[1], bits)
            w = self.write_op(insn, ops[0], "r", bits)
            if m == "adc":
                return f"{{ uint32_t a = {a}, b = {b}, c = ctx.cf ? 1u : 0u, r = a + b + c; {w} ctx.setFlagsAdd(a, b + c, r, {bits}); }}"
            return f"{{ uint32_t a = {a}, b = {b}, c = ctx.cf ? 1u : 0u, r = a - b - c; {w} ctx.setFlagsSub(a, b + c, r, {bits}); }}"

        if m in ("inc", "dec") and len(ops) == 1:
            bits = ops[0].size * 8 or 32
            a = self.read_op(insn, ops[0], bits)
            w = self.write_op(insn, ops[0], "r", bits)
            if m == "inc":
                return f"{{ bool oldCf = ctx.cf; uint32_t a = {a}, r = a + 1u; {w} ctx.setFlagsAdd(a, 1u, r, {bits}); ctx.cf = oldCf; }}"
            return f"{{ bool oldCf = ctx.cf; uint32_t a = {a}, r = a - 1u; {w} ctx.setFlagsSub(a, 1u, r, {bits}); ctx.cf = oldCf; }}"

        if m == "neg" and len(ops) == 1:
            bits = ops[0].size * 8 or 32
            a = self.read_op(insn, ops[0], bits)
            w = self.write_op(insn, ops[0], "r", bits)
            return f"{{ uint32_t a = {a}, r = 0u - a; {w} ctx.setFlagsSub(0u, a, r, {bits}); }}"

        if m == "not" and len(ops) == 1:
            bits = ops[0].size * 8 or 32
            return self.write_op(insn, ops[0], f"~({self.read_op(insn, ops[0], bits)})", bits)

        if m in ("shl", "sal", "shr", "sar") and len(ops) == 2:
            bits = ops[0].size * 8 or 32
            a = self.read_op(insn, ops[0], bits)
            c = self.read_op(insn, ops[1], 8)
            w = self.write_op(insn, ops[0], "r", bits)
            if m in ("shl", "sal"):
                return f"{{ uint32_t c = ({c}) & 31u; uint32_t r = ({a}) << c; {w} if (c) ctx.setFlagsLogic(r, {bits}); }}"
            if m == "shr":
                return f"{{ uint32_t c = ({c}) & 31u; uint32_t r = ({a}) >> c; {w} if (c) ctx.setFlagsLogic(r, {bits}); }}"
            return f"{{ uint32_t c = ({c}) & 31u; uint32_t r = static_cast<uint32_t>(static_cast<int32_t>({a}) >> c); {w} if (c) ctx.setFlagsLogic(r, {bits}); }}"

        if m == "imul":
            if len(ops) == 2:
                a = self.read_op(insn, ops[0], 32)
                b = self.read_op(insn, ops[1], 32)
                w = self.write_op(insn, ops[0], "static_cast<uint32_t>(p)", 32)
                return f"{{ int64_t p = static_cast<int64_t>(static_cast<int32_t>({a})) * static_cast<int32_t>({b}); {w} }}"
            if len(ops) == 3:
                a = self.read_op(insn, ops[1], 32)
                b = self.read_op(insn, ops[2], 32)
                w = self.write_op(insn, ops[0], "static_cast<uint32_t>(p)", 32)
                return f"{{ int64_t p = static_cast<int64_t>(static_cast<int32_t>({a})) * static_cast<int32_t>({b}); {w} }}"
            if len(ops) == 1:
                b = self.read_op(insn, ops[0], 32)
                return f"{{ int64_t p = static_cast<int64_t>(static_cast<int32_t>(ctx.eax)) * static_cast<int32_t>({b}); ctx.eax = static_cast<uint32_t>(p); ctx.edx = static_cast<uint32_t>(static_cast<uint64_t>(p) >> 32); }}"

        if m == "mul" and len(ops) == 1:
            bits = ops[0].size * 8 or 32
            b = self.read_op(insn, ops[0], bits)
            if bits == 8:
                return f"{{ uint16_t p = static_cast<uint16_t>(X86Context::lo8(ctx.eax)) * static_cast<uint16_t>({b}); X86Context::setLo16(ctx.eax, p); ctx.cf = ctx.of = ((p >> 8) != 0u); }}"
            if bits == 16:
                return f"{{ uint32_t p = static_cast<uint32_t>(X86Context::lo16(ctx.eax)) * static_cast<uint32_t>({b}); X86Context::setLo16(ctx.eax, static_cast<uint16_t>(p)); X86Context::setLo16(ctx.edx, static_cast<uint16_t>(p >> 16)); ctx.cf = ctx.of = ((p >> 16) != 0u); }}"
            return f"{{ uint64_t p = static_cast<uint64_t>(ctx.eax) * static_cast<uint32_t>({b}); ctx.eax = static_cast<uint32_t>(p); ctx.edx = static_cast<uint32_t>(p >> 32); ctx.cf = ctx.of = (ctx.edx != 0u); }}"

        if m == "idiv" and len(ops) == 1:
            d = self.read_op(insn, ops[0], 32)
            return f"{{ int64_t num = (static_cast<int64_t>(static_cast<int32_t>(ctx.edx)) << 32) | ctx.eax; int32_t den = static_cast<int32_t>({d}); if (den) {{ ctx.eax = static_cast<uint32_t>(num / den); ctx.edx = static_cast<uint32_t>(num % den); }} }}"

        if m == "div" and len(ops) == 1:
            d = self.read_op(insn, ops[0], 32)
            return f"{{ uint64_t num = (static_cast<uint64_t>(ctx.edx) << 32) | ctx.eax; uint32_t den = {d}; if (den) {{ ctx.eax = static_cast<uint32_t>(num / den); ctx.edx = static_cast<uint32_t>(num % den); }} }}"

        if m == "cmp" and len(ops) == 2:
            bits = ops[0].size * 8 or 32
            a = self.read_op(insn, ops[0], bits)
            b = self.read_op(insn, ops[1], bits)
            return f"{{ uint32_t a = {a}, b = {b}; ctx.setFlagsSub(a, b, a - b, {bits}); }}"

        if m == "test" and len(ops) == 2:
            bits = ops[0].size * 8 or 32
            a = self.read_op(insn, ops[0], bits)
            b = self.read_op(insn, ops[1], bits)
            return f"ctx.setFlagsLogic(({a}) & ({b}), {bits});"

        if m in SETCC_MAP and len(ops) == 1:
            return self.write_op(insn, ops[0], f"({SETCC_MAP[m]} ? 1u : 0u)", 8)

        # --- Control Flow ---
        if m == "ret":
            if ops and ops[0].type == X86_OP_IMM:
                return f"ctx.esp += {4 + ops[0].imm}u; return;"
            return "ctx.esp += 4u; return;"

        if m == "call" and len(ops) == 1:
            if ops[0].type == X86_OP_IMM:
                tgt = ops[0].imm & 0xFFFFFFFF
                if tgt in self.known_vas and tgt not in HOST_OVERRIDE_VAS:
                    return f"ctx.push32(0x{insn.address + insn.size:08X}u); sub_{tgt:08X}(ctx);"
                return f"ctx.push32(0x{insn.address + insn.size:08X}u); CallGuestVA(ctx, 0x{tgt:08X}u);"
            tgt_expr = self.read_op(insn, ops[0], 32)
            return f"{{ uint32_t t = {tgt_expr}; ctx.push32(0x{insn.address + insn.size:08X}u); CallGuestVA(ctx, t); }}"

        if m == "jmp" and len(ops) == 1:
            if ops[0].type == X86_OP_IMM:
                tgt = ops[0].imm & 0xFFFFFFFF
                if tgt in local_labels:
                    return f"goto L_{tgt:08X};"
                if tgt in self.known_vas and tgt not in HOST_OVERRIDE_VAS:
                    return f"sub_{tgt:08X}(ctx); return;"
                return f"CallGuestVA(ctx, 0x{tgt:08X}u); return;"
            tgt_expr = self.read_op(insn, ops[0], 32)
            jt = jump_tables.get(insn.address)
            if jt:
                cases = " ".join(f"case 0x{t:08X}u: goto L_{t:08X};" for t in jt)
                return f"{{ uint32_t jt = {tgt_expr}; switch (jt) {{ {cases} default: CallGuestVA(ctx, jt); return; }} }}"
            return f"CallGuestVA(ctx, {tgt_expr}); return;"

        if m in COND_MAP and len(ops) == 1 and ops[0].type == X86_OP_IMM:
            tgt = ops[0].imm & 0xFFFFFFFF
            cond = COND_MAP[m]
            if tgt in local_labels:
                return f"if ({cond}) goto L_{tgt:08X};"
            return f"if ({cond}) {{ CallGuestVA(ctx, 0x{tgt:08X}u); return; }}"

        # --- x87 FPU ---
        if m == "fld" and len(ops) == 1:
            return f"ctx.fpuPush({self.read_fpu_op(insn, ops[0])});"
        if m == "fldz":
            return "ctx.fpuPush(0.0);"
        if m == "fld1":
            return "ctx.fpuPush(1.0);"
        if m == "fild" and len(ops) == 1:
            bits = ops[0].size * 8 or 32
            stype = "int16_t" if bits == 16 else "int32_t"
            return f"ctx.fpuPush(static_cast<double>(static_cast<{stype}>({self.read_op(insn, ops[0], bits)})));"
        if m in ("fiadd", "fisub", "fisubr", "fimul", "fidiv", "fidivr") and len(ops) == 1:
            bits = ops[0].size * 8 or 32
            stype = "int16_t" if bits == 16 else "int32_t"
            rhs = f"static_cast<double>(static_cast<{stype}>({self.read_op(insn, ops[0], bits)}))"
            if m == "fiadd":  return f"ctx.fpuReg(0) += ({rhs});"
            if m == "fisub":  return f"ctx.fpuReg(0) -= ({rhs});"
            if m == "fisubr": return f"ctx.fpuReg(0) = ({rhs}) - ctx.fpuReg(0);"
            if m == "fimul":  return f"ctx.fpuReg(0) *= ({rhs});"
            if m == "fidiv":  return f"ctx.fpuReg(0) /= ({rhs});"
            if m == "fidivr": return f"ctx.fpuReg(0) = ({rhs}) / ctx.fpuReg(0);"
        if m in ("fst", "fstp") and len(ops) == 1:
            pop = "ctx.fpuPop()" if m == "fstp" else "ctx.fpuReg(0)"
            if ops[0].type == X86_OP_MEM:
                addr = self.mem_addr(insn, ops[0])
                if ops[0].size == 8:
                    return f"ctx.wf64({addr}, {pop});"
                return f"ctx.wf32({addr}, static_cast<float>({pop}));"
            if ops[0].type == X86_OP_REG:
                rn = self.reg_name(insn, ops[0].reg)
                idx = int(rn[3:-1]) if rn.startswith("st(") else 0
                if m == "fstp":
                    return f"{{ double v = ctx.fpuReg(0); ctx.fpuReg({idx}) = v; ctx.fpuPop(); }}"
                return f"ctx.fpuReg({idx}) = ctx.fpuReg(0);"
        if m == "fistp" and len(ops) == 1 and ops[0].type == X86_OP_MEM:
            bits = ops[0].size * 8 or 32
            return self.write_op(insn, ops[0], "static_cast<uint32_t>(static_cast<int32_t>(ctx.fpuPop()))", bits)
        if m in ("fadd", "fsub", "fsubr", "fmul", "fdiv", "fdivr") and len(ops) >= 1:
            if len(ops) == 1:
                rhs = self.read_fpu_op(insn, ops[0])
                if m == "fadd":  return f"ctx.fpuReg(0) += ({rhs});"
                if m == "fsub":  return f"ctx.fpuReg(0) -= ({rhs});"
                if m == "fsubr": return f"ctx.fpuReg(0) = ({rhs}) - ctx.fpuReg(0);"
                if m == "fmul":  return f"ctx.fpuReg(0) *= ({rhs});"
                if m == "fdiv":  return f"ctx.fpuReg(0) /= ({rhs});"
                if m == "fdivr": return f"ctx.fpuReg(0) = ({rhs}) / ctx.fpuReg(0);"
            else:
                lhs = self.read_fpu_op(insn, ops[0])
                rhs = self.read_fpu_op(insn, ops[1])
                if m == "fadd":  return f"{lhs} += ({rhs});"
                if m == "fsub":  return f"{lhs} -= ({rhs});"
                if m == "fsubr": return f"{lhs} = ({rhs}) - ({lhs});"
                if m == "fmul":  return f"{lhs} *= ({rhs});"
                if m == "fdiv":  return f"{lhs} /= ({rhs});"
                if m == "fdivr": return f"{lhs} = ({rhs}) / ({lhs});"
        if m in ("faddp", "fsubp", "fsubrp", "fmulp", "fdivp", "fdivrp"):
            dst = self.read_fpu_op(insn, ops[0]) if ops else "ctx.fpuReg(1)"
            if m == "faddp":  return f"{dst} += ctx.fpuReg(0); ctx.fpuPop();"
            if m == "fsubp":  return f"{dst} -= ctx.fpuReg(0); ctx.fpuPop();"
            if m == "fsubrp": return f"{dst} = ctx.fpuReg(0) - ({dst}); ctx.fpuPop();"
            if m == "fmulp":  return f"{dst} *= ctx.fpuReg(0); ctx.fpuPop();"
            if m == "fdivp":  return f"{dst} /= ctx.fpuReg(0); ctx.fpuPop();"
            if m == "fdivrp": return f"{dst} = ctx.fpuReg(0) / ({dst}); ctx.fpuPop();"
        if m == "fchs":
            return "ctx.fpuReg(0) = -ctx.fpuReg(0);"
        if m == "fabs":
            return "ctx.fpuReg(0) = std::fabs(ctx.fpuReg(0));"
        if m == "fsqrt":
            return "ctx.fpuReg(0) = std::sqrt(ctx.fpuReg(0));"
        if m == "fsin":
            return "ctx.fpuReg(0) = std::sin(ctx.fpuReg(0));"
        if m == "fcos":
            return "ctx.fpuReg(0) = std::cos(ctx.fpuReg(0));"
        if m == "fsincos":
            return "{ double a = ctx.fpuReg(0); ctx.fpuReg(0) = std::sin(a); ctx.fpuPush(std::cos(a)); }"
        if m == "fptan":
            return "{ ctx.fpuReg(0) = std::tan(ctx.fpuReg(0)); ctx.fpuPush(1.0); }"
        if m == "fpatan":
            return "{ double x = ctx.fpuPop(); ctx.fpuReg(0) = std::atan2(ctx.fpuReg(0), x); }"
        if m == "frndint":
            return "ctx.fpuReg(0) = std::nearbyint(ctx.fpuReg(0));"
        if m == "fldl2e":
            return "ctx.fpuPush(1.4426950408889634);"
        if m == "fldln2":
            return "ctx.fpuPush(0.6931471805599453);"
        if m == "fldlg2":
            return "ctx.fpuPush(0.3010299956639812);"
        if m == "fldl2t":
            return "ctx.fpuPush(3.3219280948873623);"
        if m == "f2xm1":
            return "ctx.fpuReg(0) = std::pow(2.0, ctx.fpuReg(0)) - 1.0;"
        if m == "fscale":
            return "ctx.fpuReg(0) = std::ldexp(ctx.fpuReg(0), static_cast<int>(std::trunc(ctx.fpuReg(1))));"
        if m == "fyl2x":
            return "{ double x = ctx.fpuPop(); ctx.fpuReg(0) *= std::log2(x); }"
        if m == "fyl2xp1":
            return "{ double x = ctx.fpuPop(); ctx.fpuReg(0) *= std::log2(x + 1.0); }"
        if m in ("fprem", "fprem1"):
            return "ctx.fpuReg(0) = std::fmod(ctx.fpuReg(0), ctx.fpuReg(1));"
        if m == "repe cmpsb":
            return "{ uint32_t d = ctx.df ? 0xFFFFFFFFu : 1u; while (ctx.ecx) { uint8_t a = ctx.r8(ctx.esi), b = ctx.r8(ctx.edi); ctx.setFlagsSub(a, b, a - b, 8); ctx.esi += d; ctx.edi += d; --ctx.ecx; if (!ctx.zf) break; } }"
        if m == "rdtsc":
            return "{ uint64_t t = static_cast<uint64_t>(UwpTimerMicros32()); ctx.eax = static_cast<uint32_t>(t); ctx.edx = static_cast<uint32_t>(t >> 32); }"
        if m == "fxch":
            dst = self.read_fpu_op(insn, ops[-1]) if ops else "ctx.fpuReg(1)"
            return f"{{ double t = ctx.fpuReg(0); ctx.fpuReg(0) = {dst}; {dst} = t; }}"
        if m in ("fcom", "fcomp", "fcompp", "fucom", "fucomp", "fucompp"):
            rhs = self.read_fpu_op(insn, ops[0]) if ops else "ctx.fpuReg(1)"
            pops = " ctx.fpuPop(); ctx.fpuPop();" if m.endswith("pp") else (" ctx.fpuPop();" if m.endswith("p") else "")
            return f"ctx.fpuCompare(ctx.fpuReg(0), {rhs});{pops}"
        if m in ("fnstsw", "fstsw") and len(ops) == 1:
            return self.write_op(insn, ops[0], "ctx.fpuSw", 16)

        return None


def main() -> int:
    ap = argparse.ArgumentParser(description="Static x86 -> Portable x64 C++ Lifter for BtVS UWP Recomp")
    ap.add_argument("--xbe", default="extracted/Default.xbe")
    ap.add_argument("--tsv", default="decompile/functions.tsv")
    ap.add_argument("--out", default="uwp_recomp/src/generated")
    ap.add_argument("--module", default=None, help="Lift only functions from a specific .obj (e.g. console.obj)")
    ap.add_argument("--limit", type=int, default=0, help="Lift at most N SlateEngine functions")
    ap.add_argument("--all", action="store_true", help="Lift all SlateEngine functions")
    ap.add_argument("--chunk-size", type=int, default=500, help="Functions per generated .cpp chunk")
    args = ap.parse_args()

    t0 = time.time()
    xbe_data, secs = load_xbe(args.xbe)
    all_funcs = load_functions(args.tsv)

    # Filter to SlateEngine functions in .text and deduplicate COMDAT-folded VAs
    seen_va = set()
    targets = []
    for f in all_funcs:
        if f["library"] == "SlateEngine" and f["size"] > 0 and f["va"] not in seen_va:
            seen_va.add(f["va"])
            targets.append(f)
    if args.module:
        mod_l = args.module.lower()
        targets = [f for f in targets if f["object"].lower() == mod_l]
    elif args.limit > 0:
        targets = targets[: args.limit]
    elif not args.all:
        # Default safe vertical slice: first 200 SlateEngine functions
        targets = targets[:200]

    known_vas = {f["va"] for f in targets}
    lifter = X86Lifter(known_vas, xbe_data, secs)
    os.makedirs(args.out, exist_ok=True)

    # Emit forward declarations header
    with open(os.path.join(args.out, "recomp_decls.h"), "w", encoding="utf-8") as fh:
        fh.write("#pragma once\n#include \"x86_cpu.h\"\n\nnamespace uwp_recomp {\n")
        for f in targets:
            fh.write(f"void sub_{f['va']:08X}(X86Context& ctx);\n")
        fh.write("void RegisterAllLiftedFunctions();\n} // namespace uwp_recomp\n")

    # Emit lifted C++ chunks
    chunk_files = []
    for c_idx in range(0, len(targets), args.chunk_size):
        chunk = targets[c_idx : c_idx + args.chunk_size]
        fname = f"recomp_chunk_{c_idx // args.chunk_size:03d}.cpp"
        chunk_files.append(fname)
        with open(os.path.join(args.out, fname), "w", encoding="utf-8") as fc:
            fc.write("#include \"recomp_decls.h\"\n\nnamespace uwp_recomp {\n\n")
            for f in chunk:
                raw = va_to_raw(secs, f["va"])
                if raw is None:
                    continue
                code = xbe_data[raw : raw + f["size"]]
                fc.write(lifter.lift_function(f, code))
                fc.write("\n\n")
            fc.write("} // namespace uwp_recomp\n")

    # Emit registration table
    with open(os.path.join(args.out, "recomp_register.cpp"), "w", encoding="utf-8") as fr:
        fr.write("#include \"recomp_decls.h\"\n\nnamespace uwp_recomp {\n\nvoid RegisterAllLiftedFunctions() {\n")
        for f in targets:
            fr.write(f"    RegisterGuestFunc(0x{f['va']:08X}u, &sub_{f['va']:08X});\n")
        fr.write("}\n\n} // namespace uwp_recomp\n")

    elapsed = time.time() - t0
    pct = (100.0 * lifter.lifted_insns / lifter.total_insns) if lifter.total_insns else 0.0
    print(f"[Lift] Lifted {len(targets):,} functions into {len(chunk_files)} chunk(s) in {elapsed:.2f}s")
    print(f"[Lift] Instruction coverage: {lifter.lifted_insns:,} / {lifter.total_insns:,} ({pct:.2f}%), switch tables: {lifter.switch_tables:,}")
    if lifter.unhandled:
        top_u = ", ".join(f"{k}:{v}" for k, v in lifter.unhandled.most_common(12))
        print(f"[Lift] Top remaining TODO mnemonics: {top_u}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
