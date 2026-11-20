#!/usr/bin/env python3
"""Serves the dot test page and saves each finished run as JSON.

    python3 server.py [results_dir] [port]

Open http://127.0.0.1:8766/ in the browser. Results go to results_dir (default ./results).
"""
import http.server, json, os, sys, time

root = os.path.dirname(os.path.abspath(__file__))
out = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(root, "results")
port = int(sys.argv[2]) if len(sys.argv) > 2 else 8766
os.makedirs(out, exist_ok=True)

class H(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k): super().__init__(*a, directory=root, **k)
    def do_POST(self):
        if self.path != "/save": self.send_error(404); return
        n = int(self.headers.get("Content-Length", 0))
        data = self.rfile.read(n)
        try:
            run = json.loads(data)
            label = "".join(c for c in str(run.get("label", "run")) if c.isalnum() or c in "-_")[:30] or "run"
        except Exception:
            self.send_error(400); return
        name = f"{label}-{time.strftime('%Y%m%d-%H%M%S')}.json"
        with open(os.path.join(out, name), "wb") as f: f.write(data)
        self.send_response(200); self.send_header("Content-Type", "application/json"); self.end_headers()
        self.wfile.write(json.dumps({"saved": name}).encode())
    def log_message(self, *a): pass

print(f"dot test on http://127.0.0.1:{port}/  results in {out}", flush=True)
http.server.ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
