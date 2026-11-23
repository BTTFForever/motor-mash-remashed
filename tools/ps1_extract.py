"""Extract the ISO9660 file system from a PS1 BIN (MODE2/2352, track 1).

Usage: python -I ps1_extract.py <disc.bin> <out_dir>
Each raw sector is 2352 bytes: 12 sync + 4 header + 8 subheader + 2048 data (Form 1).
"""
import os, struct, sys

SECTOR, DATA_OFF = 2352, 24


def read_sector(f, lba):
    f.seek(lba * SECTOR + DATA_OFF)
    return f.read(2048)


def read_extent(f, lba, size):
    out = bytearray()
    n = (size + 2047) // 2048
    for i in range(n):
        out += read_sector(f, lba + i)
    return bytes(out[:size])


def walk(f, lba, size, path, out_dir, listing):
    data = read_extent(f, lba, size)
    i = 0
    while i < len(data):
        ln = data[i]
        if ln == 0:                       # pad to next sector
            i = (i // 2048 + 1) * 2048
            continue
        rec = data[i:i + ln]
        ext = struct.unpack_from('<I', rec, 2)[0]
        sz = struct.unpack_from('<I', rec, 10)[0]
        flags = rec[25]
        nlen = rec[32]
        name = rec[33:33 + nlen]
        i += ln
        if name in (b'\x00', b'\x01'):
            continue
        name = name.decode('ascii', 'replace').split(';')[0]
        p = os.path.join(path, name)
        if flags & 2:
            os.makedirs(os.path.join(out_dir, p), exist_ok=True)
            walk(f, ext, sz, p, out_dir, listing)
        else:
            listing.append((p, ext, sz))
            with open(os.path.join(out_dir, p), 'wb') as o:
                o.write(read_extent(f, ext, sz))


def main():
    src, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    with open(src, 'rb') as f:
        pvd = read_sector(f, 16)
        assert pvd[1:6] == b'CD001', 'no ISO9660 volume descriptor'
        root = pvd[156:156 + 34]
        lba = struct.unpack_from('<I', root, 2)[0]
        size = struct.unpack_from('<I', root, 10)[0]
        listing = []
        walk(f, lba, size, '', out_dir, listing)
    for p, ext, sz in listing:
        print(f'{sz:10d}  {p}')


if __name__ == '__main__':
    main()
