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


SCRAP_CMDS = [b"Set Wimp$ScrapDir ADFS:$\0", b"Set Wimp$Scrap ADFS:$.ScrapFile\0"]
DUMPER_NUMBER = 40      # no Acorn dumper uses it
PDRIVER_DP = 7          # PDriverDP's number with the PDriver sharer
PDRIVER_VERSION = 0x148 # the newest PDriverDP takes; has to fit a MOV
XDREAM_PRINT_DUMP = 0xEF0EDC19  # X form of DreamNet SWI +25
PRINT_ERROR = b"The network printer failed. See !NetPrint for why.\0"


def assemble(items):
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
    return bytes(out)


def adr(reg, label):
    def encode(at, labels):
        offset = labels[label] - (at + 8)
        assert 0 <= offset < 256
        return 0xE28F0000 | reg << 12 | offset
    return encode


def branch(cond, label):
    return lambda at, labels: cond << 28 | 0x0A000000 | ((labels[label] - (at + 8)) >> 2 & 0xFFFFFF)


def offset(label):
    return lambda at, labels: labels[label]


EQ, HI, VS = 0, 8, 6


def arm_module():
    """Registers the pack, the printer dumper and the scrap variables !Scrap would set.

    Also answers ResourceFS restarting. The dumper is a veneer PDriverDP calls with a
    reason in r11. Colours are PDumperSupport's, as in PDumperDM, and for 1 bit strips
    that's just arithmetic. We skip its StartJob, which insists on a palette file off
    a printer definition disc. The rest goes to src/net.c, which says 1 to be called
    again while the ring drains.
    It registers in init because PDumperDM just took this same path, so boot stays
    free of overlays.
    """
    title = b"DreamApps\0"
    help_text = b"DreamApps\t0.03 (1 Nov 2026)\0"
    body = assemble([
        0,                 # start
        offset("init"),
        0,                 # final
        offset("service"),
        offset("title"),
        offset("help"),
        0,                 # commands
        "init",
        0xE92D4100,        # STMFD sp!,{r8,lr}
        0xE3A0050F,        # MOV r0,#PACK_BASE
        0xEF061B40,        # SWI XResourceFS_RegisterFiles
        adr(0, "cmd1"),    # ADR r0,cmd1
        0xEF020005,        # SWI XOS_CLI
        adr(0, "cmd2"),    # ADR r0,cmd2
        0xEF020005,        # SWI XOS_CLI
        0xE3A00102,        # MOV r0,#&80000000 (add a dumper)
        0xE3A01000 | DUMPER_NUMBER,  # MOV r1,#number
        0xE3A02F00 | PDRIVER_VERSION >> 2,  # MOV r2,#version
        0xE3A03000,        # MOV r3,#0
        adr(4, "dumper"),  # ADR r4,dumper
        0xE3A050FF,        # MOV r5,#255 (branches)
        0xE3A06007,        # MOV r6,#7 (strip types)
        0xE3A08000 | PDRIVER_DP,  # MOV r8,#PDriverDP
        0xEF0A015B,        # SWI XPDriver_MiscOpForDriver
        0xE1500000,        # CMP r0,r0 (a failure must not fail the boot)
        0xE8BD8100,        # LDMFD sp!,{r8,pc}
        "cmd1",
        SCRAP_CMDS[0],
        "cmd2",
        SCRAP_CMDS[1],
        "service",
        0xE3510060,        # CMP r1,#&60 (Service_ResourceFSStarting)
        0x11A0F00E,        # MOVNE pc,lr
        0xE92D4001,        # STMFD sp!,{r0,lr}
        0xE3A0050F,        # MOV r0,#PACK_BASE
        0xE1A0E00F,        # MOV lr,pc
        0xE1A0F002,        # MOV pc,r2
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
        "title",
        title,
        "help",
        help_text,
    ])
    return body


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
