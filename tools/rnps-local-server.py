#!/usr/bin/env python3
"""ps5-vsh: local reroute server for RNPS/FW HTTP traffic (NPXS40003 etc.).

The firmware expects its service requests rerouted locally instead of
reaching the internet. Run this on the host and point kyty's HTTP bridges
at it (default http://127.0.0.1:8123); unknown routes return a JSON
404 AND are logged so future runs reveal exactly which endpoints a title
needs next.

Stdlib only. No kyty changes required to run it.
"""

import base64
import http.server
import json
import socketserver
import sys

HOST = "127.0.0.1"
PORT = 8123

# 1x1 transparent PNG for every image/CDN route.
_PNG_1X1 = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg=="
)


def _json(handler, obj, code=200):
    body = json.dumps(obj).encode("utf-8")
    handler.send_response(code)
    handler.send_header("Content-Type", "application/json")
    handler.send_header("Content-Length", str(len(body)))
    handler.end_headers()
    handler.wfile.write(body)


def _png(handler):
    handler.send_response(200)
    handler.send_header("Content-Type", "image/png")
    handler.send_header("Content-Length", str(len(_PNG_1X1)))
    handler.end_headers()
    handler.wfile.write(_PNG_1X1)


# (host-substring, path-prefix) -> handler name. First match wins.
ROUTES = [
    (("image.api", "image.mgmt", "image.e1-np", "apsnobj", "community.",
      "rockstargames", "ytimg", "cursecdn", "wallpaperbetter"), None, "png"),
    (("api.twitch.tv",), ("/helix/channels", "/helix/games",
                           "/helix/streams", "/helix/users"), "twitch"),
    (("googleapis",), None, "youtube"),
    (("rnps-registry",), None, "rnps_registry"),
    (("playstation.net", "playstation.com"), None, "psn_empty"),
]


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "KytyRnpsLocal/1"

    def log_message(self, fmt, *args):  # keep stdout for route logs only
        pass

    def _route(self):
        host = self.headers.get("Host", "")
        # Path may carry the original absolute URI when used as a proxy.
        path = self.path
        print("[route] %s %s (Host: %s)" % (self.command, path, host),
              flush=True)
        for hosts, prefixes, kind in ROUTES:
            if not any(h in host or h in path for h in hosts):
                continue
            if prefixes and not any(path.startswith(p) or p in path
                                    for p in prefixes):
                continue
            return kind
        return None

    def _serve(self):
        length = int(self.headers.get("Content-Length") or 0)
        if length > 0:
            self.rfile.read(min(length, 65536))
        kind = self._route()
        if kind == "png":
            return _png(self)
        if kind == "twitch":
            return _json(self, {"data": [], "pagination": {}})
        if kind == "youtube":
            return _json(self, {"items": [], "pageInfo": {}})
        if kind == "rnps_registry":
            return _json(self, {"ok": True, "bundles": []})
        if kind == "psn_empty":
            return _json(self, {"data": [], "kyty": "stub-route"})
        print("[route] UNSTUBBED %s %s -> 404 (add a ROUTES entry)"
              % (self.command, self.path), flush=True)
        return _json(self, {"kyty": "unstubbed-route", "path": self.path},
                     code=404)

    do_GET = _serve
    do_POST = _serve
    do_PUT = _serve


def main(argv):
    port = int(argv[1]) if len(argv) > 1 else PORT
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer((HOST, port), Handler) as srv:
        print("kyty RNPS local server on http://%s:%d" % (HOST, port),
              flush=True)
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main(sys.argv)
