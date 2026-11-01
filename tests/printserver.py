#!/usr/bin/env python3
"""A stand-in IPP printer: takes Print-Job, keeps each page as a PNG, then checks.

usage: printserver.py <port> <out prefix>. Writes <prefix>.urf and <prefix>-N.png,
and prints a summary per job. It answers like the Brother HL-L2305, which takes URF
and raster only, so a job that isn't good URF fails here too.
"""
import http.server
import struct
import sys
import zlib
from pathlib import Path


def attributes(data):
    """IPP attributes, and where the document starts."""
    pos, seen, name = 8, {}, ""
    while True:
        tag = data[pos]
        pos += 1
        if tag == 3:
            return seen, pos
        if tag < 0x10:
            continue
        size = struct.unpack(">H", data[pos:pos + 2])[0]
        name = data[pos + 2:pos + 2 + size].decode() or name
        pos += 2 + size
        size = struct.unpack(">H", data[pos:pos + 2])[0]
        seen[name] = data[pos + 2:pos + 2 + size].decode()
        pos += 2 + size


def pages(data):
    """Decode URF into (width, height, dpi, rows), failing on anything malformed."""
    if data[:8] != b"UNIRAST\0":
        raise ValueError(f"bad file header {data[:8]!r}")
    pos = 12
    while pos < len(data):
        head = data[pos:pos + 32]
        pos += 32
        if head[0] != 8 or head[1] != 0:
            raise ValueError(f"bad page type {head[:2]!r}")
        width, height, dpi = struct.unpack(">III", head[12:24])
        rows = []
        while len(rows) < height:
            repeat = data[pos] + 1
            pos += 1
            line = bytearray()
            while len(line) < width:
                code = data[pos]
                pos += 1
                if code < 128:
                    line += data[pos:pos + 1] * (code + 1)
                    pos += 1
                elif code > 128:
                    line += data[pos:pos + 257 - code]
                    pos += 257 - code
                else:
                    raise ValueError("fill code, which the driver never sends")
            if len(line) != width:
                raise ValueError(f"line {len(rows)} is {len(line)} pixels, not {width}")
            rows += [bytes(line)] * repeat
        if len(rows) != height:
            raise ValueError(f"{len(rows)} rows, not {height}")
        yield width, height, dpi, rows


def png(width, rows):
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    raw = b"".join(b"\0" + row for row in rows)
    head = struct.pack(">IIBBBBB", width, len(rows), 8, 0, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", head) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


class Printer(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    prefix = "print"

    def read_chunked(self):
        body = bytearray()
        while True:
            size = int(self.rfile.readline().strip(), 16)
            if not size:
                self.rfile.readline()
                return bytes(body)
            body += self.rfile.read(size)
            self.rfile.readline()

    def do_POST(self):
        body = self.read_chunked()
        seen, start = attributes(body)
        document = body[start:]
        Path(self.prefix + ".urf").write_bytes(document)
        code = 0
        try:
            for n, (width, height, dpi, rows) in enumerate(pages(document), 1):
                inked = sum(1 for row in rows if row.count(255) != width)
                print(f"page {n}: {width}x{height} at {dpi}dpi, {inked} rows with ink", flush=True)
                Path(f"{self.prefix}-{n}.png").write_bytes(png(width, rows))
        except (ValueError, IndexError) as error:
            print(f"bad document: {error}", flush=True)
            code = 0x040A
        print(f"job: {len(document)} bytes, {seen}", flush=True)
        reply = bytes([2, 0]) + struct.pack(">HI", code, 1) + b"\x03"
        self.send_response(200)
        self.send_header("Content-Type", "application/ipp")
        self.send_header("Content-Length", str(len(reply)))
        self.end_headers()
        self.wfile.write(reply)

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    Printer.prefix = sys.argv[2]
    http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), Printer).serve_forever()
