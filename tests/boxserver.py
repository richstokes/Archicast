#!/usr/bin/env python3
"""dcvmu.com's own service over plain HTTP, for Dreambox on the harness.

boxserver.py <service dir> <port> <token file> [database]

The service is projects/dcvmu.com/service in the dreamcast-homebrew repo, with
its requirements installed (run this with its .venv python). Makes a user
called tester, links a console to it and writes the console's token, so runs
with DREAM_DREAMBOX_TOKEN pointing at the file start out linked. Leave the
token file out (pass -) to see the linking error instead.
"""
import os
import re
import sys
import tempfile

service, port, token_file = sys.argv[1], int(sys.argv[2]), sys.argv[3]
database = sys.argv[4] if len(sys.argv) > 4 else os.path.join(tempfile.mkdtemp(), "box.sqlite3")
sys.path.insert(0, service)
from app import create_app  # noqa: E402
from werkzeug.serving import WSGIRequestHandler, run_simple  # noqa: E402

app = create_app({"TESTING": True, "DATABASE": database})
web = app.test_client()


def csrf(path):
    return re.search(r'name="csrf" value="([^"]+)"', web.get(path).text)[1]


web.post("/register", data={"csrf": csrf("/register"), "username": "tester",
                            "password": "a-long-test-password", "email": "tester@example.com"})
if token_file != "-":
    console = app.test_client()
    device, code = console.post("/api/v1/drive/pair").text.split("\n")[:2]
    web.post("/account/archicast/link", data={"csrf": csrf("/account/archicast"), "code": code})
    token = console.post("/api/v1/drive/pair/claim", query_string={"device": device}).text.strip()
    with open(token_file, "w") as f:
        f.write(token + "\n")
print(f"database {database}", flush=True)
WSGIRequestHandler.protocol_version = "HTTP/1.1"  # keep-alive, as Caddy gives the console
run_simple("127.0.0.1", port, app, threaded=True)
