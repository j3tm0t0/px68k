#!/usr/bin/env python3
"""Minimal reader/writer for Human68k 2HD floppy images (.hdm/.xdf, 1232 KB).

  hdmfs.py ls <image> [dir]                list a directory ("/" or "/SUB")
  hdmfs.py tree <image>                    list every file
  hdmfs.py df <image>                      free space
  hdmfs.py cat <image> <path>              print a file
  hdmfs.py get <image> <path> <local>      copy a file out
  hdmfs.py put <image> <path> <local>      create or replace a file (any size)
  hdmfs.py mkdir <image> <path>            create a directory
  hdmfs.py rm <image> <path>...            delete files (directories with
                                           everything in them)

Both boot sector layouts are handled: the old one (Hudson soft IPL, BPB
big-endian at 0x12) and the one Human68k 3.0x FORMAT.X writes ("X68IPL30",
little-endian MS-DOS style BPB at 0x0b). 1024-byte sectors, 1 sector per
cluster, FAT12, all FAT copies kept equal. Names are 18.3 (Human68k).
Writes are checked by reading the file back.
"""
import struct
import sys
import time


class Disk:
    def __init__(self, path):
        self.path = path
        self.data = bytearray(open(path, "rb").read())
        d = self.data
        if d[0x0b:0x0d] == b"\x00\x04":
            # X68IPL30: little-endian BPB at 0x0b
            self.bps = struct.unpack("<H", d[0x0b:0x0d])[0]
            self.spc = d[0x0d]
            self.reserved = struct.unpack("<H", d[0x0e:0x10])[0]
            self.nfats = d[0x10]
            self.rootents = struct.unpack("<H", d[0x11:0x13])[0]
            self.total = struct.unpack("<H", d[0x13:0x15])[0]
            self.spf = struct.unpack("<H", d[0x16:0x18])[0]
        else:
            self.bps = struct.unpack(">H", d[0x12:0x14])[0]
            self.spc = d[0x14]
            self.nfats = d[0x15]
            self.reserved = struct.unpack(">H", d[0x16:0x18])[0]
            self.rootents = struct.unpack(">H", d[0x18:0x1a])[0]
            self.total = struct.unpack(">H", d[0x1a:0x1c])[0]
            self.spf = d[0x1d]
        self.fat_off = self.reserved * self.bps
        self.root_off = (self.reserved + self.nfats * self.spf) * self.bps
        self.data_off = self.root_off + self.rootents * 32
        self.csize = self.bps * self.spc
        self.nclusters = (len(d) - self.data_off) // self.csize + 2
        if self.total:
            self.nclusters = min(self.nclusters, (self.total * self.bps - self.data_off) // self.csize + 2)

    def fat(self, n):
        off = self.fat_off + n * 3 // 2
        v = self.data[off] | self.data[off + 1] << 8
        return v >> 4 if n & 1 else v & 0xfff

    def setfat(self, n, val):
        for f in range(self.nfats):
            off = self.fat_off + f * self.spf * self.bps + n * 3 // 2
            v = self.data[off] | self.data[off + 1] << 8
            v = (v & 0xf) | val << 4 if n & 1 else (v & 0xf000) | val
            self.data[off] = v & 0xff
            self.data[off + 1] = v >> 8

    def chain(self, c):
        while 2 <= c < 0xff8:
            yield c
            c = self.fat(c)

    def free_clusters(self):
        return [c for c in range(2, self.nclusters) if self.fat(c) == 0]

    def alloc(self, n):
        free = self.free_clusters()
        if len(free) < n:
            sys.exit(f"disk full: need {n} cluster(s), {len(free)} free")
        got = free[:n]
        for a, b in zip(got, got[1:] + [0xfff]):
            self.setfat(a, b)
        return got

    def free_chain(self, c):
        for x in list(self.chain(c)):
            self.setfat(x, 0)

    def cluster_off(self, c):
        return self.data_off + (c - 2) * self.csize

    def slots(self, cluster=None):
        """Offsets of the directory entry slots of a directory, in order."""
        if cluster is None:
            return list(range(self.root_off, self.root_off + self.rootents * 32, 32))
        return [off for c in self.chain(cluster)
                for off in range(self.cluster_off(c), self.cluster_off(c) + self.csize, 32)]

    def entries(self, cluster=None):
        for off in self.slots(cluster):
            e = self.data[off:off + 32]
            if e[0] == 0:
                return
            if e[0] == 0xe5:
                continue
            name = (e[0:8] + e[0x0c:0x16]).rstrip(b" \0")
            ext = e[8:11].rstrip(b" ")
            full = name.decode("shift_jis", "replace") + ("." + ext.decode("shift_jis", "replace") if ext else "")
            attr = e[11]
            clus = struct.unpack("<H", e[0x1a:0x1c])[0]
            size = struct.unpack("<I", e[0x1c:0x20])[0]
            yield full, attr, clus, size, off

    def lookup(self, cluster, name):
        for ent in self.entries(cluster):
            if ent[0].upper() == name.upper():
                return ent
        return None

    def find(self, path):
        cluster = None
        parts = [p for p in path.split("/") if p]
        for i, p in enumerate(parts):
            ent = self.lookup(cluster, p)
            if ent is None:
                sys.exit(f"{path}: not found")
            if i == len(parts) - 1:
                return ent
            cluster = ent[2]
        sys.exit(f"{path}: is the root directory")

    def parent(self, path):
        """(cluster of the parent directory or None for the root, last name)."""
        parts = [p for p in path.split("/") if p]
        if not parts:
            sys.exit(f"{path}: bad path")
        cluster = None
        if len(parts) > 1:
            name, attr, cluster, _, _ = self.find("/".join(parts[:-1]))
            if not attr & 0x10:
                sys.exit(f"{path}: parent is not a directory")
        return cluster, parts[-1]

    def read(self, clus, size):
        out = bytearray()
        for c in self.chain(clus):
            out += self.data[self.cluster_off(c):][:self.csize]
        return bytes(out[:size])

    @staticmethod
    def encode_name(name):
        if name in (".", ".."):
            return name.encode().ljust(8), b"   ", bytes(10)
        base, dot, ext = name.upper().rpartition(".")
        if not dot:
            base, ext = ext, ""
        base, ext = base.encode("shift_jis"), ext.encode("shift_jis")
        if not base or len(base) > 18 or len(ext) > 3 or b"." in base:
            sys.exit(f"{name}: not an 18.3 name")
        return base[:8].ljust(8), ext.ljust(3), base[8:].ljust(10, b"\0")

    def new_slot(self, cluster):
        """A free entry slot in a directory, growing a subdirectory if needed."""
        for off in self.slots(cluster):
            if self.data[off] in (0, 0xe5):
                return off
        if cluster is None:
            sys.exit("root directory full")
        c = self.alloc(1)[0]
        last = list(self.chain(cluster))[-1]
        self.setfat(last, c)
        base = self.cluster_off(c)
        self.data[base:base + self.csize] = bytes(self.csize)
        return base

    def write_entry(self, off, name, attr, clus, size):
        n8, ext, n10 = self.encode_name(name)
        t = time.localtime()
        tm = t.tm_hour << 11 | t.tm_min << 5 | t.tm_sec // 2
        dt = (t.tm_year - 1980) << 9 | t.tm_mon << 5 | t.tm_mday
        self.data[off:off + 32] = n8 + ext + bytes([attr]) + n10 + struct.pack("<HHHI", tm, dt, clus, size)

    def write_clusters(self, data):
        if not data:
            return 0
        chain = self.alloc((len(data) + self.csize - 1) // self.csize)
        for i, c in enumerate(chain):
            chunk = data[i * self.csize:(i + 1) * self.csize]
            self.data[self.cluster_off(c):self.cluster_off(c) + self.csize] = chunk + bytes(self.csize - len(chunk))
        return chain[0]

    def put(self, path, data):
        cluster, name = self.parent(path)
        ent = self.lookup(cluster, name)
        attr = 0x20
        if ent:
            if ent[1] & 0x18:
                sys.exit(f"{path}: is a directory or volume label")
            attr, off = ent[1], ent[4]
            self.free_chain(ent[2])
            self.data[off] = 0xe5
        off = self.new_slot(cluster)
        self.write_entry(off, ent[0] if ent else name, attr, self.write_clusters(data), len(data))
        got = self.find(path)
        if self.read(got[2], got[3]) != data:
            sys.exit(f"{path}: read-back mismatch")

    def mkdir(self, path):
        cluster, name = self.parent(path)
        if self.lookup(cluster, name):
            sys.exit(f"{path}: exists")
        off = self.new_slot(cluster)
        c = self.alloc(1)[0]
        base = self.cluster_off(c)
        self.data[base:base + self.csize] = bytes(self.csize)
        self.write_entry(off, name, 0x10, c, 0)
        self.write_entry(base, ".", 0x10, c, 0)
        self.write_entry(base + 32, "..", 0x10, cluster or 0, 0)

    def rm(self, path):
        name, attr, clus, size, off = self.find(path)
        if attr & 0x10:
            for sub in list(self.entries(clus)):
                if sub[0] not in (".", ".."):
                    self.rm(path.rstrip("/") + "/" + sub[0])
        self.free_chain(clus)
        self.data[off] = 0xe5

    def walk(self, cluster=None, prefix=""):
        for ent in self.entries(cluster):
            if ent[0] in (".", "..") or ent[1] & 0x08:
                continue
            yield prefix + ent[0], ent
            if ent[1] & 0x10:
                yield from self.walk(ent[2], prefix + ent[0] + "/")

    def save(self):
        open(self.path, "wb").write(self.data)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd, disk, args = sys.argv[1], Disk(sys.argv[2]), sys.argv[3:]
    if cmd == "ls":
        cluster = None
        if args and args[0] not in ("", "/"):
            cluster = disk.find(args[0])[2]
        for name, attr, clus, size, _ in disk.entries(cluster):
            print(f"{name:24} {'<DIR>' if attr & 0x10 else size:>8}  attr {attr:02x}")
    elif cmd == "tree":
        for path, (name, attr, clus, size, _) in disk.walk():
            print(f"{path + ('/' if attr & 0x10 else ''):40} {'' if attr & 0x10 else size:>8}")
    elif cmd == "df":
        free = len(disk.free_clusters())
        print(f"{free * disk.csize} bytes free ({free} of {disk.nclusters - 2} clusters)")
    elif cmd == "cat":
        _, _, clus, size, _ = disk.find(args[0])
        sys.stdout.buffer.write(disk.read(clus, size))
    elif cmd == "get":
        _, _, clus, size, _ = disk.find(args[0])
        open(args[1], "wb").write(disk.read(clus, size))
    elif cmd == "put":
        new = open(args[1], "rb").read()
        disk.put(args[0], new)
        disk.save()
        print(f"put: {args[0]} ({len(new)} bytes)")
    elif cmd == "mkdir":
        disk.mkdir(args[0])
        disk.save()
    elif cmd == "rm":
        for p in args:
            disk.rm(p)
        disk.save()
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
