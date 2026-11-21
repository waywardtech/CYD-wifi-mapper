#!/usr/bin/env python3
"""Stand-in for the CYD's setup hotspot, for working on web/index.html without hardware.

    python tools/mock_device.py [--port 8032] [--root ./mock_sd]

Serves web/index.html and the same /api endpoints as src/main.cpp, storing files under --root
exactly as the CYD lays them out on the SD card (so the folder can be copied to a real card).
"""
import argparse
import json
import os
import shutil
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def make_handler(root):
    def local(path):
        if not path.startswith("/wifimap/") or ".." in path or len(path) >= 120:
            return None
        return os.path.join(root, path.lstrip("/"))

    class H(BaseHTTPRequestHandler):
        def _send(self, code, body, ctype="application/json"):
            if isinstance(body, str):
                body = body.encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            u = urlparse(self.path)
            q = parse_qs(u.query)
            if u.path == "/":
                with open(os.path.join(HERE, "web", "index.html"), "rb") as f:
                    return self._send(200, f.read(), "text/html")
            if u.path == "/api/state":
                base = os.path.join(root, "wifimap")
                projects = sorted(d for d in os.listdir(base) if os.path.exists(os.path.join(base, d, "project.json")))
                return self._send(200, json.dumps({"version": "mock", "sd": True, "wifi": "", "projects": projects}))
            if u.path == "/api/file":
                p = local(q.get("path", [""])[0])
                if not p:
                    return self._send(400, '{"error":"bad path"}')
                if not os.path.isfile(p):
                    return self._send(404, '{"error":"not found"}')
                with open(p, "rb") as f:
                    return self._send(200, f.read(), "application/octet-stream")
            self.send_response(302)
            self.send_header("Location", "/")
            self.end_headers()

        def do_POST(self):
            u = urlparse(self.path)
            q = parse_qs(u.query)
            body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
            p = local(q.get("path", [""])[0])
            if u.path == "/api/file":
                if not p:
                    return self._send(400, '{"error":"bad request"}')
                os.makedirs(os.path.dirname(p), exist_ok=True)
                with open(p, "wb") as f:
                    f.write(body)
                return self._send(200, '{"ok":true}')
            if u.path == "/api/delete":
                if not p or len(q["path"][0]) < 10:
                    return self._send(400, '{"error":"bad path"}')
                shutil.rmtree(p, ignore_errors=True) if os.path.isdir(p) else (os.path.exists(p) and os.remove(p))
                return self._send(200, '{"ok":true}')
            if u.path == "/api/wifi":
                return self._send(200, '{"ok":true}')
            self._send(404, '{"error":"not found"}')

        def log_message(self, *a):
            pass

    return H


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int, default=8032)
    ap.add_argument("--root", default=os.path.join(HERE, "mock_sd"))
    a = ap.parse_args()
    os.makedirs(os.path.join(a.root, "wifimap"), exist_ok=True)
    print("Mock CYD at http://localhost:%d  (files in %s)" % (a.port, a.root))
    ThreadingHTTPServer(("127.0.0.1", a.port), make_handler(a.root)).serve_forever()


if __name__ == "__main__":
    main()
