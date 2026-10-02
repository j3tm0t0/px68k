#!/usr/bin/env python3
"""Build a self-starting test floppy from the free Human68k 3.02 system disk.

  mktestdisk.py <HUMAN302.XDF> <out.xdf> <autoexec> [dest=local ...]

Copies HUMAN302.XDF (from HUMN302I.LZH, see
http://retropc.net/x68000/software/sharp/human302/), strips it to HUMAN.SYS,
COMMAND.X, SYS/FLOAT2.X and SYS/IOCS.X with a minimal CONFIG.SYS, adds the
given files ("DIR/NAME.X=local/path"; directories are created) and writes
AUTOEXEC.BAT from <autoexec>, lines separated by "|" (written CR LF, ended
with ^Z like the system disk's own files). Local paths may be directories:
"DIR=localdir" copies the whole tree.
"""
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hdmfs import Disk  # noqa: E402

CONFIG = [
    "FILES     = 30",
    "BUFFERS   = 20 1024",
    "LASTDRIVE = Z:",
    "DEVICE    = \\SYS\\FLOAT2.X",
    "DEVICE    = \\SYS\\IOCS.X",
]
KEEP = {"HUMAN.SYS", "COMMAND.X", "CONFIG.SYS", "AUTOEXEC.BAT", "SYS"}
KEEP_SYS = {"FLOAT2.X", "IOCS.X"}


def text(lines):
    return "".join(l + "\r\n" for l in lines).encode("shift_jis") + b"\x1a"


def ensure_dir(disk, path):
    parts = [p for p in path.split("/") if p]
    for i in range(1, len(parts) + 1):
        sub = "/".join(parts[:i])
        cluster, name = disk.parent(sub)
        if disk.lookup(cluster, name) is None:
            disk.mkdir(sub)


def add(disk, dest, local):
    if os.path.isdir(local):
        ensure_dir(disk, dest)
        for f in sorted(os.listdir(local)):
            add(disk, dest + "/" + f, os.path.join(local, f))
        return
    if "/" in dest:
        ensure_dir(disk, dest.rsplit("/", 1)[0])
    disk.put(dest, open(local, "rb").read())


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    src, out, autoexec = sys.argv[1:4]
    shutil.copyfile(src, out)
    disk = Disk(out)
    for name, *_ in list(disk.entries()):
        if name.upper() not in KEEP and not name.startswith("Human68k"):
            disk.rm(name)
    for name, *_ in list(disk.entries(disk.find("SYS")[2])):
        if name not in (".", "..") and name.upper() not in KEEP_SYS:
            disk.rm("SYS/" + name)
    disk.put("CONFIG.SYS", text(CONFIG))
    disk.put("AUTOEXEC.BAT", text(autoexec.split("|")))
    for arg in sys.argv[4:]:
        dest, local = arg.split("=", 1)
        add(disk, dest, local)
    disk.save()
    free = len(disk.free_clusters()) * disk.csize
    print(f"{out}: {free} bytes free")


if __name__ == "__main__":
    main()
