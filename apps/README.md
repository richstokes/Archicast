# Bundled apps

Each folder here describes one RISC OS application that ships on the disc. Nothing in `build/` is committed: the game binaries are fetched at build time, same as the ROM stays out of the repo.

`tools/mkapps.py` reads every `app.json` and writes `build/rom.bin`, which is the ROM with a small `DreamApps` module added, followed by a ResourceFS pack of the app files. The app folder then appears in the Apps window. `tools/armxlat.py` recompiles the app's ARM code ahead of time into its own overlay unit, `ov_N.bin` on the disc.

Neither part is loaded at boot. Pack pages stream through the ROM page cache when a file is read, and the code unit loads when the app first jumps to its entry point.

## app.json

- `name`: application folder, listed under `Apps`
- `upstream`: files to download, pinned by commit and sha256
- `files`: folder contents, from `local` files here, `upstream` keys, `basic` text to tokenise or `art` for a sprite file from text art, with a filetype or a load and exec address
- `files[].unpack`: set to `scorpress` to store the file unpacked, for code the app would decompress itself. Names can hold a subfolder, as in `Data.AllMods`
- `code`: leave this out for a BASIC app, which needs no unit of its own
- `code.file`: the file holding ARM code, which must have `load` and `exec`
- `code.seeds`: extra entry points, as `tools/converge.sh` finds them
- `code.tables`: tables of code pointers, as `[address, count]`
- `code.frame_us`: vertical sync period while the app runs, for games whose speed depends on frame rate. Lander runs at 25Hz. An 8MHz ARM2 managed ~15 frames/s by instruction count, so raise this for a slower, closer game
- `code.modules`: `file`, `base` and `count` for voice modules the app inserts in place from a packed file. Their words go in the RAM table, as they run from the app's memory
- `code.pad`: key codes (row in the high nibble) for `left`, `right`, `up`, `down`, `a`, `b`, `x` and `start` on the controller, for games with no use for a pointer. Without it the controller stays a mouse

Code the static walk misses shows up as "no native code" on the harness. Inside the app's own image, add the address to `code.seeds`. `tools/converge.sh` only knows about ROM and RAM words, so it will fill `trace/ramwords.txt` with junk if pointed at those.

## Tertis

We skip its launcher, `!RunImage` in the upstream app. That's a splash screen and a ScorPress decompressor, so we unpack `TertisCode` at build time (`tools/scorpress.py`) and ship that as `!RunImage`. `!Run` does the launcher's other job, setting the screen to 160K. Doing it from BASIC is the only way that's simple, as `*ChangeDynamicArea` has no screen option on 3.11.

Of note is that `adfs/Code/TetrisCode` upstream is a different build from the one that shipped, so don't use it. The launcher's exec address, `0x749F4`, sits 8 bytes ahead of the code at `0x749FC`, on two words that do nothing. We enter at the code.

## Logo

`!Logo` is our own, written in BBC BASIC (`logo/Logo.bas`). No open source Logo exists for RISC OS, and UCBLogo would have needed an ARM toolchain plus a recompile of a large C program. BASIC is already in the ROM as native code, so this costs ~23KB of pack and nothing at boot.

Demos are plain Logo text in `logo/Demos`. Add one by listing it in `app.json` and in the `DATA` line under `PROCmenus`. A different count also means changing `DN$`, the loop there and the menu size.

Watch out for `FOR`, which always runs once in BBC BASIC, and for anything live across an allocation, which has to be on `VS%` or the collector frees it. `tools/basic.py` matches keywords as prefixes like BASIC does, so a variable can't start with one (hence `zRUN%`), and it knows `TIME` only as a value.

## NetPrint

`!NetPrint` is BASIC as well, and does the job of `!Printers`, which needs printer definitions off a disc we don't have. It has to stay running: `!Edit` and `!Draw` won't print unless a printer manager answers them on the Wimp. See Printing in [docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md).

`tools/basic.py` knows no `PTR#`, in either form, so file positions go through `OS_Args`.
