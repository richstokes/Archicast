#!/usr/bin/env python3
"""Ahead-of-time ARM2 to C recompiler for the RISC OS 3.11 ROM.

Every ARM instruction proven reachable (static walk plus boot traces) becomes C.
Code seen running from RAM is compiled per (address, word) with a guard.
Nothing here runs on the console: the output is ordinary C linked into the ELF.
"""
import argparse
import struct
from pathlib import Path

import re

from mkapps import box_entries, load_apps
from romscan import ENTRIES, ROM_BASE, SKIP_MODULES, find_chain, module_info, plausible, sweep, walk, word
from scorpress import unpack

MASK26 = 0x3FFFFFC
# Code outside the boot trace in these modules lives on disc, one unit in memory at
# a time. Apps used together must not share a unit with what they depend on, so
# SharedCLibrary, BASIC and FileCore stay resident. So do PDriverDP and PDumperSupport:
# a print job calls them around every plot, from !Draw's overlay among others.
OVERLAYS = {
    "draw": {"!Draw"},
    "paint": {"!Paint"},
    "misc": {"PDumperDM", "Printers", "NetFS",
             "NetFiler", "NetPrint", "NetStatus", "NetUtils", "BroadcastLoader", "Econet",
             "BBCEconet", "DOSFS", "Debugger", "Portable", "SerialDeviceDriver",
             "SerialDeviceSupport", "ParallelDeviceDriver", "Podule"},
}
# Instructions changed when recompiling; the ROM data itself stays as it is.
ROM_PATCHES = {
    # ADFS only starts IDE on IOEB machines or if a podule claims it. This machine
    # reports old-style I/O so the floppy driver stays WD1772, so skip that check.
    0xE1470: (0x1A000003, 0xE1A00000),
}
RAM_IMAGES = {"WaveSynth", "StringLib", "Percussion"}
ROM_BYTES = 0x200000
CHUNK = 3000
COND = ["F_Z", "!F_Z", "F_C", "!F_C", "F_N", "!F_N", "F_V", "!F_V",
        "(F_C&&!F_Z)", "(!F_C||F_Z)", "(F_N==F_V)", "(F_N!=F_V)",
        "(!F_Z&&F_N==F_V)", "(F_Z||F_N!=F_V)", None, "0"]
NZCV_ALL = frozenset("NZCV")


def ror(value, amount):
    amount &= 31
    return ((value >> amount) | (value << (32 - amount))) & 0xFFFFFFFF if amount else value


def is_mul(i):
    return i & 0x0FC000F0 == 0x00000090


def is_dp(i):
    k = (i >> 25) & 7
    return k == 1 or (k == 0 and not (i & 0x90) == 0x90)


def flags_read(i):
    cond = i >> 28
    used = {0: "Z", 1: "Z", 2: "C", 3: "C", 4: "N", 5: "N", 6: "V", 7: "V",
            8: "CZ", 9: "CZ", 10: "NV", 11: "NV", 12: "NZV", 13: "NZV"}.get(cond, "")
    used = set(used)
    if is_dp(i):
        if (i >> 21) & 15 in (5, 6, 7):  # ADC, SBC, RSC
            used.add("C")
        if not i & (1 << 25) and not i & 0x10 and i & 0xFE0 == 0x60:  # RRX
            used.add("C")
        if not i & (1 << 25) and i & 0x10:  # a register shift of 0 keeps carry
            used.add("C")
    return used


def flags_written(i):
    """Flags definitely overwritten when this instruction executes."""
    if is_dp(i) and i & (1 << 20) and (i >> 12) & 15 != 15:
        op = (i >> 21) & 15
        if op in (2, 3, 4, 5, 6, 7, 10, 11):
            return set("NZCV")
        return set("NZ") | ({"C"} if shifter_sets_carry(i) else set())
    return set()


def shifter_sets_carry(i):
    if i & (1 << 25):
        return (i >> 8) & 15 != 0
    return bool(i & 0x10) or (i >> 7) & 31 != 0 or (i >> 5) & 3 != 0


def uses_psr(i):
    """True when a write or read of the whole PSR means flags must be current."""
    k = (i >> 25) & 7
    if k == 5 and i & (1 << 24):  # BL stores flags in R14
        return True
    if k == 7:
        return True
    if is_dp(i) and ((i >> 12) & 15 == 15 or (i >> 16) & 15 == 15 or i & 15 == 15):
        return True
    if k in (2, 3):
        return (i >> 12) & 15 == 15 or (i & (1 << 25) and i & 15 == 15)
    if k == 4:
        return True
    return False


class Emit:
    def __init__(self, pc, insn, ram=False):
        self.pc, self.i, self.ram = pc, insn, ram
        self.out = []
        self.terminal = False

    def line(self, text):
        self.out.append(text)

    def lit(self, offset):
        """The value of PC+offset. RAM routines receive their PC in _u."""
        if self.ram:
            return f"((_u+{offset}u)&{MASK26:#x}u)"
        return f"{(self.pc + offset) & MASK26:#x}u"

    def pc_arg(self):
        return "_u" if self.ram else f"{self.pc}u"

    def reg(self, n, offset=8):
        if n == 15:
            return f"R15_FULL({self.lit(offset)})"
        return f"C->r[{n}]"

    def rn(self, n, offset=8):
        return self.lit(offset) if n == 15 else f"C->r[{n}]"

    def goto_indirect(self, expr):
        self.line(f"TAIL(dispatch(({expr})&{MASK26:#x}u));")

    def shifter(self, want_carry):
        """Return (operand expr, carry expr or None); may emit a temporary."""
        i = self.i
        if i & (1 << 25):
            rot = ((i >> 8) & 15) * 2
            value = ror(i & 0xFF, rot)
            carry = str(value >> 31) if rot else None
            return f"{value:#x}u", carry
        rm, kind = i & 15, (i >> 5) & 3
        if i & 0x10:
            rs = (i >> 8) & 15
            m, s = self.reg(rm, 12), self.reg(rs, 12)
            if want_carry:
                self.line(f"u32 nc=C->fc; u32 b=shift_c({kind},{m},{s}&255,&nc);")
                return "b", "nc"
            return f"shift_r({kind},{m},{s}&255)", None
        amount, m = (i >> 7) & 31, self.reg(rm)
        if kind == 0:
            if amount == 0:
                return m, None
            return f"({m}<<{amount})", f"(({m}>>{32 - amount})&1)"
        if kind == 1:
            if amount == 0:
                return "0u", f"({m}>>31)"
            return f"({m}>>{amount})", f"(({m}>>{amount - 1})&1)"
        if kind == 2:
            n = amount or 32
            expr = f"((u32)((s32){m}>>{min(n, 31)}))"
            return expr, f"(({m}>>{n - 1})&1)"
        if amount == 0:
            return f"((C->fc<<31)|({m}>>1))", f"({m}&1)"
        return f"(({m}>>{amount})|({m}<<{32 - amount}))", f"(({m}>>{amount - 1})&1)"

    def data(self, live):
        i = self.i
        op, s, rn, rd = (i >> 21) & 15, bool(i & (1 << 20)), (i >> 16) & 15, (i >> 12) & 15
        logical = op in (0, 1, 8, 9, 12, 13, 14, 15)
        compare = 8 <= op <= 11
        reg_shift = not i & (1 << 25) and i & 0x10
        offset = 12 if reg_shift else 8
        need_c = s and logical and "C" in live and shifter_sets_carry(i) and rd != 15
        b, carry = self.shifter(need_c)
        a = self.rn(rn, offset)
        v = "1" if s and rd != 15 and "V" in live else ""
        cflag = s and rd != 15 and "C" in live
        expr = {0: f"{a}&({b})", 1: f"{a}^({b})", 8: f"{a}&({b})", 9: f"{a}^({b})",
                12: f"{a}|({b})", 13: b, 14: f"{a}&~({b})", 15: f"~({b})"}.get(op)
        if logical:
            if compare and rd == 15:
                self.line(f"psr_write({expr});")
                self.line(f"TAIL(dispatch({self.lit(4)}));")
                return
            if b != "b" and need_c:
                self.line(f"u32 nc={carry};")
                carry = "nc"
            self.line(f"u32 r={expr};")
        else:
            self.line(f"u32 a={a},o={b},r;")
            kind = {2: ("a", "o", 0), 3: ("o", "a", 0), 4: ("a", "o", 1), 5: ("a", "o", 2),
                    6: ("a", "o", 3), 7: ("o", "a", 3), 10: ("a", "o", 0), 11: ("a", "o", 1)}[op]
            x, y, form = kind
            if form == 1:
                self.line(f"r={x}+{y};")
                if cflag:
                    self.line("C->fc=r<a;")
                if v:
                    self.line("C->fv=((a^r)&(o^r))>>31;")
            elif form == 2:
                self.line("{u32 ci=C->fc; r=a+o+ci;")
                if cflag:
                    self.line("C->fc=ci?r<=a:r<a;")
                if v:
                    self.line("C->fv=((a^r)&(o^r))>>31;")
                self.line("}")
            elif form == 0:
                self.line(f"r={x}-{y};")
                if cflag:
                    self.line(f"C->fc={x}>={y};")
                if v:
                    self.line(f"C->fv=(({x}^{y})&({x}^r))>>31;")
            else:
                self.line(f"{{u32 ci=C->fc; r={x}-{y}-!ci;")
                if cflag:
                    self.line(f"C->fc=ci?{x}>={y}:{x}>{y};")
                if v:
                    self.line(f"C->fv=(({x}^{y})&({x}^r))>>31;")
                self.line("}")
            if compare and rd == 15:
                self.line("psr_write(r);")
                self.line(f"TAIL(dispatch({self.lit(4)}));")
                return
        if s and rd != 15:
            if "N" in live:
                self.line("C->fn=r;")
            if "Z" in live:
                self.line("C->fz=r;")
            if logical and need_c and carry:
                self.line(f"C->fc={carry};")
        if compare:
            return
        if rd == 15:
            if s:
                self.line("psr_write(r);")
            self.goto_indirect("r")
            return
        self.line(f"C->r[{rd}]=r;")

    def mul(self, live):
        i = self.i
        rd, rn, rs, rm = (i >> 16) & 15, (i >> 12) & 15, (i >> 8) & 15, i & 15
        expr = f"C->r[{rm}]*C->r[{rs}]"
        if i & (1 << 21):
            expr += f"+C->r[{rn}]"
        self.line(f"u32 r={expr}; C->r[{rd}]=r;")
        if i & (1 << 20):
            self.line("C->fn=r; C->fz=r;")

    def single(self):
        i = self.i
        load, wb, byte, up, pre = (bool(i & (1 << b)) for b in (20, 21, 22, 23, 24))
        rn, rd = (i >> 16) & 15, (i >> 12) & 15
        if i & (1 << 25):
            if i & 0x10:
                self.line(f"TAIL(undef_insn({self.pc_arg()}));")
                return
            # Bit 25 means "register" here but "immediate" for the shifter.
            shift = Emit(self.pc, i & ~(1 << 25), self.ram)
            offset, _ = shift.shifter(False)
        else:
            offset = f"{i & 0xFFF}u"
        base = self.rn(rn)
        sign = "+" if up else "-"
        self.line(f"u32 base={base},ea=base{sign}({offset});")
        addr = "ea" if pre else "base"
        user = "T" if not pre and wb else ""
        pc = f",{self.pc_arg()}" if user else ""
        size = 8 if byte else 32
        if load:
            self.line(f"u32 v=ld{size}{user}({addr}{pc});")
        else:
            value = self.reg(rd, 12)
            self.line(f"st{size}{user}({addr},{value}{pc});")
        if (not pre or wb) and rn != 15:
            self.line(f"C->r[{rn}]=ea;")
        if load:
            if rd == 15:
                self.goto_indirect("v")
            else:
                self.line(f"C->r[{rd}]=v;")

    def multiple(self):
        i = self.i
        load, wb, s, up, pre = (bool(i & (1 << b)) for b in (20, 21, 22, 23, 24))
        rn = (i >> 16) & 15
        regs = [r for r in range(16) if i & (1 << r)]
        n = len(regs) or 1
        size = n * 4
        self.line(f"u32 base={self.rn(rn)};")
        if up:
            start = "base+4u" if pre else "base"
            final = f"base+{size}u"
        else:
            start = f"base-{size}u" if pre else f"base-{size - 4}u"
            final = f"base-{size}u"
        self.line(f"u32 a=({start})&~3u, wbv={final};")
        user_bank = s and not (load and 15 in regs)
        psr = s and load and 15 in regs
        plain = [r for r in regs if r != 15]
        mask = sum(1 << r for r in plain)
        if load:
            if len(plain) >= 3 and not (user_bank and plain[-1] >= 8):
                self.line(f"a=ldm_regs(a,{mask:#x}u);")
            else:
                for r in plain:
                    if user_bank and r >= 8:
                        self.line(f"usr_set({r},ld32(a)); a+=4;")
                    else:
                        self.line(f"C->r[{r}]=ld32(a); a+=4;")
            if 15 in regs:
                self.line("u32 npc=ld32(a);")
            if wb and rn != 15 and (rn not in regs or (user_bank and rn >= 8)):
                self.line(f"C->r[{rn}]=wbv;")
            if 15 in regs:
                if psr:
                    self.line("psr_write(npc);")
                self.goto_indirect("npc")
        else:
            if len(plain) >= 3 and 15 not in regs and not user_bank and not (wb and rn in regs and regs[0] != rn):
                self.line(f"stm_regs(a,{mask:#x}u);")
            else:
                for index, r in enumerate(regs):
                    if r == 15:
                        value = self.reg(15, 12)
                    elif user_bank and r >= 8:
                        value = f"usr_get({r})"
                    elif r == rn and index and wb:
                        value = "wbv"
                    else:
                        value = f"C->r[{r}]"
                    self.line(f"st32(a,{value}); a+=4;")
            if wb and rn != 15:
                self.line(f"C->r[{rn}]=wbv;")

    def branch(self, known):
        i, pc = self.i, self.pc
        disp = (((i & 0xFFFFFF) ^ 0x800000) - 0x800000) * 4 + 8
        if i & (1 << 24):
            self.line(f"C->r[14]=R15_FULL({self.lit(4)});")
        if self.ram:
            self.line(f"TAIL(dispatch((_u+({disp}))&0x3FFFFFFu));")
            return
        target = (pc + disp) & 0x3FFFFFF
        if target not in known:
            self.line(f"TAIL(dispatch({target:#x}u));")
        elif target <= pc:
            self.line(f"if(--C->budget<0) TAIL(mach_poll({target:#x}u));")
            self.line(f"TAIL(A_{target:07x}(_u));")
        else:
            self.line(f"TAIL(A_{target:07x}(_u));")

    def translate(self, known, live, note_pc=False):
        """Append C for one instruction."""
        i, pc = self.i, self.pc
        if note_pc:
            self.line(f"C->cur={self.pc_arg()};")
        cond = COND[i >> 28]
        body = Emit(pc, i, self.ram)
        k = (i >> 25) & 7
        if is_mul(i):
            body.mul(live)
        elif k in (0, 1):
            if k == 0 and i & 0x90 == 0x90:
                body.line(f"TAIL(undef_insn({self.pc_arg()}));")
            else:
                body.data(live)
        elif k in (2, 3):
            body.single()
        elif k == 4:
            body.multiple()
        elif k == 5:
            body.branch(known)
        elif k == 7 and i & (1 << 24):
            body.line(f"TAIL(swi_insn({self.pc_arg()}));")
        else:
            body.line(f"TAIL(undef_insn({self.pc_arg()}));")
        if cond is None:
            self.line("{" + " ".join(body.out) + "}")
        else:
            self.line(f"if({cond}){{" + " ".join(body.out) + "}")


def slot(reg):
    """State layout: see cpu_t. Hot state sits in SH-4 short displacement range."""
    return reg if reg < 8 else reg + 8 if reg < 12 else reg - 4


def ends_block(i):
    """True when an instruction never falls through unconditionally."""
    cond = i >> 28
    k = (i >> 25) & 7
    if cond != 14:
        return False
    if k == 5 and not i & (1 << 24):
        return True
    return writes_pc_any(i)


def writes_pc_any(i):
    k = (i >> 25) & 7
    if is_mul(i):
        return False
    if is_dp(i):
        op = (i >> 21) & 15
        return (i >> 12) & 15 == 15 and not 8 <= op <= 11
    if k in (2, 3):
        return bool(i & (1 << 20)) and (i >> 12) & 15 == 15
    if k == 4:
        return bool(i & (1 << 20)) and bool(i & (1 << 15))
    return False


def psr_side_effect(i):
    """Instructions after which translated flow must go through dispatch."""
    return is_dp(i) and 8 <= (i >> 21) & 15 <= 11 and (i >> 12) & 15 == 15


def read_bitmap(path):
    data = Path(path).read_bytes()
    result = []
    start = ROM_BASE >> 5
    for index in range(start, start + (0x200000 >> 5)):
        byte = data[index]
        for bit in range(8):
            if byte & (1 << bit):
                result.append((index * 8 + bit) * 4 - ROM_BASE)
    return result


def collect(rom, traces, extra):
    modules = [module_info(rom, offset) for offset in find_chain(rom)]
    seeds = [0x1AC] + extra
    seeds += [seed for m in modules if m["title"] not in SKIP_MODULES for seed in m["seeds"]]
    if any(m["title"] == "DreamApps" for m in modules):
        seeds += box_entries()
    jumps, ram, traced = set(), set(), set()
    for prefix in traces:
        hits = read_bitmap(prefix + ".bits")
        seeds += hits
        traced |= set(hits)
        jumps |= set(read_bitmap(prefix + ".jump"))
        blob = Path(prefix + ".ram").read_bytes()
        for (key,) in struct.iter_unpack("<Q", blob):
            if key:
                ram.add((key & 0xFFFFFFFE, key >> 32))
    code = sweep(rom, modules, walk(rom, seeds))
    return code, jumps | set(extra), ram, traced


def leaders(rom, code, jumps, base=ROM_BASE):
    lead = {o for o in jumps if o in code}
    lead |= {0x1AC} | ENTRIES
    for pc in code:
        i = word(rom, pc)
        k = (i >> 25) & 7
        if k == 5:
            target = (base + pc + 8 + (((i & 0xFFFFFF) ^ 0x800000) - 0x800000) * 4) & 0x3FFFFFF
            if target >= base and target - base in code:
                lead.add(target - base)
            if i & (1 << 24):
                lead.add(pc + 4)
        elif k >= 6:
            lead.add(pc + 4)  # SWI and undefined-trap returns
        if ends_block(i) or psr_side_effect(i):
            lead.add(pc + 4)
        if pc + 4 not in code:
            lead.add(pc)
    return {o for o in lead if o in code}


def is_memory(i):
    return (i >> 25) & 7 in (2, 3, 4)


def block_insns(rom, start, lead, code):
    pc, insns = start, []
    while pc in code:
        insns.append((pc, word(rom, pc)))
        if ends_block(insns[-1][1]) or psr_side_effect(insns[-1][1]):
            break
        pc += 4
        if pc in lead:
            break
    return insns


def branch_target(pc, i, base):
    """Image offset of a direct branch, or None if it leaves the image."""
    target = (base + pc + 8 + (((i & 0xFFFFFF) ^ 0x800000) - 0x800000) * 4) & 0x3FFFFFF
    return target - base if target >= base else None


def flag_liveness(rom, lead, code, base=ROM_BASE):
    """Backward dataflow over the whole ROM: which flags each instruction must set.

    Indirect exits, SWIs and calls keep everything live, because R14 captures the
    flags and callers read them back. Interrupts save and restore whatever is there.
    """
    blocks = {start: block_insns(rom, start, lead, code) for start in lead}
    live_in = dict.fromkeys(blocks, frozenset())
    live_after = {}

    def transfer(start, record):
        insns = blocks[start]
        pc, i = insns[-1]
        falls = not ends_block(i) and not (psr_side_effect(i) and i >> 28 == 14)
        live = frozenset(live_in.get(pc + 4, NZCV_ALL)) if falls else frozenset()
        for pc, i in reversed(insns):
            if record:
                live_after[pc] = live
            klass = (i >> 25) & 7
            if klass == 5 and not i & (1 << 24):
                target = branch_target(pc, i, base)
                taken = live_in.get(target, NZCV_ALL)
                live = frozenset(taken) if i >> 28 == 14 else live | taken
            elif uses_psr(i) or klass >= 6 or writes_pc_any(i) or psr_side_effect(i):
                live = NZCV_ALL
            elif i >> 28 == 14:
                live = live - flags_written(i)
            live = live | flags_read(i)
        return live

    # Flags flow backwards, so walking high to low settles in far fewer passes.
    order = sorted(blocks, reverse=True)
    changed = True
    while changed:
        changed = False
        for start in order:
            value = transfer(start, False)
            if value != live_in[start]:
                live_in[start] = value
                changed = True
    for start in blocks:
        transfer(start, True)
    return live_after


def block(rom, start, lead, code, known, exact_end, live_after, base=ROM_BASE):
    src = []
    noted = False
    insns = block_insns(rom, start, lead, code)
    for pc, i in insns:
        live = live_after.get(pc, NZCV_ALL)
        # Aborts report the block's first memory access, except in the kernel,
        # whose self test resumes after a deliberate abort.
        note = is_memory(i) and (not noted or pc < exact_end)
        noted = noted or is_memory(i)
        e = Emit(base + pc, i)
        e.translate(known, live, note)
        src.extend(e.out)
    end = insns[-1]
    if not ends_block(end[1]) and not (psr_side_effect(end[1]) and end[1] >> 28 == 14):
        next_pc = base + end[0] + 4
        if next_pc in known:
            src.append(f"TAIL(A_{next_pc:07x}(_u));")
        else:
            src.append(f"TAIL(dispatch({next_pc:#x}u));")
    return src


def ram_block(insn):
    """A native routine for one instruction word seen running from RAM."""
    e = Emit(0, insn, ram=True)
    e.translate(set(), set(NZCV_ALL), is_memory(insn))
    e.line(f"TAIL(dispatch({e.lit(4)}));")
    return e.out


def app_code(app):
    """Recompile one bundled app: (base, end, entry, entry word, functions, insns)."""
    entry = next(e for e in app["files"] if e["name"] == app["code"]["file"])
    image, base, exec_addr = entry["data"], int(entry["load"], 16), int(entry["exec"], 16)
    seeds = [exec_addr - base] + [int(s, 16) - base for s in app["code"]["seeds"]]
    for table, count in app["code"].get("tables", []):
        seeds += [word(image, int(table, 16) - base + n * 4) - base for n in range(count)]
    seeds += [o for o in range(0, len(image) - 3, 4)  # STMFD sp!,{..,lr}
              if word(image, o) & 0xFFFF4000 == 0xE92D4000 and plausible(image, o)]
    ENTRIES.clear()
    code = walk(image, seeds, base)
    lead = leaders(image, code, set(seeds), base)
    live_after = flag_liveness(image, lead, code, base)
    known = {base + o for o in lead}
    funcs = [(f"A_{base + start:07x}", base + start, block(image, start, lead, code, known, 0, live_after, base))
             for start in sorted(lead)]
    entries = {base + o for o in seeds} | {base + o + 4 for o in code if (word(image, o) >> 25) & 7 >= 6
                                           or (word(image, o) >> 24) & 15 == 11}
    entries |= {base + o for o in ENTRIES}
    for o in code:  # polls land on backward branch targets
        i = word(image, o)
        if (i >> 25) & 7 == 5 and base + o + 8 + (((i & 0xFFFFFF) ^ 0x800000) - 0x800000) * 4 <= base + o:
            entries.add(base + o + 8 + (((i & 0xFFFFFF) ^ 0x800000) - 0x800000) * 4)
    for _, _, src in funcs:  # flow that goes through dispatch, such as after TEQP
        entries.update(int(pc, 16) for pc in re.findall(r"dispatch\((0x[0-9a-f]+)u\)", "\n".join(src)))
    return base, base + len(image), exec_addr, word(image, exec_addr - base), funcs, len(code), entries & known


def pad_keys(app):
    """The app's controller buttons as key codes, in the order ov_app_t holds them."""
    pad = app["code"].get("pad", {})
    names = ("left", "right", "up", "down", "a", "b", "x", "start")
    return ",".join(pad.get(name, "0xFF") for name in names)


def voice_words(app):
    """Instruction words of modules an app inserts from a packed file, in place."""
    spec = app["code"].get("modules")
    if not spec:
        return set()
    data = unpack(next(e for e in app["files"] if e["name"] == spec["file"])["data"])
    seeds = []
    for n in range(spec["count"]):
        module = word(data, n * 4)
        seeds += [module + word(data, module + slot) for slot in (4, 8)]
        seeds += [module + 0x160 + slot * 4 for slot in range(7)]  # the voice's entry branches
    return {word(data, o) for o in walk(data, seeds, int(spec["base"], 16))}


def hash_table(entries, name):
    size = 1
    while size * 7 < len(entries) * 8:
        size *= 2
    slots = [None] * size
    for key, fn in entries:
        slot = ((key >> 2) * 2654435761 & 0xFFFFFFFF) >> 8 & (size - 1)
        while slots[slot]:
            slot = (slot + 1) & (size - 1)
        slots[slot] = (key, fn)
    text = [f"const u32 {name}_mask={size - 1}u;\nconst block_t {name}[{size}]={{\n"]
    text += [f"{{{e[0]:#x}u,{e[1]}}},\n" if e else "{0,0},\n" for e in slots]
    text.append("};\n")
    return text


def inline_memory(text):
    """Swap the memory helpers for inline fast paths, in app units only."""
    text = re.sub(r"\b(ld32|ld8|st32|st8)\(", r"\1i(", text)

    def regs(mask):
        return [r for r in range(15) if mask >> r & 1]

    def ldm(m):
        rs = regs(int(m.group(1), 16))
        body = " ".join(f"C->r[{r}]=q[{i}];" for i, r in enumerate(rs))
        return (f"{{u32 *q=block_at(C->rt,a,{len(rs) * 4}u); if(LIKELY(q)){{{body} a+={len(rs) * 4}u;}} "
                f"else a=ldm_regs(a,{m.group(1)}u);}}")

    def stm(m):
        rs = regs(int(m.group(1), 16))
        body = " ".join(f"q[{i}]=C->r[{r}];" for i, r in enumerate(rs))
        return (f"{{u32 *q=block_at(C->wt,a,{len(rs) * 4}u); if(LIKELY(q)){{{body}}} "
                f"else stm_regs(a,{m.group(1)}u);}}")

    text = re.sub(r"a=ldm_regs\(a,(0x[0-9a-f]+)u\);", ldm, text)
    return re.sub(r"stm_regs\(a,(0x[0-9a-f]+)u\);", stm, text)


def app_unit(number, unit, unit_funcs, image):
    """One app as a single file, so the compiler can merge its blocks.

    Only addresses reached indirectly are visible to dispatch. Apps share their
    addresses with whatever else runs there, so an entry from outside only counts
    if memory still holds the instruction it was compiled from. Inside the unit a
    return goes straight through app_map.
    """
    base, end, entries = unit[0], unit[1], sorted(unit[6])
    last = max(f[1] for f in unit_funcs)
    text = ['#include "appunit.h"\n']
    text += [f"static void {f[0]}(u32);\n" for f in unit_funcs]
    table = hash_table([(pc, f"A_{pc:07x}") for pc in entries], f"ov_table_{number}")
    mask = re.search(rf"ov_table_{number}_mask=(\d+)u", table[0]).group(1)
    slots = re.findall(r"^\{(0x[0-9a-f]+u|0),", "".join(table), re.M)
    guards = [word(image, int(k[:-1], 16) - base) if k != "0" else 0 for k in slots]
    text.append(f"static const block_fn app_map[{(last - base) // 4 + 1}]={{")
    text += [f"[{(pc - base) // 4}]=A_{pc:07x}," for pc in entries]
    text.append("};\n")
    text.append(f"static void app_jump(u32 pc){{u32 i=pc-{base:#x}u; "
                f"if(LIKELY(i<={last - base:#x}u&&!(i&3)&&--C->budget>=0)){{block_fn f=app_map[i>>2]; "
                f"if(LIKELY(f!=0)) TAIL(f(pc));}} TAIL(dispatch(pc));}}\n")
    for name, _, _, src in unit_funcs:
        body = "\n".join(src)
        body = re.sub(r"TAIL\(dispatch\(\(", "TAIL(app_jump((", body)
        body = inline_memory(body)
        body = re.sub(r"C->r\[(\d+)\]", lambda m: f"C->s[{slot(int(m.group(1)))}]", body)
        text.append(f"static void {name}(u32 _u){{\n{body}\n}}\n")
    text += table
    text.append(f"static const u32 ov_guard_{number}[]={{" + ",".join(f"{v:#x}u" for v in guards) + "};\n")
    text.append(f"const ov_header_t ov_header_{number} OV_SECTION="
                f"{{OV_MAGIC,ov_table_{number},{mask}u,ov_guard_{number}}};\n")
    return "".join(text)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("rom", help="rom.bin from mkapps.py")
    parser.add_argument("out_dir")
    parser.add_argument("--trace", action="append", default=[])
    parser.add_argument("--seeds", help="text file of extra ROM entry points")
    parser.add_argument("--ram-words", help="text file of extra RAM instruction words")
    args = parser.parse_args()
    image = Path(args.rom).read_bytes()
    rom = bytearray(image[:ROM_BYTES])
    # BASIC apps are data in the pack, with nothing to recompile
    apps = [app for app in load_apps(Path(args.rom).parent / "upstream") if "code" in app]
    for offset, (old, new) in ROM_PATCHES.items():
        if word(rom, offset) != old:
            raise SystemExit(f"ROM patch at {offset:#x} does not match this ROM")
        struct.pack_into("<I", rom, offset, new)
    extra = []
    if args.seeds and Path(args.seeds).exists():
        for line in Path(args.seeds).read_text().split("\n"):
            line = line.split("#")[0].strip()
            if line:
                value = int(line, 16)
                extra.append(value - ROM_BASE if value >= ROM_BASE else value)
    code, jumps, ram, traced = collect(rom, args.trace, extra)
    lead = leaders(rom, code, jumps)
    kernel_end = find_chain(rom)[0]
    modules = [module_info(rom, offset) for offset in find_chain(rom)]
    ranges = []
    unit_of = {}
    for index, (unit, names) in enumerate(OVERLAYS.items()):
        for m in modules:
            if m["title"] in names:
                ranges.append((m["offset"], m["offset"] + m["length"], index))
                for pc in range(m["offset"], m["offset"] + m["length"], 4):
                    if pc in lead and pc not in traced:
                        unit_of[pc] = index
    resident = {ROM_BASE + o for o in lead if o not in unit_of}
    unit_known = [resident | {ROM_BASE + o for o, u in unit_of.items() if u == i}
                  for i in range(len(OVERLAYS))]
    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    for stale in list(out.glob("xlat_*.c")) + list(out.glob("ov_*.c")):
        stale.unlink()

    live_after = flag_liveness(rom, lead, code)
    funcs = []
    for start in sorted(lead):
        name = f"A_{ROM_BASE + start:07x}"
        unit = unit_of.get(start)
        known = resident if unit is None else unit_known[unit]
        funcs.append((name, ROM_BASE + start, unit, block(rom, start, lead, code, known, kernel_end, live_after)))
    app_units = [app_code(app) for app in apps]
    words = {insn for _, insn in ram}
    if args.ram_words and Path(args.ram_words).exists():
        for line in Path(args.ram_words).read_text().split("\n"):
            line = line.split("#")[0].strip()
            if line:
                words.add(int(line, 16))
    for app in apps:
        words |= voice_words(app)
    # Sound voice modules run from RAM, and their code is known ahead of time.
    # Identical words share one routine.
    for m in (module_info(rom, offset) for offset in find_chain(rom)):
        if m["title"] in RAM_IMAGES:
            for pc in range(m["offset"] + 4, m["offset"] + m["length"], 4):
                w = word(rom, pc)
                if w >> 28 != 15 and w > 0xFFFF and w >> 24 not in range(0x20, 0x7F):
                    words.add(w)
    ram_funcs = [(f"R_{w:08x}", w) for w in sorted(words)]
    for name, w in ram_funcs:
        funcs.append((name, 0, None, ram_block(w)))

    def write_chunks(group_funcs, stem):
        for chunk in range(0, len(group_funcs), CHUNK):
            group = group_funcs[chunk:chunk + CHUNK]
            body = []
            for name, _, _, src in group:
                body.append(f"void {name}(u32 _u){{")
                body.extend(src)
                body.append("}")
            text = re.sub(r"C->r\[(\d+)\]", lambda m: f"C->s[{slot(int(m.group(1)))}]", "\n".join(body))
            refs = set(re.findall(r"\bA_[0-9a-f]{7}\b", text))
            decls = "".join(f"void {r}(u32);\n" for r in sorted(refs))
            (out / f"{stem}_{chunk // CHUNK:03d}.c").write_text('#include "cpu.h"\n' + decls + text + "\n")

    residents = [f for f in funcs if f[2] is None]
    write_chunks(residents, "xlat")
    for index in range(len(OVERLAYS)):
        write_chunks([f for f in funcs if f[2] == index], f"ov_{index}")
    first_app = len(OVERLAYS)
    units = len(OVERLAYS) + len(app_units)
    app_funcs = {}
    for index, unit in enumerate(app_units):
        app_funcs[first_app + index] = [(name, pc, first_app + index, src) for name, pc, src in unit[4]]

    rom_entries = [(pc, name) for name, pc, unit, _ in residents if pc]
    lines = ['#include "cpu.h"\n']
    lines += [f"void {name}(u32);\n" for name, _, _, _ in residents]
    lines += hash_table(rom_entries, "block_table")
    lines += hash_table([(w, name) for name, w in ram_funcs], "ram_table")
    lines.append(f"const ov_range_t ov_ranges[{len(ranges)}]={{\n")
    lines += [f"{{{ROM_BASE + a:#x}u,{ROM_BASE + b:#x}u,{u}}},\n" for a, b, u in ranges]
    lines.append(f"}};\nconst u32 ov_range_count={len(ranges)}u;\nconst u32 ov_units={units}u;\n")
    lines.append(f"const ov_app_t ov_apps[{len(app_units) + 1}]={{\n")
    lines += [f"{{{u[0]:#x}u,{u[1]:#x}u,{u[2]:#x}u,{u[3]:#x}u,{first_app + i},"
              f"{apps[i]['code'].get('frame_us', 20000)},{{{pad_keys(apps[i])}}}}},\n"
              for i, u in enumerate(app_units)]
    lines.append(f"{{0,0,0,0,0,0,{{0}}}}}};\nconst u32 ov_app_count={len(app_units)}u;\n")
    lines.append(f"const u32 rom_ext_pages={(len(image) - ROM_BYTES) // 0x8000}u;\n")
    for index in range(len(OVERLAYS)):
        unit_funcs = [f for f in funcs if f[2] == index]
        table = hash_table([(f[1], f[0]) for f in unit_funcs], f"ov_table_{index}")
        mask = re.search(rf"ov_table_{index}_mask=(\d+)u", table[0]).group(1)
        text = ['#include "cpu.h"\n'] + [f"void {f[0]}(u32);\n" for f in unit_funcs] + table
        text.append(f"const ov_header_t ov_header_{index} OV_SECTION="
                    f"{{OV_MAGIC,ov_table_{index},{mask}u,0}};\n")
        (out / f"ov_{index}_table.c").write_text("".join(text))
    for index, unit in enumerate(app_units):
        number = first_app + index
        entry = next(e for e in apps[index]["files"] if e["name"] == apps[index]["code"]["file"])
        (out / f"ov_{number}_table.c").write_text(app_unit(number, unit, app_funcs[number], entry["data"]))
    index_text = ['#include "cpu.h"\n']
    index_text += [f"extern const ov_header_t ov_header_{i};\n" for i in range(units)]
    index_text.append("const ov_header_t *const ov_headers[]={")
    index_text += [f"&ov_header_{i}," for i in range(units)]
    index_text.append("};\n")
    (out / "ov_index.c").write_text("".join(index_text))
    (out / "xlat_table.c").write_text("".join(lines))
    instructions = len(code) + len(words)
    print(f"overlay blocks {len(unit_of)} of {len(lead)}")
    for app, unit in zip(apps, app_units):
        print(f"app {app['name']}: {unit[5]} insns, {len(unit[4])} blocks")
    print(f"code {len(code)} insns, ram words {len(words)}, blocks {len(funcs)}, "
          f"chunks {len(funcs) // CHUNK + 1}, {instructions * 4} ARM bytes")


if __name__ == "__main__":
    main()
