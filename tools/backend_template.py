"""This game's backend: the code that runs on a server instead of on the phone.

While you develop, dev.py runs it on your PC. The game calls it from Lua with

    net.backend({ path = "/scores" }, function(status, body, err) ... end)
    net.backend({ path = "/scores", method = "POST", body = '{"name":"Ann","score":42}' }, function(status, body, err) ... end)

and dev.py passes each call to handle() below. Save this file and the next call uses the new code.
Errors show in the dev.py window. Data is kept in data.db next to this file, which git ignores.

For a release, run this same file on a real server behind HTTPS (python server.py 8080), and put its
address in the game's game.cfg:  backend = https://api.example.com
"""

import json
import sqlite3
import sys
from pathlib import Path

DB = Path(__file__).with_name("data.db")


def db():
    connection = sqlite3.connect(DB)
    connection.execute("CREATE TABLE IF NOT EXISTS scores (name TEXT, score INTEGER, at TEXT DEFAULT CURRENT_TIMESTAMP)")
    return connection


def handle(method, path, query, body, headers):
    """One call from the game. Returns (status, text) or (status, text, content type). Text is JSON by default."""
    if method == "GET" and path == "/scores":
        connection = db()
        rows = connection.execute("SELECT name, score FROM scores ORDER BY score DESC LIMIT 10").fetchall()
        connection.close()
        return 200, json.dumps([{"name": name, "score": score} for name, score in rows])

    if method == "POST" and path == "/scores":
        data = json.loads(body or "{}")
        name, score = str(data.get("name", "?"))[:20], int(data.get("score", 0))   # never trust what a client sends
        connection = db()
        with connection:
            connection.execute("INSERT INTO scores (name, score) VALUES (?, ?)", (name, score))
        connection.close()
        return 201, json.dumps({"ok": True})

    return 404, json.dumps({"error": f"nothing at {method} {path}"})


# ---------------------------------------------------------------- running on its own, on a real server

if __name__ == "__main__":
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
    from urllib.parse import parse_qs, urlparse

    class Handler(BaseHTTPRequestHandler):
        def answer(self):
            url = urlparse(self.path)
            length = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(length).decode("utf-8", "replace") if length else ""
            status, text, *kind = handle(self.command, url.path, parse_qs(url.query), body, dict(self.headers))
            data = text.encode()
            self.send_response(status)
            self.send_header("Content-Type", kind[0] if kind else "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        do_GET = do_POST = do_PUT = do_PATCH = do_DELETE = answer

    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    print(f"Serving on port {port}. Put HTTPS in front of it (a reverse proxy or your host's) before players use it.")
    ThreadingHTTPServer(("0.0.0.0", port), Handler).serve_forever()
