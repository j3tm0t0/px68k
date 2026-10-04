#!/usr/bin/env python3
"""Wrap a position-independent raw binary as a Human68k .X (no relocations).

  mkx.py <in.bin> <out.x>
"""
import struct
import sys

text = open(sys.argv[1], "rb").read()
if len(text) & 1:
    text += b"\0"
# "HU", reserved, base address, entry, text, data, bss, relocation, symbols
head = struct.pack(">2sHIIIIIII", b"HU", 0, 0, 0, len(text), 0, 0, 0, 0)
open(sys.argv[2], "wb").write(head.ljust(64, b"\0") + text)
