"""Decode every Track/*.MAP plus the per-world tile tables in MotorMsh.exe into JSON.

Map file: 8 x u16 header [width, height, a, b, world, 0, 0, 0], then width*height u16
cells, row-major: bits 0-11 tile id, 12-13 height step, 14-15 rotation (90 deg steps).
Tile tables (from the race loader 0x401070): 200 entries x 12 bytes per world:
  +0 ptr to mesh name, +4 flags (bit0 = special/animated, handled by object pool),
  +8 type (0xc = per-cell animation table).

Usage: python -I dump_tracks.py <extracted dir> <out.json>
"""
import json, os, re, struct, sys, collections

src, out = sys.argv[1], sys.argv[2]
exe = open(os.path.join(src, "MotorMsh.exe"), "rb").read()
SECS = []
pe = struct.unpack_from("<I", exe, 0x3C)[0]
nsec = struct.unpack_from("<H", exe, pe + 6)[0]
opt = struct.unpack_from("<H", exe, pe + 20)[0]
for i in range(nsec):
    o = pe + 24 + opt + 40 * i
    vs, va, rs, rp = struct.unpack_from("<IIII", exe, o + 8)
    SECS.append((0x400000 + va, max(vs, rs), rp))

def off(va):
    for base, size, rp in SECS:
        if base <= va < base + size:
            return va - base + rp
    return None

def cstr(va):
    o = off(va)
    if o is None:
        return None
    e = exe.find(b"\0", o)
    s = exe[o:e]
    return s.decode("latin-1") if s and all(32 <= c < 127 for c in s) else None

WORLDS = {  # world type in map header -> (name, tile table VA) from the 0x401070 switch
    0: ("West", 0x440918), 1: ("Arctic", 0x441278), 2: ("Atlantis", 0x441BD8),
    3: ("Nightmare", 0x442538), 4: ("Amazon", 0x442E98), 5: ("City", 0x4437F8),
}
tables = {}
for wt, (wname, va) in WORLDS.items():
    o = off(va)
    rows = []
    for i in range(200):
        p, flags, typ = struct.unpack_from("<III", exe, o + 12 * i)
        rows.append({"name": cstr(p) if p else None, "special": bool(flags & 1), "flags": flags, "type": typ})
    tables[wt] = {"world": wname, "tiles": rows}

tracks = []
for fn in sorted(os.listdir(os.path.join(src, "Track")), key=str.lower):
    if not fn.lower().endswith(".map"):
        continue
    d = open(os.path.join(src, "Track", fn), "rb").read()
    hdr = list(struct.unpack_from("<8H", d, 0))
    w, h = hdr[0], hdr[1]
    if not (0 < w <= 256 and 0 < h <= 256) or 16 + 2 * w * h > len(d):
        continue
    cells = list(struct.unpack_from(f"<{w * h}H", d, 16))
    ids = collections.Counter(c & 0xFFF for c in cells)
    bg = ids.most_common(1)[0][0]
    flags = []
    for r in range(h):
        for c in range(w):
            v = cells[r * w + c]
            nb = [cells[rr * w + cc] for rr, cc in ((r - 1, c), (r + 1, c), (r, c - 1), (r, c + 1))
                  if 0 <= rr < h and 0 <= cc < w]
            f = 0
            if (v & 0xFFF) != bg and all((x & 0xFFF) == bg for x in nb):
                f |= 1                                    # isolated
            if (v & 0xFFF) != bg and nb and all(((x >> 12) & 3) != ((v >> 12) & 3) for x in nb):
                f |= 2                                    # height outlier
            flags.append(f)
    m = re.match(r"([A-Za-z]+)(\d+)", fn)
    tracks.append({
        "file": fn, "label": f"{m.group(1).capitalize()} {m.group(2)}" if m else fn,
        "width": w, "height": h, "header": hdr, "world": hdr[4], "background": bg,
        "cells": cells, "flags": flags, "trailing_bytes": len(d) - 16 - 2 * w * h,
    })

json.dump({"tables": tables, "tracks": tracks}, open(out, "w"), separators=(",", ":"))
print(f"{len(tracks)} tracks, {sum(len(t['cells']) for t in tracks)} cells -> {out} "
      f"({os.path.getsize(out) / 1e3:.0f} KB)")
for t in tracks[:3]:
    names = tables[t["world"]]["tiles"]
    print(t["label"], t["width"], "x", t["height"], "world", tables[t["world"]]["world"],
          "bg", names[t["background"]]["name"], "isolated", sum(1 for f in t["flags"] if f & 1))
