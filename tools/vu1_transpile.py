#!/usr/bin/env python3
"""Generic PS2 VU1 microcode -> C++ static transpiler (Stage 4).

Translates 16 KiB VU1 microcode binaries into native C++ functions dispatched by
exact 64-bit FNV-1a hash of the microcode image. Emitted code preserves VU1
denormal/Inf normalization, raw MIN/MAX semantics, 4-cycle CLIP pipeline delay,
same-pair upper/lower VF shadowing, 1-cycle VI branch delay, FE_TOWARDZERO, and
XGKICK GIF packet emission. Unknown microcode images fall back to VU1Interpreter.
"""
import argparse
import struct
import sys
from pathlib import Path


def fnv1a_64(data: bytes) -> int:
    h = 14695981039346656037
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def s11(instr: int) -> int:
    v = instr & 0x7FF
    return v - 0x800 if (v & 0x400) else v


def s5(v: int) -> int:
    v &= 0x1F
    return v - 32 if (v & 0x10) else v


def u_dest(instr: int) -> int:
    return (instr >> 21) & 0xF


def u_ft(instr: int) -> int:
    return (instr >> 16) & 0x1F


def u_fs(instr: int) -> int:
    return (instr >> 11) & 0x1F


def u_fd(instr: int) -> int:
    return (instr >> 6) & 0x1F


def emit_dest(dst_expr: str, vals: list[str], dest: int) -> str:
    if dest == 0:
        return ""
    lines = []
    for c in range(4):
        if dest & (8 >> c):
            lines.append(f"{dst_expr}[{c}] = {vals[c]};")
    return " ".join(lines)


def upper_written_vf(hi: int) -> int:
    op = hi & 0x3F
    if op < 0x3C:
        return u_fd(hi)
    sp = (hi & 3) | ((hi >> 4) & 0x7C)
    if sp in (0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x1D):
        return u_ft(hi)
    return 0


def lower_reads_vf(lo: int, hi: int, reg: int) -> bool:
    if reg == 0 or (hi & 0x80000000):
        return False
    if lo == 0 or lo == 0x8000033C:
        return False
    top = (lo >> 25) & 0x7F
    if top == 0x01:  # SQ
        return u_fs(lo) == reg
    if top == 0x40:
        funct = lo & 0x3F
        if funct >= 0x3C:
            f2 = (lo & 3) | ((lo >> 4) & 0x7C)
            if f2 in (0x30, 0x31, 0x35, 0x37, 0x3C, 0x42, 0x43, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7C, 0x7D):
                return u_fs(lo) == reg
            if f2 in (0x38, 0x3A):  # DIV, RSQRT
                return u_fs(lo) == reg or u_ft(lo) == reg
            if f2 == 0x39:  # SQRT
                return u_ft(lo) == reg
    return False


def lower_writes_vi_lsu(lo: int, hi: int) -> int:
    """Return VI register written by 4-cycle LSU load (ILW/ILWR), else 0."""
    if (hi & 0x80000000) or lo == 0 or lo == 0x8000033C:
        return 0
    top = (lo >> 25) & 0x7F
    if top == 0x04:  # ILW
        return (lo >> 16) & 0xF
    if top == 0x40:
        funct = lo & 0x3F
        if funct >= 0x3C:
            f2 = (lo & 3) | ((lo >> 4) & 0x7C)
            if f2 == 0x3E:  # ILWR
                return (lo >> 16) & 0xF
    return 0


def lower_writes_vi_delay(lo: int, hi: int) -> int:
    """Return VI register written by lower op (1..15) if it delays next branch read, else 0.
    Matches VU1Interpreter::decodeLowerUsage (delaysNextBranchRead): only IALU, LQI/LQD, SQI/SQD, MTIR."""
    if (hi & 0x80000000) or lo == 0 or lo == 0x8000033C:
        return 0
    top = (lo >> 25) & 0x7F
    if top in (0x08, 0x09):  # IADDIU, ISUBIU
        return (lo >> 16) & 0xF
    if top == 0x40:
        funct = lo & 0x3F
        if funct in (0x30, 0x31, 0x34, 0x35):  # IADD, ISUB, IAND, IOR
            return (lo >> 6) & 0xF
        if funct == 0x32:  # IADDI
            return (lo >> 16) & 0xF
        if funct >= 0x3C:
            f2 = (lo & 3) | ((lo >> 4) & 0x7C)
            if f2 in (0x34, 0x36):  # LQI, LQD (modifies is)
                return (lo >> 11) & 0xF
            if f2 in (0x35, 0x37):  # SQI, SQD (modifies it)
                return (lo >> 16) & 0xF
            if f2 == 0x3C:  # MTIR
                return (lo >> 16) & 0xF
    return 0


def emit_upper(hi: int, norm_vf: list[int] | None = None) -> str:
    op = hi & 0x3F
    dest = u_dest(hi)
    ft = u_ft(hi)
    fs = u_fs(hi)
    fd = u_fd(hi)

    def nvf(reg: int, lane: int) -> str:
        if reg == 0 or (norm_vf is not None and (norm_vf[reg] & (8 >> lane))):
            return f"vf[{reg}][{lane}]"
        return f"vuNorm(vf[{reg}][{lane}])"

    if op < 0x3C:
        if fd == 0 or dest == 0:
            return ""
        dst = f"vf[{fd}]"
        # Broadcast ops
        if op <= 0x03:  # ADDbc
            bc = op & 3
            return f"{{ float b={nvf(ft, bc)}; float r[4]; " + \
                   " ".join(f"r[{c}]=vuNormRes({nvf(fs, c)}+b);" for c in range(4) if dest & (8 >> c)) + \
                   " " + emit_dest(dst, [f"r[{c}]" for c in range(4)], dest) + " }"
        if op <= 0x07:  # SUBbc
            bc = op & 3
            return f"{{ float b={nvf(ft, bc)}; float r[4]; " + \
                   " ".join(f"r[{c}]=vuNormRes({nvf(fs, c)}-b);" for c in range(4) if dest & (8 >> c)) + \
                   " " + emit_dest(dst, [f"r[{c}]" for c in range(4)], dest) + " }"
        if op <= 0x0B:  # MADDbc
            bc = op & 3
            return f"{{ float b={nvf(ft, bc)}; float r[4]; " + \
                   " ".join(f"r[{c}]=vuMaddN(acc[{c}],{nvf(fs, c)},b);" for c in range(4) if dest & (8 >> c)) + \
                   " " + emit_dest(dst, [f"r[{c}]" for c in range(4)], dest) + " }"
        if op <= 0x0F:  # MSUBbc
            bc = op & 3
            return f"{{ float b={nvf(ft, bc)}; float r[4]; " + \
                   " ".join(f"r[{c}]=vuMsubN(acc[{c}],{nvf(fs, c)},b);" for c in range(4) if dest & (8 >> c)) + \
                   " " + emit_dest(dst, [f"r[{c}]" for c in range(4)], dest) + " }"
        if op <= 0x13:  # MAXbc
            bc = op & 3
            return f"{{ float b=vf[{ft}][{bc}]; float r[4]; " + \
                   " ".join(f"r[{c}]=vuMinMax(vf[{fs}][{c}],b,true);" for c in range(4) if dest & (8 >> c)) + \
                   " " + emit_dest(dst, [f"r[{c}]" for c in range(4)], dest) + " }"
        if op <= 0x17:  # MINIbc
            bc = op & 3
            return f"{{ float b=vf[{ft}][{bc}]; float r[4]; " + \
                   " ".join(f"r[{c}]=vuMinMax(vf[{fs}][{c}],b,false);" for c in range(4) if dest & (8 >> c)) + \
                   " " + emit_dest(dst, [f"r[{c}]" for c in range(4)], dest) + " }"
        if op <= 0x1B:  # MULbc
            bc = op & 3
            return f"{{ float b={nvf(ft, bc)}; float r[4]; " + \
                   " ".join(f"r[{c}]=vuNormRes({nvf(fs, c)}*b);" for c in range(4) if dest & (8 >> c)) + \
                   " " + emit_dest(dst, [f"r[{c}]" for c in range(4)], dest) + " }"
        if op == 0x1C:  # MULq
            return f"{{ float b=vuNorm(q); float r[4]; " + \
                   " ".join(f"r[{c}]=vuNormRes({nvf(fs, c)}*b);" for c in range(4) if dest & (8 >> c)) + \
                   " " + emit_dest(dst, [f"r[{c}]" for c in range(4)], dest) + " }"
        if op == 0x1D:  # MAXi
            return f"{{ float r[4]; " + \
                   " ".join(f"r[{c}]=vuMinMax(vf[{fs}][{c}],regI,true);" for c in range(4) if dest & (8 >> c)) + \
                   " " + emit_dest(dst, [f"r[{c}]" for c in range(4)], dest) + " }"
        if op == 0x1E:  # MULi
            return f"{{ float b=vuNorm(regI); float r[4]; " + \
                   " ".join(f"r[{c}]=vuNormRes({nvf(fs, c)}*b);" for c in range(4) if dest & (8 >> c)) + \
                   " " + emit_dest(dst, [f"r[{c}]" for c in range(4)], dest) + " }"
        if op == 0x1F:  # MINIi
            return f"{{ float r[4]; " + \
                   " ".join(f"r[{c}]=vuMinMax(vf[{fs}][{c}],regI,false);" for c in range(4) if dest & (8 >> c)) + \
                   " " + emit_dest(dst, [f"r[{c}]" for c in range(4)], dest) + " }"
        if op == 0x20:  # ADDq
            return f"{{ float b=vuNorm(q); float r[4]; " + \
                   " ".join(f"r[{c}]=vuNormRes({nvf(fs, c)}+b);" for c in range(4) if dest & (8 >> c)) + \
                   " " + emit_dest(dst, [f"r[{c}]" for c in range(4)], dest) + " }"
        if op == 0x21:  # MADDq
            return f"{{ float b=vuNorm(q); float r[4]; " + \
                   " ".join(f"r[{c}]=vuMaddN(acc[{c}],{nvf(fs, c)},b);" for c in range(4) if dest & (8 >> c)) + \
                   " " + emit_dest(dst, [f"r[{c}]" for c in range(4)], dest) + " }"
        if op == 0x22:  # ADDi
            return f"{{ float b=vuNorm(regI); float r[4]; " + \
                   " ".join(f"r[{c}]=vuNormRes({nvf(fs, c)}+b);" for c in range(4) if dest & (8 >> c)) + \
                   " " + emit_dest(dst, [f"r[{c}]" for c in range(4)], dest) + " }"
        if op == 0x23:  # MADDi
            return f"{{ float b=vuNorm(regI); float r[4]; " + \
                   " ".join(f"r[{c}]=vuMaddN(acc[{c}],{nvf(fs, c)},b);" for c in range(4) if dest & (8 >> c)) + \
                   " " + emit_dest(dst, [f"r[{c}]" for c in range(4)], dest) + " }"
        if op == 0x24:  # SUBq
            return f"{{ float b=vuNorm(q); float r[4]; " + \
                   " ".join(f"r[{c}]=vuNormRes({nvf(fs, c)}-b);" for c in range(4) if dest & (8 >> c)) + \
                   " " + emit_dest(dst, [f"r[{c}]" for c in range(4)], dest) + " }"
        if op == 0x25:  # MSUBq
            return f"{{ float b=vuNorm(q); float r[4]; " + \
                   " ".join(f"r[{c}]=vuMsubN(acc[{c}],{nvf(fs, c)},b);" for c in range(4) if dest & (8 >> c)) + \
                   " " + emit_dest(dst, [f"r[{c}]" for c in range(4)], dest) + " }"
        if op == 0x26:  # SUBi
            return f"{{ float b=vuNorm(regI); float r[4]; " + \
                   " ".join(f"r[{c}]=vuNormRes({nvf(fs, c)}-b);" for c in range(4) if dest & (8 >> c)) + \
                   " " + emit_dest(dst, [f"r[{c}]" for c in range(4)], dest) + " }"
        if op == 0x27:  # MSUBi
            return f"{{ float b=vuNorm(regI); float r[4]; " + \
                   " ".join(f"r[{c}]=vuMsubN(acc[{c}],{nvf(fs, c)},b);" for c in range(4) if dest & (8 >> c)) + \
                   " " + emit_dest(dst, [f"r[{c}]" for c in range(4)], dest) + " }"
        if op == 0x28:  # ADD
            return f"{{ float r[4]; " + \
                   " ".join(f"r[{c}]=vuNormRes({nvf(fs, c)}+{nvf(ft, c)});" for c in range(4) if dest & (8 >> c)) + \
                   " " + emit_dest(dst, [f"r[{c}]" for c in range(4)], dest) + " }"
        if op == 0x29:  # MADD
            return f"{{ float r[4]; " + \
                   " ".join(f"r[{c}]=vuMaddN(acc[{c}],{nvf(fs, c)},{nvf(ft, c)});" for c in range(4) if dest & (8 >> c)) + \
                   " " + emit_dest(dst, [f"r[{c}]" for c in range(4)], dest) + " }"
        if op == 0x2A:  # MUL
            return f"{{ float r[4]; " + \
                   " ".join(f"r[{c}]=vuNormRes({nvf(fs, c)}*{nvf(ft, c)});" for c in range(4) if dest & (8 >> c)) + \
                   " " + emit_dest(dst, [f"r[{c}]" for c in range(4)], dest) + " }"
        if op == 0x2B:  # MAX
            return f"{{ float r[4]; " + \
                   " ".join(f"r[{c}]=vuMinMax(vf[{fs}][{c}],vf[{ft}][{c}],true);" for c in range(4) if dest & (8 >> c)) + \
                   " " + emit_dest(dst, [f"r[{c}]" for c in range(4)], dest) + " }"
        if op == 0x2C:  # SUB
            return f"{{ float r[4]; " + \
                   " ".join(f"r[{c}]=vuNormRes({nvf(fs, c)}-{nvf(ft, c)});" for c in range(4) if dest & (8 >> c)) + \
                   " " + emit_dest(dst, [f"r[{c}]" for c in range(4)], dest) + " }"
        if op == 0x2D:  # MSUB
            return f"{{ float r[4]; " + \
                   " ".join(f"r[{c}]=vuMsubN(acc[{c}],{nvf(fs, c)},{nvf(ft, c)});" for c in range(4) if dest & (8 >> c)) + \
                   " " + emit_dest(dst, [f"r[{c}]" for c in range(4)], dest) + " }"
        if op == 0x2E:  # OPMSUB
            return f"{{ float r[4]={{vuMsubN(acc[0],{nvf(fs, 1)},{nvf(ft, 2)}), vuMsubN(acc[1],{nvf(fs, 2)},{nvf(ft, 0)}), vuMsubN(acc[2],{nvf(fs, 0)},{nvf(ft, 1)}), 0.0f}}; " + \
                   emit_dest(dst, ["r[0]", "r[1]", "r[2]", "r[3]"], dest) + " }"
        if op == 0x2F:  # MINI
            return f"{{ float r[4]; " + \
                   " ".join(f"r[{c}]=vuMinMax(vf[{fs}][{c}],vf[{ft}][{c}],false);" for c in range(4) if dest & (8 >> c)) + \
                   " " + emit_dest(dst, [f"r[{c}]" for c in range(4)], dest) + " }"
        raise ValueError(f"Unsupported upper op 0x{op:02x} (instr=0x{hi:08x})")

    sp = (hi & 3) | ((hi >> 4) & 0x7C)
    if sp == 0x2F:  # NOP
        return ""
    if sp == 0x1F:  # CLIP
        return f"vuQueueClip(clipPipe, workingClip, vuClip6(vf[{fs}], vf[{ft}][3]));"
    if sp in (0x10, 0x11, 0x12, 0x13):  # ITOF0/4/12/15
        if ft == 0 or dest == 0:
            return ""
        divs = {0x10: "1.0f", 0x11: "16.0f", 0x12: "4096.0f", 0x13: "32768.0f"}[sp]
        return f"{{ float r[4]; " + \
               " ".join(f"{{ int32_t iv; std::memcpy(&iv,&vf[{fs}][{c}],4); r[{c}]=static_cast<float>(iv)/{divs}; }}" for c in range(4) if dest & (8 >> c)) + \
               " " + emit_dest(f"vf[{ft}]", [f"r[{c}]" for c in range(4)], dest) + " }"
    if sp in (0x14, 0x15, 0x16, 0x17):  # FTOI0/4/12/15
        if ft == 0 or dest == 0:
            return ""
        scales = {0x14: "1.0f", 0x15: "16.0f", 0x16: "4096.0f", 0x17: "32768.0f"}[sp]
        return f"{{ float r[4]; " + \
               " ".join(f"{{ int32_t iv=vuFloatToInt({nvf(fs, c)},{scales}); std::memcpy(&r[{c}],&iv,4); }}" for c in range(4) if dest & (8 >> c)) + \
               " " + emit_dest(f"vf[{ft}]", [f"r[{c}]" for c in range(4)], dest) + " }"
    if sp == 0x1D:  # ABS
        if ft == 0 or dest == 0:
            return ""
        return f"{{ float r[4]; " + \
               " ".join(f"r[{c}]=std::fabs({nvf(fs, c)});" for c in range(4) if dest & (8 >> c)) + \
               " " + emit_dest(f"vf[{ft}]", [f"r[{c}]" for c in range(4)], dest) + " }"
    if dest == 0:
        return ""
    if sp <= 0x03:  # ADDAbc
        bc = sp & 3
        return f"{{ float b={nvf(ft, bc)}; float r[4]; " + \
               " ".join(f"r[{c}]=vuNormRes({nvf(fs, c)}+b);" for c in range(4) if dest & (8 >> c)) + \
               " " + emit_dest("acc", [f"r[{c}]" for c in range(4)], dest) + " }"
    if sp <= 0x07:  # SUBAbc
        bc = sp & 3
        return f"{{ float b={nvf(ft, bc)}; float r[4]; " + \
               " ".join(f"r[{c}]=vuNormRes({nvf(fs, c)}-b);" for c in range(4) if dest & (8 >> c)) + \
               " " + emit_dest("acc", [f"r[{c}]" for c in range(4)], dest) + " }"
    if sp <= 0x0B:  # MADDAbc
        bc = sp & 3
        return f"{{ float b={nvf(ft, bc)}; float r[4]; " + \
               " ".join(f"r[{c}]=vuMaddN(acc[{c}],{nvf(fs, c)},b);" for c in range(4) if dest & (8 >> c)) + \
               " " + emit_dest("acc", [f"r[{c}]" for c in range(4)], dest) + " }"
    if sp <= 0x0F:  # MSUBAbc
        bc = sp & 3
        return f"{{ float b={nvf(ft, bc)}; float r[4]; " + \
               " ".join(f"r[{c}]=vuMsubN(acc[{c}],{nvf(fs, c)},b);" for c in range(4) if dest & (8 >> c)) + \
               " " + emit_dest("acc", [f"r[{c}]" for c in range(4)], dest) + " }"
    if 0x18 <= sp <= 0x1B:  # MULAbc
        bc = sp & 3
        return f"{{ float b={nvf(ft, bc)}; float r[4]; " + \
               " ".join(f"r[{c}]=vuNormRes({nvf(fs, c)}*b);" for c in range(4) if dest & (8 >> c)) + \
               " " + emit_dest("acc", [f"r[{c}]" for c in range(4)], dest) + " }"
    if sp == 0x1C:  # MULAq
        return f"{{ float b=vuNorm(q); float r[4]; " + \
               " ".join(f"r[{c}]=vuNormRes({nvf(fs, c)}*b);" for c in range(4) if dest & (8 >> c)) + \
               " " + emit_dest("acc", [f"r[{c}]" for c in range(4)], dest) + " }"
    if sp == 0x1E:  # MULAi
        return f"{{ float b=vuNorm(regI); float r[4]; " + \
               " ".join(f"r[{c}]=vuNormRes({nvf(fs, c)}*b);" for c in range(4) if dest & (8 >> c)) + \
               " " + emit_dest("acc", [f"r[{c}]" for c in range(4)], dest) + " }"
    if sp == 0x20:  # ADDAq
        return f"{{ float b=vuNorm(q); float r[4]; " + \
               " ".join(f"r[{c}]=vuNormRes({nvf(fs, c)}+b);" for c in range(4) if dest & (8 >> c)) + \
               " " + emit_dest("acc", [f"r[{c}]" for c in range(4)], dest) + " }"
    if sp == 0x21:  # MADDAq
        return f"{{ float b=vuNorm(q); float r[4]; " + \
               " ".join(f"r[{c}]=vuMaddN(acc[{c}],{nvf(fs, c)},b);" for c in range(4) if dest & (8 >> c)) + \
               " " + emit_dest("acc", [f"r[{c}]" for c in range(4)], dest) + " }"
    if sp == 0x22:  # ADDAi
        return f"{{ float b=vuNorm(regI); float r[4]; " + \
               " ".join(f"r[{c}]=vuNormRes({nvf(fs, c)}+b);" for c in range(4) if dest & (8 >> c)) + \
               " " + emit_dest("acc", [f"r[{c}]" for c in range(4)], dest) + " }"
    if sp == 0x23:  # MADDAi
        return f"{{ float b=vuNorm(regI); float r[4]; " + \
               " ".join(f"r[{c}]=vuMaddN(acc[{c}],{nvf(fs, c)},b);" for c in range(4) if dest & (8 >> c)) + \
               " " + emit_dest("acc", [f"r[{c}]" for c in range(4)], dest) + " }"
    if sp == 0x24:  # SUBAq
        return f"{{ float b=vuNorm(q); float r[4]; " + \
               " ".join(f"r[{c}]=vuNormRes({nvf(fs, c)}-b);" for c in range(4) if dest & (8 >> c)) + \
               " " + emit_dest("acc", [f"r[{c}]" for c in range(4)], dest) + " }"
    if sp == 0x25:  # MSUBAq
        return f"{{ float b=vuNorm(q); float r[4]; " + \
               " ".join(f"r[{c}]=vuMsubN(acc[{c}],{nvf(fs, c)},b);" for c in range(4) if dest & (8 >> c)) + \
               " " + emit_dest("acc", [f"r[{c}]" for c in range(4)], dest) + " }"
    if sp == 0x26:  # SUBAi
        return f"{{ float b=vuNorm(regI); float r[4]; " + \
               " ".join(f"r[{c}]=vuNormRes({nvf(fs, c)}-b);" for c in range(4) if dest & (8 >> c)) + \
               " " + emit_dest("acc", [f"r[{c}]" for c in range(4)], dest) + " }"
    if sp == 0x27:  # MSUBAi
        return f"{{ float b=vuNorm(regI); float r[4]; " + \
               " ".join(f"r[{c}]=vuMsubN(acc[{c}],{nvf(fs, c)},b);" for c in range(4) if dest & (8 >> c)) + \
               " " + emit_dest("acc", [f"r[{c}]" for c in range(4)], dest) + " }"
    if sp == 0x28:  # ADDA
        return f"{{ float r[4]; " + \
               " ".join(f"r[{c}]=vuNormRes({nvf(fs, c)}+{nvf(ft, c)});" for c in range(4) if dest & (8 >> c)) + \
               " " + emit_dest("acc", [f"r[{c}]" for c in range(4)], dest) + " }"
    if sp == 0x29:  # MADDA
        return f"{{ float r[4]; " + \
               " ".join(f"r[{c}]=vuMaddN(acc[{c}],{nvf(fs, c)},{nvf(ft, c)});" for c in range(4) if dest & (8 >> c)) + \
               " " + emit_dest("acc", [f"r[{c}]" for c in range(4)], dest) + " }"
    if sp == 0x2A:  # MULA
        return f"{{ float r[4]; " + \
               " ".join(f"r[{c}]=vuNormRes({nvf(fs, c)}*{nvf(ft, c)});" for c in range(4) if dest & (8 >> c)) + \
               " " + emit_dest("acc", [f"r[{c}]" for c in range(4)], dest) + " }"
    if sp == 0x2C:  # SUBA
        return f"{{ float r[4]; " + \
               " ".join(f"r[{c}]=vuNormRes({nvf(fs, c)}-{nvf(ft, c)});" for c in range(4) if dest & (8 >> c)) + \
               " " + emit_dest("acc", [f"r[{c}]" for c in range(4)], dest) + " }"
    if sp == 0x2D:  # MSUBA
        return f"{{ float r[4]; " + \
               " ".join(f"r[{c}]=vuMsubN(acc[{c}],{nvf(fs, c)},{nvf(ft, c)});" for c in range(4) if dest & (8 >> c)) + \
               " " + emit_dest("acc", [f"r[{c}]" for c in range(4)], dest) + " }"
    if sp == 0x2E:  # OPMULA
        return f"{{ float r[4]={{vuNormRes(vuNorm(vf[{fs}][1])*vuNorm(vf[{ft}][2])), vuNormRes(vuNorm(vf[{fs}][2])*vuNorm(vf[{ft}][0])), vuNormRes(vuNorm(vf[{fs}][0])*vuNorm(vf[{ft}][1])), 0.0f}}; " + \
               emit_dest("acc", ["r[0]", "r[1]", "r[2]", "r[3]"], dest) + " }"
    raise ValueError(f"Unsupported upper special 0x{sp:02x} (instr=0x{hi:08x})")


def emit_lower_nonbranch(lo: int) -> str:
    if lo == 0 or lo == 0x8000033C:
        return ""
    top = (lo >> 25) & 0x7F
    dest = u_dest(lo)
    ft = u_ft(lo)
    fs = u_fs(lo)
    vit = (lo >> 16) & 0xF
    vis = (lo >> 11) & 0xF
    vid = (lo >> 6) & 0xF

    if top == 0x00:  # LQ
        if ft == 0 or dest == 0:
            return ""
        imm = s11(lo)
        if dest == 0xF:
            return f"{{ uint32_t a=(((uint32_t)(int32_t)(vi[{vis}]+({imm})))*16u)&dataMask; std::memcpy(vf[{ft}],vuData+a,16); }}"
        return f"{{ uint32_t a=(((uint32_t)(int32_t)(vi[{vis}]+({imm})))*16u)&dataMask; float t[4]; std::memcpy(t,vuData+a,16); " + \
               emit_dest(f"vf[{ft}]", ["t[0]", "t[1]", "t[2]", "t[3]"], dest) + " }"
    if top == 0x01:  # SQ
        if dest == 0:
            return ""
        imm = s11(lo)
        if dest == 0xF:
            return f"{{ uint32_t a=(((uint32_t)(int32_t)(vi[{vit}]+({imm})))*16u)&dataMask; std::memcpy(vuData+a,vf[{fs}],16); }}"
        return f"{{ uint32_t a=(((uint32_t)(int32_t)(vi[{vit}]+({imm})))*16u)&dataMask; float t[4]; std::memcpy(t,vuData+a,16); " + \
               emit_dest("t", [f"vf[{fs}][{c}]" for c in range(4)], dest) + " std::memcpy(vuData+a,t,16); }"
    if top == 0x04:  # ILW
        if vit == 0:
            return ""
        imm = s11(lo)
        comp = 0 if (dest & 8) else (1 if (dest & 4) else (2 if (dest & 2) else 3))
        return f"{{ uint32_t a=(((uint32_t)(int32_t)(vi[{vis}]+({imm})))*16u)&dataMask; uint32_t v; std::memcpy(&v,vuData+a+{comp*4},4); vi[{vit}]=(int32_t)(int16_t)(v&0xFFFFu); }}"
    if top == 0x05:  # ISW
        if dest == 0:
            return ""
        imm = s11(lo)
        return f"{{ uint32_t a=(((uint32_t)(int32_t)(vi[{vis}]+({imm})))*16u)&dataMask; uint32_t v=(uint32_t)(uint16_t)(vi[{vit}]&0xFFFF); uint32_t w[4]; std::memcpy(w,vuData+a,16); " + \
               " ".join(f"w[{c}]=v;" for c in range(4) if dest & (8 >> c)) + " std::memcpy(vuData+a,w,16); }"
    if top == 0x08:  # IADDIU
        if vit == 0:
            return ""
        imm = ((lo & 0x7FF) | ((lo >> 10) & 0x7800)) & 0xFFFF
        if imm & 0x8000:
            imm -= 0x10000
        return f"vi[{vit}] = (int16_t)(vi[{vis}] + ({imm}));"
    if top == 0x09:  # ISUBIU
        if vit == 0:
            return ""
        imm = ((lo & 0x7FF) | ((lo >> 10) & 0x7800)) & 0xFFFF
        if imm & 0x8000:
            imm -= 0x10000
        return f"vi[{vit}] = (int16_t)(vi[{vis}] - ({imm}));"
    if top == 0x10:  # FCEQ
        return f"vi[1] = ((clip & 0xFFFFFFu) == 0x{lo & 0xFFFFFF:x}u) ? 1 : 0;"
    if top == 0x11:  # FCSET
        return f"vuQueueFcset(clipPipe, workingClip, 0x{lo & 0xFFFFFF:x}u);"
    if top == 0x12:  # FCAND
        return f"vi[1] = ((clip & 0x{lo & 0xFFFFFF:x}u) != 0u) ? 1 : 0;"
    if top == 0x13:  # FCOR
        return f"vi[1] = ((clip | 0x{lo & 0xFFFFFF:x}u) == 0xFFFFFFu) ? 1 : 0;"
    if top == 0x1C:  # FCGET
        return "" if vit == 0 else f"vi[{vit}] = (int32_t)(clip & 0x0FFFu);"
    if top == 0x40:
        funct = lo & 0x3F
        if funct == 0x30:  # IADD
            return "" if vid == 0 else f"vi[{vid}] = (int16_t)(vi[{vis}] + vi[{vit}]);"
        if funct == 0x31:  # ISUB
            return "" if vid == 0 else f"vi[{vid}] = (int16_t)(vi[{vis}] - vi[{vit}]);"
        if funct == 0x32:  # IADDI
            return "" if vit == 0 else f"vi[{vit}] = (int16_t)(vi[{vis}] + ({s5(lo >> 6)}));"
        if funct == 0x34:  # IAND
            return "" if vid == 0 else f"vi[{vid}] = vi[{vis}] & vi[{vit}];"
        if funct == 0x35:  # IOR
            return "" if vid == 0 else f"vi[{vid}] = vi[{vis}] | vi[{vit}];"
        if funct >= 0x3C:
            f2 = (lo & 3) | ((lo >> 4) & 0x7C)
            if f2 == 0x30:  # MOVE
                if ft == 0 or dest == 0:
                    return ""
                if dest == 0xF:
                    return f"std::memcpy(vf[{ft}],vf[{fs}],16);"
                return f"{{ float t[4]; std::memcpy(t,vf[{fs}],16); " + emit_dest(f"vf[{ft}]", ["t[0]", "t[1]", "t[2]", "t[3]"], dest) + " }"
            if f2 == 0x31:  # MR32
                if ft == 0 or dest == 0:
                    return ""
                return f"{{ float t[4]={{vf[{fs}][1],vf[{fs}][2],vf[{fs}][3],vf[{fs}][0]}}; " + emit_dest(f"vf[{ft}]", ["t[0]", "t[1]", "t[2]", "t[3]"], dest) + " }"
            if f2 == 0x34:  # LQI
                parts = []
                if ft != 0 and dest != 0:
                    if dest == 0xF:
                        parts.append(f"{{ uint32_t a=((uint32_t)(uint16_t)vi[{vis}]*16u)&dataMask; std::memcpy(vf[{ft}],vuData+a,16); }}")
                    else:
                        parts.append(f"{{ uint32_t a=((uint32_t)(uint16_t)vi[{vis}]*16u)&dataMask; float t[4]; std::memcpy(t,vuData+a,16); " + emit_dest(f"vf[{ft}]", ["t[0]", "t[1]", "t[2]", "t[3]"], dest) + " }")
                if vis != 0:
                    parts.append(f"vi[{vis}]=(int16_t)(vi[{vis}]+1);")
                return " ".join(parts)
            if f2 == 0x35:  # SQI
                parts = []
                if dest != 0:
                    if dest == 0xF:
                        parts.append(f"{{ uint32_t a=((uint32_t)(uint16_t)vi[{vit}]*16u)&dataMask; std::memcpy(vuData+a,vf[{fs}],16); }}")
                    else:
                        parts.append(f"{{ uint32_t a=((uint32_t)(uint16_t)vi[{vit}]*16u)&dataMask; float t[4]; std::memcpy(t,vuData+a,16); " + emit_dest("t", [f"vf[{fs}][{c}]" for c in range(4)], dest) + " std::memcpy(vuData+a,t,16); }")
                if vit != 0:
                    parts.append(f"vi[{vit}]=(int16_t)(vi[{vit}]+1);")
                return " ".join(parts)
            if f2 == 0x36:  # LQD
                parts = []
                if vis != 0:
                    parts.append(f"vi[{vis}]=(int16_t)(vi[{vis}]-1);")
                if ft != 0 and dest != 0:
                    parts.append(f"{{ uint32_t a=((uint32_t)(uint16_t)vi[{vis}]*16u)&dataMask; float t[4]; std::memcpy(t,vuData+a,16); " + emit_dest(f"vf[{ft}]", ["t[0]", "t[1]", "t[2]", "t[3]"], dest) + " }")
                return " ".join(parts)
            if f2 == 0x37:  # SQD
                parts = []
                if vit != 0:
                    parts.append(f"vi[{vit}]=(int16_t)(vi[{vit}]-1);")
                if dest != 0:
                    if dest == 0xF:
                        parts.append(f"{{ uint32_t a=((uint32_t)(uint16_t)vi[{vit}]*16u)&dataMask; std::memcpy(vuData+a,vf[{fs}],16); }}")
                    else:
                        parts.append(f"{{ uint32_t a=((uint32_t)(uint16_t)vi[{vit}]*16u)&dataMask; float t[4]; std::memcpy(t,vuData+a,16); " + emit_dest("t", [f"vf[{fs}][{c}]" for c in range(4)], dest) + " std::memcpy(vuData+a,t,16); }")
                return " ".join(parts)
            if f2 == 0x38:  # DIV
                fsf = (lo >> 21) & 3
                ftf = (lo >> 23) & 3
                return f"{{ uint32_t _di=0; float _qv=vuDiv(vf[{fs}][{fsf}], vf[{ft}][{ftf}], _di); vuQueueQ(qPipe, _qv, _di, 7); }}"
            if f2 == 0x39:  # SQRT
                ftf = (lo >> 23) & 3
                return f"{{ uint32_t _di=0; float _qv=vuSqrt(vf[{ft}][{ftf}], _di); vuQueueQ(qPipe, _qv, _di, 7); }}"
            if f2 == 0x3A:  # RSQRT
                fsf = (lo >> 21) & 3
                ftf = (lo >> 23) & 3
                return f"{{ uint32_t _di=0; float _qv=vuRsqrt(vf[{fs}][{fsf}], vf[{ft}][{ftf}], _di); vuQueueQ(qPipe, _qv, _di, 13); }}"
            if f2 == 0x3B:  # WAITQ
                return ""  # Handled before upper op in emit_pair_body (stall before pair issue)
            if f2 == 0x3C:  # MTIR
                if vit == 0:
                    return ""
                comp = (lo >> 21) & 3
                return f"{{ uint32_t fv; std::memcpy(&fv,&vf[{fs}][{comp}],4); vi[{vit}]=(int32_t)(int16_t)(fv&0xFFFFu); }}"
            if f2 == 0x3D:  # MFIR
                if ft == 0 or dest == 0:
                    return ""
                return f"{{ int32_t iv=(int32_t)(int16_t)(vi[{vis}]&0xFFFF); float fv; std::memcpy(&fv,&iv,4); " + emit_dest(f"vf[{ft}]", ["fv", "fv", "fv", "fv"], dest) + " }"
            if f2 == 0x3E:  # ILWR
                if vit == 0:
                    return ""
                comp = 0 if (dest & 8) else (1 if (dest & 4) else (2 if (dest & 2) else 3))
                return f"{{ uint32_t a=((uint32_t)(uint16_t)vi[{vis}]*16u)&dataMask; uint32_t v; std::memcpy(&v,vuData+a+{comp*4},4); vi[{vit}]=(int32_t)(int16_t)(v&0xFFFFu); }}"
            if f2 == 0x3F:  # ISWR
                if dest == 0:
                    return ""
                return f"{{ uint32_t a=((uint32_t)(uint16_t)vi[{vis}]*16u)&dataMask; uint32_t v=(uint32_t)(uint16_t)(vi[{vit}]&0xFFFF); uint32_t w[4]; std::memcpy(w,vuData+a,16); " + \
                       " ".join(f"w[{c}]=v;" for c in range(4) if dest & (8 >> c)) + " std::memcpy(vuData+a,w,16); }"
            if f2 == 0x64:  # MFP
                if ft == 0 or dest == 0:
                    return ""
                return emit_dest(f"vf[{ft}]", ["p", "p", "p", "p"], dest)
            if f2 == 0x68:  # XTOP
                return "" if vit == 0 else f"vi[{vit}] = (int32_t)(st.top & 0x3FFu);"
            if f2 == 0x69:  # XITOP
                return "" if vit == 0 else f"vi[{vit}] = (int32_t)(st.itop & 0x3FFu);"
            if f2 == 0x6C:  # XGKICK
                return f"vuXgkick((uint32_t)(uint16_t)vi[{vis}], vuData, dataSize, gs, memory, outGifHash, outGifBytes, replaySink);"
            if f2 == 0x73:  # ERLENG
                return f"{{ float x=vuNorm(vf[{fs}][0]),y=vuNorm(vf[{fs}][1]),z=vuNorm(vf[{fs}][2]),l=std::sqrt(x*x+y*y+z*z); p=vuNormRes(l!=0.0f?1.0f/l:l); }}"
            if f2 == 0x7B:  # WAITP
                return ""
            raise ValueError(f"Unsupported lower special 0x{f2:02x} (instr=0x{lo:08x})")
    raise ValueError(f"Unsupported lower top 0x{top:02x} (instr=0x{lo:08x})")


def branch_info(lo: int, hi: int, pc: int, code_size: int):
    if (hi & 0x80000000) or lo == 0 or lo == 0x8000033C:
        return None
    top = (lo >> 25) & 0x7F
    if top not in (0x20, 0x21, 0x24, 0x25, 0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F):
        return None
    vit = (lo >> 16) & 0xF
    vis = (lo >> 11) & 0xF
    imm = s11(lo)
    target = (pc + 8 + imm * 8) & 0x3FFF
    return (top, vis, vit, target)


def lower_stalls_on_q(lo: int, hi: int) -> bool:
    if (hi & 0x80000000) or lo == 0 or lo == 0x8000033C:
        return False
    top = (lo >> 25) & 0x7F
    if top == 0x40 and (lo & 0x3F) >= 0x3C:
        f2 = (lo & 3) | ((lo >> 4) & 0x7C)
        return f2 in (0x38, 0x39, 0x3A, 0x3B)  # DIV, SQRT, RSQRT, WAITQ
    return False


def update_norm_vf_upper(hi: int, norm_vf: list[int]) -> None:
    op = hi & 0x3F
    dest = u_dest(hi)
    if dest == 0:
        return
    if op < 0x3C:
        fd = u_fd(hi)
        if fd == 0:
            return
        # MAX / MINI do raw integer-bit comparison without normalizing denormals/Inf.
        if (0x10 <= op <= 0x17) or op in (0x1D, 0x1F, 0x2B, 0x2F):
            norm_vf[fd] &= ~dest
        else:
            norm_vf[fd] |= dest
        return
    sp = (hi & 3) | ((hi >> 4) & 0x7C)
    ft = u_ft(hi)
    if ft == 0:
        return
    if sp in (0x10, 0x11, 0x12, 0x13, 0x1D):  # ITOF0/4/12/15, ABS
        norm_vf[ft] |= dest
    elif sp in (0x14, 0x15, 0x16, 0x17):  # FTOI0/4/12/15
        norm_vf[ft] &= ~dest


def update_norm_vf_lower(lo: int, hi: int, norm_vf: list[int]) -> None:
    if (hi & 0x80000000) or lo == 0 or lo == 0x8000033C:
        return
    top = (lo >> 25) & 0x7F
    dest = (lo >> 21) & 0xF
    ft = (lo >> 16) & 0x1F
    if top == 0x00:  # LQ
        if ft: norm_vf[ft] &= ~dest
        return
    if top == 0x40:
        funct = lo & 0x3F
        if funct >= 0x3C:
            f2 = (lo & 3) | ((lo >> 4) & 0x7C)
            if f2 in (0x30, 0x31, 0x34, 0x35, 0x3D, 0x40, 0x41, 0x42, 0x43):  # MOVE, MR32, LQI, LQD, MFIR, RNEXT, RGET, RINIT, RXOR
                if ft: norm_vf[ft] &= ~dest
            elif f2 in (0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7C, 0x7D):  # EFU/MFP
                if ft: norm_vf[ft] &= ~dest


def emit_pair_body(lo: int, hi: int, is_delay_slot: bool = False, norm_vf: list[int] | None = None, next_needs_bvi: bool = True) -> str:
    """Emit execution of one non-branch (or delay-slot) VU1 pair, updating clipPipe and bVi."""
    u_code = emit_upper(hi, norm_vf)
    i_bit = bool(hi & 0x80000000)
    l_code = ""
    if i_bit:
        l_code = f"{{ uint32_t ib=0x{lo:08x}u; std::memcpy(&regI,&ib,4); }}"
    else:
        l_code = emit_lower_nonbranch(lo)

    u_wr = upper_written_vf(hi)
    shadow = u_wr != 0 and lower_reads_vf(lo, hi, u_wr)
    vi_w = lower_writes_vi_delay(lo, hi)
    vi_lsu = lower_writes_vi_lsu(lo, hi)

    parts = []
    if lower_stalls_on_q(lo, hi):
        parts.append("if (qPipe.delay) { bViReg = 0; cyc += qPipe.delay; } vuWaitQ(qPipe, clipPipe, clip, q, status);")
    if vi_w != 0 and next_needs_bvi:
        parts.append(f"int32_t _oldVi = vi[{vi_w}];")
    if shadow:
        parts.append(f"float _sh[4]; std::memcpy(_sh, vf[{u_wr}], 16);")
        if u_code:
            parts.append(u_code)
        parts.append(f"float _up[4]; std::memcpy(_up, vf[{u_wr}], 16); std::memcpy(vf[{u_wr}], _sh, 16);")
        if l_code:
            parts.append(l_code)
        parts.append(f"std::memcpy(vf[{u_wr}], _up, 16);")
    else:
        if u_code:
            parts.append(u_code)
        if l_code:
            parts.append(l_code)

    if norm_vf is not None:
        update_norm_vf_upper(hi, norm_vf)
        update_norm_vf_lower(lo, hi, norm_vf)
        if shadow:
            # Upper write was restored last over lower write
            update_norm_vf_upper(hi, norm_vf)

    if vi_lsu != 0:
        parts.append(f"viReady[{vi_lsu}] = cyc + 4u;")
    if next_needs_bvi:
        if vi_w != 0:
            parts.append(f"bViReg = {vi_w}; bViVal = _oldVi;")
        else:
            parts.append("bViReg = 0;")
    parts.append("++cyc; vuTickPipes(clipPipe, clip, qPipe, q, status);")
    return " ".join(parts)


def collect_leaders(pairs: list[tuple[int, int]], code_len: int) -> set[int]:
    """Identify all basic-block leaders (entry points, branch targets, BAL/JR continuations,
    post-zero starts, and alternate shader entry points following short preambles)."""
    leaders: set[int] = {0x0000, 0x0050}
    after_uncond: set[int] = set()
    n = len(pairs)
    for idx, (lo, hi) in enumerate(pairs):
        if lo == 0 and hi == 0:
            continue
        pc = idx * 8
        if hi & 0x40000000:
            nxt = (pc + 16) & 0x3FFF
            leaders.add(nxt)
            after_uncond.add(nxt)
        if idx > 0 and pairs[idx - 1] == (0, 0):
            leaders.add(pc)
        binfo = branch_info(lo, hi, pc, code_len)
        if binfo is not None:
            top, _, _, target = binfo
            if target is not None:
                leaders.add(target)
            if top in (0x20, 0x21, 0x24, 0x25):  # B, BAL, JR, JALR
                nxt = (pc + 16) & 0x3FFF
                leaders.add(nxt)
                after_uncond.add(nxt)
    # Multi-entry VU1 shaders often have a 2-4 instruction preamble after B/JR/E-bit
    # followed by an alternate entry starting with upper NOP (0x000002FF).
    for base in list(after_uncond):
        for delta in (8, 16, 24, 32):
            p = (base + delta) & 0x3FFF
            idx = p // 8
            if idx < n and pairs[idx][1] == 0x000002FF:
                leaders.add(p)
    return {pc for pc in leaders if (pc // 8) < n and pairs[pc // 8] != (0, 0)}


def transpile_program(code: bytes, h: int) -> str:
    pairs = [struct.unpack_from("<II", code, i) for i in range(0, len(code), 8)]
    n = len(pairs)
    leaders = collect_leaders(pairs, len(code))
    is_branch_at = [branch_info(pairs[i][0], pairs[i][1], i * 8, len(code)) is not None for i in range(n)]
    out = []
    out.append(f"static bool vu1_prog_{h:016x}(VU1State &st, uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory, uint64_t *outGifHash, uint64_t *outGifBytes, bool replaySink)")
    out.append("{")
    out.append("    float vf[32][4]; std::memcpy(vf, st.vf, sizeof(vf));")
    out.append("    vf[0][0]=0.0f; vf[0][1]=0.0f; vf[0][2]=0.0f; vf[0][3]=1.0f;")
    out.append("    float acc[4]; for (int _c = 0; _c < 4; ++_c) acc[_c] = vuNorm(st.acc[_c]);")
    out.append("    int32_t vi[16]; std::memcpy(vi, st.vi, sizeof(vi)); vi[0]=0;")
    out.append("    float q = st.q, p = st.p, regI = st.i;")
    out.append("    uint32_t clip = st.clip, workingClip = st.clip, status = st.status;")
    out.append("    VuClipPipe clipPipe{}; VuQPipe qPipe{};")
    out.append("    uint8_t bViReg = 0; int32_t bViVal = 0;")
    out.append("    uint32_t cyc = 0; uint32_t viReady[16] = {};")
    out.append("    const uint32_t dataMask = dataSize - 1u;")
    out.append("    uint32_t pc = st.pc & 0x3FFFu;")
    out.append("    auto readBVi = [&](uint8_t r) DRA2_ALWAYS_INLINE_ATTR -> int16_t { return (int16_t)((r != 0 && bViReg == r) ? bViVal : vi[r]); };")
    out.append("    auto stallVi = [&](uint8_t r1, uint8_t r2) DRA2_ALWAYS_INLINE_ATTR {")
    out.append("        uint32_t tgt = cyc;")
    out.append("        if (r1 != 0 && viReady[r1] > tgt) tgt = viReady[r1];")
    out.append("        if (r2 != 0 && viReady[r2] > tgt) tgt = viReady[r2];")
    out.append("        if (__builtin_expect(tgt > cyc, 0)) { bViReg = 0; while (cyc < tgt) { ++cyc; vuTickPipes(clipPipe, clip, qPipe, q, status); } }")
    out.append("    };")
    out.append("    goto dispatch;")
    out.append("dispatch:")
    out.append("    switch (pc) {")
    for pc in sorted(leaders):
        out.append(f"    case 0x{pc:04x}u: goto L_{pc:04x};")
    out.append("    default: return false;")
    out.append("    }")

    norm_vf = [0xF] + [0] * 31
    for idx in range(n):
        lo, hi = pairs[idx]
        if lo == 0 and hi == 0:
            norm_vf = [0xF] + [0] * 31
            continue
        pc = idx * 8
        if pc in leaders:
            norm_vf = [0xF] + [0] * 31
            out.append(f"L_{pc:04x}:")
        out.append("{")
        e_bit = bool(hi & 0x40000000)
        binfo = branch_info(lo, hi, pc, len(code))
        if binfo is not None:
            top, vis, vit, target = binfo
            u_code = emit_upper(hi, norm_vf)
            cond = None
            indirect = False
            link_reg = 0
            if top == 0x20:  # B
                cond = "true"
            elif top == 0x21:  # BAL
                cond = "true"; link_reg = vit
            elif top == 0x24:  # JR
                cond = "true"; indirect = True
            elif top == 0x25:  # JALR
                cond = "true"; indirect = True; link_reg = vit
            elif top == 0x28:  # IBEQ
                cond = f"(readBVi({vis}) == readBVi({vit}))"
            elif top == 0x29:  # IBNE
                cond = f"(readBVi({vis}) != readBVi({vit}))"
            elif top == 0x2C:  # IBLTZ
                cond = f"(readBVi({vis}) < 0)"
            elif top == 0x2D:  # IBGTZ
                cond = f"(readBVi({vis}) > 0)"
            elif top == 0x2E:  # IBLEZ
                cond = f"(readBVi({vis}) <= 0)"
            elif top == 0x2F:  # IBGEZ
                cond = f"(readBVi({vis}) >= 0)"

            if top in (0x24, 0x25, 0x2C, 0x2D, 0x2E, 0x2F):
                out.append(f"    stallVi({vis}, 0);")
            elif top in (0x28, 0x29):
                out.append(f"    stallVi({vis}, {vit});")
            if indirect:
                out.append(f"    uint32_t _tgt = ((uint32_t)(uint16_t)readBVi({vis}) * 8u) & 0x3FFFu;")
            out.append(f"    bool _taken = {cond};")
            if u_code:
                out.append(f"    {u_code}")
            update_norm_vf_upper(hi, norm_vf)
            if link_reg != 0:
                out.append(f"    vi[{link_reg}] = (int32_t)((0x{pc:04x}u + 16u) / 8u);")
            out.append("    bViReg = 0; ++cyc; vuTickPipes(clipPipe, clip, qPipe, q, status);")
            if idx + 1 < n:
                d_lo, d_hi = pairs[idx + 1]
                d_ebit = bool(d_hi & 0x40000000)
                out.append(f"    if (_taken) {{")
                out.append(f"        {emit_pair_body(d_lo, d_hi, True, list(norm_vf), True)}")
                if e_bit:
                    out.append(f"        pc = 0x{(pc+16)&0x3FFF:04x}u; goto done;")
                elif d_ebit:
                    out.append(f"        pc = {'_tgt' if indirect else f'0x{target:04x}u'}; goto done_after_delay;")
                elif indirect:
                    out.append(f"        pc = _tgt; goto dispatch;")
                else:
                    out.append(f"        goto L_{target:04x};")
                out.append("    }")
        else:
            next_needs_bvi = (idx + 1 < n) and is_branch_at[idx + 1]
            out.append(f"    {emit_pair_body(lo, hi, False, norm_vf, next_needs_bvi)}")
            if e_bit:
                if idx + 1 < n:
                    d_lo, d_hi = pairs[idx + 1]
                    out.append(f"    {emit_pair_body(d_lo, d_hi, True, list(norm_vf), False)}")
                out.append(f"    pc = 0x{(pc+16)&0x3FFF:04x}u; goto done;")
        out.append("}")

    out.append("done_after_delay:")
    out.append("    // E-bit on branch delay slot: execute one more pair at target")
    out.append("    return false;")
    out.append("done:")
    out.append("    vuFlushClip(clipPipe, clip); vuFlushQ(qPipe, q, status);")
    out.append("    std::memcpy(st.vf, vf, sizeof(vf));")
    out.append("    std::memcpy(st.acc, acc, sizeof(acc));")
    out.append("    std::memcpy(st.vi, vi, sizeof(vi));")
    out.append("    st.q = q; st.p = p; st.i = regI; st.clip = clip; st.status = status;")
    out.append("    st.pc = pc; st.ebit = false; st.branchPending = false;")
    out.append("    return true;")
    out.append("}")
    return "\n".join(out)


HEADER = r'''// Auto-generated by tools/vu1_transpile.py (Stage 4 VU1 AOT recompilation).
#include "runtime/ps2_vu1.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_dbgstat.h"
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

using Ps2Vu1NativeFn = bool (*)(uint64_t codeHash, VU1State &st, uint8_t *vuData, uint32_t dataSize,
                                GS &gs, PS2Memory *memory, uint64_t *outGifHash, uint64_t *outGifBytes, bool replaySink);
void ps2_vu1_register_native_executor(Ps2Vu1NativeFn fn);

namespace
{
#if defined(__GNUC__) || defined(__clang__)
#define DRA2_ALWAYS_INLINE_ATTR __attribute__((always_inline))
#define DRA2_ALWAYS_INLINE __attribute__((always_inline)) inline
#define DRA2_NOINLINE __attribute__((noinline))
#else
#define DRA2_ALWAYS_INLINE_ATTR
#define DRA2_ALWAYS_INLINE inline
#define DRA2_NOINLINE
#endif

DRA2_ALWAYS_INLINE float vuNorm(float v) noexcept
{
    uint32_t b;
    std::memcpy(&b, &v, 4);
    const uint32_t absB = b & 0x7FFFFFFFu;
    if (__builtin_expect((absB - 0x00800000u) < 0x7F000000u, 1))
        return v;
    b = (absB < 0x00800000u) ? (b & 0x80000000u) : ((b & 0x80000000u) | 0x7F7FFFFFu);
    std::memcpy(&v, &b, 4);
    return v;
}

DRA2_ALWAYS_INLINE float vuNormRes(float v) noexcept
{
    return vuNorm(v);
}

DRA2_ALWAYS_INLINE float vuMaddN(float a, float b, float c) noexcept
{
    return vuNormRes(static_cast<float>(static_cast<double>(a) + static_cast<double>(b) * static_cast<double>(c)));
}

DRA2_ALWAYS_INLINE float vuMsubN(float a, float b, float c) noexcept
{
    return vuNormRes(static_cast<float>(static_cast<double>(a) - static_cast<double>(b) * static_cast<double>(c)));
}

DRA2_ALWAYS_INLINE float vuMadd(float a, float b, float c) noexcept
{
    return vuMaddN(vuNorm(a), vuNorm(b), vuNorm(c));
}

DRA2_ALWAYS_INLINE float vuMsub(float a, float b, float c) noexcept
{
    return vuMsubN(vuNorm(a), vuNorm(b), vuNorm(c));
}

DRA2_ALWAYS_INLINE float vuMinMax(float a, float b, bool maximum) noexcept
{
    int32_t l, r;
    std::memcpy(&l, &a, 4);
    std::memcpy(&r, &b, 4);
    const bool rev = l < 0 && r < 0;
    const int32_t bits = ((maximum != rev) ? (l > r) : (l < r)) ? l : r;
    float res;
    std::memcpy(&res, &bits, 4);
    return res;
}

DRA2_ALWAYS_INLINE int32_t vuFloatToInt(float value, float scale) noexcept
{
    const double scaled = static_cast<double>(value) * static_cast<double>(scale);
    if (scaled >= static_cast<double>(std::numeric_limits<int32_t>::max()))
        return std::numeric_limits<int32_t>::max();
    if (scaled <= static_cast<double>(std::numeric_limits<int32_t>::min()))
        return std::numeric_limits<int32_t>::min();
    return static_cast<int32_t>(scaled);
}

inline uint32_t vuClip6(const float *vs, float w) noexcept
{
    uint32_t wBits;
    std::memcpy(&wBits, &w, 4);
    const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;
    auto exc = [limit](float val, uint32_t sm) -> bool {
        uint32_t bits; std::memcpy(&bits, &val, 4); bits ^= sm;
        int32_t ob; std::memcpy(&ob, &bits, 4); return ob > limit;
    };
    uint32_t f = 0;
    if (exc(vs[0], 0x00000000u)) f |= 0x01u;
    if (exc(vs[0], 0x80000000u)) f |= 0x02u;
    if (exc(vs[1], 0x00000000u)) f |= 0x04u;
    if (exc(vs[1], 0x80000000u)) f |= 0x08u;
    if (exc(vs[2], 0x00000000u)) f |= 0x10u;
    if (exc(vs[2], 0x80000000u)) f |= 0x20u;
    return f;
}

struct VuClipPipe
{
    uint32_t val[4]{};
    uint8_t valid[4]{};
    uint8_t head = 0;
    uint8_t pending = 0;
};

inline void vuQueueClip(VuClipPipe &cp, uint32_t &workingClip, uint32_t clip6) noexcept
{
    workingClip = ((workingClip << 6) | (clip6 & 0x3Fu)) & 0xFFFFFFu;
    const uint8_t slot = (cp.head + 3u) & 3u;
    if (!cp.valid[slot]) cp.pending++;
    cp.val[slot] = workingClip;
    cp.valid[slot] = 1;
}

inline void vuQueueFcset(VuClipPipe &cp, uint32_t &workingClip, uint32_t imm24) noexcept
{
    workingClip = imm24 & 0xFFFFFFu;
    const uint8_t slot = (cp.head + 3u) & 3u;
    if (!cp.valid[slot]) cp.pending++;
    cp.val[slot] = workingClip;
    cp.valid[slot] = 1;
}

DRA2_NOINLINE void vuTickClipSlow(VuClipPipe &cp, uint32_t &clip) noexcept
{
    if (cp.valid[cp.head])
    {
        clip = cp.val[cp.head];
        cp.valid[cp.head] = 0;
        cp.pending--;
    }
    cp.head = (cp.head + 1u) & 3u;
}

DRA2_ALWAYS_INLINE void vuTickClip(VuClipPipe &cp, uint32_t &clip) noexcept
{
    if (__builtin_expect(!cp.pending, 1))
        return;
    vuTickClipSlow(cp, clip);
}

inline void vuFlushClip(VuClipPipe &cp, uint32_t &clip) noexcept
{
    while (cp.pending)
        vuTickClipSlow(cp, clip);
}

struct VuQPipe
{
    float val = 0.0f;
    uint32_t di = 0;
    uint8_t delay = 0;
};

DRA2_ALWAYS_INLINE void vuQueueQ(VuQPipe &qp, float val, uint32_t di, uint8_t delay) noexcept
{
    qp.val = val;
    qp.di = di;
    qp.delay = delay;
}

DRA2_ALWAYS_INLINE void vuTickQ(VuQPipe &qp, float &q, uint32_t &status) noexcept
{
    if (__builtin_expect(qp.delay != 0, 0) && --qp.delay == 0)
    {
        q = qp.val;
        status = (status & 0xFCFu) | qp.di | (qp.di << 6);
    }
}

DRA2_NOINLINE void vuTickPipesSlow(VuClipPipe &cp, uint32_t &clip, VuQPipe &qp, float &q, uint32_t &status) noexcept
{
    if (cp.pending)
        vuTickClipSlow(cp, clip);
    if (qp.delay && --qp.delay == 0)
    {
        q = qp.val;
        status = (status & 0xFCFu) | qp.di | (qp.di << 6);
    }
}

DRA2_ALWAYS_INLINE void vuTickPipes(VuClipPipe &cp, uint32_t &clip, VuQPipe &qp, float &q, uint32_t &status) noexcept
{
    if (__builtin_expect((cp.pending | qp.delay) != 0, 0))
        vuTickPipesSlow(cp, clip, qp, q, status);
}

DRA2_ALWAYS_INLINE void vuWaitQ(VuQPipe &qp, VuClipPipe &cp, uint32_t &clip, float &q, uint32_t &status) noexcept
{
    if (__builtin_expect(qp.delay != 0, 0))
    {
        uint8_t d = qp.delay;
        while (d-- && cp.pending)
            vuTickClipSlow(cp, clip);
        q = qp.val;
        status = (status & 0xFCFu) | qp.di | (qp.di << 6);
        qp.delay = 0;
    }
}

inline void vuFlushQ(VuQPipe &qp, float &q, uint32_t &status) noexcept
{
    if (qp.delay)
    {
        q = qp.val;
        status = (status & 0xFCFu) | qp.di | (qp.di << 6);
        qp.delay = 0;
    }
}

inline float vuDiv(float a, float b, uint32_t &outDi) noexcept
{
    const float num = vuNorm(a), den = vuNorm(b);
    uint32_t di = 0;
    float res = 0.0f;
    if (den == 0.0f)
    {
        di = num == 0.0f ? 0x10u : 0x20u;
        res = (std::signbit(num) != std::signbit(den)) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
    }
    else
        res = num / den;
    outDi = di;
    return vuNormRes(res);
}

inline float vuSqrt(float a, uint32_t &outDi) noexcept
{
    const float val = vuNorm(a);
    outDi = val < 0.0f ? 0x10u : 0u;
    return vuNormRes(std::sqrt(std::fabs(val)));
}

inline float vuRsqrt(float a, float b, uint32_t &outDi) noexcept
{
    const float num = vuNorm(a), rad = vuNorm(b);
    const float den = std::sqrt(std::fabs(rad));
    uint32_t di = rad < 0.0f ? 0x10u : 0u;
    float res = 0.0f;
    if (den != 0.0f)
        res = num / den;
    else
    {
        di = num == 0.0f ? 0x10u : 0x20u;
        res = std::signbit(num) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
    }
    outDi = di;
    return vuNormRes(res);
}

DRA2_NOINLINE void vuXgkick(uint32_t qwordAddr, uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory,
                            uint64_t *outGifHash, uint64_t *outGifBytes, bool replaySink)
{
    if (!vuData || dataSize < 16u)
        return;
    dbgstat::xgkick.fetch_add(1, std::memory_order_relaxed);
    const bool pow2 = (dataSize & (dataSize - 1u)) == 0u;
    const uint32_t mask = dataSize - 1u;
    const uint32_t startOff = pow2 ? ((qwordAddr * 16u) & mask) : ((qwordAddr * 16u) % dataSize);

    // Fast path: scan packet length directly in vuData without copying.
    // Most XGKICK packets are contiguous (do not wrap around the 16 KiB VU1 data memory).
    uint32_t total = 0;
    uint32_t cur = startOff;
    bool wrapped = false;
    while (total + 16u <= 0x10000u)
    {
        uint64_t tagLo = 0;
        std::memcpy(&tagLo, vuData + cur, 8);
        const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
        const bool eop = ((tagLo >> 15) & 1u) != 0u;
        const uint32_t flg = static_cast<uint32_t>((tagLo >> 58) & 3u);
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
        if (nreg == 0u) nreg = 16u;
        uint32_t dataQwords = 0;
        if (flg == 0u) dataQwords = nloop * nreg;
        else if (flg == 1u) dataQwords = (nloop * nreg + 1u) / 2u;
        else dataQwords = nloop;
        const uint32_t chunkBytes = 16u + dataQwords * 16u;
        const uint32_t maxRemain = 0x10000u - total;
        const uint32_t takeBytes = (chunkBytes <= maxRemain) ? chunkBytes : (maxRemain & ~15u);
        if (cur + takeBytes > dataSize)
        {
            wrapped = true;
            break;
        }
        total += takeBytes;
        cur += takeBytes;
        if (cur == dataSize)
            cur = 0;
        if (eop || chunkBytes > maxRemain)
            break;
    }

    alignas(16) uint8_t wrapBuf[0x10000];
    const uint8_t *emitPtr = vuData + startOff;
    if (__builtin_expect(wrapped, 0))
    {
        uint32_t src = startOff;
        total = 0;
        while (total + 16u <= sizeof(wrapBuf))
        {
            uint64_t tagLo = 0;
            std::memcpy(&tagLo, vuData + src, 8);
            std::memcpy(wrapBuf + total, vuData + src, 16);
            src = pow2 ? ((src + 16u) & mask) : ((src + 16u) % dataSize);
            total += 16u;
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const bool eop = ((tagLo >> 15) & 1u) != 0u;
            const uint32_t flg = static_cast<uint32_t>((tagLo >> 58) & 3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u) nreg = 16u;
            uint32_t dataQwords = 0;
            if (flg == 0u) dataQwords = nloop * nreg;
            else if (flg == 1u) dataQwords = (nloop * nreg + 1u) / 2u;
            else dataQwords = nloop;
            for (uint32_t q = 0; q < dataQwords && total + 16u <= sizeof(wrapBuf); ++q)
            {
                std::memcpy(wrapBuf + total, vuData + src, 16);
                src = pow2 ? ((src + 16u) & mask) : ((src + 16u) % dataSize);
                total += 16u;
            }
            if (eop)
                break;
        }
        emitPtr = wrapBuf;
    }

    if (__builtin_expect(outGifHash != nullptr && outGifBytes != nullptr, 0))
    {
        uint64_t h = *outGifHash;
        for (uint32_t i = 0; i < total; ++i)
            h = (h ^ emitPtr[i]) * 1099511628211ull;
        *outGifHash = h;
        *outGifBytes += total;
    }
    if (replaySink)
        return;
    if (memory)
        memory->submitGifPacket(GifPathId::Path1, emitPtr, total);
    else
        gs.processGIFPacket(emitPtr, total);
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("inputs", nargs="+", type=Path)
    args = parser.parse_args()

    progs = {}
    for p in args.inputs:
        data = p.read_bytes()
        h = fnv1a_64(data)
        progs[h] = data

    parts = [HEADER]
    for h, data in progs.items():
        parts.append(transpile_program(data, h))

    parts.append("""
static bool dra2Vu1NativeDispatch(uint64_t codeHash, VU1State &st, uint8_t *vuData, uint32_t dataSize,
                                  GS &gs, PS2Memory *memory, uint64_t *outGifHash, uint64_t *outGifBytes, bool replaySink)
{
    // VU1Interpreter::run() already sets FE_TOWARDZERO before invoking the native executor.
    switch (codeHash)
    {""")
    for h in progs:
        parts.append(f"    case 0x{h:016x}ull: return vu1_prog_{h:016x}(st, vuData, dataSize, gs, memory, outGifHash, outGifBytes, replaySink);")
    parts.append("""    default: return false;
    }
}

struct Vu1NativeAutoRegister {
    Vu1NativeAutoRegister() { ps2_vu1_register_native_executor(&dra2Vu1NativeDispatch); }
} g_vu1NativeAutoRegister;
} // namespace
""")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(parts))
    print(f"Generated {args.output} ({len(progs)} VU1 program(s): {[hex(h) for h in progs]})")


if __name__ == "__main__":
    main()
