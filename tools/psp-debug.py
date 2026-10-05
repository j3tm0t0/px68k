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
  psp-debug.py benchf <frame> <frames> <skip>...
                                      reset, then time <frames> frames from
                                      <frame> on flat out without the WLAN, once
                                      per frame skip given; prints the results
                                      (BENCH_PROF=0 (default): without a profiler,
                                      1: the timers (builds with -DPROF_TIMERS),
                                      2: the sampling profiler, every
                                      BENCH_PERIOD us (default 250),
                                      BENCH_RT=1: paced in real time with sound)
  psp-debug.py pause | resume         stop / restart the emulation
  psp-debug.py back                   return to pspbrew.dev (ends `psp.py run`)
  psp-debug.py bench [skip...]        with the profiler on, run "bench" at each
                                      frame skip (default 5 2 1; BENCH_SEC each,
                                      default 6) and print the averages

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
        drain(s)
        # Only px68k serves this port (pspbrew.dev uses 8023).
        s.settimeout(120)
        s.sendall(f"push {len(data)}\n".encode() + data)
        s.sendall(b"exec\n")
        follow(s, 30)
        print("--- waiting for restart ---")
        time.sleep(5)
        follow(connect(), float(os.environ.get("FOLLOW_SEC", "0")) or None)
    elif cmd == "bench":
        seconds = int(os.environ.get("BENCH_SEC", "6"))
        results = []
        for skip in (args or ["5", "2", "1"]):
            s = connect()
            drain(s)
            s.sendall(f"prof on\nskip {skip}\nbench {seconds}\n".encode())
            # Wait until the emulator has taken the command (it leaves the WLAN).
            seen, s_deadline = b"", time.time() + 30
            s.settimeout(1)
            while b"bench: %d s" % seconds not in seen and time.time() < s_deadline:
                try:
                    chunk = s.recv(4096)
                except socket.timeout:
                    continue
                if not chunk:
                    break
                seen += chunk
            s.close()
            time.sleep(seconds + 1)
            # The WLAN was left during the run; the log comes back with it.
            s = connect()
            text = b""
            s.settimeout(1)
            deadline = time.time() + 60
            while b"bench: done" not in text.split(b"bench: %d s" % seconds)[-1] and time.time() < deadline:
                try:
                    chunk = s.recv(65536)
                except socket.timeout:
                    continue
                if not chunk:
                    break
                text += chunk
            s.close()
            run = text.decode("utf-8", "replace").split(f"bench: {seconds} s")[-1].split("bench: done")[0]
            lines = run.splitlines()
            fps = [l for l in lines if l.startswith("fps:")][1:]  # the first second is partial
            prof = [l for l in lines if l.startswith("prof:")][1:]
            def avg(rows, key):
                vals = []
                for r in rows:
                    w = r.split()
                    if key in w:
                        v = w[w.index(key) + 1].rstrip("ms").split("/")[0]
                        vals.append(float(v))
                return sum(vals) / len(vals) if vals else float("nan")
            row = {"skip": skip, "emu": avg(fps, "emu"), "drawn": avg(fps, "drawn"), "exec": avg(fps, "avg")}
            for k in ("cpu", "grp", "text", "bg", "mix", "draw", "snd", "lines"):
                row[k] = avg(prof, k) / (1 if k == "lines" else 10)
            results.append(row)
        print("skip  emu  drawn  exec | cpu   grp   text  bg    mix   draw  snd  (ms/frame) | lines/f")
        for r in results:
            print(f"{r['skip']:>4} {r['emu']:4.0f} {r['drawn']:6.0f} {r['exec']:5.1f} |"
                  f" {r['cpu']:4.1f}  {r['grp']:4.1f}  {r['text']:4.1f}  {r['bg']:4.1f}  {r['mix']:4.1f}"
                  f"  {r['draw']:4.1f}  {r['snd']:4.1f}             | {r['lines']:5.0f}")
        s = connect()
        s.sendall(b"skip 5\n")
        s.close()
    elif cmd == "benchf" and len(args) >= 3:
        start, frames = args[0], args[1]
        for skip in args[2:]:
            s = connect()
            drain(s)
            s.sendall(f"benchf {start} {frames} {skip} {os.environ.get('BENCH_PROF', '0')} {os.environ.get('BENCH_RT', '0')} "
                      f"{os.environ.get('BENCH_PERIOD', '250')}\n".encode())
            s.close()
            deadline = time.time() + float(os.environ.get("BENCH_TIMEOUT", "900"))
            result = None
            while result is None and time.time() < deadline:
                time.sleep(10)
                try:
                    s = connect(retries=3)
                except SystemExit:
                    continue
                text = b""
                s.settimeout(1)
                read_until = time.time() + 3  # the log keeps coming; take what is there
                try:
                    while time.time() < read_until:
                        chunk = s.recv(65536)
                        if not chunk:
                            break
                        text += chunk
                except socket.timeout:
                    pass
                s.close()
                run = text.decode("utf-8", "replace").split(f"debug: > benchf {start} {frames} {skip} ")[-1]
                samp = []
                for line in run.splitlines():
                    if line.startswith("benchf: ") and " us/frame" in line:
                        result = line
                    elif line.startswith("samp benchf: "):
                        samp.append(line)
            print(f"skip {skip}: {result or 'timed out'}")
            for line in samp:
                print(f"skip {skip}: {line}")
    elif cmd in ("pause", "resume"):
        s = connect()
        drain(s)
        s.sendall(f"{cmd}\n".encode())
        follow(s, 2)
    elif cmd == "back":
        s = connect()
        drain(s)
        s.sendall(f"launch {PSPBREW_DEV}\n".encode())
        follow(s, 5)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
