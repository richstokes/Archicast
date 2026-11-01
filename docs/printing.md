# Printing

`!NetPrint` prints over the network to an AirPrint printer that takes URF raster, from `!Edit`, `!Draw` and anything else using the RISC OS printer drivers. So far it has been used with a Brother HL-L2305.

## How to use it

1. Run `!NetPrint` from the Apps folder. It sits on the icon bar and has to stay running
2. Type the printer's IP address, such as `192.168.1.78`, pick Letter or A4, and click Save to keep both on a VMU. You can add `:port` or `/path` if your printer needs them; the default is port 631 and `/ipp/print`
3. Print as usual from `!Edit` or `!Draw`, or click Test page. Dropping a text file on the icon should also work, but that's untested

## How it works

- **Rendering**: the printer only takes raster over IPP (no PostScript or PCL), so RISC OS's own `PDriverDP` in ROM still draws the pages, with its real fonts
- **New native part**: a small dumper module passes each strip of the page to `src/print.c`, which sends it to the printer as URF over IPP from a worker thread. A slow printer holds up the job, not the desktop
- **`!NetPrint`**: it replaces `!Printers`, which needs printer definitions off a disc. `!Edit` and `!Draw` refuse to print unless a printer manager answers them, and each takes a different route through that handshake

## Limitations

- Pages are 300dpi black and white, and greys come out as halftones
- The address is typed in, as `.local` names don't resolve. Give the printer a DHCP reservation or the address will eventually go stale
- "Sent to the printer" only means it accepted the job. A paper jam afterwards won't show up
- Printing from `!Edit` needs a writable disc, as it saves a scrap file first. Dropped files must be text

For the design and the protocol details, see Printing in [ARCHITECTURE.md](ARCHITECTURE.md).
