#!/usr/bin/env python3
"""metallib.py: parse Apple's MTLB (.metallib) container and extract each function's AIR module.

Clean-room parser written from the bytes of the macOS 26.6.2 metallibs (docs/m3-air-spike.md) and the public
write-ups (worthdoingbadly.com/metalbitcode, YuAo/MetalLibraryArchive layout notes). No Apple code is used.

Layout as observed (all little endian):
  0x00  "MTLB"
  0x04  u16 target (0x8001 on every macOS file seen), u16 file version major, u16 minor,
        u16 os-type word (0x8100), u16 os major, u16 os minor           [field meanings: INFER]
  0x10  u64 file size
  0x18  u64 function-list offset, size          (offset 0x58: u32 count, then count entries)
  0x28  u64 public-metadata offset, size
  0x38  u64 private-metadata offset, size
  0x48  u64 bitcode offset, size
  entry: u32 entry_size (counts its own 4 bytes), then tags, each  4-char name, u16 length, data;
         the closing tag "ENDT" has no length. Tags seen: NAME (NUL-terminated), TYPE (u8), HASH (32 B sha256),
         OFFT (3 x u64: public md, private md, bitcode offsets, relative to the section starts),
         VERS (u16 air major, u16 air minor, u16 lang major, u16 lang minor), MDSZ (u64 module size), RFLT (u64),
         and a few optional ones (TESS, LAYR, ARGM, ... kept raw in .tags).
  module at bitcode_offset + OFFT[2], MDSZ bytes: an LLVM bitcode wrapper (magic 0x0b17c0de: version, offset,
         size, cputype as u32) around the raw bitcode ("BC" 0xC0DE).

Usage:
  metallib.py list   FILE.metallib [--json]
  metallib.py extract FILE.metallib OUTDIR [--name REGEX] [--wrapped]   writes <NNNN>_<name>.bc (raw bitcode by default)
"""
import argparse
import hashlib
import json
import mmap
import os
import re
import struct
import sys
from dataclasses import dataclass, field

WRAPPER_MAGIC = 0x0B17C0DE
RAW_MAGIC = b"BC\xc0\xde"
FUNC_TYPES = {0: "vertex", 1: "fragment", 2: "kernel", 3: "unqualified", 4: "visible", 5: "extern", 6: "intersection", 7: "mesh", 8: "object"}


class FormatError(Exception):
    pass


@dataclass
class Function:
    index: int
    name: str = ""
    type: int = -1
    hash: bytes = b""
    off_public: int = 0
    off_private: int = 0
    off_bitcode: int = 0
    module_size: int = 0
    air_version: tuple = ()
    tags: dict = field(default_factory=dict)  # tags this parser does not interpret, raw bytes

    @property
    def type_name(self):
        return FUNC_TYPES.get(self.type, "type%d" % self.type)


@dataclass
class Metallib:
    path: str
    size: int
    target: int
    file_version: tuple
    os_word: int
    os_version: tuple
    sections: dict
    functions: list


def parse(data, path="?"):
    """data: bytes-like (mmap ok). Raises FormatError when the container does not look like MTLB."""
    if len(data) < 0x58 or bytes(data[:4]) != b"MTLB":
        raise FormatError("not an MTLB container")
    target, vmaj, vmin, osw, osmaj, osmin = struct.unpack_from("<6H", data, 4)
    fsize, fo, fs, po, ps, qo, qs, bo, bs = struct.unpack_from("<9Q", data, 0x10)
    if fsize != len(data):
        raise FormatError("header file size %d != actual %d" % (fsize, len(data)))
    sections = {"functions": (fo, fs), "public": (po, ps), "private": (qo, qs), "bitcode": (bo, bs)}
    if fo + 4 > len(data):
        raise FormatError("function list outside the file")
    count = struct.unpack_from("<I", data, fo)[0]
    pos = fo + 4
    funcs = []
    for i in range(count):
        if pos + 4 > len(data):
            raise FormatError("function %d: entry outside the file" % i)
        esz = struct.unpack_from("<I", data, pos)[0]
        end = pos + esz
        o = pos + 4
        fn = Function(index=i)
        while True:
            tag = bytes(data[o:o + 4])
            if tag == b"ENDT":
                o += 4
                break
            if len(tag) < 4 or not tag.isalpha() or o + 6 > end:
                raise FormatError("function %d: bad tag %r at 0x%x" % (i, tag, o))
            ln = struct.unpack_from("<H", data, o + 4)[0]
            v = bytes(data[o + 6:o + 6 + ln])
            o += 6 + ln
            if tag == b"NAME":
                fn.name = v.rstrip(b"\0").decode("utf-8", "replace")
            elif tag == b"TYPE":
                fn.type = v[0]
            elif tag == b"HASH":
                fn.hash = v
            elif tag == b"OFFT":
                fn.off_public, fn.off_private, fn.off_bitcode = struct.unpack("<3Q", v)
            elif tag == b"MDSZ":
                fn.module_size = struct.unpack("<Q", v)[0]
            elif tag == b"VERS":
                fn.air_version = struct.unpack("<%dH" % (len(v) // 2), v)
            else:
                fn.tags[tag.decode()] = v
        if o != end:
            raise FormatError("function %d: entry size says 0x%x, tags end at 0x%x" % (i, end, o))
        funcs.append(fn)
        pos = end
    return Metallib(path, len(data), target, (vmaj, vmin), osw, (osmaj, osmin), sections, funcs)


def module_bytes(data, lib, fn, wrapped=False):
    """The function's bitcode (raw "BC C0DE" by default; the wrapper too when wrapped)."""
    a = lib.sections["bitcode"][0] + fn.off_bitcode
    if fn.module_size == 0 or a + fn.module_size > len(data):
        raise FormatError("%s: module 0x%x+0x%x outside the file" % (fn.name, a, fn.module_size))
    m = bytes(data[a:a + fn.module_size])
    if struct.unpack_from("<I", m, 0)[0] == WRAPPER_MAGIC:
        _, _, off, size, _ = struct.unpack_from("<5I", m, 0)
        raw = m[off:off + size]
        return m if wrapped else raw
    if m[:4] == RAW_MAGIC:
        return m
    raise FormatError("%s: neither a bitcode wrapper nor raw bitcode (first bytes %s)" % (fn.name, m[:8].hex()))


def open_lib(path):
    f = open(path, "rb")
    data = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    return data, parse(data, path)


def safe(name):
    return re.sub(r"[^A-Za-z0-9_.-]", "_", name)[:120]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    pl = sub.add_parser("list")
    pl.add_argument("file")
    pl.add_argument("--json", action="store_true")
    pe = sub.add_parser("extract")
    pe.add_argument("file")
    pe.add_argument("outdir")
    pe.add_argument("--name", help="only functions whose name matches this regex")
    pe.add_argument("--wrapped", action="store_true", help="keep the 0x0b17c0de wrapper")
    a = ap.parse_args()
    data, lib = open_lib(a.file)
    if a.cmd == "list":
        if a.json:
            json.dump({"path": lib.path, "size": lib.size, "target": lib.target, "file_version": lib.file_version,
                       "os_version": lib.os_version, "functions": [{"name": f.name, "type": f.type_name, "module_size": f.module_size,
                       "air_version": f.air_version, "hash": f.hash.hex(), "tags": {k: v.hex() for k, v in f.tags.items()}} for f in lib.functions]},
                      sys.stdout, indent=1)
            print()
            return
        print("%s: MTLB target=0x%04x file v%d.%d os %d.%d, %d bytes, %d functions" % (lib.path, lib.target, *lib.file_version, *lib.os_version, lib.size, len(lib.functions)))
        for f in lib.functions:
            print("%4d %-12s %9d  air %s  %s" % (f.index, f.type_name, f.module_size, ".".join(map(str, f.air_version[:2])), f.name))
        return
    os.makedirs(a.outdir, exist_ok=True)
    rx = re.compile(a.name) if a.name else None
    n = 0
    for f in lib.functions:
        if rx and not rx.search(f.name):
            continue
        out = os.path.join(a.outdir, "%04d_%s.bc" % (f.index, safe(f.name)))
        with open(out, "wb") as fh:
            fh.write(module_bytes(data, lib, f, a.wrapped))
        n += 1
    print("wrote %d module(s) to %s" % (n, a.outdir))


if __name__ == "__main__":
    main()
