#!/usr/bin/env python3
"""Fetch Archimedes software collections and bundle them as an ADFS hard disc image.

The collections are read in SOURCES order and each file is downloaded once into the
cache. Their floppies (ADF, or JFD track dumps with the protection tracks skipped)
are unpacked onto the disc under Apps, Games, Demos or Compilatns, one folder per
title, and the disks of a multi-disk title share a folder.

The collections overlap heavily and spell titles differently, so duplicates are
found by what is on the disc as well as by name: a disk goes if its title and disk
number are taken, if nearly all of its bytes are already on the image, or if about
half are and its title is close to the one they came from. Sound dumps are placed
before damaged ones, so a broken copy only survives when nothing better exists. A
!Sprites that is not sprite data stops the Filer with an error box whenever the
folder around it opens, so a blank one is emptied and an app whose file holds other
files' bytes comes off the disc. A ReadMe in the root maps each folder back to
its archive name.

usage: archive-hd.py OUT [--cache DIR] [--limit N] [--refresh]

No-Intro's files are 7z: that source needs a 7z or 7zz binary, or the py7zr module,
and is skipped with a warning otherwise.
"""
import argparse
import difflib
import hashlib
import html
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request
import zipfile

import adfs

INDEX = ("https://ia903209.us.archive.org/view_archive.php?archive=/24/items/"
         "Acorn_Archimedes_TOSEC_2012_04_23/Acorn_Archimedes_TOSEC_2012_04_23.zip")
CACHE = "/tmp/archimedes-archive"
# archive.org items after the index above, best first
SOURCES = ("TOSEC-Acorn", "No-Intro_AA", "20240825_20240825_0053", "netmanyagi_acorn-archimedes", "Acron-Archimedes")
FOLDERS = {"Applications": "Apps", "Educational": "Apps", "Miscellaneous": "Apps", "Games": "Games",
           "Demos": "Demos", "Compilations": "Compilatns"}
# A floppy path: drive 0 or a disc by name, optionally through $, then the next name
FLOPPY_PATH = re.compile(rb"(?i)(?:adfs::[a-z0-9_-]+|(?:adfs:)?:0)\.(?:\$\.)?()([!a-z0-9_]+)")
NOT_APPS = {"!system", "!scrap", "!boot", "!fonts"}  # support folders, not things to run
DULL_FILES = {"!run", "!boot", "!sprites", "!sprites22", "!sprites23", "!help", "!runimage", "templates",
              "messages", "sprites"}
# Filed under Games by the collections that carry them
APPS = re.compile(r"^(impression|pen down|startwrite|the hacker|hacker, the|desktophacker|coconizer|alps)", re.I)
SKIP = re.compile(r"blank \d+k|catalogue|boot floppy|fileserver|^zzz", re.I)  # not software to run
SAME_BYTES = 0.9  # share of a disk already on the image that makes it a duplicate
LIKELY_SAME = 0.5  # the same, when its title is also close to the title they came from
SAME_TITLE = 0.8
DISC_NAME = "Archive"
BAD_DUMP = ("b", "o", "u", "v")  # TOSEC flags for dumps known to be broken
SPRITE_TYPE = 0xFFFF9  # load address top bits of a sprite file


def fetch(url, path, refresh=False):
    """Download url to path unless it is cached. Three tries, then give up."""
    if os.path.exists(path) and not refresh:
        return True
    os.makedirs(os.path.dirname(path), exist_ok=True)
    for attempt in range(3):
        try:
            with urllib.request.urlopen(url, timeout=60) as r, open(path + ".part", "wb") as f:
                f.write(r.read())
            os.replace(path + ".part", path)
            return True
        except Exception as e:  # noqa: BLE001 - any failure just means another try
            print("  retry %s: %s" % (os.path.basename(path), e), file=sys.stderr)
            time.sleep(2 * (attempt + 1))
    return False


def listing(cache, index_url, refresh):
    """[(download url, archive path)] for every zip in the archive's index page."""
    path = os.path.join(cache, "index.html")
    if not fetch(index_url, path, refresh) and not os.path.exists(path):
        sys.exit("cannot fetch the archive index and nothing is cached")
    text = open(path, encoding="utf-8", errors="replace").read()
    rows = []
    for m in re.finditer(r'<a href="(//archive\.org/download/[^"]+\.zip)">', text):
        url = html.unescape(m.group(1))
        rows.append(("https:" + url, urllib.parse.unquote(url.split(".zip/", 1)[1])))
    if not rows:
        sys.exit("the index page lists no zips; the archive's layout may have changed")
    return rows


def metadata_files(item, cache, refresh):
    """[(download url, file name)] for an archive.org item."""
    path = os.path.join(cache, "meta", item + ".json")
    url = "https://archive.org/metadata/" + item
    if not fetch(url, path, refresh):
        print("cannot list %s, skipping it" % item, file=sys.stderr)
        return []
    rows = []
    for f in json.load(open(path)).get("files", []):
        name = f["name"]
        if name.endswith((".zip", ".7z")) and (item != "TOSEC-Acorn" or name.startswith("Archimedes/")):
            rows.append(("https://archive.org/download/%s/%s" % (item, urllib.parse.quote(name)), name))
    return rows


def members(path):
    """[(name, bytes)] of the disc images in a downloaded archive."""
    images = (".adf", ".adl", ".img", ".jfd")
    if path.endswith(".zip"):
        z = zipfile.ZipFile(path)
        return [(m, z.read(m)) for m in z.namelist() if m.lower().endswith(images)]
    with tempfile.TemporaryDirectory() as tmp:
        tool = shutil.which("7zz") or shutil.which("7z")
        if tool:
            subprocess.run([tool, "x", "-y", "-o" + tmp, path], check=True, stdout=subprocess.DEVNULL)
        else:
            import py7zr  # noqa: E402 - optional, only for 7z sources
            with py7zr.SevenZipFile(path) as z:
                z.extractall(tmp)
        out = []
        for name in sorted(os.listdir(tmp)):
            if name.lower().endswith(images):
                with open(os.path.join(tmp, name), "rb") as f:
                    out.append((name, f.read()))
        return out


def can_unpack_7z():
    if shutil.which("7zz") or shutil.which("7z"):
        return True
    try:
        import py7zr  # noqa: F401
    except ImportError:
        return False
    return True


def parse_name(stem):
    """TOSEC or No-Intro name -> (title, disk number or 0, rank: 0 plain, 1 variant, 2 bad)."""
    title = re.split(r" \(|\[", stem, 1)[0].strip()
    title = re.sub(r",? [Tt]he$", "", title)
    disk = re.search(r"\(Disk (\d+)", stem)
    flags = re.findall(r"\[([a-z]+)[^\]]*\]", stem)
    rank = 1 if re.search(r"\((Alt|Beta|Proto)", stem) else 0
    for f in flags:
        if f in BAD_DUMP:
            rank = 2
        elif f in ("a", "cr", "h", "t", "f", "m", "p", "tr") and rank < 1:
            rank = 1
    return title, int(disk.group(1)) if disk else 0, rank


def title_key(title):
    """What is left of a title when spelling, articles and version numbers go."""
    title = re.sub(r" [vV]\d[\d.]*[a-zA-Z]?\b.*$", "", re.sub(r"^The ", "", title))
    return re.sub(r"[^a-z0-9]", "", title.lower())


def category_of(path, source):
    """The disc folder for a file, or None for what is not software to run."""
    parts = path.split("/")
    if source == "TOSEC-Acorn":
        return FOLDERS.get(parts[1])
    if len(parts) > 1 and parts[1].startswith("Acorn Archimedes - "):
        return FOLDERS.get(parts[1][len("Acorn Archimedes - "):].split(" (")[0])
    name = parts[-1]
    if re.search(r"multi-program|collection|compilation", name, re.I):
        return "Compilatns"
    return "Demos" if re.search(r"\bdemos?\b", name, re.I) and "(demo" not in name else "Games"


def file_hashes(tree):
    """{hash: size} of the files in a tree. Tiny ones say nothing about identity."""
    return {hashlib.sha1(e.data).digest(): len(e.data)
            for _, e in tree.walk() if not e.is_dir and len(e.data) >= 256}


def folder_name(title, taken):
    """A unique ADFS name for the title: the words run together, cut to ten.

    A cut prefix (OhNoMoreLe) reads better in a Filer window than dropped vowels,
    and keeps the alphabetical order of the full titles. The ReadMe has the rest."""
    words = re.findall(r"[A-Za-z0-9]+", title) or ["Untitled"]
    name = "".join(w[:1].upper() + w[1:] for w in words)
    number = re.search(r" (\d+)$", title)
    if len(name) > adfs.NAME_LEN and number:
        # Multi-Program 14 stays MultiPro14, not a numbered clash with the others
        name = name[:adfs.NAME_LEN - len(number.group(1))] + number.group(1)
    name = name[:adfs.NAME_LEN]
    base, n = name, 2
    while name.lower() in taken:
        suffix = str(n)
        name = base[:adfs.NAME_LEN - len(suffix)] + suffix
        n += 1
    taken.add(name.lower())
    return name


def legal(name):
    """Make a floppy's file name legal on the hard disc (spaces become hard spaces)."""
    out = "".join(chr(0xA0) if c == " " else ("_" if c in adfs.BAD_CHARS else c) for c in name)
    return out[:adfs.NAME_LEN] or "_"


def sanitise(node):
    """Rename everything under node to names the hard disc accepts."""
    children = list(node.children.values())
    node.children = {}
    for child in children:
        child.name = legal(child.name)
        if child.is_dir:
            sanitise(child)
        node.add(child)


def sprite_ok(data):
    """Whether the sprites a sprite file's header promises are all in the file."""
    if len(data) < 12:
        return False
    count, at = struct.unpack_from("<II", data)
    at -= 4  # offsets count from the area's size word, which files leave out
    if at < 12:
        return False
    for _ in range(count):
        if at + 4 > len(data):
            return False
        size = struct.unpack_from("<I", data, at)[0]
        if not size or at + size > len(data):
            return False
        at += size
    return True


def prune_broken(node, path, problems):
    """Take off every !Sprites that would raise an error when the Filer sees it.

    Only sprite-typed files count: packed titles keep theirs under their own types.
    Zeros are a lost sector, so the app is probably fine and only loses its icon. It
    gets a valid file with no sprites in it, as a !Boot may *IconSprites the name.
    Anything else means the directory points at other files' bytes, and the rest of
    the app is no better (the Application Suite's second disk does this to !Puzzle,
    !TinyDirs and !Usage in every dump)."""
    for child in list(node.children.values()):
        if not child.is_dir:
            continue
        where = path + "." + child.name
        sprites = next((e for e in child.children.values()
                        if e.name.lower() == "!sprites" and not e.is_dir), None)
        if sprites and (sprites.load >> 8) & 0xFFFFF == SPRITE_TYPE and not sprite_ok(sprites.data):
            if any(sprites.data[:12]):
                del node.children[child.name.lower()]
                problems.append("%s: dropped, its !Sprites is not sprite data" % where)
                continue
            sprites.data = struct.pack("<III", 0, 16, 16)
            problems.append("%s: !Sprites emptied, it is blank" % where)
        prune_broken(child, where, problems)


def merge(dest, src, into_disk=None):
    """Copy src's children into dest. A clash that is not the same bytes goes under DiskN."""
    for child in src.children.values():
        have = dest.get(child.name)
        if have is None:
            dest.add(child)
        elif have.is_dir and child.is_dir:
            merge(have, child, into_disk)
        elif not have.is_dir and not child.is_dir and have.data == child.data:
            continue
        elif into_disk:
            dest.subdir(into_disk).add(child)


def rewrite_path(data, root, names):
    """data with floppy paths into this title pointed at its folder on the hard disc."""
    def swap(m):
        return root + b"." + m.group(2) if m.group(2).lower() in names else m.group(0)
    return FLOPPY_PATH.sub(swap, data)


def rewrite_basic(data, root, names):
    """The same inside a tokenised BASIC program, keeping each line's length byte."""
    out = bytearray()
    at = 0
    while at + 3 < len(data) and data[at] == 13 and data[at + 1] != 0xFF and data[at + 3] >= 4:
        size = data[at + 3]
        body = rewrite_path(data[at + 4:at + size], root, names)
        if len(body) + 4 > 255:  # no room: this line keeps its floppy path
            body = data[at + 4:at + size]
        out += data[at:at + 3] + bytes([len(body) + 4]) + body
        at += size
    return bytes(out) + data[at:]


def relocate(title, path):
    """Floppies assume they are drive 0. Point their Obey, Exec and BASIC files at the
    title's folder instead, when the path leads to something in it. Paths inside
    compiled code keep pointing at the floppy, as they cannot change length."""
    root = ("adfs::%s.$.%s" % (DISC_NAME, path)).encode("latin-1")
    names = {c.name.lower().encode("latin-1") for c in title.children.values()}
    moved = 0
    for _, e in title.walk():
        kind = (e.load >> 8) & 0xFFF if e.load >> 20 == 0xFFF else None
        if e.is_dir or kind not in (0xFEB, 0xFFE, 0xFFB):
            continue
        new = rewrite_basic(e.data, root, names) if kind == 0xFFB else rewrite_path(e.data, root, names)
        moved += new != e.data
        e.data = new
    return moved


def write_index(root, titles):
    """The Index file !Find reads: folders, then a line per title, app and file.

    Line one is the folder count and the next lines are those folders, so a file
    needs only a number. Then, separated by tabs and ending in CR, D or R, the name,
    what to show beside it and a path (R when it can be run, D for a folder only), or
    F, the name and a folder number. Titles come first, as results follow this order."""
    folders, numbers, tops, apps, files = [], {}, [], [], []
    for path, entry in root.walk():
        if entry.is_dir:
            numbers[path] = len(folders)
            folders.append(path)
    def owner(path):
        while path and path not in titles:
            path = path.rpartition(".")[0]
        return titles.get(path)
    for path in sorted(titles, key=lambda p: titles[p][1].lower()):
        entry, title, folder = titles[path]
        runnable = [c for c in entry.children.values() if c.is_dir and c.name.startswith("!")
                    and c.name.lower() not in NOT_APPS]
        kind, target = ("R", path + "." + runnable[0].name) if len(runnable) == 1 else ("D", path)
        tops.append("%s%s\t%s\t%s" % (kind, title, folder, target))
    sole = {t.rsplit("\t", 1)[1] for t in tops}
    for path, entry in root.walk():
        if path == "ReadMe" or path in titles or path.count(".") < 1:
            continue
        where = owner(path)
        if entry.is_dir:
            if entry.name.startswith("!") and entry.name.lower() not in NOT_APPS and path not in sole:
                apps.append("R%s\t%s\t%s" % (entry.name, where[1] if where else "", path))
        elif entry.name.lower() not in DULL_FILES:
            files.append("F%s\t%d" % (entry.name, numbers[path.rpartition(".")[0]]))
    lines = [str(len(folders))] + folders + tops + apps + files
    return ("\r".join(lines) + "\r").encode("latin-1", "replace")


def split_big(folder):
    """Directories hold 77 entries; past that, titles go under alphabetical ranges."""
    if len(folder.children) <= adfs.DIR_ENTRIES:
        return
    titles = sorted(folder.children.values(), key=lambda e: e.name.lower())
    groups = -(-len(titles) // adfs.DIR_ENTRIES)
    per = -(-len(titles) // groups)
    folder.children = {}
    for i in range(0, len(titles), per):
        chunk = titles[i:i + per]
        first, last = chunk[0].name[0].upper(), chunk[-1].name[0].upper()
        sub = folder.subdir(first if first == last else "%s-%s" % (first, last))
        for t in chunk:
            sub.add(t)


def candidates(args):
    """One tuple per disc image in every source, in the order they should be tried."""
    found = []
    sources = [("TOSEC", listing(args.cache, args.index, args.refresh))]
    for item in SOURCES:
        if item == "No-Intro_AA" and not can_unpack_7z():
            print("no 7z tool or py7zr, skipping %s" % item, file=sys.stderr)
            continue
        sources.append((item, metadata_files(item, args.cache, args.refresh)))
    for order, (source, rows) in enumerate(sources):
        for url, path in rows:
            folder = category_of(path, source)
            leaf = os.path.basename(path)
            if not folder or SKIP.search(leaf):
                continue
            if APPS.search(leaf):
                folder = "Apps"
            where = "zips" if source == "TOSEC" else os.path.join("sources", source)
            local = path.replace("/", "__") if source != "TOSEC" else leaf
            zip_path = os.path.join(args.cache, where, local)
            if not fetch(url, zip_path):
                print("  cannot download %s" % leaf, file=sys.stderr)
                continue
            try:
                parts = members(zip_path)
            except Exception as e:  # noqa: BLE001 - a bad archive just loses that title
                print("  unreadable %s: %s" % (leaf, e), file=sys.stderr)
                continue
            for member, raw in parts:
                stem = os.path.splitext(os.path.basename(member))[0]
                problems = []
                try:
                    if member.lower().endswith(".jfd"):
                        image, holes = adfs.read_jfd(raw)
                        tree = adfs.read_floppy(image, problems, holes)
                    else:
                        tree = adfs.read_floppy(raw, problems)
                except (ValueError, IndexError, KeyError, OSError, struct.error) as e:
                    print("  unreadable %s: %s" % (stem, e), file=sys.stderr)
                    continue
                prune_broken(tree, "$", problems)
                if not tree.children:
                    continue
                title, disk, rank = parse_name(stem)
                damaged = rank == 2 or bool(problems)
                found.append((damaged, order, rank, folder, title, disk, stem, tree, problems))
    found.sort(key=lambda c: c[:3])
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("out")
    ap.add_argument("--cache", default=CACHE)
    ap.add_argument("--index", default=INDEX)
    ap.add_argument("--limit", type=int, default=0, help="only the first N titles (for testing)")
    ap.add_argument("--refresh", action="store_true", help="fetch the listings again")
    args = ap.parse_args()

    titles = {}  # title key -> [folder, title, {disk: tree}, [sources]]
    seen = {}  # file hash -> title key that brought it
    dropped = {"same title": 0, "same bytes": 0}
    for damaged, _, _, folder, title, disk, stem, tree, problems in candidates(args):
        key = title_key(title)
        if key in titles and disk in titles[key][2]:
            dropped["same title"] += 1
            continue
        hashes = file_hashes(tree)
        total = sum(hashes.values()) or 1
        overlap = {}
        for h, size in hashes.items():
            if h in seen and seen[h] != key:
                overlap[seen[h]] = overlap.get(seen[h], 0) + size
        shared = sum(hashes[h] for h in hashes if h in seen and seen[h] != key) / total
        near = max(overlap, key=overlap.get, default=None)
        # Numbers differ between sequels, which often reuse most of their predecessor
        close = (near and difflib.SequenceMatcher(None, near, key).ratio() >= SAME_TITLE
                 and re.sub(r"\D", "", near) == re.sub(r"\D", "", key))
        if shared >= SAME_BYTES or (close and shared >= LIKELY_SAME):
            dropped["same bytes"] += 1
            print("  duplicate of %s: %s" % (near, stem), file=sys.stderr)
            continue
        for p in problems:
            print("  %s: %s" % (stem, p), file=sys.stderr)
        entry = titles.setdefault(key, [folder, title, {}, []])
        entry[2][disk] = tree
        entry[3].append(stem + (" (damaged)" if damaged else ""))
        for h in hashes:
            seen.setdefault(h, key)

    root = adfs.Entry("$", attrs=0x08)
    readme = ["DreamRISC software archive", "", "Folder: archive name", ""]
    taken = {}
    chosen = sorted(titles.values(), key=lambda t: (t[0], t[1].lower()))
    if args.limit:
        chosen = chosen[:args.limit]
    done = 0
    bundled = {}
    for folder, title, disks, stems in chosen:
        name = folder_name(title, taken.setdefault(folder, set()))
        dest = adfs.Entry(name, attrs=0x08)
        for disk in sorted(disks):
            tree = disks[disk]
            sanitise(tree)
            merge(dest, tree, "Disk%d" % disk if disk else None)
        root.subdir(folder).add(dest)
        bundled[id(dest)] = title
        readme.append("%s.%s: %s" % (folder, name, "; ".join(stems)))
        done += 1
    for folder in root.children.values():
        split_big(folder)
    moved = 0
    titled = {}
    for path, entry in list(root.walk()):
        if id(entry) in bundled:
            moved += relocate(entry, path)
            titled[path] = (entry, bundled[id(entry)], path.split(".")[0])
    print("  rewrote floppy paths in %d files" % moved, file=sys.stderr)
    text = ("\n".join(readme) + "\n").encode("latin-1", "replace")
    root.add(adfs.Entry("ReadMe", 0xFFFFFFFF, 0, 0x13, text))
    root.add(adfs.Entry("Index", 0xFFFFFFFF, 0, 0x13, write_index(root, titled)))

    image, log2_bpmb = adfs.build_smallest(root, DISC_NAME)
    with open(args.out, "wb") as f:
        f.write(image)
    with open(args.out + ".txt", "w") as f:
        f.write("\n".join(readme) + "\n")
    print("%s: %d titles, %d MB, %d byte map units; dropped %d by title and %d by content"
          % (args.out, done, len(image) >> 20, 1 << log2_bpmb, dropped["same title"], dropped["same bytes"]))


if __name__ == "__main__":
    main()
