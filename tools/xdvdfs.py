"""Minimal XDVDFS (Xbox 360 game disc) reader: lists and extracts an ISO.

A port of android/app/src/main/java/org/renut/android/DiscImage.java, with the
same bounds, so the build tooling and the app read a disc the same way.

    python tools/xdvdfs.py <game.iso> <out dir>
"""
import os
import struct
import sys

MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Where the volume descriptor can start, depending on how the disc was dumped.
BASES = (0, 0xFB20, 0x20600, 0x2080000, 0xFD90000)


class DiscError(Exception):
    pass


class Disc:
    def __init__(self, path):
        self.path = path
        self.size = os.path.getsize(path)
        self.entries = []  # (path, offset, size)
        self._names = set()
        self._dirs = set()
        self.total = 0
        with open(path, "rb") as f:
            self._open(f)

    def _open(self, f):
        for candidate in BASES:
            if candidate + 65536 + 2048 > self.size:
                continue
            f.seek(candidate + 65536)
            header = f.read(2048)
            if header[:20] == MAGIC and header[2028:2048] == MAGIC:
                self.base = candidate
                root, length = struct.unpack_from("<II", header, 20)
                self._read_dir(f, root, length, "", 0)
                break
        else:
            raise DiscError("Imagem de disco Xbox 360 não reconhecida.")
        if not any(p.lower() == "default.xex" for p, _, _ in self.entries):
            raise DiscError("O disco não contém default.xex na raiz.")

    def _read_dir(self, f, sector, length, parent, depth):
        if depth > 64 or length < 14 or length > 32 * 1024 * 1024 or sector in self._dirs:
            raise DiscError("Estrutura de diretórios inválida.")
        self._dirs.add(sector)
        offset = self.base + sector * 2048
        if offset + length > self.size:
            raise DiscError("Imagem truncada.")
        f.seek(offset)
        data = f.read(length)
        pending, seen = [0], set()
        while pending:
            p = pending.pop()
            if p in seen or p < 0 or p > len(data) - 14 or len(self._names) > 100000:
                raise DiscError("Árvore de diretórios inválida.")
            seen.add(p)
            left, right, child_sector, child_size, attrs, name_len = struct.unpack_from("<HHIIBB", data, p)
            if name_len == 0 or p + 14 + name_len > len(data):
                raise DiscError("Nome inválido no disco.")
            name = data[p + 14:p + 14 + name_len].decode("latin-1")
            if name in (".", "..") or any(c in name for c in "/\\:") or any(ord(c) < 32 for c in name):
                raise DiscError("Nome de arquivo inválido no disco: %r" % name)
            path = parent + name
            if path.lower() in self._names:
                raise DiscError("Nome duplicado no disco.")
            self._names.add(path.lower())
            if attrs & 16:
                if child_size > 0:
                    self._read_dir(f, child_sector, child_size, path + "/", depth + 1)
            else:
                child_offset = self.base + child_sector * 2048
                if child_offset + child_size > self.size:
                    raise DiscError("Arquivo além do fim da imagem.")
                self.total += child_size
                self.entries.append((path, child_offset, child_size))
            if left:
                pending.append(left * 4)
            if right:
                pending.append(right * 4)

    def extract(self, destination, only=None, progress=None):
        """Writes every entry (or those `only` accepts) under destination. Existing files of the right size are kept."""
        root = os.path.realpath(destination) + os.sep
        done = 0
        buffer = 1 << 20
        with open(self.path, "rb") as f:
            for path, offset, size in self.entries:
                if only is not None and not only(path):
                    continue
                target = os.path.realpath(os.path.join(destination, *path.split("/")))
                if not target.startswith(root):
                    raise DiscError("Caminho fora do destino: %s" % path)
                os.makedirs(os.path.dirname(target), exist_ok=True)
                if os.path.isfile(target) and os.path.getsize(target) == size:
                    done += size
                    continue
                temp = target + ".part"
                f.seek(offset)
                with open(temp, "wb") as out:
                    remaining = size
                    while remaining:
                        chunk = f.read(min(buffer, remaining))
                        if not chunk:
                            raise DiscError("Imagem truncada.")
                        out.write(chunk)
                        remaining -= len(chunk)
                        done += len(chunk)
                        if progress:
                            progress(done, self.total)
                os.replace(temp, target)


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    disc = Disc(argv[1])
    print("%d arquivos, %.2f GB" % (len(disc.entries), disc.total / 2 ** 30))
    disc.extract(argv[2])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
