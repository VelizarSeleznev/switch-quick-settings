#!/usr/bin/env python3
"""Upload dist/sd to a Switch over FTP (Sphaira's server: root shows sdmc:).

Usage:
  scripts/deploy.py HOST [PORT]           upload everything in dist/sd (default port 5000)
  scripts/deploy.py HOST [PORT] --ovl     upload only switch/.overlays/ovlmenu.ovl
  scripts/deploy.py HOST [PORT] --log     print sdmc:/config/quick-settings/log.txt

New sysmodules (nx-ovlloader, sys-clk) start after a reboot. A new ovlmenu.ovl
is picked up the next time nx-ovlloader loads it (reboot, or after another
overlay launched from the panel exits).
"""
import hashlib
import io
import os
import sys
from ftplib import FTP, error_perm, error_temp

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(ROOT, "dist", "sd")
OVL = "switch/.overlays/ovlmenu.ovl"
LOG = "config/quick-settings/log.txt"
FTP_ERRORS = (error_perm, error_temp)


def sd(path):
    return "sdmc:/" + path


def fetch(ftp, path):
    buf = io.BytesIO()
    try:
        ftp.retrbinary("RETR " + sd(path), buf.write)
    except FTP_ERRORS:
        return None
    return buf.getvalue()


def put(ftp, path, data):
    parts = path.split("/")[:-1]
    for i in range(1, len(parts) + 1):
        try:
            ftp.mkd(sd("/".join(parts[:i])))
        except FTP_ERRORS:
            pass
    ftp.storbinary("STOR " + sd(path), io.BytesIO(data))
    if fetch(ftp, path) != data:
        sys.exit(f"verify failed: {path}")
    print(f"  wrote {path} ({len(data)} B, sha256 {hashlib.sha256(data).hexdigest()[:12]})")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        sys.exit(__doc__)
    host = args[0]
    port = int(args[1]) if len(args) > 1 else 5000
    ftp = FTP()
    ftp.connect(host, port, timeout=20)
    ftp.login()
    ftp.set_pasv(True)

    if "--log" in sys.argv:
        log = fetch(ftp, LOG)
        print(log.decode(errors="replace") if log else "(no log yet)")
        return

    for dirpath, _, files in os.walk(DIST):
        for name in sorted(files):
            local = os.path.join(dirpath, name)
            rel = os.path.relpath(local, DIST)
            if "--ovl" in sys.argv and rel != OVL:
                continue
            with open(local, "rb") as f:
                put(ftp, rel, f.read())
    ftp.quit()
    print("done")


if __name__ == "__main__":
    main()
