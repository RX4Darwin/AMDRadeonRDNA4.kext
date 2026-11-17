#!/usr/bin/env python3
"""serve.py: serve cube.html on 127.0.0.1:8765 and print what the page reports (GET /report?k=...&v=...), one line each.

  python3 tools/metal/webgl/serve.py &   then   open -a Safari http://127.0.0.1:8765/cube.html
"""
import http.server
import os
import sys
import urllib.parse


class Handler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        if url.path == '/report':
            q = urllib.parse.parse_qs(url.query)
            print(f"webgl: {q.get('k', ['?'])[0]}: {q.get('v', [''])[0]}", flush=True)
            self.send_response(204)
            self.end_headers()
            return
        super().do_GET()

    def log_message(self, *args):
        pass


os.chdir(os.path.dirname(os.path.abspath(__file__)))
http.server.ThreadingHTTPServer(('127.0.0.1', int(sys.argv[1]) if len(sys.argv) > 1 else 8765), Handler).serve_forever()
