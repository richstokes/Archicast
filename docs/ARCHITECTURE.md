# Architecture

## Problem Statement

Dream-TOS could compile EmuTOS because it is C. RISC OS 3.11 only exists as 2MB of 26-bit ARM code, and the goal is a native port with no emulator or JIT.

11/1 update: that still holds for RISC OS and bundled apps, and there is still no JIT. Legacy software from disc is the one exception, and it runs through an ARM interpreter. See [Code that runs from RAM](#code-that-runs-from-ram).

## Proposal

1. `tools/romscan.py` maps the ROM (kernel plus 88 modules) and finds code
2. `tools/armxlat.py` turns every reachable ARM instruction into C. Blocks are functions joined by `musttail` calls, so ARM calls never use the host stack
3. `src/mach.c` is the machine layer: PSR and register banks, page tables, and native drivers behind the old MEMC, IOC, VIDC, keyboard and CMOS register addresses
4. `src/dc/main.c` is KOS video and Maple input. `src/host/main.c` is a Mac harness that runs the same generated C with deterministic time

## How code gets found

Boot traces come from a dev-only hook in Arculator (`ARC_PCMAP`). Static discovery adds module entry points, command tables, ADR targets, jump tables, stored ROM addresses and `lr`-saving prologues. Network and printer modules are skipped unless a trace proves they run.

A jump to code that was not recompiled stops the harness with its address. `tools/converge.sh` turns those stops into `trace/seeds.txt` and `trace/ramwords.txt`. Builds with the interpreter fall back to it instead, which is the Dreamcast default. With `INTERP=` the console raises a prefetch abort, so RISC OS reports an error and only the task dies.

## Code that runs from RAM

RISC OS copies and generates a little code at runtime (the IRQ head, SWI stubs, sound voices, BASIC's `SYS`, SharedCLibrary branch tables).

`src/interp.c` runs all of it, apart from recompiled apps. It fetches, decodes and executes one ARM instruction at a time, and shares machine state with recompiled code, so SWIs from a game land in the native ROM. It also catches ROM paths the static scan missed. The Dreamcast build carries it by default, and it's why unmodified Archimedes binaries run from disc.

**This is an emulated ARM CPU, so to be plain about it:** anything without a recompiled block runs this way. It's slow, as every instruction is decoded each time it runs. Nothing is generated at runtime, so it's not a JIT. RISC OS and bundled apps stay native.

Without the interpreter (`INTERP=`, or the default harness), each known instruction word gets one precompiled routine that takes its PC as an argument, looked up by word. SWI words, branch words and the `LDMIA r12!,{list}` that C apps build on the stack for SWI veneers share a routine each, since those read the word for their number, offset or register list. Unknown words do not run there, so new ARM binaries cannot either.

## Disc streaming

The disc holds `rom.bin` and `ov_N.bin`. Under dcload the console reads the same files over `/pc`.

- ROM data: hot pages load into RAM at boot; 12 cold pages stream through an 8-slot cache, using the page tables. Fewer are cold than you'd hope. The kernel walks module headers on every service call and ResourceFS scans Messages and ROMFonts. Those cold pages also go hot once `!Draw` runs (28k disc reads with 3 slots, 21 with 8), so only ~130KB leaves RAM
- Code: blocks outside the boot trace in `!Draw`, `!Paint` and a misc group (printer dumpers, network, DOSFS) become overlay units linked to one fixed region. A dispatch miss loads the unit. Blocks keep no native stack, so swapping at any dispatch is safe. Boot never loads code from disc
- SharedCLibrary, BASIC and FileCore stay resident on purpose, as every app uses them and paging them would mean a seek per task switch

Console check (10/31): the `tests/draw.replay` drawing session matched the harness at both checkpoints, with its two key events removed on both sides because hardware replays do not support keys yet.


Net saving is ~0.9MB. Running `!Draw` and `!Paint` together will reload a unit each time focus moves between them, ~1s each: untested and it will be ugly.

Machine state lives in r14 on SH-4 (`-ffixed-r14` is required, or GCC borrows the register) which saves ~0.5MB and gained ~14% idle throughput on the console.

## Bundled apps

A dropped ARM binary runs through the interpreter, which is far slower than native code, so a bundled app is recompiled like the ROM. `apps/` has one folder per app and [apps/README.md](../apps/README.md) covers the manifest. There are two parts, and neither loads at boot:

- Files: `tools/mkapps.py` appends a ResourceFS pack to `rom.bin`, mapped at `0x3C00000` through the cold page cache. A small `DreamApps` module, added to the ROM's module chain, registers it, so the app appears under `Apps` like any other. Every ResourceFS lookup walks every file header, so the pack keeps headers in its first page and files only cost a disc read when opened. Boot reads that one page
- Code: each app is an overlay unit. It runs at its load address, which other tasks share, so a unit only starts from its entry point with the right instruction there, and entries from outside are checked against the instruction they were compiled from

Decision: extend the Apps folder, not a new filing system. A virtual drive would need a FileSwitch filing system plus a Filer icon, and it would get us nothing until a game needs more than 2MB of files, the size of the pack window. Down the road a real drive is the way to go for that, and for the game's own save files.

App units are small enough to spend code on speed. Each is one file at `-O2`, memory access and `LDM`/`STM` are inline, and returns go through a direct map. On the console that took Lander's heavy scene from 25 to ~48 frames/s, worst case 17 to 40, for 47KB to 93KB of unit, which only exists in memory while the app runs.

Apps can be far bigger than Lander. Tertis has ~20k instructions, or ~840KB of unit against the 1.1MB region, and code from `ADD`/`SUB` address pairs and `STMFD ...,lr` prologues is walked as well. Everything translated flow dispatches to has to be an entry too, as one failed lookup marks the app as stopped.

Lander's physics run per frame, with only the vsync wait to limit them, so that speed made it far too fast. `code.frame_us` sets the app's vertical sync period, without touching OS timers. Lander uses 25Hz. An 8MHz ARM2 would manage ~15 frames/s going by ~150-290K ARM instructions a frame at ~4 MIPS, which is an estimate and not a measurement on real Acorn hardware, so it's probably still a bit quicker than the original. Open question: does it feel right on a pad?

BASIC apps skip all of that. `!Logo` is tokenised BASIC in the pack, run by the ROM's BASIC, so it has no unit and no `code` block. The cost is speed: it's an interpreter in an interpreter. The Tree demo took ~15-20s in Flycast, which is not a hardware timing. New BASIC statements can still reach ROM code that was never recompiled, so `tools/converge.sh` applies to BASIC apps as well.

The Logo canvas is a mode 27 sprite plotted into a window with no background. With a white background the Wimp clears the canvas before each plot, which flickered badly while a program ran. A running program polls the Wimp every 4cs from inside the interpreter, so the desktop and the Stop button stay live.

Flycast check (10/31): launched Logo and picked Tree from the menu from a disc image, matching the harness at all four checkpoints, and a full framebuffer dump looks right. One earlier dump showed the turtle still on screen after `HT` with matching checksums. Two more runs were clean and both buffers matched guest memory, so no idea why, but keep an eye on it. Not run on a real console yet, and typing has only run on the harness, which sometimes drops a key around the 15th character of a `type` line since the rebase. Cause unknown.

## Storage

`src/fdc.c` is a native driver behind the WD1772 registers, so RISC OS's own ADFS and FileCore run unchanged, including their FIQ byte transfers. The disc (`assets/adfs.adf.gz`) is an 800KB ADFS E image that RISC OS formatted itself on the harness via `tests/format.replay`. It loads into RAM at boot, so writes work but vanish at power off. Only 9 of its 800 sectors hold real data, so blank ones just record their fill byte and get memory when first written. That saves ~790KB.

Things that bit us, for whoever touches this next:

- IOC control bit 2 set means a disc is present
- Sector IDs have to arrive at about 40ms each. ADFS collects a track's IDs into a 512-byte stack buffer until a timer says one revolution passed, so answering instantly overruns the buffer and smashes the stack
- ADFS copies a different FIQ routine to the vector page per operation, and runs one from the SVC stack. Those words are in `trace/ramwords.txt`

Harness: `DREAM_ADF` loads an image (a missing file means blank), `DREAM_ADF_OUT` saves it on exit, and the replay verb `type` drives the keyboard.

Console check (10/31): opened `:0` in the Filer with the checksum matching the harness. Reading from a physical disc or GDEMU is untested, and writing has only run on the harness.

## Display

`assets/cmos.bin` sets MonitorType 3 (VGA), WimpMode 27 and clears the auto mode bit (logical `0xC3` bit 4), so the desktop boots at 640x480 and draws to the Dreamcast framebuffer 1:1. Hand patched and checksummed, as `*Configure` isn't in the recompiled code.

It also sets WimpFlags to 15 (physical byte `0x15`). The usual 111 lets a window be dragged fully off the bottom, where neither the pointer nor a click on its icon can get it back.

Colours follow Arculator's default Acorn black level, `(n-3)*255/12` per 4-bit VIDC level, so levels 0-3 are all black. A straight 4-bit expansion looks washed out next to a real machine.

There's no boot disc, so nothing runs !Scrap. `DreamApps` sets `Wimp$ScrapDir` and `Wimp$Scrap` at init instead, pointing at `ADFS:$`, as the C library, Draw, Edit and Paint all expect them. With a card the default drive becomes `:4`, as the floppy isn't there. Scrap files land in the root of the disc, which is a bit untidy but needs no directory to exist. The harness can save CMOS with `DREAM_CMOS_OUT`.

A framebuffer dump in Flycast (`HW_DUMP=1`) shows the desktop sharp at full size with the right colours.

The old mode 12 screen was only 256 lines, so this nearly doubles the lines summed per frame. Present stays ~4% of the frame budget in Flycast. It costs ~70KB of guest RAM for the bigger screen.

Things to watch out for:

- Windows sit ~16 pointer units lower than they did in mode 12, while the icon bar stays anchored to the bottom. The replays were moved to match
- On a TV cable KOS outputs 640x480 interlaced, so 1 pixel lines will flicker, where the line doubled 256 line modes did not. VGA is fine. Untested on a real TV, and if it's bad the answer is probably to pick mode 12 at boot by cable type
- VSync stays at 50Hz, though real mode 27 is 60Hz. Nothing seems to care

## Sound

The ROM's own SoundDMA, SoundChannels and voice modules run unchanged. A native driver in `src/mach.c` sits behind the MEMC sound registers: at each buffer swap it converts VIDC's log samples to 16-bit stereo using the stereo image registers and hands them to `plat_sound`. Several channels fold into one output frame, so 8 channels at a 6us period still output at 20.8kHz.

On the Dreamcast, a ring feeds a KOS `snd_stream` from its own thread, so sound keeps going while the machine blocks on disc, and a short gap holds the last frame instead of clicking. Latency is ~100-150ms. That thread runs inside KOS, where r14 is not the machine state, so it must never touch `C`.

Mid-buffer changes are not modelled, as buffers go out whole, and the VIDC clock is assumed to be 24MHz. SoundDMA's 48us period gives the right pitch in mode 27 regardless.

Harness: `DREAM_WAV` records output. `tests/sound.replay` plays middle C from BASIC and `tools/soundcheck.py` checks its pitch (258Hz measured, by counting zero crossings). Lander has no Sound SWIs at all, so it's silent, as it should be.

Console check (10/31, Flycast only): checkpoints matched the harness, and over 60s the ring stayed in step at 20.8kHz with no gaps after start up. Nobody has listened to it yet.

## SD hard disc

`src/ide.c` is a native ATA drive at the IDE registers ROM ADFS already drives (`0x30107C0`, IOC IRQB bit 3). Sectors map 1:1 onto SD blocks, read with KOS's `sd_read_blocks`. At boot, a detected card sets `IDEDiscs` in CMOS (top bits of byte 0x87, checksum at 0x3F) and the floppy image is not loaded.

ROM patch: ADFS only starts IDE on IOEB machines or if a podule claims it, and reporting IOEB would move the floppy to the 82C711 path. So `tools/armxlat.py` NOPs that one branch at recompile time, and checks the old word first.

`src/hdformat.c` writes the disc, as HForm is not in ROM: 120 zones, 1K map units, ~476MB. Map and directory checksums were checked against a disc RISC OS formatted itself. `*verify 4` passes, on the harness and on a real 4GB card.

Things that bit us:

- ADFS reads sector 1 by polling with nIEN set, then re-enables interrupts and waits. The drive has to latch an interrupt raised while masked, or the transfer hangs
- A read failure must never count as "unformatted". `hd_is_formatted()` returns -1 for that, and only a readable, wrong boot block offers to format

Console results (10/31): real card formatted, written, power cycled and read back, with `Work` still there and every checkpoint matching the harness. Boot block detection was stable over 4 more power cycles.

On the final merged build (sound, 640x480) the card test passed by eye from a console screen dump: `Work` persisted, spool wrote, `Verified OK`. Its checksum differed from the harness only because one scripted space was dropped (`*cdiradfs::4...`). That may be a real keyboard timing issue while the disc is busy, so check it if typing ever loses characters.

Unresolved: during bring-up the second boot looked freshly formatted, which suggests the first check said "not formatted" on a card that had just been formatted. That build could format unattended. I never found out why and it has not recurred, but treat it as open while the card holds anything you care about.

Not tested: the format prompt on a real controller or keyboard (hardware tests use `HW_AUTOFORMAT`), SD speed under load, and pulling the card out while running.

## Software archive

A second IDE drive carries Archimedes software as a read-only ADFS hard disc, `archive.hd` on the CD. `src/ide.c` now models master and slave: the SD card is the master when there is one and the archive the slave, otherwise the archive is the master on its own, and `IDEDiscs` in CMOS is set to however many are there. Writes to the archive fail with an abort error. On the Dreamcast, reads go through a 32KB chunk cache in `src/dc/main.c`, since ADFS asks for a sector at a time and the GD-ROM is slow to seek. The harness takes the image as `DREAM_HD_RO`.

`tools/archive-hd.py` builds the image from the TOSEC 2012 set plus `SOURCES`, five collections on archive.org in all, which between them hold ~1300 disc images and ~260 distinct titles. It downloads each file once into `/tmp/archimedes-archive`, reads the floppies with `tools/adfs.py` and lays them out under `Apps`, `Games`, `Demos` and `Compilatns`, one folder per title, split alphabetically past 77 entries. Multi-disk titles merge into one folder and a `ReadMe` in the root maps folders back to archive names.

Duplicates go in two passes, sound dumps first so a damaged copy only survives when nothing better exists. Same normalised title and disk number drops ~930 images. The rest are compared by SHA-1 of their files: 90% of a disk's bytes already on the image drops it, as does 50% when the title is also close (`Air Supremecy`, `Air Supremacy`) and carries the same numbers, since sequels reuse their predecessor's files. That catches ~85 more. Run the tool and read its stderr to see each decision.

A `!Sprites` of type FF9 that isn't sprite data gets a `Bad sprite file` box from the Filer every time the folder around it opens, so `prune_broken` deals with those before any of this. A blank one (Simon's `!Update`, a lost sector) becomes a valid file with no sprites, as its `!Boot` loads it by name. Anything else means the directory points at other files' bytes, and the app goes: the end of the Application Suite's disk 2 is mangled in every dump, which costs us `!Puzzle`, `!TinyDirs` and `!Usage`. Packed titles keep theirs under other types and are left alone.

The interpreter models one piece of the ARM pipeline for this software: a word store over the next instruction leaves the old instruction to run, which Elite's decrypter depends on.

Floppy software often names its own disc (`adfs::0.!Sp_Spell`), which fails from a hard disc. `relocate` rewrites those paths to the title's folder in Obey, Exec and BASIC files, ~90 of them, but only when the next name in the path exists there. Paths in compiled code can't change length so they stay, and Chocks Away still says `Drive empty`.

`!Find` searches a ~240KB `Index` file in the root of the disc, which `write_index` builds: every folder, then a line per title, app and file, ~11,600 in all, with files pointing at a folder number to save ~700KB. Scanning that in BASIC would take seconds, so `DN_FIND` does it natively and the app is only the windows. `DN_FIND_DRIVE` says which drive is the archive, as it's the one that can't be written, and asking for a disc by name that isn't there would prompt for it. SD and floppy have no index, so those are walked a folder per poll.

Most of the No-Intro set and some of TOSEC are JFD, a track dump that keeps copy protection. `read_jfd` takes the standard sectors by id and records which are missing, and a file on a missing sector counts as damage. Gods came through that way and runs, but anything that checks its protection at runtime will fail, as `fdc.c` has no protected tracks to show it.

`tools/adfs.py` writes the same new-map layout as `hdformat.c`, with the files in place. Things FileCore insists on, each found the hard way with `*CheckMap`:

- With map units smaller than a sector, every fragment has to start and end on a sector boundary, or directories read back broken
- Every zone needs a free fragment; a full zone makes the disc "not understood"
- Small objects may only share a fragment with their siblings. `*CheckMap` frees a whole fragment once no sibling shares it, then spins forever looking for it when the next directory's file turns up. So the builder bins small files by parent directory
- Reading floppies: free fragments' id fields are links, not ids, and object 2 starts at the map zone, as on an F disc the boot block is part of it too
- An empty file has disc address 1 (id 0) and no fragment

`*CheckMap 4` reports `Map good` on a 40-title disc. It has not finished on a full disc, since it rescans the whole map for each object, and an earlier 136-title disc was still walking the tree after 15 minutes.

## Network

RISC OS 3.11 has no TCP/IP in ROM, and Acorn's Internet module was a disc-based ARM binary. So the stack is native (KOS) and the guest reaches it through DreamNet SWIs at `&CDC00`, a chunk from the range RISC OS leaves to users. `src/net.c` catches them in `swi_insn` before the kernel sees them. Results come back in r0 as a value or -errno, and there's no V-set error path, as nothing guest-side owns memory for an error block. We skipped Acorn's `Socket_*` numbers since no existing binary could call them anyway.

| SWI | Name | Registers |
|---|---|---|
| +0 | Status | r0 -> 80 byte block: state, link, ip, mask, gateway, dns, mac, device, text |
| +1 | Connect | r0 -> 80 byte config |
| +2 | Disconnect | |
| +3 | Ping | r0 = ip, r1 = seq |
| +4 | PingResult | r0 = seq; out r0 = 1 if answered, r1 = us, r2 = ttl |
| +5 | Resolve | r0 -> name |
| +6 | ResolveResult | out r0 = 1 ok, 0 waiting, -1 failed; r1 = ip |
| +7 | GetConfig | r0 -> 80 byte block |
| +8 | SaveConfig | r0 -> config, written to a VMU |
| +9 to +16 | SockOpen, Bind, Connect, Send, Recv, SendTo, RecvFrom, Close | non-blocking; -35 means try again |
| +25 | PrintDump | the dumper veneer, see Printing; out r0 = 0 ok, 1 call again, 2 failed |
| +26 to +28 | PrintGetConfig, PrintSetConfig, PrintSaveConfig | r0 -> 64 byte block: paper, host |
| +29 | PrintStatus | r0 -> 60 byte block: state, pages, bytes, text |

Addresses are one word, most significant byte first. Blocks are laid out in `src/net.h`.

The abstraction is a table of links in `src/dc/net.c`. KOS's `netif_t` already hides the BBA, LAN Adapter and W5500, and PPP registers as a netif too, so a link only says how to bring itself up:

- Auto tries BBA then LAN, with DHCP, from power on. No setup needed. `!Ping` defaults to the gateway it got
- Modem dials, then runs PPP (`libppp`). A DreamPi is just a PPP peer, so `111-1111`, `dream`, `dreamcast` work OOTB. Never automatic, as it ties up a phone line. Pick it in `!NetSetup` and Save
- W5500 has to be picked by name. Its probe flips SCIF into SPI, which would upset a serial console, and it refuses outright if an SD card holds the port. One serial port, so it's W5500 or SD hard disc, not both

Things that bit us, for whoever touches this next:

- Everything blocking runs on one worker thread, which must never touch `C`: r14 only holds machine state on the main thread. `src/net.h` stays free of `cpu.h` for that reason
- `net_init` waits up to 10s for link, so it can't run at boot on the main thread. The exception is dcload-ip, where KOS has to take the BBA over before anything uses it. `arch_init_net_dcload_ip` then carries dcload's traffic, and `/pc` keeps working
- KOS's dialler stops at any character with no tone, so dashes are stripped
- Never print on the receive path, including the ICMP echo callback. Under dcload a `printf` travels through that same path and waits for itself, which froze the whole console at the first ping reply
- BASIC 1.05 only tokenises text from `*BASIC -load`, and prints "Program renumbered" over the desktop while it does. `tools/basic.py` tokenises at build time. Its output matched BASIC's own `SAVE` byte for byte on both apps

The harness has a scripted link in virtual time (`src/host/net.c`), so replays checksum the apps. Sockets are shared code in `src/net_sock.c`.

Memory: ~100KB of code, and ~120KB of heap once the BBA is up, leaving ~900KB free.

Console (10/31), real BBA under dcload-ip: the KOS takeover works, `/pc` and the log keep running, DHCP gave the address and gateway, and a native test pinged the gateway 12 of 12 times at 2-3.6ms. `!Ping` opened on the real desktop and filled in the gateway.

Open question: under dcload a scripted `!Ping` launch only opened 1 time in 3, with no error, and that one time Return then did nothing. ROM page sums matched Flycast, an SD card makes no difference on the harness, and the F12 line typed correctly, so it's not corrupt data or a bad command. I suspect dcload sharing the BBA, since nothing else here differs and it never happened in Flycast or on the harness, but that's unproven. Next step is booting the CDI from GDEMU with no dcload at all. `!Ping` driven by hand on a console, Modem on a real DreamPi, and the W5500 are all untested.

Flycast (10/31): `!Ping` launched by replay and ran through the real KOS driver against the emulated BBA (DHCP, ICMP replies at 2-22ms) and emulated modem (PPP at 33600, replies at 60-80ms).

## Browser

`!Web` is written from scratch. NetSurf is the obvious port, but it's far too big for 4MB of guest RAM, and every ARM binary it needs would have to go through the recompiler. A BASIC app can't lay out a page at a usable speed, so the split matches the network layer: `src/web.c` is native and runs on its own thread, and the app only draws lines it has already laid out, through SWIs +17 to +22 (Open, Back, Status, Info, Line, Click).

- Pages become a fixed grid in the system font, as lines of styled runs. Text, links, headings, lists, tables as rows, `pre`, entities, UTF-8 folded to Latin-1. No images, CSS or scripts
- HTTP/1.1 with redirects, meta refresh and chunked bodies, parsed as they arrive. Google's home page is ~87KB of mostly script, so buffering first left nothing visible. Now raw page size doesn't matter, only how much text there is. HTTPS is mbedTLS from kos-ports
- Forms: text boxes, hidden fields and buttons, GET and POST. Clicking a text box opens a small editor and Return submits. No checkboxes, radios or selects, and no cookies, so no logins
- Words in the address bar search with DuckDuckGo Lite. Google's results need JavaScript, which no browser here can run, so searches from its box go to DuckDuckGo too (`search_elsewhere`)
- Up and Down scroll a few lines, Page Up and Down a screen
- **Certificates are not checked.** There's no CA store and no trusted clock, so HTTPS stops a passive listener but doesn't prove who you're talking to. Fine for reading, not for anything private, and the home page says so
- Memory: mbedTLS adds ~410KB of code. Connections are set up first, then the page takes the largest of 256KB to 32KB of text that leaves room for the network, and a longer page stops with a note. Page loads in a row in Flycast showed no leak

The harness has no TLS, but its sockets are real: `tests/webserver.py` serves a redirect, chunked text, links, forms, a 300KB script and a very long page. Replays in the checked set only use the built-in `about:home`, so they need no server.

Things that bit us:

- KOS `malloc` isn't zeroed where the Mac's fresh memory happened to be. A missing `line_at[0] = 0` hung the console and not the harness
- KOS gives every TCP socket 64KB each way, ~130KB a connection, which failed with `ENOBUFS` when the floppy still filled the heap. `SO_RCVBUF` before connecting leaks, as `connect()` overwrites the pointer, so `--wrap=realloc` hands KOS a placeholder for that one call (`plat_sock_tune`). Sockets are now 16KB in and 4KB out
- newlib's errno values aren't BSD's, so `net_sock.c` maps the ones that drive control flow
- In BASIC an uppercase name starting with a keyword tokenises as that keyword, so `LINEH` is `LINE H`

Console (10/31), real BBA under dcload, with the fetch started natively: info.cern.ch, and over HTTPS example.com, text.npr.org and en.wikipedia.org (cut at 96KB, 352 lines), with the desktop running throughout. Without an SD card, Flycast fetched https://dcvmu.com/ (270 lines). One earlier Flycast run died in its own process during a fetch, which hasn't happened since.

Flycast (10/31), no SD card, the real internet: Google's home page with its search box, DuckDuckGo Lite, a typed search with full results and Hacker News. Wikipedia's Dreamcast article reached 734 lines before the floppy went sparse, on the smallest page size.

Open question: scripted typing in hardware replays loses bursts of characters while `!Web` runs (`news.ycombinator.com` arrived as `news.ynator.om`). Whether a real keyboard can do this is unknown.

Open question: this is the same launch problem as `!Ping`. Under dcload a scripted launch of `!Web` didn't come up in 2 of 2 runs (`Filer_Run` and `WimpTask` alike), so the app and the fetch have not yet run together on a console. Needs a run from GDEMU with no dcload.

`!Web` and `!NetSetup` sit on the icon bar: closing a window leaves them running, and the icon menu quits. `!NetSetup` doesn't need to stay running for anything, as TCP/IP is native and up from power on. The icon is only there for status.

## Printing

### Problem Statement

RISC OS has a full printing stack in ROM, but it ends at a parallel port and the printer definitions were on a disc. The printer on hand, a Brother HL-L2305, is on the network and takes no PostScript or PCL, only raster over IPP (`document-format-supported` is URF and PWG raster).

### Proposal

Keep everything up to the raster, as `PDriverDP` already renders pages with the ROM's own fonts and Draw module, so `!Edit` and `!Draw` print unchanged. Replace the last stage:

1. `PDriverDP` calls a dumper with a strip of 1 bit pixels and a reason in r11. Ours is ~30 words of ARM in the `DreamApps` module (`tools/mkapps.py`), registered as dumper 40 at init
2. The veneer hands everything but colours to the PrintDump SWI. `src/print.c` turns rows into URF in a 128KB ring, held only during a job
3. A worker thread streams the ring as one chunked HTTP POST, an IPP Print-Job. The SWI says "call again" while the ring is full, so a slow printer holds up the job and not the machine

300dpi 1 bit, not grey: strips live in the Wimp free pool, so 8 bits a pixel would cost 8x the redraws. The printer gets 8 bit lines either way, as W8 is all URF offers. They compress away, ~530KB for the test page. **Dithering is `PDriverDP`'s, not the printer's.**

`!NetPrint` calls `PDriver_SetDriver`, `SetInfo` and `SetPageSize`, then acts as the printer manager on the Wimp. That took a read of `!Printers`, which is BASIC inside the ROM:

- An application's Print sends `PrintSave`. If nothing answers that's "Printer application must be loaded"
- We reply with `PrintFile`. An application can answer `WillPrint`, and neither of ours does
- When that bounces we send `PrintTypeOdd`. `!Draw` answers `PrintTypeKnown` and prints through PDriver itself
- When that bounces too it's a file transfer. `!Edit` saves to `<Wimp$Scrap>` and `!NetPrint` typesets the text in Corpus. So printing from `!Edit` needs a writable disc, and any type other than text gets refused. Dropping a text file on the icon takes the same path

Decision: mono for now and no queue. Down the road a grey strip type would give nicer photos from `!Paint`, at the cost above.

### Considerations

- `PDriverDP` and `PDumperSupport` came out of the misc overlay. A print job calls them around every plot, and one overlay at a time means `!Draw` would swap a unit in for each, ~1s a go. Costs ~126KB of ELF
- `PDriverDP` takes r3 back from StartPage as rows still to send blank, so handling them and leaving r3 alone prints everything twice as low
- `PDumper_StartJob` insists on a palette file, even for 1 bit. Skip it: for type 0 `PDumper_SetColour` is arithmetic and never touches the job's block
- A URF page header is 32 bytes. Not 44, as the first go had it, and `tests/printserver.py` shared the mistake, so a fake printer passed what the real one rejected with `document-format-error`. libcups's `CUPS_RASTER_WRITE_APPLE` is the thing to compare against. The file header's page count is 0, as we don't know it yet, and the Brother doesn't care
- The Brother answers as soon as it has the job, then takes ~30s to print a page. `Sent to the printer` doesn't mean it worked: paper jams and the like aren't reported. `job-state` would tell us, if anyone wants to poll it
- An application prints with `printer:` open and nothing goes down it. The file is only PDriver's handle for the job

Open question: nothing resolves `.local`, and I doubt KOS can send multicast (untested), so the address is typed in and goes stale if DHCP moves the printer. A DHCP reservation is the easy fix.

### Results

11/1, Mac harness, real sockets: the test page printed on the HL-L2305 (job 931, `job-completed-successfully`, 1 impression). `!Edit` and `!Draw` printed to `tests/printserver.py`, which decodes URF to PNGs. The real printer has only had the test page.

11/1, Flycast, emulated BBA: the Dreamcast build sent the test page to `tests/printserver.py` through the KOS stack, with the same 1368 rows of ink. Used a temporary build with the address preset, as replays lose typed characters. Not tried on a console, and the VMU save has not run anywhere.

Page time on hardware is unknown. BASIC redraws a page once per strip, and the number of strips depends on free memory.

```bash
python3 tests/printserver.py 8631 build/job
```

```bash
DREAM_PRINTER=127.0.0.1:8631/ipp/print DREAM_REPLAY=tests/netprint.replay ./build/dreamrisc-host build/rom.bin assets/cmos.bin 170 build/shot.ppm
```

## Considerations

- Size: ~1.2MB of ARM becomes ~8.5MB of SH-4 plus ~1MB of tables, so ~7x. Out-of-line memory helpers and whole-ROM flag liveness are already in, and the latter only saved ~1%
- The old hardware registers are still the interface between ROM code and drivers. Replacing the kernel's drivers outright is a later step, not needed to reach the desktop
- Open question: Flycast timing is not hardware timing, and interpreter-free does not automatically mean fast. Needs a real console run

## Testing

Flycast's pointer can drift from the host mouse, so use scripted input on the harness.

Of note is that the sum reads screen memory in a straight line and ignores hardware scrolling. Once a command line has scrolled the screen, the sum covers RAM past the end of the screen, so an unrelated allocation can move it with every visible pixel the same. Compare frames before chasing one of those.

```bash
tools/converge.sh tests/apps.replay 136
```

Replays hold `move`, `button`, `key` and `shot` lines. Slam to a corner first for a fixed origin. Pointer scale is ~1.5px per unit across and down in mode 27, ~0.75 down in mode 12.

The harness and the screen checksums read guest RAM, so neither one sees bugs in the Dreamcast display path in `src/dc/main.c`. Check that in Flycast by eye. A window capture goes stale while another window covers Flycast, so check an `HW_DUMP=1` framebuffer dump as well. `dump_screen` only fires on a replay's seventh `shot`, and serial is slow, so a dump takes a few minutes.

## Hardware Results (10/31)

The Good: 8 of 9 boots reached the desktop, checksum `C50F35BC`, same as the harness. The replay run matched all three checkpoints, so the Filer, BASIC and a C app behave identically on the SH-4. Boot is ~26-31s, at ~49 of 50 frames/s.

The Bad: boot 2 stuck at the 1bpp screen (`66733280`) for 90s+ with polls taking ~73us instead of ~15us. That looks like an interrupt or keyboard retry storm, but that's a guess: the run had no PC logging. 88 randomised clock runs on the harness and 6 more console boots did not reproduce it.

Not checked: actual speed under load (polls count branches and IO, so they are not a MIPS figure), physical input, and what the video output looks like on a screen.

Heartbeat builds use `HW_LOG=1`, and `HW_REPLAY=1` adds a generated `build/replay.h` from `tools/replay2c.py`. Screen checksums are the only way to verify a console run unattended. If a boot is stuck in 1bpp at 30s the heartbeat dumps IOC state and recent SWIs.

Lander on the console (10/31): 124-125 game frames per 5s for a whole scripted flight, ~60% of the CPU left over in the heaviest scene, launch with no visible stall over dcload. Before pacing, the profile put the heavy scene at ~22% vertex projection (software division), ~23% triangle setup and ~12% span fills.

The video output was checked from a framebuffer dump in Flycast (`HW_DUMP=1` and `tools/fbdump.py`): full width, correct colours, matching the harness. It is still not a real TV. The dump also found a line that checksums cannot see, as they only cover guest memory: a line hash built on rotates cancelled on solid 256 pixel fills and left stale pixels.

Console and harness checkpoints stop matching in Lander once it moves, because replays run on wall time and the frame the game has reached differs. Static scenes matched.

Tertis in Flycast (10/31): launched from Apps and played by script through to a framebuffer dump, same score as the harness. ~250 frames per 5s with the sound ring flowing and no gaps during play, with its voice modules running through the RAM table. Real hardware is untested, and nobody has listened to it, only measured the recording on the harness. The controller map is untested too, as replays drive keys and mouse.

Overlay files are padded to whole sectors. In Flycast a read of `ov_4.bin`'s short last sector never returned, with sound or without. No idea why, as `ov_3.bin` had a short tail and loaded fine, and padding fixed it, so it might be worth a look down the road.

## Next Steps

1. Look at the real screen, try physical keyboard and mouse, and catch the boot hang if it recurs
2. Use more of the card: FileCore in 3.11 tops out near 512MB per disc, so maybe several drives
3. Listen to the sound on a console, and check mode 27 on a TV as well as VGA
4. Network: run `!Ping` and `!Web` on a console without dcload, dial a real DreamPi, try a real W5500, and exercise the socket SWIs from a BASIC program
