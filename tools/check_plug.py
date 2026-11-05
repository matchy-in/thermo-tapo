"""Check that your Tapo plug accepts local control, from your PC, without the board.

    pip install python-kasa
    python tools/check_plug.py 192.168.0.106 you@example.com "your password"
    python tools/check_plug.py 192.168.0.106 you@example.com "your password" --toggle

If this works but the board doesn't, the problem is on the board side. If this fails
too, fix the plug settings, account or IP first.
"""

import argparse
import asyncio
import sys

from kasa import Credentials, Discover
from kasa.exceptions import AuthenticationError, KasaException


async def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("ip")
    ap.add_argument("email")
    ap.add_argument("password")
    ap.add_argument("--toggle", action="store_true", help="switch the plug on for 3 s, then restore it")
    args = ap.parse_args()

    print(f"1. Looking for a Tapo device at {args.ip} ...")
    try:
        dev = await Discover.discover_single(
            args.ip, credentials=Credentials(args.email, args.password), discovery_timeout=5
        )
    except KasaException as ex:
        print(f"   FAIL: nothing answered as a Tapo/Kasa device ({ex}).")
        print("   Check the IP in the Tapo app: plug > settings > Device Info.")
        return 1
    print(f"   OK: found it, protocol {dev.config.connection_type}")

    print("2. Logging in with your Tapo account ...")
    try:
        await dev.update()
    except AuthenticationError as ex:
        print(f"   FAIL: the plug refused the login ({ex}).")
        print("   Check email/password (case-sensitive) and turn on Third-Party Compatibility")
        print("   in the Tapo app (Me > Third-Party Services), then unplug the plug for 10 s.")
        return 1
    except KasaException as ex:
        print(f"   FAIL: {ex}")
        return 1
    print(f"   OK: {dev.alias} ({dev.model}), currently {'ON' if dev.is_on else 'OFF'}")

    if args.toggle:
        was_on = dev.is_on
        print("3. Switching the plug ON for 3 seconds ...")
        await dev.turn_on()
        await asyncio.sleep(3)
        await dev.update()
        print(f"   plug reports {'ON' if dev.is_on else 'OFF'}")
        if not was_on:
            await dev.turn_off()
            print("   switched back OFF")
    await dev.disconnect()
    print("All checks passed: the plug accepts local control.")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
