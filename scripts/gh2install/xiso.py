"""Minimal reader for Xbox / Xbox 360 game discs (XDVDFS, ".iso"). No proprietary data is embedded here."""
import os
import struct

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Start of the game partition inside the image: plain xiso, XGD1, XGD2 (most 360 discs), XGD3.
PARTITION_OFFSETS = (0x0, 0x18300000, 0x0FD90000, 0x02080000, 0x02090000)


class XisoError(Exception):
    pass


class Xiso:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        for off in PARTITION_OFFSETS:
            self.f.seek(off + 32 * SECTOR)
            if self.f.read(len(MAGIC)) == MAGIC:
                self.base = off
                break
        else:
            raise XisoError("not an Xbox disc image (XDVDFS signature not found)")
        self.f.seek(self.base + 32 * SECTOR + len(MAGIC))
        self.root_sector, self.root_size = struct.unpack("<II", self.f.read(8))

    def close(self):
        self.f.close()

    def _read(self, sector, size):
        self.f.seek(self.base + sector * SECTOR)
        return self.f.read(size)

    def walk(self, sector=None, size=None, prefix=""):
        """Yields (relative_path, is_dir, sector, size) for every entry below a directory."""
        if sector is None:
            sector, size = self.root_sector, self.root_size
        if size == 0:
            return
        data = self._read(sector, size)
        seen = set()

        def node(offset_dwords):
            off = offset_dwords * 4
            if off in seen or off + 14 > len(data):
                return
            seen.add(off)
            left, right, sec, sz, attr, nlen = struct.unpack_from("<HHIIBB", data, off)
            if left == 0xFFFF and right == 0xFFFF and sec == 0xFFFFFFFF:
                return  # padding
            name = data[off + 14: off + 14 + nlen].decode("cp1252", "replace")
            if left not in (0, 0xFFFF):
                yield from node(left)
            yield (prefix + name, bool(attr & 0x10), sec, sz)
            if right not in (0, 0xFFFF):
                yield from node(right)

        for entry in node(0):
            yield entry
            path, is_dir, sec, sz = entry
            if is_dir:
                yield from self.walk(sec, sz, path + "/")

    def read_file(self, name):
        """Contents of a file at the root of the image, or None."""
        for p, d, sec, sz in self.walk():
            if p.lower() == name.lower() and not d:
                return self._read(sec, sz)
        return None

    def has_root_file(self, name):
        return any(p.lower() == name.lower() and not d for p, d, _, _ in self.walk() if "/" not in p)

    def extract(self, dest, progress=None, skip=("$SystemUpdate",)):
        """Extracts the image into `dest` (the console system update folder is skipped). progress(done, total, name) is optional."""
        entries = [e for e in self.walk() if not any(e[0] == k or e[0].startswith(k + "/") for k in skip)]
        total = sum(sz for _, d, _, sz in entries if not d)
        done = 0
        for path, is_dir, sec, sz in entries:
            out = os.path.join(dest, *path.split("/"))
            if is_dir:
                os.makedirs(out, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(out), exist_ok=True)
            self.f.seek(self.base + sec * SECTOR)
            left = sz
            with open(out, "wb") as o:
                while left > 0:
                    chunk = self.f.read(min(left, 8 << 20))
                    if not chunk:
                        raise XisoError("image is truncated while reading " + path)
                    o.write(chunk)
                    left -= len(chunk)
                    done += len(chunk)
                    if progress:
                        progress(done, total, path)
        return total
