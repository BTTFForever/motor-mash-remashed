"""trackmap.py - read and write Motor Mash track maps (Track/*.MAP). See TRACKS.md.

  python -I trackmap.py dump  <in.map> <out.json>          map -> JSON
  python -I trackmap.py build <in.json> <out.map>          JSON -> map
  python -I trackmap.py check <Track folder>               round-trip every map, byte for byte,
                                                           and test a generated line on each

Map layout (little-endian 16-bit values; N = width x height; Q = 4w x 4h quarter-tile squares):
  header   8 x u16  width, height, start x, start y (cells), world, 0, 0, 0
           worlds: 0 West, 1 Ice, 2 Atlantis, 3 Night, 4 Amazon, 5 City
  tiles    N x u16  row-major; bits 0-11 tile id, 12-13 height step, 14-15 rotation (x 90 degrees)
  line     Q bytes  racing line: per quarter-tile square the direction the AI goes on from
                    there: 1 up, 3 up-right, 5 right, 7 down-right, 9 down, 11 down-left,
                    13 left, 15 up-left (up = towards row 0); 0 and 64 = keep going (64 marks
                    some crossings in West maps); bit 7 ignored. Each car walks a look-ahead
                    point along it from the start cell's centre in 128-unit steps (a tile is
                    1016 world units), so it must be followable that way (see trace_line).
  gategrid Q x s16  the gate whose line crosses the square, -1 = none (= the gates rasterised,
                    the first gate winning where two overlap)
  gates    8 x s16 per gate: type, x1, y1, x2, y2 (quarter tiles), number, rx, ry.
           type: driving direction in the low 3 bits (0 up, 2 right, 4 down, 6 left, odd =
           diagonal) plus flag bits; (rx / 2, ry / 2) is the gate's racing point, where a car
           that leaves the track is put back. The first record's type slot holds the list
           length instead: records x 8 - 8 (16-bit units), counting the end record (x1 = -1;
           its other values are leftovers). Race positions count gates passed; laps = gates / count.
  pickups  8 x 4 bytes: x, y (quarter tiles), number, height step; 0xff = unused.
           What spawns comes from a per-track table in the exe (0x45b650).

The JSON keeps the line and the gate grid as given, so dump + build is exact. For a new map
leave them out ("line": null / "gategrid": null): build then draws the gate grid from the
gates and the racing line through the gates' racing points (trace_line), checked to lap on
every shipped track.
"""
import json, struct, sys, os

def bresenham(x0, y0, x1, y1):
    pts = []
    dx, dy = abs(x1 - x0), -abs(y1 - y0)
    sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
    e = dx + dy
    while True:
        pts.append((x0, y0))
        if x0 == x1 and y0 == y1:
            return pts
        e2 = 2 * e
        if e2 >= dy:
            e += dy
            x0 += sx
        if e2 <= dx:
            e += dx
            y0 += sy


def heading(dx, dy):
    """The 8-way direction code of a segment (1 up, 3 up-right ... 15 up-left)."""
    import math
    if dx == 0 and dy == 0:
        return 1
    a = math.atan2(dx, -dy)                         # 0 = up, clockwise
    return 1 + 2 * (int(round(a / (math.pi / 4))) % 8)


# The AI's look-ahead point lives in world units: a tile is 1016 (0x3f8) wide, the point steps
# 128 (0x80) at a time, and the square it reads is found as the game does (0x421a90).
TILE = 0x3F8
WSTEP = {1: (0, 0x80), 3: (0x80, 0x80), 5: (0x80, 0), 7: (0x80, -0x80), 9: (0, -0x80),
         11: (-0x80, -0x80), 13: (-0x80, 0), 15: (-0x80, 0x80)}   # (x, z); z is minus the row


def _tdiv(a, b):
    return int(a / b)                                   # C division, truncating towards zero


def world_square(x, z):
    """The quarter-tile square (column, row) the game reads for world point (x, z)."""
    x2 = x + _tdiv(x, TILE) * 8
    z2 = z + _tdiv(z, TILE) * 8
    return _tdiv(x2, 256), -_tdiv(z2, 256)


def start_point(start):
    """Where a car's look-ahead point starts: the start cell's centre."""
    return start[0] * TILE + TILE // 2, -(start[1] * TILE + TILE // 2)


def follow_line(line, W, H, start, steps):
    """Simulate a car's look-ahead point on a racing line from the start cell; returns the
    squares (row * W + column) it reads, in order. As in the game, 0 and 64 keep the old
    direction and bit 7 is ignored."""
    x, z = start_point(start)
    d, seen = 0, []
    for _ in range(steps):
        c, r = world_square(x, z)
        if not (0 <= c < W and 0 <= r < H):
            break
        v = line[r * W + c] & 0x7F
        if v and v != 64:
            d = v
        if not d:
            break
        seen.append(r * W + c)
        dx, dz = WSTEP[d]
        x += dx
        z += dz
    return seen


def lap_progress(m, seen):
    """How many gates a walk passes in order (skipping up to 5, as a car cutting a corner can)."""
    n, nxt, passed = len(m['gates']), 0, 0
    for sq in seen:
        g = m['gategrid'][sq] if m.get('gategrid') else -1
        if g >= 0 and (g - nxt) % n < 6:
            passed += (g - nxt) % n + 1
            nxt = g + 1
    return passed


def trace_line(W, H, start, gates):
    """Draw a racing line the AI can follow: walk the look-ahead point from the start cell
    through the gate midpoints in order (twice round, so the lap closes), writing the
    direction towards the next midpoint into each empty square it reads and then stepping
    the way the line says, exactly as the game will. A square the track crosses again
    later in another direction is cleared (0 = keep straight), as in the shipped maps."""
    line = [0] * (W * H)
    first = [0] * (W * H)                               # step at which a square was written
    x, z = start_point(start)
    targets = [(g.get('cx', g['x1'] + g['x2']) * 128, -g.get('cy', g['y1'] + g['y2']) * 128) for g in gates]   # racing points, as the respawn (0x41e8b0)
    targets[0] = (x, z)                                 # gate 0 is the start line: aim at the start
    home = world_square(x, z)
    k = 0
    t = 0
    for lap in range(3):                                # later laps only fill squares still empty,
        for n in range(len(targets)):                   # which closes the lap at the start
            tx, tz = targets[(k + 1 + n) % len(targets)]
            for i in range(64):
                if i and abs(tx - x) <= 0xC0 and abs(tz - z) <= 0xC0:
                    break
                c, r = world_square(x, z)
                if not (0 <= c < W and 0 <= r < H):
                    return line
                sq = r * W + c
                d = heading(tx - x, -(tz - z))
                t += 1
                if line[sq] == 0 and first[sq] == 0:
                    line[sq], first[sq] = d, t
                elif (lap == 0 and line[sq] and t - first[sq] > 16 and (c, r) != home
                      and 4 <= (line[sq] - d) % 16 <= 12):
                    line[sq] = 0                        # the track crosses itself here
                step = line[sq] or d
                dx, dz = WSTEP[step]
                x += dx
                z += dz
    return line


def read(path):
    d = open(path, 'rb').read()
    hdr = list(struct.unpack_from('<8H', d, 0))
    w, h = hdr[0], hdr[1]
    n, W, H = w * h, 4 * w, 4 * h
    o = 16
    tiles = list(struct.unpack_from('<%dH' % n, d, o)); o += 2 * n
    line = list(d[o:o + W * H]); o += W * H
    grid = list(struct.unpack_from('<%dh' % (W * H), d, o)); o += 2 * W * H
    length = struct.unpack_from('<h', d, o)[0]
    nrec = (length + 8) // 8
    gates, end = [], None
    for i in range(nrec):
        r = list(struct.unpack_from('<8h', d, o + 16 * i))
        if r[1] == -1 and i == nrec - 1:               # end record: x1 = -1
            end = r
            continue
        gates.append({'type': 4 if i == 0 else r[0], 'x1': r[1], 'y1': r[2], 'x2': r[3], 'y2': r[4],
                      'number': r[5], 'cx': r[6], 'cy': r[7]})
    o += 2 * (length + 8)
    rest = d[o:]
    pickups = [list(rest[4 * k:4 * k + 4]) for k in range(len(rest) // 4)]
    return {'width': w, 'height': h, 'start': [hdr[2], hdr[3]], 'world': hdr[4], 'header_rest': hdr[5:],
            'tiles': tiles, 'gates': gates, 'gate_end': end, 'list_length': length, 'pickups': pickups, 'line': line, 'gategrid': grid}


def build(m):
    w, h = m['width'], m['height']
    W, H = 4 * w, 4 * h
    out = bytearray(struct.pack('<8H', w, h, m['start'][0], m['start'][1], m['world'], *m.get('header_rest', [0, 0, 0])))
    out += struct.pack('<%dH' % (w * h), *m['tiles'])
    line = m.get('line')
    if line is None:                                   # draw the racing line from its points
        line = trace_line(W, H, m['start'], m['gates'])
    out += bytes(line)
    grid = m.get('gategrid')
    if grid is None:                                   # draw the gate grid from the gates
        grid = [-1] * (W * H)
        for g in m['gates']:
            for x, y in bresenham(g['x1'], g['y1'], g['x2'], g['y2']):
                if 0 <= x < W and 0 <= y < H and grid[y * W + x] < 0:   # where gates overlap the first one stays
                    grid[y * W + x] = g['number']
    out += struct.pack('<%dh' % (W * H), *grid)
    gates = m['gates']
    end = m.get('gate_end', [4, -1, -1, -1, -1, -1, -1, -1])   # None: no end marker
    has_end = end is not None
    length = m.get('list_length', 8 * (len(gates) + has_end) - 8)   # records x 8 - 8
    for i, g in enumerate(gates):
        cx = g.get('cx', g['x1'] + g['x2'])
        cy = g.get('cy', g['y1'] + g['y2'])
        out += struct.pack('<8h', length if i == 0 else g.get('type', 4), g['x1'], g['y1'], g['x2'], g['y2'],
                           g['number'], cx, cy)
    if has_end:
        out += struct.pack('<8h', *end)
    for p in m['pickups']:
        out += bytes(p)
    return bytes(out)


def main():
    cmd = sys.argv[1]
    if cmd == 'dump':
        json.dump(read(sys.argv[2]), open(sys.argv[3], 'w'))
    elif cmd == 'build':
        open(sys.argv[3], 'wb').write(build(json.load(open(sys.argv[2]))))
    elif cmd == 'check':
        ok = bad = 0
        seen = set()
        for f in sorted(os.listdir(sys.argv[2]), key=str.lower):
            if not f.lower().endswith('.map') or f.lower() in seen:
                continue
            seen.add(f.lower())
            p = os.path.join(sys.argv[2], f)
            orig = open(p, 'rb').read()
            m = read(p)
            same = build(m) == orig
            m2 = dict(m, gategrid=None)
            regen = build(m2) == orig
            W, H = 4 * m['width'], 4 * m['height']
            laps = lap_progress(m, follow_line(trace_line(W, H, m['start'], m['gates']), W, H, m['start'], 6000)) / len(m['gates'])
            print('%-12s %s  gates %3d  pickups %d  gate grid from gates: %s  generated line: %.1f laps' % (
                f, 'exact' if same else 'DIFFERS', len(m['gates']),
                sum(1 for p_ in m['pickups'] if p_[0] != 0xFF), 'exact' if regen else 'differs', laps))
            ok += same
            bad += not same
        print('%d exact, %d differ' % (ok, bad))


if __name__ == '__main__':
    main()
