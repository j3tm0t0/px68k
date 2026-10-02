#!/usr/bin/python3
"""Talk to px68k's debug server (psp/debug.h) on the PSP.

  psp-debug.py log [seconds]          follow the log (Ctrl-C or seconds to stop)
  psp-debug.py cmd <text...>          send one command, then follow the log for
                                      FOLLOW_SEC seconds (default 3), e.g.
                                        cmd fdd 0 /PSP/GAME/PX68K/disk/game.xdf
                                        cmd pad circle 200
                                        cmd reset
  psp-debug.py shot [out.png]         screenshot (default shot.png)
  psp-debug.py get <remote> <local>   download a file ("/PSP/..." = the device
                                      px68k runs from, or "ms0:/...")
  psp-debug.py push [EBOOT.PBP]       replace the running EBOOT (default ./EBOOT.PBP),
                                      restart it and follow the log
  psp-debug.py back                   return to pspbrew.dev (ends `psp.py run`)

The key is PSP_DEBUG_KEY_FILE (default: ../pspbrew/tools/.debug.key, the key
pspbrew.dev uses); px68k must have the same key as debug.key next to its
EBOOT. PSP_HOST / PSP_PORT override the address (default 192.168.1.102:8026).
Runs with /usr/bin/python3: interpreters without macOS Local Network
permission get "No route to host".
"""
import os
import socket
import subprocess
import sys
import time


def default_key_file():
    """pspbrew/tools/.debug.key in the nearest ancestor holding a pspbrew checkout."""
    d = os.path.dirname(os.path.abspath(__file__))
    while d != os.path.dirname(d):
        d = os.path.dirname(d)
        path = os.path.join(d, "pspbrew", "tools", ".debug.key")
        if os.path.exists(path):
            return path
    return "debug.key"


KEY_FILE = os.environ.get("PSP_DEBUG_KEY_FILE") or default_key_file()
HOST = os.environ.get("PSP_HOST", "192.168.1.102")
PORT = int(os.environ.get("PSP_PORT", "8026"))
PSPBREW_DEV = "/PSP/GAME/pspbrew.dev/EBOOT.PBP"


def connect(retries=60):
    with open(KEY_FILE) as f:
        key = f.read().strip()
    for _ in range(retries):
        try:
            s = socket.create_connection((HOST, PORT), timeout=5)
            s.sendall(f"auth {key}\n".encode())
            s.settimeout(10)
            if s.recv(8) == b"OK auth\n":
                s.settimeout(None)
                return s
            s.close()
            sys.exit("debug: key rejected")
        except OSError:
            time.sleep(2)
    sys.exit(f"debug: {HOST}:{PORT} not reachable")


def drain(s):
    """Skip the log history so replies are not mixed up with old lines."""
    s.settimeout(0.5)
    try:
        while s.recv(65536):
            pass
    except socket.timeout:
        pass
    s.settimeout(30)


def follow(s, seconds=None):
    """Print the log until disconnect, Ctrl-C, or `seconds` have passed."""
    deadline = time.time() + seconds if seconds else None
    s.settimeout(1)
    try:
        while not deadline or time.time() < deadline:
            try:
                data = s.recv(4096)
            except socket.timeout:
                continue
            if not data:
                print("\n[disconnected]")
                return
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()
    except KeyboardInterrupt:
        return


def recv_sized(s, tag):
    """Read a "<tag> <size>\\n<bytes>" reply; returns the bytes or None (size -1)."""
    buf = b""
    while tag + b" " not in buf or b"\n" not in buf.split(tag + b" ", 1)[1]:
        chunk = s.recv(65536)
        if not chunk:
            sys.exit("debug: disconnected")
        buf += chunk
        if b"ERR " in buf:
            sys.exit(buf[buf.index(b"ERR "):].split(b"\n")[0].decode())
    head, data = buf.split(tag + b" ", 1)[1].split(b"\n", 1)
    size = int(head)
    if size < 0:
        return None
    while len(data) < size:
        chunk = s.recv(65536)
        if not chunk:
            sys.exit("debug: truncated")
        data += chunk
    return data[:size]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == "log":
        follow(connect(), float(args[0]) if args else None)
    elif cmd == "cmd":
        s = connect()
        drain(s)
        s.sendall((" ".join(args) + "\n").encode())
        follow(s, float(os.environ.get("FOLLOW_SEC", "3")))
    elif cmd == "shot":
        out = args[0] if args else "shot.png"
        s = connect()
        drain(s)
        s.sendall(b"shot\n")
        data = recv_sized(s, b"SHOT")
        bmp = out[:-4] + ".bmp" if out.endswith(".png") else out
        with open(bmp, "wb") as f:
            f.write(data)
        if bmp != out:
            subprocess.run(["sips", "-s", "format", "png", bmp, "--out", out], check=True,
                           stdout=subprocess.DEVNULL)
            os.unlink(bmp)
        print(out)
    elif cmd == "get" and len(args) == 2:
        s = connect()
        drain(s)
        s.sendall(f"get {args[0]}\n".encode())
        data = recv_sized(s, b"FILE")
        if data is None:
            sys.exit(f"get: cannot read {args[0]}")
        with open(args[1], "wb") as f:
            f.write(data)
        print(f"{args[1]} ({len(data)} bytes)")
    elif cmd == "push":
        data = open(args[0] if args else "EBOOT.PBP", "rb").read()
        s = connect()
        # pspbrew.dev answers a different port, but make sure it is px68k anyway.
        s.settimeout(1)
        seen, deadline = b"", time.time() + 3
        while b"PX68K" not in seen and time.time() < deadline:
            try:
                seen += s.recv(4096)
            except socket.timeout:
                pass
        if b"PX68K" not in seen:
            sys.exit(f"push: the app on {HOST}:{PORT} is not px68k")
        s.settimeout(120)
        s.sendall(f"push {len(data)}\n".encode() + data)
        s.sendall(b"exec\n")
        follow(s, 30)
        print("--- waiting for restart ---")
        time.sleep(5)
        follow(connect(), float(os.environ.get("FOLLOW_SEC", "0")) or None)
    elif cmd == "back":
        s = connect()
        drain(s)
        s.sendall(f"launch {PSPBREW_DEV}\n".encode())
        follow(s, 5)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
