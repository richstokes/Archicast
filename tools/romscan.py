#!/usr/bin/env python3
"""Map a RISC OS 3.11 ROM: module chain, reachable code, and porting hazards.

Reachability is a recursive walk from the reset vector and module entry points.
Computed branches are not followed, so the code figure is a lower bound.
"""
import argparse
import collections
import hashlib
import struct
import sys
import urllib.request
from pathlib import Path

ROM_BASE = 0x3800000
ROM_CHIPS = ("IC24.ROM", "IC25.ROM", "IC26.rom", "IC27.ROM")
CHIP_SHA1 = (
    "12d060e0401dd0523d44453f947bdc55dd2c3240",
    "78f5d0e6f1e8ee603317807f53ff8fe65a3b3518",
    "46b870876bc1f68f242726415f0c49fef7be0c72",
    "345199a33fed23996374b9db8170a52ab63f0380",
)
CHIP_SIZE = 0x80000
# The four chips back to back, inside a RAR that archive.org serves members of.
ROM_URL = "https://archive.org/download/RISC-OS-ROM-collection/ROMS.rar/ROMS%2F1.%20Major%2FROM311"


def fetch_rom(directory):
    """Download the 3.11 image and split it into whichever chip files are missing."""
    print(f"ROM not found in {directory}, fetching {ROM_URL}", file=sys.stderr)
    try:
        with urllib.request.urlopen(ROM_URL) as reply:
            image = reply.read()
    except OSError as error:
        sys.exit(f"couldn't download the ROM ({error}); put the chip dumps in {directory} by hand")
    directory.mkdir(parents=True, exist_ok=True)
    for i, (name, sha1) in enumerate(zip(ROM_CHIPS, CHIP_SHA1)):
        chip = image[i * CHIP_SIZE:(i + 1) * CHIP_SIZE]
        if hashlib.sha1(chip).hexdigest() != sha1:
            sys.exit(f"downloaded ROM doesn't match {name}'s SHA-1; put the chip dumps in {directory} by hand")
        if not (directory / name).exists():
            (directory / name).write_bytes(chip)


def load_rom(directory):
    directory = Path(directory)
    if not all((directory / name).exists() for name in ROM_CHIPS):
        fetch_rom(directory)
    return b"".join((directory / name).read_bytes() for name in ROM_CHIPS)


def word(rom, offset):
    return struct.unpack_from("<I", rom, offset)[0]


def cstring(rom, offset, limit=64):
    end = rom.find(b"\0", offset, offset + limit)
    return rom[offset:end if end >= 0 else offset + limit].decode("latin-1")


def valid_module(rom, offset):
    """offset addresses the length word that precedes a module header."""
    if offset + 48 > len(rom):
        return False
    length = word(rom, offset)
    if length < 32 or length & 3 or offset + length > len(rom):
        return False
    title = word(rom, offset + 4 + 16)
    if not 28 <= title < length:
        return False
    text = cstring(rom, offset + 4 + title)
    return len(text) >= 2 and text.isprintable()


def find_chain(rom):
    """Follow the chain from the UtilityModule header, the first ROM module."""
    title = rom.index(b"UtilityModule\0")
    for base in range(title & ~3, max(title - 0x10000, 0), -4):
        if base + word(rom, base + 16) == title and valid_module(rom, base - 4):
            chain, offset = [], base - 4
            while valid_module(rom, offset):
                chain.append(offset)
                offset += word(rom, offset)
            return chain
    return []


def module_info(rom, offset):
    base = offset + 4
    length = word(rom, offset)
    entries = [word(rom, base + i * 4) for i in range(4)]
    swi_handler = word(rom, base + 32) if word(rom, base + 16) >= 36 else 0
    seeds = [base + e for e in entries + [swi_handler] if e and not e & 3 and e < length]
    seeds += command_handlers(rom, base, length)
    return {
        "offset": offset,
        "length": length,
        "title": cstring(rom, base + word(rom, base + 16)),
        "seeds": seeds,
    }


def command_handlers(rom, base, length):
    """Code offsets from the module's * command table."""
    table = word(rom, base + 24)
    handlers = []
    if not table or table >= length:
        return handlers
    pos = base + table
    end = base + length
    while pos < end and rom[pos]:
        stop = rom.find(b"\0", pos, pos + 64)
        if stop < 0:
            break
        pos = (stop + 4) & ~3
        code = word(rom, pos)
        if code and not code & 3 and code < length:
            handlers.append(base + code)
        pos += 16
    return handlers


def plausible(rom, pc, run=6):
    """Heuristic: a run of well-formed instructions rather than text or tables."""
    if pc < 0 or pc + run * 4 > len(rom) or pc & 3:
        return False
    for i in range(run):
        insn = word(rom, pc + i * 4)
        cond, klass = insn >> 28, (insn >> 25) & 7
        if cond == 0xF and insn not in (0xF0000000, 0xF1A00000):
            return False
        if klass == 0 and insn & 0x90 == 0x90 and insn & 0x0FC000F0 != 0x90:
            return False
        if klass == 6 or (klass == 7 and not insn & (1 << 24) and insn & 0x0F000F00 != 0x0E000100):
            return False
        if insn & 0xFFFFFF00 == 0:  # small integers are table data
            return False
    return True


# Not speculatively recompiled: data modules and hardware this port does not have. Code a trace proves runs is still compiled.
SKIP_MODULES = {
    "Messages", "ROMFonts", "Printers", "Econet",
    "BBCEconet", "NetFS", "NetFiler", "NetPrint", "NetStatus", "NetUtils", "BroadcastLoader",
    "PDriver", "PDriverDP", "PDumperSupport", "PDumperDM", "SerialDeviceDriver",
    "SerialDeviceSupport", "ParallelDeviceDriver", "Portable", "Podule", "DOSFS",
}

ENTRIES = set()  # every address reached indirectly or by branch: needs a block entry


def long_adr(rom, pc, rd, target):
    """Follow ADD/SUB rd,rd,#imm after an ADR, which reaches further than one can."""
    while 0 <= pc < len(rom) - 3:
        insn = word(rom, pc)
        if insn & 0xFFEFF000 != 0xE2800000 | rd << 16 | rd << 12 and insn & 0xFFEFF000 != 0xE2400000 | rd << 16 | rd << 12:
            break
        rot = ((insn >> 8) & 15) * 2
        value = insn & 0xFF
        value = ((value >> rot) | (value << (32 - rot))) & 0xFFFFFFFF if rot else value
        target += value if insn & (1 << 23) else -value
        pc += 4
    return target


def walk(rom, seeds, base=ROM_BASE):
    """Return image offsets reached by static control flow, jump tables and ADR.

    Apps pass their load address as base. ROM code has few inline strings and they
    were found by tracing, but apps are walked statically, so step over OS_WriteS.
    """
    seen, stack, forced = set(), list(seeds), set(seeds)
    while stack:
        pc = stack.pop()
        ENTRIES.add(pc)
        while 0 <= pc < len(rom) - 3 and pc not in seen:
            insn = word(rom, pc)
            cond = insn >> 28
            if cond == 0xF and pc not in forced and insn not in (0xF0000000, 0xF1A00000):
                break  # NV never executes: probably data unless a trace passed through
            seen.add(pc)
            klass = (insn >> 25) & 7
            if klass == 5:  # B / BL
                target = pc + 8 + (((insn & 0xFFFFFF) ^ 0x800000) - 0x800000) * 4
                target &= 0x3FFFFFF  # 26-bit PC; reset branches wrap into high ROM
                stack.append(target - base if base == ROM_BASE and target >= base else target)
                if insn & (1 << 24) == 0 and cond == 0xE:
                    break
            elif insn & 0x0FEF0000 in (0x028F0000, 0x024F0000):  # ADR
                rot = ((insn >> 8) & 15) * 2
                value = insn & 0xFF
                value = ((value >> rot) | (value << (32 - rot))) & 0xFFFFFFFF if rot else value
                target = pc + 8 + (value if insn & (1 << 23) else -value)
                if base != ROM_BASE:
                    target = long_adr(rom, pc + 4, (insn >> 12) & 15, target)
                if plausible(rom, target):
                    stack.append(target)
            elif is_jump_table(insn):
                table = pc + 8 if (insn >> 12) & 15 == 15 else pc + 4
                while 0 <= table < len(rom) - 3 and (word(rom, table) >> 24) & 0xFF in (0xEA, 0xEB):
                    stack.append(table)
                    table += 4
            if cond == 0xE and writes_pc(insn):
                break
            pc += 4
            if base != ROM_BASE and insn & 0xFFFDFFFF == 0xEF000001:
                pc = (rom.index(b"\0", pc) + 4) & ~3
                ENTRIES.add(pc)
    return seen


def is_jump_table(insn):
    """ADD pc, pc, Rn, LSL #2 with an optional preceding fallthrough slot."""
    return insn & 0x0FFFF000 == 0x008FF000 or insn & 0x0FFFF000 == 0x008FE000


def writes_pc(insn):
    klass = (insn >> 25) & 7
    if klass in (0, 1):
        opcode = (insn >> 21) & 15
        return (insn >> 12) & 15 == 15 and not 8 <= opcode <= 11
    if klass in (2, 3):
        return bool(insn & (1 << 20)) and (insn >> 12) & 15 == 15
    if klass == 4:
        return bool(insn & (1 << 20)) and bool(insn & (1 << 15))
    return False


def address_seeds(rom):
    """Absolute ROM addresses stored in tables, as the kernel uses for dispatch."""
    seeds = []
    for offset in range(0, len(rom) - 3, 4):
        value = word(rom, offset) & 0x3FFFFFC
        if ROM_BASE + 0x40 <= value < ROM_BASE + len(rom) and word(rom, offset) >> 28 in (0, 2, 6):
            if plausible(rom, value - ROM_BASE, 3):
                seeds.append(value - ROM_BASE)
    return seeds


def sweep(rom, modules, code):
    """Add unreferenced routines that sit after known code."""
    regions = [(0x40, modules[0]["offset"])]
    regions += [(m["offset"] + 4, m["offset"] + m["length"] - 4) for m in modules
                if m["title"] not in SKIP_MODULES]
    wanted = {pc for start, end in regions for pc in range(start & ~3, end, 4)}
    code |= walk(rom, [pc for pc in address_seeds(rom) if pc in wanted])
    # A run of plain branches is a dispatch table.
    is_branch = lambda pc: word(rom, pc) >> 24 == 0xEA
    code |= walk(rom, [pc for start, end in regions for pc in range(start, end - 8, 4)
                       if is_branch(pc) and (is_branch(pc + 4) or is_branch(pc - 4))])
    # So is one interleaved with numbers, as in a Wimp message handler table.
    in_region = lambda pc: any(start <= pc < end for start, end in regions)
    target = lambda pc: pc + 8 + (((word(rom, pc) & 0xFFFFFF) ^ 0x800000) - 0x800000) * 4
    code |= walk(rom, [pc for start, end in regions for pc in range(start, end - 8, 4)
                       if is_branch(pc) and in_region(target(pc)) and plausible(rom, target(pc), 3)])
    # Handlers reached through relative tables nearly always begin by saving lr.
    code |= walk(rom, [pc for start, end in regions for pc in range(start, end, 4)
                       if word(rom, pc) & 0xFFFF4000 == 0xE92D4000 and plausible(rom, pc, 3)])
    # Compiled C saves them the APCS way: MOV ip,sp then STMFD sp!,{..fp,ip,lr,pc}.
    code |= walk(rom, [pc for start, end in regions for pc in range(start, end - 4, 4)
                       if word(rom, pc) == 0xE1A0C00D and word(rom, pc + 4) & 0xFFFFD800 == 0xE92DD800])
    added = True
    while added:
        added = False
        for start, end in regions:
            for pc in range(start, end, 4):
                if pc - 4 in code and pc not in code and plausible(rom, pc):
                    new = walk(rom, [pc]) - code
                    if new:
                        code |= new
                        added = True
    return code


def hazards(rom, code):
    counts = collections.Counter()
    for pc in code:
        insn = word(rom, pc)
        klass = (insn >> 25) & 7
        if klass == 7 and insn & (1 << 24):
            counts["swi"] += 1
        elif klass in (6, 7):
            counts["coprocessor"] += 1
        if writes_pc(insn):
            counts["computed_branch"] += 1
        if klass in (0, 1) and (insn >> 12) & 15 == 15 and insn & (1 << 20):
            counts["pc_psr_restore"] += 1
        if (insn >> 28) != 0xE:
            counts["conditional"] += 1
        if klass in (0, 1) and insn & (1 << 20) and not 8 <= (insn >> 21) & 15 <= 11:
            counts["sets_flags"] += 1
        if klass == 2 and not insn & (1 << 24) or klass == 3 and not insn & (1 << 24):
            counts["post_index"] += 1
    literals = collections.Counter()
    for offset in range(0, len(rom) - 3, 4):
        value = word(rom, offset)
        for name, base in (("ioc", 0x3200000), ("vidc", 0x3400000), ("memc", 0x3600000)):
            if value & 0xFFF00000 == base:
                literals[name] += 1
    return counts, literals


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("rom_dir")
    args = parser.parse_args()
    rom = load_rom(args.rom_dir)
    chain = find_chain(rom)
    modules = [module_info(rom, offset) for offset in chain]
    kernel_end = chain[0] if chain else len(rom)
    print(f"rom {len(rom)} bytes, kernel/pre-chain {kernel_end:#x}, modules {len(modules)}")
    for m in modules:
        print(f"  {m['offset']:#08x} {m['length']:8d} {m['title']}")
    seeds = [0, 4, 8, 12, 16, 24, 28]
    seeds += [seed for m in modules for seed in m["seeds"]]
    code = sweep(rom, modules, walk(rom, seeds))
    print(f"statically reached: {len(code)} instructions ({len(code) * 4} bytes, "
          f"{len(code) * 400 // len(rom)}% of ROM)")
    counts, literals = hazards(rom, code)
    for key, value in sorted(counts.items()):
        print(f"  {key}: {value}")
    print("  literal words pointing at hardware:", dict(literals))


if __name__ == "__main__":
    main()
