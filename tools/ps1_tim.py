"""Convert PS1 TIM images (possibly several concatenated in one file) to PNG.

Usage: python -I ps1_tim.py <file.TIM> <out_prefix> [scale]
Writes <out_prefix>_<n>.png per image and prints sizes. 4/8-bit images use their first CLUT
row; colour 0 (fully black, STP clear) is shown as magenta-free transparency -> dark grey.
"""
import struct, sys, zlib


def png(path, w, h, rgb, scale=1):
    rows = []
    for y in range(h):
        line = bytearray()
        for x in range(w):
            line += bytes(rgb[y * w + x]) * scale
        rows += [b'\0' + bytes(line)] * scale
    W, H = w * scale, h * scale

    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b''))


def c15(v):
    if v == 0:
        return (40, 40, 40)
    return ((v & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, ((v >> 10) & 31) * 255 // 31)


def parse(d):
    o = 0
    while o + 8 <= len(d):
        magic, flags = struct.unpack_from('<II', d, o)
        if magic != 0x10:
            break
        o += 8
        bpp = flags & 7
        clut = []
        if flags & 8:
            ln, x, y, w, h = struct.unpack_from('<IHHHH', d, o)
            clut = list(struct.unpack_from('<%dH' % (w * h), d, o + 12))
            o += ln
        ln, x, y, w16, h = struct.unpack_from('<IHHHH', d, o)
        pix = d[o + 12:o + ln]
        o += ln
        if bpp == 0:
            w = w16 * 4
            idx = []
            for b in pix[:w * h // 2]:
                idx += [b & 15, b >> 4]
            rgb = [c15(clut[i]) if i < len(clut) else (255, 0, 255) for i in idx]
        elif bpp == 1:
            w = w16 * 2
            rgb = [c15(clut[i]) if i < len(clut) else (255, 0, 255) for i in pix[:w * h]]
        elif bpp == 2:
            w = w16
            rgb = [c15(v) for v in struct.unpack_from('<%dH' % (w * h), pix)]
        else:
            w = w16 * 2 // 3
            rgb = [tuple(pix[i * 3:i * 3 + 3]) for i in range(w * h)]
        yield w, h, rgb, (x, y), bpp


def archive(d):
    """Motor Mash bundle: u32 count, count x (char name[16] (space padded, '\\n'), u32 offset,
    u32 size), then the files. Returns [(name, bytes)] or None."""
    n = struct.unpack_from('<I', d, 0)[0]
    if not 0 < n < 2000 or len(d) < 4 + n * 24:
        return None
    out = []
    for i in range(n):
        name = d[4 + i * 24:4 + i * 24 + 16].decode('latin-1').strip()
        off, size = struct.unpack_from('<II', d, 4 + i * 24 + 16)
        if off + size > len(d):
            return None
        out.append((name, d[off:off + size]))
    return out


def main():
    src, out = sys.argv[1], sys.argv[2]
    scale = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    d = open(src, 'rb').read()
    items = [('', d)] if struct.unpack_from('<I', d, 0)[0] == 0x10 else (archive(d) or [('', d)])
    for name, blob in items:
        for n, (w, h, rgb, pos, bpp) in enumerate(parse(blob)):
            tag = (name.replace('.tim', '') or 'img') + (f'_{n}' if n else '')
            png(f'{out}_{tag}.png', w, h, rgb, scale)
            print(f'{tag}: {w}x{h} {[4, 8, 16, 24][bpp]}bpp')


if __name__ == '__main__':
    main()
