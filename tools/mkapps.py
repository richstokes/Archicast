#!/usr/bin/env python3
"""Build rom.bin: the ROM, a module that registers bundled apps, and a file pack.

The pack follows the 2MB ROM in rom.bin and is mapped at PACK_BASE by mach.c, one
cold page at a time. ResourceFS only ever sees ordinary memory.
"""
import hashlib
import json
import struct
import sys
import urllib.request
from pathlib import Path

import basic
from romscan import find_chain, load_rom, module_info, word
from scorpress import unpack

PACK_BASE = 0x3C00000
PAGE = 0x8000
MODULE_AT = 0x1FFD00  # padding after the credits text, before the checksum words
ROM_END = 0x1FFFE0
ROOT = Path(__file__).resolve().parent.parent


def fetch(app, cache):
    """Download the app's pinned upstream files once."""
    paths = {}
    upstream = app.get("upstream", {})
    for key, entry in upstream.get("files", {}).items():
        path = cache / app["name"] / key
        if not path.exists():
            path.parent.mkdir(parents=True, exist_ok=True)
            url = upstream["base"] + urllib.request.quote(entry["path"])
            print(f"fetching {url}")
            with urllib.request.urlopen(url) as reply:
                path.write_bytes(reply.read())
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest != entry["sha256"]:
            sys.exit(f"{path}: sha256 {digest} is not the pinned {entry['sha256']}")
        paths[key] = path
    return paths


def sprite_file(art):
    """A sprite file from text art: the name, then a hex Wimp colour or . per pixel."""
    name, *rows = art.split()
    width, height = len(rows[0]), len(rows)
    image, mask = bytearray(), bytearray()
    for row in rows:
        pixels = [int(c, 16) if c != "." else 0 for c in row]
        solid = [15 if c != "." else 0 for c in row]
        for source, dest in ((pixels, image), (solid, mask)):
            line = bytes(source[n] | (source[n + 1] << 4 if n + 1 < width else 0) for n in range(0, width, 2))
            dest += line + bytes(-len(line) & 3)
    words = len(image) // height // 4
    head = struct.pack("<I12s", 44 + len(image) * 2, name.encode())
    head += struct.pack("<8I", words - 1, height - 1, 0, (width * 4 - 1) & 31, 44, 44 + len(image), 12, 0)[:28]
    area = struct.pack("<III", 1, 16, 16 + len(head) + len(image) * 2)
    return area + head + image + mask


def load_apps(cache):
    apps = []
    for manifest in sorted((ROOT / "apps").glob("*/app.json")):
        app = json.loads(manifest.read_text())
        paths = fetch(app, cache)
        for entry in app["files"]:
            if "basic" in entry:
                source = manifest.parent / entry["basic"]
                entry["data"] = basic.tokenise(source.read_text(encoding="latin-1"))
                continue
            if "art" in entry:
                entry["data"] = sprite_file((manifest.parent / entry["art"]).read_text())
                continue
            source = paths[entry["upstream"]] if "upstream" in entry else manifest.parent / entry["local"]
            entry["data"] = source.read_bytes()
            if entry.get("unpack") == "scorpress":
                entry["data"] = unpack(entry["data"])
        apps.append(app)
    return apps


def file_record(app, entry):
    """One ResourceFS file, minus the leading link to the next one."""
    if "type" in entry:
        load, exec_addr = 0xFFF00043 | int(entry["type"], 16) << 8, 0
    else:
        load, exec_addr = int(entry["load"], 16), int(entry["exec"], 16)
    data = entry["data"]
    name = f"Apps.{app['name']}.{entry['name']}".encode() + b"\0"
    name += bytes(-len(name) & 3)
    head = struct.pack("<IIII", load, exec_addr, len(data), 3) + name
    return head + struct.pack("<I", len(data) + 4) + data + bytes(-len(data) & 3)


def build_pack(apps):
    """Files ascending by size, so headers share as few pages as possible.

    Every ResourceFS lookup walks every header. The last file links back to a
    zero word ahead of it rather than on past its data, so that walk does not
    pull in the page where the largest file ends.
    """
    records = sorted((file_record(app, e) for app in apps for e in app["files"]), key=len)
    pack = bytearray()
    for index, record in enumerate(records):
        link = 4 + len(record)
        if index == len(records) - 2:
            link += 4
        elif index == len(records) - 1:
            pack += bytes(4)
            link = -4 & 0xFFFFFFFF
        pack += struct.pack("<I", link) + record
    return bytes(pack)


DUMPER_NUMBER = 40      # no Acorn dumper uses it
PDRIVER_DP = 7          # PDriverDP's number with the PDriver sharer
PDRIVER_VERSION = 0x148 # the newest PDriverDP takes; has to fit a MOV
XDREAM_PRINT_DUMP = 0xEF0EDC19  # X form of DreamNet SWI +25
XDREAM_BOX_FS = 0xEF0EDC21      # +33, src/dreambox.c
XDREAM_BOX_INIT = 0xEF0EDC22    # +34
XDREAM_BOX_TASK = 0xEF0EDC23    # +35
XDREAM_RESET = 0xEF0EDC24       # +36
BOX_FS_NUMBER = 0xDC            # BOX_FS_NUMBER in src/dreambox.h
BOX_FILES = 4                   # HANDLES there; FileSwitch refuses a fifth open
BOX_WORKSPACE = 576             # RMA for the error block, the icon bar task's blocks and the boot commands
SCRAP_CMD2 = 480                # SCRAP_FILE_CMD in src/net.c
SYSTEM_CMD = 512                # SYSTEM_PATH_CMD in src/net.c
BOX_ENTRIES = ["fs_open", "fs_get", "fs_put", "fs_args", "fs_close", "fs_file", "fs_func"]
# The SWIs the icon bar task asks for, in the order of TASK_* in src/dreambox.h
TASK_SWIS = [
    0xEF0600C0,  # XWimp_Initialise
    0xEF0600C2,  # XWimp_CreateIcon
    0xEF0600C7,  # XWimp_Poll
    0xEF0600D4,  # XWimp_CreateMenu
    0xEF02000C,  # XOS_GBPB
    0xEF0600DF,  # XWimp_ReportError
    0xEF020005,  # XOS_CLI
    0xEF0600DD,  # XWimp_CloseDown
]
PRINT_ERROR = b"The network printer failed. See !NetPrint for why.\0"


def assemble(items, want_labels=False):
    """Words, or functions of (address, labels) giving a word; strings are labels, bytes data."""
    labels, at = {}, 0
    for item in items:
        if isinstance(item, str):
            labels[item] = at
        elif isinstance(item, bytes):
            at += len(item) + (-len(item) & 3)
        else:
            at += 4
    out, at = bytearray(), 0
    for item in items:
        if isinstance(item, str):
            continue
        if isinstance(item, bytes):
            data = item + bytes(-len(item) & 3)
        else:
            data = struct.pack("<I", item(at, labels) if callable(item) else item)
        out += data
        at += len(data)
    return labels if want_labels else bytes(out)


def adr(reg, label):
    def encode(at, labels):
        offset = labels[label] - (at + 8)
        if offset < 0:
            assert -offset < 256
            return 0xE24F0000 | reg << 12 | -offset  # SUB reg,pc,#
        assert offset < 256
        return 0xE28F0000 | reg << 12 | offset
    return encode


def mov_offset(reg, label):
    """MOV reg,#label's offset from the module start, as a rotated immediate"""
    def encode(at, labels):
        value = labels[label]
        for rot in range(16):
            imm = (value << 2 * rot | value >> (32 - 2 * rot)) & 0xFFFFFFFF
            if imm < 256:
                return 0xE3A00000 | reg << 12 | rot << 8 | imm
        raise AssertionError(f"{label} at {value:#x} doesn't fit a MOV")
    return encode


def branch(cond, label):
    return lambda at, labels: cond << 28 | 0x0A000000 | ((labels[label] - (at + 8)) >> 2 & 0xFFFFFF)


def offset(label):
    return lambda at, labels: labels[label]


EQ, CC, HI, VS, AL = 0, 3, 8, 6, 14


def arm_module():
    return assemble(module_items())


def box_entries():
    """ROM offsets of the Dreambox veneers and the task's SWI table. FileSwitch
    reaches the veneers only through the filing system block, and the table only
    through a computed jump, so the recompiler needs telling (tools/armxlat.py)."""
    labels = assemble(module_items(), want_labels=True)
    names = BOX_ENTRIES + [f"task_{n}" for n in range(len(TASK_SWIS) + 1)]
    return [MODULE_AT + 4 + labels[name] for name in names]


def module_items():
    """Registers the pack, the printer dumper and the scrap variables !Scrap would set.

    Also answers ResourceFS restarting. The dumper is a veneer PDriverDP calls with a
    reason in r11. Colours are PDumperSupport's, as in PDumperDM, and for 1 bit strips
    that's just arithmetic. We skip its StartJob, which insists on a palette file off
    a printer definition disc. The rest goes to src/net.c, which says 1 to be called
    again while the ring drains.
    It registers in init because PDumperDM just took this same path, so boot stays
    free of overlays.

    It also adds the Dreambox filing system. Each FileSwitch entry puts its number
    in r12, which FileSwitch doesn't need back, and calls DreamNet_BoxFS until r12
    stops saying 1 (call again). 2 means r0 points at an error.

    *Desktop_Dreambox enters the module as the Dreambox icon bar task. Its code
    is a loop that issues whichever SWI DreamNet_BoxTask names in r10, so all
    the decisions are in src/dreambox.c and it needs no memory of its own.

    Ctrl+Break is a soft reset: the RMA and system variables stay, init does not
    run again, and modules only get Service_Reset. No task survives it, so that
    tells src/net.c to forget the ones it knew were running.
    """
    title = b"DreamApps\0"
    help_text = b"DreamApps\t0.06 (3 Nov 2026)\0"
    return [
        "start",
        offset("task"),
        offset("init"),
        0,                 # final
        offset("service"),
        offset("title"),
        offset("help"),
        offset("commands"),
        "init",
        0xE92D4100,        # STMFD sp!,{r8,lr}
        0xE3A0050F,        # MOV r0,#PACK_BASE
        0xEF061B40,        # SWI XResourceFS_RegisterFiles
        0xE3A00102,        # MOV r0,#&80000000 (add a dumper)
        0xE3A01000 | DUMPER_NUMBER,  # MOV r1,#number
        0xE3A02F00 | PDRIVER_VERSION >> 2,  # MOV r2,#version
        0xE3A03000,        # MOV r3,#0
        adr(4, "dumper"),  # ADR r4,dumper
        0xE3A050FF,        # MOV r5,#255 (branches)
        0xE3A06007,        # MOV r6,#7 (strip types)
        0xE3A08000 | PDRIVER_DP,  # MOV r8,#PDriverDP
        0xEF0A015B,        # SWI XPDriver_MiscOpForDriver
        0xE3A00006,        # MOV r0,#6 (claim)
        0xE3A03000 | BOX_WORKSPACE >> 2 | 15 << 8,  # MOV r3,#workspace
        0xEF02001E,        # SWI XOS_Module
        XDREAM_BOX_INIT,   # SWI XDreamNet_BoxInit (r2 -> workspace, out r0 -> first command)
        0xEF020005,        # SWI XOS_CLI (set Wimp$ScrapDir)
        0xE2820F00 | SCRAP_CMD2 >> 2,  # ADD r0,r2,#second command
        0xEF020005,        # SWI XOS_CLI (set Wimp$Scrap)
        0xE2820F00 | SYSTEM_CMD >> 2,  # ADD r0,r2,#third command
        0xEF020005,        # SWI XOS_CLI (set System$Path)
        0xE3A0000C,        # MOV r0,#12 (add a filing system)
        adr(1, "start"),   # ADR r1,start (the module base)
        mov_offset(2, "fsinfo"),  # MOV r2,#fsinfo
        0xE3A03000,        # MOV r3,#0 (private word)
        0xEF020029,        # SWI XOS_FSControl
        0xE1500000,        # CMP r0,r0 (a failure must not fail the boot)
        0xE8BD8100,        # LDMFD sp!,{r8,pc}
        "service",
        0xE3310027,        # TEQ r1,#&27 (Service_Reset)
        branch(EQ, "reset"),
        0xE3510060,        # CMP r1,#&60 (Service_ResourceFSStarting)
        0x11A0F00E,        # MOVNE pc,lr
        0xE92D4001,        # STMFD sp!,{r0,lr}
        0xE3A0050F,        # MOV r0,#PACK_BASE
        0xE1A0E00F,        # MOV lr,pc
        0xE1A0F002,        # MOV pc,r2
        0xE8BD8001,        # LDMFD sp!,{r0,pc}
        "reset",
        0xE92D4001,        # STMFD sp!,{r0,lr}
        XDREAM_RESET,      # SWI XDreamNet_Reset
        0xE8BD8001,        # LDMFD sp!,{r0,pc}
        "dumper",
        0xE33B0002,        # TEQ r11,#2
        branch(EQ, "colour"),
        0xE92D4001,        # STMFD sp!,{r0,lr}
        "again",
        0xE59D0000,        # LDR r0,[sp]
        XDREAM_PRINT_DUMP, # SWI XDreamNet_PrintDump
        0xE3500001,        # CMP r0,#1
        branch(EQ, "again"),
        branch(HI, "failed"),
        0xE8FD8001,        # LDMFD sp!,{r0,pc}^
        "failed",
        adr(0, "error"),   # ADR r0,error
        0xE28DD004,        # ADD sp,sp,#4
        0xE8BD4000,        # LDMFD sp!,{lr}
        0xE39EF201,        # ORRS pc,lr,#V
        "colour",
        0xE92D4C07,        # STMFD sp!,{r0-r2,r10,r11,lr}
        0xE1A01000,        # MOV r1,r0
        0xE1A00003,        # MOV r0,r3
        0xEF061B06,        # SWI XPDumper_SetColour
        branch(VS, "bad"),
        0xE8FD8C07,        # LDMFD sp!,{r0-r2,r10,r11,pc}^
        "bad",
        0xE28DD004,        # ADD sp,sp,#4
        0xE8BD4C06,        # LDMFD sp!,{r1,r2,r10,r11,lr}
        0xE39EF201,        # ORRS pc,lr,#V
        0,                 # end of service
        "error",
        0x00CDC000,        # error number
        PRINT_ERROR,
        "fsinfo",
        offset("fsname"),  # name
        offset("fsname"),  # startup text
        *(offset(name) for name in BOX_ENTRIES[:6]),
        BOX_FS_NUMBER | BOX_FILES << 16,
        offset("fs_func"),
        0,                 # GBPB: files are buffered
        0,                 # extra information word
        *(item for n, name in enumerate(BOX_ENTRIES) for item in (
            name,
            0xE3A0C000 | n,  # MOV r12,#entry
            branch(AL, "box"))),
        "box",
        0xE92D4000,        # STMFD sp!,{lr}
        "box_again",
        XDREAM_BOX_FS,     # SWI XDreamNet_BoxFS
        0xE35C0001,        # CMP r12,#1
        branch(EQ, "box_again"),
        0xE8BD4000,        # LDMFD sp!,{lr}
        0x33DEF201,        # BICCCS pc,lr,#V (r12 = 0, done)
        0xE39EF201,        # ORRS pc,lr,#V (r12 = 2, r0 -> error)
        "commands",
        b"Desktop_Dreambox\0",
        offset("desktop"),
        0x00FF0000,        # 0 to 255 parameters
        0,                 # syntax
        0,                 # help
        0,                 # end of commands
        "desktop",
        0xE1A02000,        # MOV r2,r0 (the tail)
        0xE3A00002,        # MOV r0,#2 (enter)
        adr(1, "title"),   # ADR r1,title
        0xEF02001E,        # SWI XOS_Module
        0xE1A0F00E,        # MOV pc,lr
        "task",
        0xE3E0A000,        # MVN r10,#0 (just started)
        "task_loop",
        XDREAM_BOX_TASK,   # SWI XDreamNet_BoxTask
        0xE08FF18A,        # ADD pc,pc,r10,LSL #3
        0xE1A00000,        # NOP (skipped)
        *(item for n, swi in enumerate(TASK_SWIS) for item in (
            f"task_{n}",
            swi,
            branch(AL, "task_loop"))),
        f"task_{len(TASK_SWIS)}",
        0xEF000011,        # SWI OS_Exit
        "fsname",
        b"Dreambox\0",
        "title",
        title,
        "help",
        help_text,
    ]


def patch_rom(rom):
    """Hang the module off the end of the chain, which is full of credits text."""
    rom = bytearray(rom)
    chain = find_chain(bytes(rom))
    last = chain[-1]
    if word(rom, last + word(rom, last)) != 0 or rom[MODULE_AT:ROM_END] != b"\xff" * (ROM_END - MODULE_AT):
        sys.exit("this ROM's layout is not the expected 3.11 image")
    module = arm_module()
    if MODULE_AT + 8 + len(module) > ROM_END:
        sys.exit(f"the module is {len(module)} bytes, too big for the gap")
    struct.pack_into("<I", rom, last, MODULE_AT - last)
    rom[MODULE_AT:MODULE_AT + 4 + len(module) + 4] = struct.pack("<I", len(module) + 4) + module + bytes(4)
    return bytes(rom)


def main():
    rom_dir, out = sys.argv[1], Path(sys.argv[2])
    apps = load_apps(out / "upstream")
    pack = build_pack(apps)
    patched = patch_rom(load_rom(rom_dir))
    out.mkdir(exist_ok=True)
    (out / "rom.bin").write_bytes(patched + pack + bytes(-len(pack) & (PAGE - 1)))
    print(f"{len(apps)} bundled apps, pack {len(pack)} bytes")


if __name__ == "__main__":
    main()
