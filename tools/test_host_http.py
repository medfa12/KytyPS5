"""Run deterministic WinHTTP integration checks against a loopback fixture."""
import http.server
import gzip
import subprocess
import sys
import threading

held_arrived = threading.Event()
held_release = threading.Event()


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        self.send_response(201)
        self.send_header('X-Echo', self.headers.get('X-Kyty-Test', ''))
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == '/held':
            held_arrived.set()
            held_release.wait(5)
        elif self.path == '/arrived':
            held_arrived.wait(5)
        elif self.path == '/release':
            held_release.set()
        elif self.path == '/timeout':
            threading.Event().wait(1)
        if self.path == '/cookie-set':
            self.send_response(200)
            self.send_header('Set-Cookie', 'retail=owned; Path=/; HttpOnly')
            self.send_header('Set-Cookie', 'limited=yes; Path=/scoped/')
            body = b'set'
        elif self.path == '/cookie-redirect':
            self.send_response(302)
            self.send_header('Set-Cookie', 'redirected=yes; Path=/')
            self.send_header('Location', '/cookie-echo')
            body = b''
        elif self.path in ('/cookie-echo', '/scoped/cookie-echo'):
            self.send_response(200)
            body = self.headers.get('Cookie', '').encode()
        elif self.path == '/redirect':
            self.send_response(302)
            self.send_header('Location', '/ok')
            body = b''
        elif self.path == '/missing':
            self.send_response(404)
            body = b'missing'
        elif self.path == '/gzip':
            self.send_response(200)
            self.send_header('Content-Encoding', 'gzip')
            body = gzip.compress(b'compressed response')
        else:
            self.send_response(200)
            body = b'ok'
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Content-Type', 'text/plain')
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass


with http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler) as server:
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        result = subprocess.run([sys.argv[1], f'http://127.0.0.1:{server.server_port}/'])
    finally:
        server.shutdown()
        thread.join()
sys.exit(result.returncode)
