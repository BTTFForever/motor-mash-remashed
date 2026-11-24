# Custom tracks: what the game already supports

Findings from 2026-11-24, for a track editor whose tracks the game can race on. Addresses are
in MotorMsh.exe; nothing here contains game data.

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
- **The menu stops at 8.** The track select page (0x40a620) builds its list from `world + 8`
  in four places (`lea reg,[eax+8]` at 0x40a6a4, 0x40a6ea, 0x40a8ba, 0x40a8e6) and shows a
  slot only if its record says it is unlocked. Offering slots 9-16 needs: those four limits,
  names for the slots, unlocked records, and a menu icon per slot (the list uses sprite
  `0x2a + n`).
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

All values little-endian, 16-bit. N = width x height.

| Part | Size | Notes |
|---|---|---|
| Header | 8 x u16 | width, height, start cell x, start cell y, world (0 West, 1 Ice, 2 Atlantis, 3 Night, 4 Amazon, 5 City), 0, 0, 0 |
| Tile grid | N x u16 | row-major; bits 0-11 tile id, 12-13 height step, 14-15 rotation (90 degree steps) |
| Layer A | N x 16 bytes | per-cell flags (start line cells hold 256, 1); read by car physics (0x421000, 0x421a90) |
| Layer B | N x 32 bytes | per cell a 4 x 4 grid of route gate numbers (-1 = none): which gate each quarter-tile belongs to |
| Route | variable | u16 byte count, then 16-byte gates: (x1, y1)-(x2, y2) in 1/8 tiles, gate number, two more values (one rises along the track), width. The AI (0x41dab0-0x41f320) steers by these; race positions most likely come from them |
| Second list | variable | follows the route (pointer 0x635a60), used by 0x426960; not decoded yet |

All shipped maps are 30 x 30 except some Ice maps (45 x 20, 25 x 36), so the size is per map.
Some maps keep editor leftovers in a corner (Ice6 has the six thin-ice stage tiles in row 0).

## What an editor has to produce

1. The tile grid, from the world's tile set (names in the tile table; a 3D preview can reuse
   the tile meshes).
2. The route: gates across the road along the racing line, in driving order, with the start
   line at the header's start cell. Could be drawn by hand or generated from the road tiles.
3. Layer B, by rasterising the gates into the 4 x 4 quarter-tile grid.
4. Layer A flags (start line, plus whatever else they mark; to be decoded).
5. The second list (to be decoded).

Layers A and B are not a pure function of the tile (the same tile carries different values
in different maps), which fits them being route data rather than tile data.

## Still to find out

- Layer A's remaining fields and the second list.
- Whether the gate values (the "rising" one) need to be continuous, and how laps are counted.
- The menu icon and loading screen picture for slots 9-16 (the loading screen shows a 3D
  picture per track).
- Lap records and unlock state for new slots (saved in the game's own files).
