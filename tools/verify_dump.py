"""Check the board's KLAP traffic piece by piece against python-kasa.

1. On the board, type `tapodebug` in the Serial Monitor.
2. Copy every line starting with [Dump] into a file, e.g. dump.txt.
3. python tools/verify_dump.py you@example.com "your password" dump.txt
"""

import hashlib
import struct
import sys

from kasa import Credentials
from kasa.transports.klaptransport import KlapEncryptionSession, KlapTransportV2


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    email, password, path = sys.argv[1:]
    values = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.strip()
        if line.startswith("[Dump] ") and "=" in line:
            key, value = line[len("[Dump] "):].split("=", 1)
            values.setdefault(key, value)
    missing = [k for k in ("local", "handshake1", "cookie", "seq", "body", "json") if k not in values]
    if missing:
        print("Missing [Dump] lines:", ", ".join(missing))
        return 2

    local = bytes.fromhex(values["local"])
    hs1 = bytes.fromhex(values["handshake1"])
    remote, server_hash = hs1[:16], hs1[16:]
    seq = int(values["seq"])
    body = bytes.fromhex(values["body"])
    auth = KlapTransportV2.generate_auth_hash(Credentials(email, password))

    def report(ok, what):
        print(("PASS " if ok else "FAIL ") + what)
        return ok

    report(len(hs1) == 48, f"handshake1 reply is 48 bytes (got {len(hs1)})")
    report(hashlib.sha256(local + remote + auth).digest() == server_hash, "plug accepted these credentials")
    report(values["cookie"].startswith("TP_SESSIONID=") and ";" not in values["cookie"],
           f"cookie looks right ({values['cookie']})")

    session = KlapEncryptionSession(local, remote, auth)
    report(seq == session._seq + 1, f"sequence number (board {seq}, expected {session._seq + 1})")

    session._seq = seq
    session._generate_cipher()
    sig_ok = body[:32] == hashlib.sha256(session._sig + struct.pack(">l", seq) + body[32:]).digest()
    report(sig_ok, "request signature")
    try:
        plain = session.decrypt(body)
        report(plain == values["json"], f"request decrypts to the JSON the board meant to send: {plain}")
    except Exception as ex:
        report(False, f"request decrypts ({ex})")

    expected, _ = KlapEncryptionSession(local, remote, auth).encrypt(values["json"])
    report(expected == body, "whole request is byte-for-byte what python-kasa would send")
    return 0


if __name__ == "__main__":
    sys.exit(main())
