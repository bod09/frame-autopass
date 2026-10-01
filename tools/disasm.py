#!/usr/bin/env python3
"""Annotated arm64 disassembly of functions in a stripped shared object.

Used to learn the signatures of SteamVR's private interfaces from the
headset's own binaries. Resolves adrp+add/ldr pairs to strings or data,
PLT stubs to imported symbol names, and RELATIVE relocations in data.

Usage: disasm.py LIB ADDR [ADDR ...] [--max N] [--until ADDR]
       disasm.py --xref LIB TARGET [TARGET ...]
Addresses are hex. Disassembles linearly from ADDR until a `ret` that is
not skipped over by an earlier forward branch, or N instructions.
"""

import re
import struct
import subprocess
import sys

import capstone
from capstone import arm64_const as A


class Elf:
    def __init__(self, path):
        self.path = path
        self.data = open(path, "rb").read()
        self.sections = []
        out = subprocess.run(["readelf", "-SW", path], capture_output=True, text=True).stdout
        for m in re.finditer(r"\]\s+(\S+)\s+\S+\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)", out):
            name, addr, off, size = m.group(1), int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16)
            self.sections.append((name, addr, off, size))
        self.relative = {}
        self.symbolic = {}
        out = subprocess.run(["readelf", "-rW", path], capture_output=True, text=True).stdout
        for line in out.splitlines():
            m = re.match(r"\s*([0-9a-f]+)\s+[0-9a-f]+\s+(R_AARCH64_\w+)\s+(\S+)?\s*(.*)", line)
            if not m:
                continue
            where, kind = int(m.group(1), 16), m.group(2)
            if kind == "R_AARCH64_RELATIVE":
                self.relative[where] = int(m.group(3), 16)
            elif kind in ("R_AARCH64_JUMP_SLOT", "R_AARCH64_GLOB_DAT", "R_AARCH64_ABS64"):
                rest = (m.group(4) or "").strip()
                sym = rest.split("+")[0].strip() if rest else ""
                self.symbolic[where] = sym.split("@")[0]
        self.plt = self._map_plt()

    def section_of(self, addr):
        for name, a, off, size in self.sections:
            if a and a <= addr < a + size:
                return name, a, off
        return None

    def read(self, addr, n):
        s = self.section_of(addr)
        if not s or s[0] == ".bss":
            return None
        return self.data[addr - s[1] + s[2]: addr - s[1] + s[2] + n]

    def cstring(self, addr, limit=160):
        raw = self.read(addr, limit)
        if not raw:
            return None
        end = raw.find(b"\0")
        if end <= 0:
            return None
        text = raw[:end]
        if all(32 <= c < 127 or c in (9, 10) for c in text):
            return text.decode()
        return None

    def u64(self, addr):
        raw = self.read(addr, 8)
        return struct.unpack("<Q", raw)[0] if raw and len(raw) == 8 else None

    def _map_plt(self):
        plt = {}
        s = [x for x in self.sections if x[0] == ".plt"]
        if not s:
            return plt
        _, addr, off, size = s[0]
        md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        md.detail = True
        page = None
        for ins in md.disasm(self.data[off:off + size], addr):
            if ins.id == A.ARM64_INS_ADRP:
                page = ins.operands[1].imm
            elif ins.id == A.ARM64_INS_LDR and page is not None and len(ins.operands) > 1:
                got = page + ins.operands[1].mem.disp
                sym = self.symbolic.get(got)
                if sym:
                    plt[ins.address - 4] = sym
                page = None
        return plt

    def describe(self, addr):
        """Best-effort label for an address used as data."""
        if addr in self.plt:
            return "plt:" + self.plt[addr]
        s = self.cstring(addr)
        if s:
            return repr(s)
        if addr in self.relative:
            target = self.relative[addr]
            s = self.cstring(target)
            return f"-> {target:#x}" + (f" {s!r}" if s else "")
        if addr in self.symbolic:
            return "got:" + self.symbolic[addr]
        sec = self.section_of(addr)
        return sec[0] if sec else None


def disassemble(elf, start, max_ins, until=None):
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    md.detail = True
    code = elf.read(start, max_ins * 4)
    regs_page = {}
    furthest = start
    for ins in md.disasm(code, start):
        note = ""
        ops = ins.operands
        if ins.id == A.ARM64_INS_ADRP:
            regs_page[ops[0].reg] = ops[1].imm
        elif ins.id == A.ARM64_INS_ADD and len(ops) == 3 and ops[1].reg in regs_page and ops[2].type == A.ARM64_OP_IMM:
            target = regs_page.pop(ops[1].reg) + ops[2].imm
            note = f"{target:#x} {elf.describe(target) or ''}"
        elif ins.id in (A.ARM64_INS_LDR, A.ARM64_INS_STR) and len(ops) > 1 and ops[1].type == A.ARM64_OP_MEM \
                and ops[1].mem.base in regs_page:
            target = regs_page[ops[1].mem.base] + ops[1].mem.disp
            note = f"[{target:#x}] {elf.describe(target) or ''}"
        elif ins.id in (A.ARM64_INS_BL, A.ARM64_INS_B) and ops and ops[0].type == A.ARM64_OP_IMM:
            target = ops[0].imm
            label = elf.plt.get(target)
            note = f"-> {label}" if label else ""
            if ins.id == A.ARM64_INS_B and target > furthest:
                furthest = target
        elif ins.group(capstone.CS_GRP_JUMP) and ops and ops[-1].type == A.ARM64_OP_IMM:
            if ops[-1].imm > furthest:
                furthest = ops[-1].imm
        print(f"  {ins.address:#x}: {ins.mnemonic:8} {ins.op_str:40} {note}".rstrip())
        if until is not None:
            if ins.address + 4 >= until:
                break
            continue
        if ins.id == A.ARM64_INS_RET and ins.address >= furthest:
            break
        if ins.id == A.ARM64_INS_B and ops and ops[0].type == A.ARM64_OP_IMM and ins.address >= furthest \
                and ops[0].imm < ins.address:
            break


def xrefs(elf, target):
    """Addresses in .text whose adrp+add (or adrp+ldr) pair forms `target`."""
    name, addr, off, size = [s for s in elf.sections if s[0] == ".text"][0]
    words = struct.unpack_from(f"<{size // 4}I", elf.data, off)
    hits = []
    page_of = {}
    for i, w in enumerate(words):
        pc = addr + i * 4
        if (w & 0x9F000000) == 0x90000000:  # adrp
            rd = w & 0x1F
            immlo = (w >> 29) & 3
            immhi = (w >> 5) & 0x7FFFF
            imm = (immhi << 2) | immlo
            if imm & (1 << 20):
                imm -= 1 << 21
            page_of[rd] = ((pc & ~0xFFF) + (imm << 12), pc)
        elif (w & 0xFF800000) == 0x91000000:  # add (immediate, 64-bit, no shift)
            rn = (w >> 5) & 0x1F
            if rn in page_of and pc - page_of[rn][1] < 64:
                if page_of[rn][0] + ((w >> 10) & 0xFFF) == target:
                    hits.append(pc)
    return hits


def main():
    args = sys.argv[1:]
    if args and args[0] == "--xref":
        elf = Elf(args[1])
        for a in args[2:]:
            print(a, [hex(h) for h in xrefs(elf, int(a, 16))])
        return
    max_ins = 400
    until = None
    if "--until" in args:
        i = args.index("--until")
        until = int(args[i + 1], 16)
        del args[i:i + 2]
    if "--max" in args:
        i = args.index("--max")
        max_ins = int(args[i + 1])
        del args[i:i + 2]
    elf = Elf(args[0])
    for a in args[1:]:
        start = int(a, 16)
        print(f"== {start:#x}")
        disassemble(elf, start, max_ins if until is None else (until - start) // 4 + 1, until)


if __name__ == "__main__":
    main()
