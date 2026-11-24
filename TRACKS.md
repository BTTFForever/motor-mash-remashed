# Custom tracks: what the game already supports

Findings from 2026-11-24, for a track editor whose tracks the game can race on. Addresses are
in MotorMsh.exe; nothing here contains game data.

## Playing a custom track

Save the map in `Track\` under a slot 9-16 name of its world: `AMAZON9.MAP`, then
`AMAZONA.MAP` .. `AMAZONG.MAP` (likewise `WEST`, `ICE`, `ATLAN`, `NIGHT`, `CITY`). On the
track select page (the parrot) it follows that world's own tracks, on a green ball labelled
9 or A-G, named "Custom 9" .. "Custom G", or by the first line of `Track\<name>.TXT`.

To try it out with a shipped track's layout:

    python -I tools/trackmap.py regen game/Track/Amazon3.map game/Track/AMAZON9.MAP

How MMCDA does it (tools/mmcda/tracks.h, game-level, so both renderers):

- **Track select page.** Page 7's handler (table 0x450ec0) is replaced by a copy of 0x40a620
  that lists 16 slots: 1-8 as before (unlocked by record, or all with `[race] tracks=all`),
  9-16 when the map file exists.
- **Balls 9-16.** The list shows front-end 3D objects: balls 1-8 are objects 0x2a-0x31
  (`Misc\JUKE01.x`..`JUKE08.x`, each ball 1's mesh with texture `juke000n.bmp` from
  FRONTEND.tex). Eight records are added to the front-end object list (0x445ae8; table
  pointer at 0x404461, loop limit at 0x404a33), giving objects 0x39-0x40. Their models
  `Misc\JUKE09.x`..`JUKE16.x` are written at start-up from the player's JUKE01.x with the
  texture name changed. The textures are made when loaded (archive lookup 0x42b1f0): ball
  1's picture with its number filled in from the surrounding pixels, the new label drawn
  in the balls' style (stroke font in tracks.h), and the palette turned green. Their names
  borrow JUKE0001's entry in the transparent-texture list (0x61bb58) while they load.
- **Object table.** Per-object state (0x5050f8, 0x24 bytes each, 57 entries, then other
  globals) moves to a 65-entry copy: 30 code sites, listed in tools/mmcda/fe_relocs.h
  (`gen_relocs.py --fe`). The page change at 0x408348 clears it with a count (0x201 dwords),
  raised to 65 objects, which hides balls 9-16 when the page is left.
- **Names.** 0x451008 entries for present custom slots point at their names.
- **Lap records.** Byte 0 of a record is the holder (characters 0-11), 12 = no record,
  13 = locked. An empty custom slot is set to 12; all zeros would show character 0 holding
  a 0:00:00 record.
- **Time-trial ghosts.** Clock.ini keeps 8 ghosts per world and finds a track's at
  `world * 8 + (slot & 7)` (load 0x409210, save 0x40c8bf), so custom slots would read and
  overwrite slots 1-8's ghosts. Opening Clock.ini (0x42f0d0) fails for custom slots; both
  callers then skip the ghost, so custom tracks keep no ghost between sessions.
- **The parrot's small ball** (mode screen) is 2D sprite `0x96 + (slot & 7)` (0x41101a).
  There are only 8, and adding sprites means growing the front-end sprite tables, so for
  custom slots it shows none (sprite id 0x89, which maps to nothing).
- **Loading screen.** Shows the world's picture, the track name and the record.

Still to do: scripted lifts and traps do not run in slots 9-16 (copying Amazon 1 there
leaves its lift shut), and a new map's tiles must belong to its world.

## The game was built for more tracks than it shipped

- **16 slots per world, 8 used.** The map name table (0x43f040, 96 entries) lists
  `WEST1`..`WEST9`, `WESTA`..`WESTG` for every world (`WEST`, `ICE`, `ATLAN`, `NIGHT`, `AMAZON`,
  `CITY`). Only 1-8 ship. Slots 9-16 already have entries everywhere it matters:
  - the display name table (0x451008, 96 entries) points slots 9-16 at one shared empty name;
  - the per-track trap routine tables (setup 0x454d98, update 0x454f38) point them at an empty
    routine (0x418570), so those slots simply have no scripted traps;
  - the lap record table (0x463a30, 0x34 bytes per slot) has room for all 96.
- **Maps are found on disk.** At start-up 0x412f30 lists `track\*.map` and marks which of the 96
  names exist (flags at 0x507340, written but never read).
- **Proven:** a map saved as `Track\AMAZON9.MAP` and selected with the debug track picker
  (`MMCDA_TRACK=72`) loads and races normally.
- **The menu stopped at 8.** The original track select page (0x40a620) builds its list from
  `world + 8` and shows a slot only if its record byte says it is unlocked. MMCDA now offers
  slots 9-16 (see "Playing a custom track" below).
- A seventh row in the trap tables (0x41c270 / 0x41c290) reads the world type from the map
  header and reuses Amazon 1's (type 4) or City-style (type 5) traps: groundwork for maps that
  bring their own world.

## Everything about the world comes from the map file

The race loader (0x401070) reads the whole map into one buffer, then switches on the header's
world type to pick the tile set (200 tiles per world, about 40 of them animated or object
tiles), textures, the world's `.hit` (collision) and `.att` (attribute) tables and the
weather effects (dust, snow). The slot only decides the file name, the display name and the
scripted traps. Tile ids must belong to the declared world: a map with Amazon tiles declared
as City fails to build its scene (crash at 0x4024de).

## Map file format

Fully decoded; `tools/trackmap.py` reads and writes it (`check` round-trips all 48 shipped
maps byte for byte). All values little-endian. N = width x height cells, Q = 4w x 4h
quarter-tile squares (the grid the AI and the race logic work on).

| Part | Size | Notes |
|---|---|---|
| Header | 8 x u16 | width, height, start cell x, start cell y, world (0 West, 1 Ice, 2 Atlantis, 3 Night, 4 Amazon, 5 City), 0, 0, 0 |
| Tile grid | N x u16 | row-major; bits 0-11 tile id, 12-13 height step, 14-15 rotation (90 degree steps) |
| Racing line | Q bytes | per square the direction the AI goes on from there: 1 up, 3 up-right, 5 right, 7 down-right, 9 down, 11 down-left, 13 left, 15 up-left (up = row 0); 0 and 64 = keep going; bit 7 ignored |
| Gate grid | Q x s16 | the gate crossing each square, -1 = none: the gates rasterised (Bresenham), the first gate winning where two overlap |
| Gates | 16 bytes each | type, x1, y1, x2, y2 (quarter tiles), number, rx, ry; then an end record (x1 = -1) |
| Pickups | 8 x 4 bytes | x, y (quarter tiles), number, height step; 0xff = unused |

All shipped maps are 30 x 30 except some Ice maps (45 x 20, 25 x 36), so the size is per map.
Some maps keep editor leftovers in a corner (Ice6 has the six thin-ice stage tiles in row 0),
and most end records carry leftover values after the -1.

### Gates, positions and laps

- A gate is a line across the road. They are numbered in driving order from 0, the start line.
- `type`: the driving direction in the low 3 bits (0 up, 2 right, 4 down, 6 left, odd =
  diagonal), plus flag bits whose use is not known yet. In the first record this slot
  holds the list length instead: records x 8 - 8 in 16-bit units, end record included.
- `(rx / 2, ry / 2)` is the gate's racing point. A car that leaves the track is put back
  there (0x41e8b0).
- Each car counts the gates it passes (0x6331a8 + car x 0x460). Positions sort by that
  count (0x41e660), and laps = count / number of gates.

### How the AI uses the racing line

Each car steers at a look-ahead point (car record +0x70 x, +0x110 z), and drags a queue of
39 earlier points behind it. At the start of a race every car's point starts at the same
place, the start cell's corner + 512 world units each way (0x420700). That is not quite
the centre, and can be the square next to it. After a respawn it starts at the gate's
racing point. Setup is 0x421000. If the point's first square is empty it has no direction
at all: the cars drive at that one point until they wreck each other and respawn.

Whenever the car catches up (0x421240), the point steps 128 world units in its current
direction and reads the racing line where it lands (0x421a90). A tile is 1016 world units,
and the square for world (x, z) is `(x + x/1016*8) / 256`, `-(z + z/1016*8) / 256`, using C
division. An empty square keeps the old direction. So a line works only if the walk itself
stays on it all the way round.

A generated line is drawn by doing that walk: from the start point, through each gate's
racing point in turn, writing the heading into every empty square the walk reads. The
walk takes three laps, so the lap closes at the start line.

Two more steps make it robust:
- **Crossings.** Neighbouring squares that point 90 degrees or more apart, and were
  written far apart in the walk, are where the track crosses itself. They are cleared
  over the whole area, because each lap may cross a row or column off from the last.
- **Width.** The line is widened to three squares, as the shipped lines are two or three
  wide.

`make_line` tries a few variants (every gate, every 2nd or 3rd; later laps filling gaps
anywhere or only near the start). It keeps the first one that laps in simulation.

Checks:
- In simulation the generated line laps every shipped track.
- In game, Amazon 3 with a generated line (raced from slot 9): the AI gets away from the
  start normally and laps at about 3.6 gates/s, against 4.3 on the original line. The
  hand-made lines take smoother lines through corners.
- Copying a track whose scripted features matter (Amazon 1's lift) into slot 9 leaves the
  AI stuck at the door, because slots 9-16 have no trap or feature routines.

## What an editor has to produce

1. The tile grid, from the world's tile set (names in the tile table; a 3D preview can reuse
   the tile meshes). Tiles that need a script (lifts, traps) do nothing in slots 9-16.
2. The gates: lines across the road in driving order, gate 0 on the start line through the
   header's start cell, each with a racing point and a direction. These could be drawn by
   hand or generated from the road tiles.
3. Everything else is derived: `trackmap.py build` with `"line": null, "gategrid": null`
   draws the gate grid and the racing line.
4. Pickups: up to 8 positions; what spawns comes from the exe's per-track table.

## Still to find out

- The flag bits in the gate type (8, 16, 64).
- Scripted features (lifts, traps) for slots 9-16: the trap tables point them at an empty
  routine; a custom track could name a shipped track whose routines it reuses.
- A small parrot-hand ball and a time-trial ghost for slots 9-16.
