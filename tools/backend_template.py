"""This game's backend: accounts and sign-in, the part nearly every game and app needs.
Add the game's own calls (saves, scores, matchmaking) at the bottom of handle(), behind signed_in_user().

Sign-in works like Supercell ID: no passwords. The player types their email, gets a 6-digit code,
types it in, and the phone stays signed in (for a year, renewed each time it is used) until they log out.
Signing up and signing in are the same steps: the account is made the first time a code is accepted.

While you develop, dev.py runs this file on your PC and the game reaches it with net.backend.
auth.lua in the game's folder wraps the calls below. Save this file and the next call uses the new code.
Data is kept in data.db next to it, which git ignores.

The calls, all JSON:
    POST   /code      {"email"}           -> 200       a code is on its way (429 if asked again within a minute)
    POST   /verify    {"email", "code"}   -> 200 {"token", "user", "new"}   401 wrong or expired, 429 too many tries
    POST   /logout    (signed in)         -> 200
    GET    /me        (signed in)         -> 200 {"user"}
    DELETE /me        (signed in)         -> 200       (the App Store requires in-app account deletion)
Signed in means the header  Authorization: Bearer <token>.

Where the code goes:
  * By default it is printed in the dev.py window. Read it there and type it on the phone.
  * To get real emails, set these before starting dev.py (any mail account that allows SMTP; for Gmail,
    turn on 2-step verification and make an "app password"):
        $env:GAMELAB_SMTP = "smtp.gmail.com:587"
        $env:GAMELAB_SMTP_USER = "you@gmail.com"
        $env:GAMELAB_SMTP_PASSWORD = "the app password"
    Never put these in a file in the repo.

How it follows current practice (OWASP authentication and session guidance, NIST SP 800-63B):
  * Codes come from the operating system's secure random source, are used once, expire after 10 minutes,
    and allow 5 tries; after that a new code is needed. Only a hash of each code is stored.
  * Asking for a code gives the same answer whether or not an account exists.
  * A new code can be asked for once a minute per email, so the endpoint can't be used to flood an inbox.
  * Session tokens are 256 random bits. Only their SHA-256 is stored, so a stolen database holds no usable
    tokens. Logging out or deleting the account ends them.
  * Secrets are compared with hmac.compare_digest, so timing reveals nothing.

Not here yet, add when needed: Sign in with Apple, a per-address daily cap on emails, and refresh tokens.
On the dev link, codes and tokens cross your Wi-Fi as plain HTTP; a release only uses HTTPS
(game.cfg's backend line). For a release, run this file on a server behind HTTPS: python server.py 8080
"""

import hashlib
import hmac
import json
import os
import secrets
import smtplib
import sqlite3
import sys
import time
import unicodedata
from contextlib import closing
from email.message import EmailMessage
from pathlib import Path

DB = Path(__file__).with_name("data.db")
GAME = Path(__file__).parent.name
CODE_SECONDS, CODE_TRIES, RESEND_SECONDS = 10 * 60, 5, 60
SESSION_SECONDS = 365 * 24 * 3600


# ---------------------------------------------------------------- storage

def run(sql, *args):
    """One SQL statement, committed. Returns its rows."""
    with closing(sqlite3.connect(DB)) as connection, connection:
        return connection.execute(sql, args).fetchall()


run("CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, email TEXT UNIQUE NOT NULL, created INTEGER NOT NULL)")
run("CREATE TABLE IF NOT EXISTS codes (email TEXT PRIMARY KEY, code TEXT NOT NULL, expires INTEGER NOT NULL, "
    "tries INTEGER NOT NULL, sent INTEGER NOT NULL)")
run("CREATE TABLE IF NOT EXISTS sessions (token TEXT PRIMARY KEY, user_id INTEGER NOT NULL, expires INTEGER NOT NULL)")


# ---------------------------------------------------------------- codes, emails and sessions

def digest(*parts):
    return hashlib.sha256("\n".join(parts).encode()).hexdigest()


def deliver(email, code):
    """Emails the code if mail is set up, otherwise prints it in the dev.py window."""
    server = os.environ.get("GAMELAB_SMTP")
    if not server:
        print(f"  [backend {GAME}] sign-in code for {email}: {code}   (set GAMELAB_SMTP to send real emails)", flush=True)
        return
    host, _, port = server.partition(":")
    message = EmailMessage()
    message["Subject"] = f"{code} is your {GAME} sign-in code"
    message["From"] = os.environ.get("GAMELAB_MAIL_FROM") or os.environ["GAMELAB_SMTP_USER"]
    message["To"] = email
    message.set_content(f"Your {GAME} sign-in code is {code}\n\nIt works once, for 10 minutes. "
                        "If you didn't ask for it, you can ignore this email.\n")
    with smtplib.SMTP(host, int(port or 587), timeout=15) as smtp:
        smtp.starttls()                                   # encrypted before the login is sent
        smtp.login(os.environ["GAMELAB_SMTP_USER"], os.environ["GAMELAB_SMTP_PASSWORD"])
        smtp.send_message(message)


def new_session(user_id):
    token = secrets.token_urlsafe(32)
    run("DELETE FROM sessions WHERE expires < ?", int(time.time()))
    run("INSERT INTO sessions (token, user_id, expires) VALUES (?, ?, ?)", digest(token), user_id, int(time.time()) + SESSION_SECONDS)
    return token


def bearer(headers):
    value = next((v for k, v in headers.items() if k.lower() == "authorization"), "")
    return value[7:].strip() if value.startswith("Bearer ") else None


def signed_in_user(headers):
    """The signed-in user as {"id", "email"}, or None. Use it at the start of every call that needs an account.
    Each use pushes the session's end a year ahead, so a player who keeps playing stays signed in."""
    token = bearer(headers)
    if not token:
        return None
    now = int(time.time())
    rows = run("SELECT users.id, users.email FROM sessions JOIN users ON users.id = sessions.user_id "
               "WHERE sessions.token = ? AND sessions.expires > ?", digest(token), now)
    if not rows:
        return None
    run("UPDATE sessions SET expires = ? WHERE token = ?", now + SESSION_SECONDS, digest(token))
    return {"id": rows[0][0], "email": rows[0][1]}


def clean_email(value):
    """The address as one form, or None if it can't be one. Line breaks are refused, so it can't add mail headers."""
    email = unicodedata.normalize("NFKC", str(value or "")).strip().lower()
    if len(email) > 254 or email.count("@") != 1 or "." not in email.split("@")[1] or any(c in email for c in "\r\n \t,;<>"):
        return None
    return email


def answer(status, **fields):
    return status, json.dumps(fields)


# ---------------------------------------------------------------- the calls

def handle(method, path, query, body, headers):
    """One call from the game. Returns (status, text) or (status, text, content type). Text is JSON by default."""
    try:
        data = json.loads(body) if body else {}
    except ValueError:
        return answer(400, error="the body is not JSON")
    now = int(time.time())

    if method == "POST" and path == "/code":
        email = clean_email(data.get("email"))
        if not email:
            return answer(400, error="that email address doesn't look right")
        last = run("SELECT sent FROM codes WHERE email = ?", email)
        if last and now - last[0][0] < RESEND_SECONDS:
            return answer(429, error="a code was just sent; wait a minute before asking again")
        code = f"{secrets.randbelow(10 ** 6):06d}"
        run("INSERT OR REPLACE INTO codes (email, code, expires, tries, sent) VALUES (?, ?, ?, 0, ?)",
            email, digest(email, code), now + CODE_SECONDS, now)
        deliver(email, code)
        return answer(200, ok=True)

    if method == "POST" and path == "/verify":
        email, code = clean_email(data.get("email")), "".join(c for c in str(data.get("code", "")) if c.isdigit())
        row = run("SELECT code, expires, tries FROM codes WHERE email = ?", email) if email else []
        if not row or row[0][1] < now:
            return answer(401, error="that code has expired; ask for a new one")
        if row[0][2] >= CODE_TRIES:
            run("DELETE FROM codes WHERE email = ?", email)
            return answer(429, error="too many wrong codes; ask for a new one")
        if not hmac.compare_digest(digest(email, code), row[0][0]):
            run("UPDATE codes SET tries = tries + 1 WHERE email = ?", email)
            return answer(401, error="wrong code")
        run("DELETE FROM codes WHERE email = ?", email)        # each code works once
        user = run("SELECT id FROM users WHERE email = ?", email)
        new = not user
        if new:
            user = run("INSERT INTO users (email, created) VALUES (?, ?) RETURNING id", email, now)
        return answer(200, token=new_session(user[0][0]), user={"id": user[0][0], "email": email}, new=new)

    user = signed_in_user(headers)
    if user is None:
        return answer(401, error="not signed in")

    if method == "GET" and path == "/me":
        return answer(200, user=user)
    if method == "POST" and path == "/logout":
        run("DELETE FROM sessions WHERE token = ?", digest(bearer(headers)))
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
