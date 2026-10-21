"""Round-trip test of the XDVDFS reader against a tiny image written here (no game data involved)."""
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from gh2install.xiso import Xiso, SECTOR, MAGIC  # noqa: E402


def _dir_table(entries):
    """entries: [(name, is_dir, sector, size)] -> one directory table. The nodes form a balanced binary tree; the root is
    stored first (offset 0), children are addressed in dwords."""
    entries = sorted(entries, key=lambda e: e[0].lower())
    order = []  # indices in storage order (preorder: root first)
    children = {}

    def build(lo, hi):
        if lo > hi:
            return None
        mid = (lo + hi) // 2
        order.append(mid)
        children[mid] = (build(lo, mid - 1), build(mid + 1, hi))
        return mid

    build(0, len(entries) - 1)

    def node_size(i):
        return (14 + len(entries[i][0].encode("cp1252")) + 3) // 4 * 4

    pos, dword_of = 0, {}
    for i in order:
        dword_of[i] = pos // 4
        pos += node_size(i)
    out = bytearray()
    for i in order:
        name, is_dir, sec, size = entries[i]
        left, right = (dword_of[c] if c is not None else 0 for c in children[i])
        nm = name.encode("cp1252")
        node = struct.pack("<HHIIBB", left, right, sec, size, 0x10 if is_dir else 0x20, len(nm)) + nm
        out += node + b"\xff" * (node_size(i) - len(node))
    out += b"\xff" * ((-len(out)) % SECTOR)
    return bytes(out)


def make_image(path, files, base=0x0FD90000):
    """files: {"default.xex": b"..", "gen/main.hdr": b".."} -> image with an XGD2-like offset."""
    sectors = {}  # sector -> bytes
    next_sector = [40]

    def alloc(data):
        s = next_sector[0]
        n = max(1, (len(data) + SECTOR - 1) // SECTOR)
        sectors[s] = data + b"\x00" * (n * SECTOR - len(data))
        next_sector[0] += n
        return s

    tree = {}
    for p, data in files.items():
        parts = p.split("/")
        d = tree
        for part in parts[:-1]:
            d = d.setdefault(part, {})
        d[parts[-1]] = data

    def write_dir(d):
        entries = []
        for name, val in d.items():
            if isinstance(val, dict):
                sec, size = write_dir(val)
                entries.append((name, True, sec, size))
            else:
                entries.append((name, False, alloc(val), len(val)))
        table = _dir_table(entries)
        return alloc(table), len(table)

    root_sec, root_size = write_dir(tree)
    vd = MAGIC + struct.pack("<II", root_sec, root_size) + b"\x00" * (SECTOR - len(MAGIC) - 8)
    with open(path, "wb") as f:
        f.truncate(base + next_sector[0] * SECTOR)
        f.seek(base + 32 * SECTOR)
        f.write(vd)
        for s, data in sectors.items():
            f.seek(base + s * SECTOR)
            f.write(data)


def _read(path):
    with open(path, "rb") as f:
        return f.read()


class XisoTest(unittest.TestCase):
    def test_roundtrip(self):
        files = {"default.xex": os.urandom(5000), "gen/main_0.ark": os.urandom(70000), "gen/main.hdr": b"hdr",
                 "videos/intro.wmv": os.urandom(2049), "a.txt": b"x"}
        with tempfile.TemporaryDirectory() as t:
            img = os.path.join(t, "g.iso")
            make_image(img, files)
            x = Xiso(img)
            out = os.path.join(t, "out")
            total = x.extract(out)
            x.close()
            self.assertEqual(total, sum(len(v) for v in files.values()))
            for p, data in files.items():
                self.assertEqual(_read(os.path.join(out, *p.split("/"))), data, p)

    def test_not_an_image(self):
        with tempfile.TemporaryDirectory() as t:
            p = os.path.join(t, "x.iso")
            open(p, "wb").write(b"\0" * 100000)
            with self.assertRaises(Exception):
                Xiso(p)


if __name__ == "__main__":
    unittest.main()
