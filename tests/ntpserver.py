#!/usr/bin/env python3
"""A stand-in NTP server that always gives the same time, for !NetTime on the harness.

usage: ntpserver.py <port> <unix seconds>. Answers every request with that time plus
a quarter second, as a stratum 2 server, and prints a line per request.
"""
import socket
import struct
import sys

port, when = int(sys.argv[1]), int(sys.argv[2])
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.1", port))
seconds = (when + 2208988800) & 0xFFFFFFFF
while True:
    data, peer = sock.recvfrom(512)
    if len(data) < 48 or data[0] & 7 != 3:
        continue
    reply = struct.pack(">BBbbII4sIIIIIIII", 0x1C, 2, 6, -20, 0, 0, b"GPS\0",
                        seconds, 0, *struct.unpack(">II", data[40:48]),
                        seconds, 0x40000000, seconds, 0x40000000)
    sock.sendto(reply, peer)
    print(f"answered {peer[0]}:{peer[1]}", flush=True)
