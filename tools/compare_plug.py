"""Find out why the board gets HTTP 403 when python-kasa doesn't.

Talks to the real plug from your PC in several ways and prints what the plug
answers at each step:

  A. python-kasa itself (known to work)
  B. an exact copy of what the board sends (same headers, same JSON)
  C. like B but with one new TCP connection per request
  D. like B but with python-kasa's HTTP headers

    pip install python-kasa
    python tools/compare_plug.py 192.168.0.100 you@example.com "your password"

Paste the whole output back.
"""

import argparse
import asyncio
import base64
import hashlib
import json
import secrets
import socket
import time
import uuid

from kasa import Credentials, DeviceConfig
from kasa.deviceconfig import DeviceConnectionParameters, DeviceEncryptionType, DeviceFamily
from kasa.transports.klaptransport import KlapEncryptionSession, KlapTransportV2

BOARD_HEADERS = [
    ("Content-Type", "application/octet-stream"),
    ("Accept", "*/*"),
    ("User-Agent", "matrixbit-thermostat"),
]
AIOHTTP_HEADERS = [
    ("Accept", "*/*"),
    ("Accept-Encoding", "gzip, deflate"),
    ("User-Agent", "Python/3.12 aiohttp/3.9.5"),
    ("Content-Type", "application/octet-stream"),
]


class RawHttp:
    def __init__(self, ip, headers, keep_alive):
        self.ip, self.headers, self.keep_alive = ip, headers, keep_alive
        self.sock = None
        self.cookie = None

    def post(self, path, body):
        if self.sock is None:
            self.sock = socket.create_connection((self.ip, 80), timeout=6)
            new = "new"
        else:
            new = "reused"
        lines = [f"POST {path} HTTP/1.1", f"Host: {self.ip}"]
        lines += [f"{k}: {v}" for k, v in self.headers]
        lines.append(f"Content-Length: {len(body)}")
        lines.append("Connection: keep-alive" if self.keep_alive else "Connection: close")
        if self.cookie:
            lines.append(f"Cookie: {self.cookie}")
        self.sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode() + body)

        f = self.sock.makefile("rb")
        status_line = f.readline().decode(errors="replace").strip()
        headers = []
        while True:
            line = f.readline().decode(errors="replace").strip()
            if not line:
                break
            headers.append(line)
        length = next((int(h.split(":", 1)[1]) for h in headers if h.lower().startswith("content-length")), None)
        data = f.read(length) if length is not None else f.read()
        close = length is None or not self.keep_alive or any(
            h.lower().startswith("connection") and "close" in h.lower() for h in headers
        )
        for h in headers:
            if h.lower().startswith("set-cookie"):
                for part in h.split(":", 1)[1].split(";"):
                    if part.strip().startswith("TP_SESSIONID="):
                        self.cookie = part.strip()
        if close:
            self.sock.close()
            self.sock = None
        status = int(status_line.split()[1]) if len(status_line.split()) > 1 else 0
        print(f"     {path.split('?')[0]:<16} -> {status_line!r} ({new} connection), headers {headers}")
        return status, data


def raw_klap(ip, email, password, headers, keep_alive):
    auth = KlapTransportV2.generate_auth_hash(Credentials(email, password))
    http = RawHttp(ip, headers, keep_alive)
    local = secrets.token_bytes(16)
    status, data = http.post("/app/handshake1", local)
    if status != 200 or len(data) != 48:
        return f"handshake1 failed ({status}, {len(data)} bytes)"
    remote = data[:16]
    if hashlib.sha256(local + remote + auth).digest() != data[16:]:
        return "credentials rejected"
    status, _ = http.post("/app/handshake2", hashlib.sha256(remote + local + auth).digest())
    if status != 200:
        return f"handshake2 failed ({status})"
    session = KlapEncryptionSession(local, remote, auth)
    uuid_ = base64.b64encode(hashlib.md5(uuid.uuid4().bytes).digest()).decode()
    req = json.dumps(
        {"method": "get_device_info", "request_time_milis": int(time.time() * 1000), "terminal_uuid": uuid_},
        separators=(",", ":"),
    )
    payload, seq = session.encrypt(req)
    status, data = http.post(f"/app/request?seq={seq}", payload)
    if status != 200:
        return f"request failed ({status})"
    resp = json.loads(session.decrypt(data))
    return f"OK, device_on={resp.get('result', {}).get('device_on')}"


async def kasa_klap(ip, email, password):
    cfg = DeviceConfig(
        host=ip,
        credentials=Credentials(email, password),
        connection_type=DeviceConnectionParameters(
            DeviceFamily.SmartTapoPlug, DeviceEncryptionType.Klap, login_version=2
        ),
    )
    t = KlapTransportV2(config=cfg)
    try:
        resp = await t.send(json.dumps({"method": "get_device_info"}))
        return f"OK, device_on={resp.get('result', {}).get('device_on')}"
    except Exception as ex:
        return f"failed: {ex}"
    finally:
        await t.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ip")
    ap.add_argument("email")
    ap.add_argument("password")
    a = ap.parse_args()

    print("A. python-kasa")
    print("   result:", asyncio.run(kasa_klap(a.ip, a.email, a.password)))
    for name, headers, keep in [
        ("B. exact copy of the board", BOARD_HEADERS, True),
        ("C. board, new connection per request", BOARD_HEADERS, False),
        ("D. board with python-kasa's headers", AIOHTTP_HEADERS, True),
    ]:
        print(name)
        try:
            print("   result:", raw_klap(a.ip, a.email, a.password, headers, keep))
        except Exception as ex:
            print("   result: error", ex)


if __name__ == "__main__":
    main()
