#!/usr/bin/env python3
"""Minimal reader/writer for Human68k 2HD floppy images (.hdm/.xdf, 1232 KB).

  hdmfs.py ls <image> [dir]                list a directory ("/" or "/SUB")
  hdmfs.py cat <image> <path>              print a file
  hdmfs.py put <image> <path> <local>      overwrite an existing file in place
                                           (the new data must fit its clusters)

The BPB is big-endian (Human68k): 1024-byte sectors, 1 sector per cluster,
FAT12. Only what a direct-boot AUTOEXEC.BAT needs is supported.
"""
import struct
import sys


class Disk:
    def __init__(self, path):
        self.path = path
        self.data = bytearray(open(path, "rb").read())
        d = self.data
        self.bps = struct.unpack(">H", d[0x12:0x14])[0]
        self.spc = d[0x14]
        self.nfats = d[0x15]
        self.reserved = struct.unpack(">H", d[0x16:0x18])[0]
        self.rootents = struct.unpack(">H", d[0x18:0x1a])[0]
        self.spf = d[0x1d]
        self.fat_off = self.reserved * self.bps
        self.root_off = (self.reserved + self.nfats * self.spf) * self.bps
        self.data_off = self.root_off + self.rootents * 32
        self.csize = self.bps * self.spc

    def fat(self, n):
        off = self.fat_off + n * 3 // 2
        v = self.data[off] | self.data[off + 1] << 8
        return v >> 4 if n & 1 else v & 0xfff

    def chain(self, c):
        while 2 <= c < 0xff8:
            yield c
            c = self.fat(c)

    def entries(self, cluster=None):
        if cluster is None:
            blobs = [(self.root_off, self.rootents * 32)]
        else:
            blobs = [(self.data_off + (c - 2) * self.csize, self.csize) for c in self.chain(cluster)]
        for base, size in blobs:
            for off in range(base, base + size, 32):
                e = self.data[off:off + 32]
                if e[0] == 0:
                    return
                if e[0] == 0xe5:
                    continue
                name = (e[0:8] + e[0x0c:0x15]).rstrip(b" \0")
                ext = e[8:11].rstrip(b" ")
                full = name.decode("shift_jis", "replace") + ("." + ext.decode("shift_jis", "replace") if ext else "")
                attr = e[11]
                clus = struct.unpack("<H", e[0x1a:0x1c])[0]
                size = struct.unpack("<I", e[0x1c:0x20])[0]
                yield full, attr, clus, size, off

    def find(self, path):
        cluster = None
        parts = [p for p in path.upper().split("/") if p]
        for i, p in enumerate(parts):
            for name, attr, clus, size, off in self.entries(cluster):
                if name.upper() == p:
                    if i == len(parts) - 1:
                        return name, attr, clus, size, off
                    cluster = clus
                    break
            else:
                sys.exit(f"{path}: not found")

    def read(self, clus, size):
        out = bytearray()
        for c in self.chain(clus):
            out += self.data[self.data_off + (c - 2) * self.csize:][:self.csize]
        return bytes(out[:size])


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd, disk = sys.argv[1], Disk(sys.argv[2])
    if cmd == "ls":
        cluster = None
        if len(sys.argv) > 3 and sys.argv[3] not in ("", "/"):
            cluster = disk.find(sys.argv[3])[2]
        for name, attr, clus, size, _ in disk.entries(cluster):
            print(f"{name:24} {'<DIR>' if attr & 0x10 else size:>8}  attr {attr:02x}")
    elif cmd == "cat":
        _, _, clus, size, _ = disk.find(sys.argv[3])
        sys.stdout.buffer.write(disk.read(clus, size))
    elif cmd == "put":
        _, _, clus, size, off = disk.find(sys.argv[3])
        new = open(sys.argv[4], "rb").read()
        chain = list(disk.chain(clus))
        if len(new) > len(chain) * disk.csize:
            sys.exit(f"put: {len(new)} bytes do not fit in {len(chain)} cluster(s)")
        for i, c in enumerate(chain):
            base = disk.data_off + (c - 2) * disk.csize
            chunk = new[i * disk.csize:(i + 1) * disk.csize]
            disk.data[base:base + disk.csize] = chunk + bytes(disk.csize - len(chunk))
        struct.pack_into("<I", disk.data, off + 0x1c, len(new))
        open(disk.path, "wb").write(disk.data)
        print(f"put: {sys.argv[3]} ({len(new)} bytes)")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
