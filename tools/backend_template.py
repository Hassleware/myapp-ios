"""This game's backend: accounts and sign-in, the part nearly every game and app needs.
Add the game's own calls (saves, scores, matchmaking) at the bottom of handle(), behind signed_in_user().

While you develop, dev.py runs this file on your PC and the game reaches it with net.backend.
auth.lua in the game's folder wraps the calls below (register, login, logout, me, delete).
Save this file and the next call uses the new code. Data is kept in data.db next to it, which git ignores.

The calls, all JSON:
    POST   /register  {"email", "password"}  -> 201 {"token", "user"}
    POST   /login     {"email", "password"}  -> 200 {"token", "user"}    401 wrong details, 429 too many tries
    POST   /logout    (signed in)            -> 200
    GET    /me        (signed in)            -> 200 {"user"}
    DELETE /me        (signed in)            -> 200   (the App Store requires in-app account deletion)
Signed in means the header  Authorization: Bearer <token>.

How it follows current practice (OWASP password storage and session cheat sheets, NIST SP 800-63B):
  * Passwords are hashed with scrypt (2^17, 8, 1), a memory-hard function, with a random salt each.
    The settings are stored with each hash, so they can be raised later without breaking old accounts.
  * Passwords need 8 to 128 characters and nothing else: no forced symbols, no expiry.
  * Session tokens are 256 random bits. Only their SHA-256 is stored, so a stolen database holds no
    usable tokens. They last 30 days and logging out or deleting the account ends them.
  * Login answers the same way, and takes about as long, whether or not the email exists.
  * Five failed logins for one email in 15 minutes and that email is refused for the rest of the window.
  * Comparisons of secrets use hmac.compare_digest, so timing reveals nothing.

Not here yet, add when needed: email verification and password reset (both need a way to send email),
Sign in with Apple, refresh tokens. And: on the dev link, passwords cross your Wi-Fi as plain HTTP.
Use test accounts while developing. A release only talks to this over HTTPS (game.cfg's backend line).

For a release, run this file on a server behind HTTPS:  python server.py 8080
"""

import base64
import hashlib
import hmac
import json
import secrets
import sqlite3
import sys
import threading
import time
import unicodedata
from contextlib import closing
from pathlib import Path

DB = Path(__file__).with_name("data.db")
SESSION_SECONDS = 30 * 24 * 3600
SCRYPT = (2 ** 17, 8, 1)              # n, r, p: OWASP's recommended minimum. About 128 MB per hash
MAX_FAILURES, FAILURE_WINDOW = 5, 15 * 60

_failures = {}                        # email -> times of recent failed logins
_failures_lock = threading.Lock()
_dummy_hash = None                    # hashed once, so unknown emails cost as much time as known ones


# ---------------------------------------------------------------- storage

def run(sql, *args):
    """One SQL statement, committed. Returns its rows."""
    with closing(sqlite3.connect(DB)) as connection, connection:
        return connection.execute(sql, args).fetchall()


run("""CREATE TABLE IF NOT EXISTS users (
         id INTEGER PRIMARY KEY, email TEXT UNIQUE NOT NULL, password TEXT NOT NULL,
         created INTEGER NOT NULL)""")
run("""CREATE TABLE IF NOT EXISTS sessions (
         token TEXT PRIMARY KEY, user_id INTEGER NOT NULL, expires INTEGER NOT NULL)""")


# ---------------------------------------------------------------- passwords and tokens

def hash_password(password, salt=None, n=SCRYPT[0], r=SCRYPT[1], p=SCRYPT[2]):
    salt = salt or secrets.token_bytes(16)
    key = hashlib.scrypt(password.encode(), salt=salt, n=n, r=r, p=p, maxmem=256 * 1024 * 1024, dklen=32)
    encode = lambda b: base64.urlsafe_b64encode(b).decode()
    return f"scrypt${n}${r}${p}${encode(salt)}${encode(key)}"


def password_matches(password, stored):
    _, n, r, p, salt, _ = stored.split("$")
    again = hash_password(password, base64.urlsafe_b64decode(salt), int(n), int(r), int(p))
    return hmac.compare_digest(again, stored)


def new_session(user_id):
    token = secrets.token_urlsafe(32)
    run("DELETE FROM sessions WHERE expires < ?", int(time.time()))
    run("INSERT INTO sessions (token, user_id, expires) VALUES (?, ?, ?)",
        hashlib.sha256(token.encode()).hexdigest(), user_id, int(time.time()) + SESSION_SECONDS)
    return token


def signed_in_user(headers):
    """The signed-in user as {"id", "email"}, or None. Use it at the start of every call that needs an account."""
    value = next((v for k, v in headers.items() if k.lower() == "authorization"), "")
    if not value.startswith("Bearer "):
        return None
    digest = hashlib.sha256(value[7:].strip().encode()).hexdigest()
    rows = run("SELECT users.id, users.email FROM sessions JOIN users ON users.id = sessions.user_id "
               "WHERE sessions.token = ? AND sessions.expires > ?", digest, int(time.time()))
    return {"id": rows[0][0], "email": rows[0][1]} if rows else None


def tidy(text):
    """The same text whichever way the keyboard encoded it (NIST asks for this before hashing)."""
    return unicodedata.normalize("NFKC", str(text or ""))


def too_many_failures(email, failed=False):
    now = time.time()
    with _failures_lock:
        recent = [t for t in _failures.get(email, []) if now - t < FAILURE_WINDOW]
        if failed:
            recent.append(now)
        _failures[email] = recent
        return len(recent) >= MAX_FAILURES


def answer(status, **fields):
    return status, json.dumps(fields)


# ---------------------------------------------------------------- the calls

def handle(method, path, query, body, headers):
    """One call from the game. Returns (status, text) or (status, text, content type). Text is JSON by default."""
    global _dummy_hash
    try:
        data = json.loads(body) if body else {}
    except ValueError:
        return answer(400, error="the body is not JSON")

    if method == "POST" and path in ("/register", "/login"):
        email, password = tidy(data.get("email")).strip().lower(), tidy(data.get("password"))

        if path == "/register":
            if len(email) > 254 or email.count("@") != 1 or "." not in email.split("@")[1]:
                return answer(400, error="that email address doesn't look right")
            if not 8 <= len(password) <= 128:
                return answer(400, error="passwords need 8 to 128 characters")
            if run("SELECT 1 FROM users WHERE email = ?", email):
                return answer(409, error="there is already an account with that email")
            user_id = run("INSERT INTO users (email, password, created) VALUES (?, ?, ?) RETURNING id",
                          email, hash_password(password), int(time.time()))[0][0]
            return answer(201, token=new_session(user_id), user={"id": user_id, "email": email})

        if too_many_failures(email):
            return answer(429, error="too many tries, wait 15 minutes")
        rows = run("SELECT id, password FROM users WHERE email = ?", email)
        if not rows:
            _dummy_hash = _dummy_hash or hash_password("never a real password")
            password_matches(password, _dummy_hash)          # same work as a real check
        if not rows or not password_matches(password, rows[0][1]):
            too_many_failures(email, failed=True)
            return answer(401, error="wrong email or password")
        return answer(200, token=new_session(rows[0][0]), user={"id": rows[0][0], "email": email})

    user = signed_in_user(headers)
    if user is None:
        return answer(401, error="not signed in")

    if method == "GET" and path == "/me":
        return answer(200, user=user)
    if method == "POST" and path == "/logout":
        value = next(v for k, v in headers.items() if k.lower() == "authorization")
        run("DELETE FROM sessions WHERE token = ?", hashlib.sha256(value[7:].strip().encode()).hexdigest())
        return answer(200, ok=True)
    if method == "DELETE" and path == "/me":
        run("DELETE FROM sessions WHERE user_id = ?", user["id"])
        run("DELETE FROM users WHERE id = ?", user["id"])
        return answer(200, ok=True)

    # The game's own calls go here. Every one below this line has a signed-in `user`.

    return answer(404, error=f"nothing at {method} {path}")


# ---------------------------------------------------------------- running on its own, on a real server

if __name__ == "__main__":
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
    from urllib.parse import parse_qs, urlparse

    class Handler(BaseHTTPRequestHandler):
        def respond(self):
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

        do_GET = do_POST = do_PUT = do_PATCH = do_DELETE = respond

    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    print(f"Serving on port {port}. Put HTTPS in front of it (a reverse proxy or your host's) before players use it.")
    ThreadingHTTPServer(("0.0.0.0", port), Handler).serve_forever()
