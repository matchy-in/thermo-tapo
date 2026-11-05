"""Fake Tapo plug speaking KLAP, built on python-kasa's reference crypto.

Behaves like real firmware in the ways that have bitten this project:
- handshake1 sets both a TP_SESSIONID and a TIMEOUT cookie
- requests are rejected with 403 unless exactly the session cookie is sent back
- the session can be made to expire after N requests (403 -> client must re-handshake)
"""

import argparse
import hashlib
import json
import os
import secrets
import struct
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

from kasa.credentials import Credentials
from kasa.transports.klaptransport import KlapEncryptionSession, KlapTransportV2

pack = struct.Struct(">l").pack


class Plug:
    def __init__(self, email, password, cookie_style, expire_after):
        self.auth = KlapTransportV2.generate_auth_hash(Credentials(email, password))
        self.cookie_style = cookie_style
        self.expire_after = expire_after
        self.device_on = False
        self.sessions = {}  # sid -> dict(local, remote, verified, session, count)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    plug: Plug

    def log_message(self, fmt, *args):
        print("[mock]", fmt % args, flush=True)

    def reply(self, status, body=b"", cookies=()):
        self.send_response(status)
        for c in cookies:
            self.send_header("Set-Cookie", c)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def session_from_cookie(self):
        raw = self.headers.get("Cookie", "")
        parts = [p.strip() for p in raw.split(";") if p.strip()]
        if len(parts) != 1 or not parts[0].startswith("TP_SESSIONID="):
            self.log_message("rejecting Cookie header %r", raw)
            return None
        return self.plug.sessions.get(parts[0].split("=", 1)[1])

    def do_POST(self):
        url = urlparse(self.path)
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        plug = self.plug

        if url.path == "/app/handshake1":
            local, remote = body, secrets.token_bytes(16)
            sid = secrets.token_hex(16).upper()
            plug.sessions[sid] = dict(local=local, remote=remote, verified=False, count=0)
            server_hash = hashlib.sha256(local + remote + plug.auth).digest()
            if plug.cookie_style == "separate":
                cookies = [f"TP_SESSIONID={sid}", "TIMEOUT=86400"]
            elif plug.cookie_style == "timeout-first":
                cookies = [f"TIMEOUT=86400;TP_SESSIONID={sid}"]
            else:
                cookies = [f"TP_SESSIONID={sid};TIMEOUT=86400"]
            return self.reply(200, remote + server_hash, cookies)

        s = self.session_from_cookie()
        if s is None:
            return self.reply(403)

        if url.path == "/app/handshake2":
            if body != hashlib.sha256(s["remote"] + s["local"] + plug.auth).digest():
                return self.reply(403)
            s["verified"] = True
            s["session"] = KlapEncryptionSession(s["local"], s["remote"], plug.auth)
            return self.reply(200)

        if url.path == "/app/request" and s["verified"]:
            s["count"] += 1
            if plug.expire_after and s["count"] > plug.expire_after:
                self.log_message("session expired")
                return self.reply(403)
            seq = int(parse_qs(url.query)["seq"][0])
            sess = s["session"]
            sess._seq = seq
            sess._generate_cipher()
            if body[:32] != hashlib.sha256(sess._sig + pack(seq) + body[32:]).digest():
                self.log_message("bad signature")
                return self.reply(403)
            req = json.loads(sess.decrypt(body))
            self.log_message("request %s", json.dumps(req))
            if req["method"] == "set_device_info":
                plug.device_on = bool(req["params"]["device_on"])
                resp = {"error_code": 0}
            elif req["method"] == "get_device_info":
                resp = {"error_code": 0, "result": {"device_on": plug.device_on, "model": "P110"}}
            else:
                resp = {"error_code": -1}
            sess._seq = seq - 1
            payload, _ = sess.encrypt(json.dumps(resp))
            return self.reply(200, payload)

        return self.reply(403)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=int(os.environ.get("TAPO_TEST_PORT", 18080)))
    ap.add_argument("--email", default="user@example.com")
    ap.add_argument("--password", default="secret")
    ap.add_argument("--cookie-style", choices=["combined", "separate", "timeout-first"], default="combined")
    ap.add_argument("--expire-after", type=int, default=0)
    args = ap.parse_args()
    Handler.plug = Plug(args.email, args.password, args.cookie_style, args.expire_after)
    ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
