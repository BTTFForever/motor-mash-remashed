# Motor Mash: porting notes (REA analysis, 2026-11-21)

## What's on the disc
| Item | Notes |
|---|---|
| `Motor.bin/.cue/.ccd/.sub` | Mixed-mode CD: track 1 is data, tracks 2–14 are **Red Book CD audio (the music)**. The cue names `Motor.img`; rename the reference to `Motor.bin` if you mount it. |
| `Motor.iso` | Data track only. **It has no music.** |
| `MotorMsh.exe` | Win95 build, May 1998, MSVC, x86 PE32. This is the main target. |
| `Matrox\MotorMsh.exe`, `DX3\MotorMsh.exe` | Variant builds of the same game. |
| `MOTORMBR.EXE` + `dos4gw.exe` + `*.BDD`/`*.DIG` | DOS build using the BRender engine and DOS/4GW. Good fallback under DOSBox. |
| `MotorRM.exe` | Uninstaller. It is not the game. |
| `directx\` | The DirectX 5 redist. It includes **d3drm.dll**. |

## How MotorMsh.exe is built (REA/Ghidra 12.2 findings)
- 837 procedures: 692 with bodies and 145 imports. All 692 decompiled with no failures and are in `decomp/MotorMsh/functions/`.
- Imports: **d3drm.dll** (`Direct3DRMCreate`), DDRAW, DINPUT, DSOUND, DPLAYX (ordinals 1, 2 and 4), WINMM (MCI CD audio, aux volume, timeGetTime), plus GDI, USER, ADVAPI and ole32.
- Rendering uses **Direct3D Retained Mode**. `0x429d80` creates IDirect3DRM, queries it for **IDirect3DRM2** (GUID `4516ecc8-8f20-11d0-9b6d-0000c0781bc3`), then builds the scene and camera frames. About 18 functions touch the D3DRM objects, mostly in `0x429d20`–`0x42a880` and `0x401ae0`–`0x404e90`.
- **Modern Windows has no d3drm.dll.** That is the main reason the game won't start.
- **There is no CD copy protection.** The only registry value it reads is `HKLM\Software\Eutechnyx\Motor Mash\1.0\Media` (`0x42e430`, called from `0x40f730`). It is used only to find `Ocean2.avi`. On its first call, `0x40f730` swaps the drive letter for the first optical drive it finds (`GetDriveTypeA == 5`), if there is one.
- CD music: `0x42bc90` opens MCI `cdaudio` and plays track N→N+1. If that fails it returns 0, so the game keeps running with no music.
- Everything else loads relative to the working directory (`Main\`, `Misc\`, `Track\`, the world folders, `Set.ini`, `Clock.ini`).

## Route A: run the original binary (quickest)
1. `game\` holds the exe, the data and the disc's own `d3drm*.dll` side by side.
2. Import `game\MotorMash-Media.reg` (needs admin). Or launch with ExIntro so the movie is skipped.
3. Add a DirectDraw wrapper (dgVoodoo2 or DDrawCompat) next to the exe. It fixes 16-bit fullscreen mode switches, palette problems and the old 3D HAL paths. Turn the in-game "hardware acceleration" off at first.
4. Music: rip tracks 2–14 from `Motor.bin` to OGG/WAV and use an MCI cdaudio emulator such as ogg-winmm (a `winmm.dll` wrapper).
5. Network play uses DirectPlay. Windows offers it as an optional feature ("DirectPlay" in Legacy Components).

## Route B: decompile and reimplement (proper source port)
Static recompilation buys nothing here. The CPU code is plain x86 Win32 and runs natively. Only the 1998 **APIs** are the problem. Suggested order:
1. Build a pass-through `d3drm.dll` shim that logs every COM method used (the vtable offsets in decomp show the exact set). This gives a complete list of the D3DRM methods the game needs.
2. Reimplement that subset on top of D3D11, OpenGL or SDL_GPU. Wine's `dlls/d3drm` is a working LGPL reference.
3. Replace DirectSound, DirectInput and MCI with SDL3, and DirectPlay with plain sockets.
4. Optionally, lift the game logic from `decomp/` into C, function by function, checking each against the original (REA `compare_functions`/`verify_reconstruction`).

## Fixes applied (2026-11-21)
1. **Double-Release crash at race start** (AV at `0x407080`). In `0x4075b0`, when `GetSurfaceDesc`/`Lock` fails on the overlay surface `0x504fa0`, the code calls `Release` but never clears the pointer, and `0x407070` releases it again. The fix NOPs the two early releases: `tools/patch_motormsh.py`, VAs `0x407620` and `0x40764b`.
2. **Race scene never built** (AV at `0x4024de`, last HRESULT `0x8876031B` = `D3DRMERR_INVALIDLIBRARY`). The 1997 d3drm.dll rejects Windows 11's `d3dxof.dll`, so `0x401ae0` fails. Its return value is ignored, and the loader then dereferences an empty frame grid. Fix: copy the disc's `directx\d3dxof.dll` into `game\`.
3. **Windowed mode**: dgVoodoo2 2.87.6 (`DDraw.dll` and `D3DImm.dll`, x86) plus `dgVoodoo.conf`: `FullScreenMode=false`, `ImageScaleFactor=2`, `CenterAppWindow=true`.
4. `tools/crashcatch.py`: a small debugger that logs DLL loads and, on a crash, dumps registers and the game's own diagnostic globals.

5. **CD music**: `tools/rip_tracks.py` rips tracks 2–14 from `Motor.bin` to `game\Music\TrackNN.wav`. `tools/mmcda` is a 32-bit DLL built with MSVC 2022 (`MMCDA.dll`) that emulates the MCI `cdaudio` device: OPEN, PLAY with TMSF from/to, the MM_MCINOTIFY that makes the game loop the track, STOP and CLOSE. It also adds a fake AUXCAPS_CDAUDIO volume device. The patch script renames the exe's `WINMM.dll` import to `MMCDA.dll`; anything that isn't CD audio is forwarded to the real winmm. Set `MMCDA_LOG=1` to get `game\mmcda.log`. Track map: 2 = ?, 3–8 = world themes (West, Arctic, Atlantis, Nightmare, Amazon, City), 9–14 = menu and jingles.
6. **Resolution**: the game itself enumerates display modes up to 1024×768 (`0x406a40`: w ≤ 0x400, h ≤ 0x300, at most 50 modes) and handles WM_SIZE in windowed mode (`0x4143d0`). In dgVoodoo (`[DirectX]`), set `AppControlledScreenMode=false` (otherwise the game's exclusive fullscreen overrides windowed mode), `Resolution=max_isf`, `ImageScaleFactor=0` and `Antialiasing=4x`.

7. **Runtime hooks in `MMCDA.dll`** (installed from DllMain; each checks the original bytes first and logs to `mmcda.log` when `MMCDA_LOG=1`):
   - `0x42b1f0` packed-texture lookup: the crash was a 12-byte stack buffer that received a full path such as `Arctic\maps\SNOW2D.bmp`, overwriting EBP with "SNOW" and EIP with "2D.b". The caller and callee read the texture-source flag `0x504fb0` separately, and the race loader changes it while loading, so the two reads can disagree. The hook trims any input to the 8-character base name.
   - `0x42b540` D3DRM texture callback: strips directory prefixes (it uses a 16-byte buffer).
   - `0x406a40` mode enumeration: keeps only 16 bpp modes (plus 320x200x8), because the table has a hard limit of 50 entries. It reports the menu's "800x600" slot as `MMCDA_WIDE` (default 1024x576, `off` to disable) and updates the remembered size so the match in `0x405160` still finds it.
   - `0x4052c0` SetDisplayMode: after each mode change, turns the window into a borderless popup covering the monitor at its current Windows resolution (the default). dgVoodoo then scales into it with `ScalingMode=stretched_ar`, `ImageScaleFactor=1` and `CenterAppWindow=false`. With `MMCDA_WINDOWED=1` it instead uses a captioned window at about 90% of the work area, at the mode's aspect ratio (320x200 and 640x400 shown at 4:3).
   - `0x42a632` viewport SetBack call: hooks viewport `SetPlane` (vtable slot 24) once. The race loop `0x403230` calls `SetPlane(±0.52, ±0.684|0.57)` every frame, and for modes with aspect > 1.7 the hook scales left/right by aspect/(4/3) (Hor+). An earlier SetField approach was overridden every frame.
   - `0x407f25` HUD overlay Blt (5 bytes replaced by a call to a thunk). In "fit" mode the 320-wide HUD is drawn in a centred 4:3 rectangle; at an integer N, each HUD pixel is N×N screen pixels, centred. The End key cycles the setting, which is saved in `mmcda.ini [hud] scale`.
   - Camera zoom: the race camera height is the float at `0x44d3bc` (56.0). Pull-back is height × `0x4889b4`, so scaling the height dollies the camera along its view line. PageDown/PageUp/Home adjust it live, and the value is saved in `mmcda.ini [camera] zoom` (default 1.3). The per-frame far clip `0x4032f5` stays at 1024 up to zoom 1 and becomes 1024 × zoom above that. A fixed 4096 wasted depth precision. A leftover developer camera control (`0x402620`) adjusts the same values from input bitmask `0x61bb54`. Known limit: the game only streams an 8×8 grid of track tiles, so high zoom shows the edge of the world.
   - Pillarboxed menu: in borderless mode the game window gets the mode's aspect ratio, centred, and a black backdrop window on a separate UI thread fills the rest of the monitor. While the game has focus, both windows are topmost so they cover the taskbar.
   - Seamless mode switches: `0x4137c0` (first step of every DirectDraw/D3DRM teardown) screenshots the monitor into a click-through topmost cover window. For menu (320-wide) modes, `0x4058a0` (the game's present call) removes the cover after 3 presented frames. For race modes the cover stays until the first race-camera `SetPlane` call, so the loading screen (2D laid out for 4:3) is never seen. Menu→race takes about 2.3–2.4 s, race→menu about 0.7 s, with a 10 s safety timeout. Disable with `MMCDA_NOSEAMLESS=1`. HUD anchoring (`[hud] anchor=edges|center`, toggled with Insert) splits the 320-wide overlay at x=88/230 into left, top-centre, top-right and bottom-right pieces.
8. The window style patch at `0x42cea3` changes WS_POPUP to a captioned window with minimise and close buttons.
9. dgVoodoo `[DirectX]` settings: `Resolution=4x`, `Antialiasing=off`, `VRAM=2048`, `AppControlledScreenMode=false`. `[DirectXExt]` settings: `DefaultEnumeratedResolutions=classics`, plus 16:9 extras. With 8x, 800x600 needs 6400x4800 render targets.

10. **Black blocks and flicker in races** (32×32 blocks in the 1024×576 render, black or leftover images, with z-fight streaks at their edges). D3DRM's viewport `Clear` only clears colour and depth where it tracked drawing in recent frames. The rest kept stale depth at about the ground's own depth, so the ground failed the depth test there. With Smooth Off, the game's present step `0x4058a0` also copied only dirty rectangles to the screen, so those areas showed old frames (flicker). Fixes:
    - the present hook always passes the full-frame flag (bit 0);
    - the viewport `Clear` hook (vtable slot 12) calls `ForceUpdate` for the whole viewport, then colour-fills the back buffer (`0x4891e4`) and depth-fills its attached z-buffer before the original `Clear`. `MMCDA_NOFULLCLEAR=1` disables it. It is installed with the interpolation vtable hooks, so `MMCDA_NOINTERP=1` also turns it off.
    - dgVoodoo `DepthBuffersBitDepth=forcemin24bit`.
    The texture hooks now log lookup or load failures ("NOT FOUND" / "FAILED"); none occur on West.

Still open: a crash when switching focus back to the game was reported but has not yet been captured by the harness.

## Debug menu (F10) — `tools/mmcda/debugmenu.h`
- Drawn in the game's own font. On the front end it goes through the game's text routine `0x4116c0`: char→glyph table `0x4517e0`, widths `0x451be0`, style block `*(0x505038)` (advance, x, y, alignment at [5]), menu sprite list `0x505ed0`/`0x505054`, drawn by `0x404c10` via the loaded-sprite map `0x4868e0` → sprite sink `0x42ef40`. During races the font sprites aren't loaded (their slots hold driver portraits). The glyphs are therefore captured on the front end from the sprite records (`0x48f378 + slot*0x410`: header, pixels (4- or 8-bit), palette[256] in screen format, w@+0x408, h@+0x40c) and painted onto the locked back buffer after the HUD blit, over a darkened panel.
- Items: Track (any of the 48; `0x401070` hook sets the track index `0x634c44`, which indexes the name table `0x43f040`, 16 slots per world with files for 1–8), Camera (chase / overview / free fly: WASD, Q/E, right mouse to look, Shift to go faster), Zoom, HUD scale, HUD layout, Draw distance, Smooth motion, Save settings.
- While the menu or free camera is active, the DirectInput key buffer `0x54a0c0` (filled by `0x415360`) is cleared, so the car isn't driven.
- `tools/gamectl.py` drives the game for testing: it focuses the window, sends keys as hardware scan codes (DirectInput needs these), and takes screenshots.

## Single-file launcher (drop-in)
`tools/launcher/MotorMash.exe` (about 33 MB) goes in an original install folder, next to MotorMsh.exe, Main\ and Track\. It embeds:
- the patched exe, regenerated from the disc original;
- MMCDA.dll, now with Ogg playback via stb_vorbis;
- dgVoodoo DDraw/D3DImm and the config;
- the disc's d3drm*/d3dxof;
- the soundtrack as Ogg Vorbis (30 MB, encoded with `tools/encode_music.py`).

On launch it unpacks once per build to `%LOCALAPPDATA%\MotorMashPort\build-<hash>\`, deleting older build folders, sets `MMCDA_HOME` to `%LOCALAPPDATA%\MotorMashPort\` (where mmcda.ini and mmcda.log live), and starts the game from the cache with the install folder as its working directory. The game opens all its data relative to the working directory; it only uses GetModuleFileNameA for CRT argv and error messages. The install folder is left untouched. To rebuild, run `python -I tools/launcher/build.py`.

The Program Compatibility Assistant added layers (WIN98, 640X480) to `game\MotorMsh.exe` after the crashes, and they made it require elevation. `game\MotorMsh_dbg.exe` is an identical copy without those layers.

## VR tabletop viewer (2026-11-22) — `tools/vrviewer`, `tools/mmcda/vrexport.h`
The race is shown as a miniature diorama in an OpenXR headset (tested: Quest 3 through
Virtual Desktop, 120 Hz) by a separate 64-bit viewer, `MotorMashVR.exe` (D3D11 + OpenXR
1.1.65). MMCDA.dll feeds it when `mmcda.ini [vr] export=1` (or `MMCDA_VR=1`). Usage and
controls: `tools/vrviewer/README.md`. Shared layout: `tools/mmcda/vrshared.h` (version 5).

**Export (game side, `vrexport.h`):**
- Every logic tick (top of the viewport Render hook, before interpolation blends anything) the
  scene frame tree is walked. Slots verified on the disc's d3drm: Frame GetChildren 19,
  GetTransform 29, GetVisuals 32; Array GetSize 3, GetElement 4; GetClassName 10. Positions
  are made absolute by adding the floating origin. The game camera is exported too.
- MeshBuilder visuals (class "Builder"): CreateMesh 48, then Mesh GetGroupCount 22, GetGroup 23,
  GetVertices 24, GetGroupColor 25, GetGroupTexture 29. Written once per race to
  `game\vrcache\<generation>\m<id>.bin`.
- Textures: D3DRM GetImage returns NULL (they are built with InitFromSurface). The game's texture
  table at 0x54def8 (0x454 bytes per entry, count 0x54a0b4) has the DirectDraw surface at +0x44c
  and the D3DRM texture at +0x450. The surface is locked and converted to RGBA, with its
  DDCKEY_SRCBLT colour key as transparency. 24 textures per tick are re-read and re-sent if they
  changed (animated textures).
- Decals (class "Texture", e.g. dust/smoke): GetDecalSize 22, GetDecalOrigin 23,
  GetDecalTransparency 28, GetDecalTransparentColor 29 (slot order checked from the vtable code).
  Exported as billboard quads.
- HUD overlay (0x407f25 source surface) and, in the front end, the whole back buffer
  (0x4891e4, from the present hook) are copied to shared memory for the headset panels.
- Controller input from the viewer is merged into the DirectInput key buffer 0x54a0c0 in the
  keyboard-poll hook (0x415360). Mapping: arrows, Enter = fire/select, P, Esc. Partial steering
  is pulsed. The game's own joystick support (0x4155f0) is digital only (90% dead zone).

**Findings along the way:**
- Animated objects (windmills, pumps, crates) are D3DRM AnimationSets (0x429fd0): table 0x54d310
  holds {set, time} per object, and a frame move callback (0x42a170) calls SetTime. Objects placed
  outside the game's 8x8 window never got the callback. `tiles_animate` (tiles.h) now advances
  their clocks.
- Water beds are 320 units below ground, so the viewer's tray floor sits at the lowest geometry.
- The game lifts cars ~10.5 units and their shadow quads ~5 units. The viewer lowers them, but
  only by that fixed amount so jumps are kept.
- The game window is minimised when it loses focus (dgVoodoo fullscreen emulation), and WM_SIZE
  minimised sets 0x617648, which stops the race logic in the main loop 0x412230. So the viewer
  never takes focus (WS_EX_NOACTIVATE, opens on the other monitor).

**Open:**
- The user reports a West windmill that turns in the game but looks static in VR. The export
  shows its parts moving; it has not been reproduced. Possibly doubled sails (two copies at
  different angles) were seen once.
- Decal size units: the DLL logs GetDecalScale (slot 27) per decal; check in mmcda.log whether
  sizes are world units (assumed) or pixels.
- The drop-in launcher includes the VR export code, but `[vr] export` is off by default there,
  and the viewer is not bundled.

## Native controllers and car handling (2026-11-23) — `tools/mmcda/pad.h`, `car.h`
**Controllers (SDL 3.4.20, `game\SDL3.dll`, bundled in the launcher).** A background thread
loads SDL3 at run time and sets `SDL_JOYSTICK_HIDAPI_STEAM=1`. SDL's Triton driver then takes
the 2026 Steam Controller (receiver 28de:1304, "Proteus") out of keyboard/mouse lizard mode
without Steam, and re-sends that every 3 s. Xbox, PlayStation, Switch and generic pads work
the same way. Misc 3–6 (the Steam Controller's stick and grip touch sensors) are ignored.
- The game's control choice per player (start-up screen, or reselect "1 Player"): purple =
  Keys 1, pink = Keys 2, joystick = Joystick 1. It offers "joystick count + 2" options
  (0x40d3c0). DirectInput sees no joystick once lizard mode is off, so while an SDL pad is
  connected the count 0x54a1c8 is set to 1, and the joystick read 0x4155f0 is hooked to
  return the pad in the game's format: 1 = button 1 (accelerate), 0x1000 = button 2 (fire),
  0x40 up (dive; also left stick up in menus), 0x80 down (brake), 0x8000 left, 0x2000 right.
  The keyboard choices stay keyboard-only, as in the original.
- Menus read Joystick 1 regardless of the choice. Esc only comes from the keyboard in this
  game, so ☰ / the Pause binding (and B in menus) write Esc into the DirectInput key buffer
  0x54a0c0 (keyboard-poll hook 0x415360).
- Calibrate (0x413430 used to start joy.cpl and wait, behind the full-screen game) now opens a
  Controller Setup screen. It is drawn into the game's own front-end frames from the present
  hook. It covers: device, bindings (press-to-bind), dead zone, steering and throttle analog or
  digital, body lean, ride height. It saves to `mmcda.ini [pad]` / `[car]` and sends one Esc
  on exit to close the game's empty Calibrate page.
- Manual facts: P = dive (Atlantis only), Esc = pause. Player 2 keys are Q/A/R/T, C fire, X dive.

**Car handling (`car.h`).** Car records 0x632da0 + i·0x460. The driving update 0x4241f0
(control word, ?, car) is hooked:
- True analog (player 1, when a race reads Joystick 1): the per-tick turn rate +0x2e0 is
  scaled by the stick (fraction carried over). For the throttle, the trigger sets a target
  speed (fraction of top speed +0x3d0); below it the power above drag (+0x3d8) is scaled
  (minimum 35%), above it the push only matches drag. Brake/reverse: power above drag scaled.
- The steering lean goes into +0x39c (a world-axis slope) when +0x5c == 2 (flat ground). It is
  scaled by `[car] lean` (percent).

The car placement 0x403230 is hooked after it runs. Flags 0x485c08[i]: 1 = body placed,
|4 = shadow placed, 2 = drawn as an animated object.
- The game orients models with the heading cut to 128 steps (+0x3a0). The missing fraction of
  the 1024-step heading +0x368 is added as a rotation before the game's (smooth turning).
- Leaning cars are rebuilt as: lean about the car's own axis, then heading, then the game's
  scale and position. The game applied it about the world x axis, so the lean was reversed or
  turned into pitch depending on the direction.
- Models are centred on the pivot (MeshBuilder GetBox; offsets 1–5 / ~4 units), shadows by
  the same amount.
- Shadows of leaning cars are laid flat.
- `[car] lowered=1` drops bodies by `drop` (default 10) and shadows by 2. The game lifts cars
  ~10 units and shadows ~5.
- Note: the VR viewer still lowers cars and shadows itself. Drop that once the game does it.

The "in a race" flag (`g_race_active`) is now updated even with smoothing off.

## Race cameras (2026-11-23) — `tools/mmcda/cams.h`
The controller's Select button (or F7) cycles the views: Original (right stick = camera height,
like PgUp/PgDn), Chase, Bumper, Top-down (stick zooms) and TV (fixed trackside cameras that
track the car). The camera aims at the mesh centre, not the frame origin. The views keep the
camera at least 30 units above the cells' floors, and set a sky colour per world.
- Crash fix: restoring the scene background touched a released scene at race exit. It now
  happens only while the scene is current and a race is active.
- Spinning pickups are centred on their pivot like the cars (`centre_objects`, `car.h`).

## Six racers (2026-11-23) — `tools/mmcda/sixcars.h`, `tools/sixcars/gen_relocs.py`
`mmcda.ini [race] cars=6` (the default; `cars=4` turns it off) allows two extra CPU cars in Single Race and
Practice (game type 1 / 0, race mode 0, not network) when all four roster slots are filled.
- Main menu: two more slots appear above the top two portraits (rows 11 and 12 of the
  front-end item table). Enter cycles Random → each character not already racing → None. None
  in one slot gives a 5-car race, and None in both gives 4. The choices are saved as
  `[race] extra1/extra2` (0–11 character, 12 none, 13 random). Random (or a character that has
  since joined the roster) picks a free character at race start.
- The item table (0x451FE0, 0x38 per row: home x,y, nav up/down/left/right, enabled,
  sprite, current x,y, state…) is moved to a 13-row copy. Rows 4/5 link up to 11/12.
  The main menu 0x4083F0 is wrapped:
  - It handles Enter on the new rows.
  - The draw-list builder 0x410CB0 gets their portraits (sprite = character + 0x7D; 223 "?"
    for Random). The gun man points as for the row below.
  - The game's label is replaced with the character name.
  - The row-label lookup 0x411620 is replaced: row 11 would read a NULL string.
- The portrait frame boxes are part of the 320×256 8-bit background picture (image slot
  *(0x61D3E8), record 0x48F37C + slot·0x410: pixels, palette, width, height), drawn in
  software by 0x4075B0. The top two boxes are copied 38 px up while the slots show. The
  original pixels are put back for other game types.
- Extra loop bounds moved with their tables: 0x4095C5 (rank init), 0x408421 (menu
  home→current, widened to 13 rows) and 0x40E354. League, Knockout, Beat The Clock and team play stay at 4: their
rosters and points tables have 4 slots.
- The game was nearly built for 6. The car records (0x632DA0, 0x460 each) have unused room for
  cars 4–5 (0x633F20–0x6347E0). The start grid (0x457328/0x457340) has 6 slots, and so do the
  character, input-slot and input-word tables.
- 19 smaller per-car tables are moved to larger copies at start-up. These are: body/shadow meshes
  and frames, placement flags, sound state and handles, HUD elements (8 → 12), last place,
  battle "alive", finish order, lap clocks and rank order. `gen_relocs.py` reads the
  disassembly (`tools/sixcars/MotorMsh.dis.txt`, `dumpbin /disasm /nobytes`) and writes
  `relocs.h`: 230 code sites, each checked before patching. If any site differs, six cars
  stay off.
- Rewritten for any count: ranking 0x41E660, car-vs-car collisions 0x422FF0 (was 6 unrolled
  pairs), single-player catch-up 0x4240E0, lap clocks 0x420F10.
- Extended:
  - the race init 0x420700 (zero cars 4–5 first, then their HUD character and the Atlantis
    handicap)
  - the path set-up 0x421000 (also for cars 4–5)
  - the finish order in 0x4221A0
- The finish order lists the 4 roster cars first, then the extras, because the results/points
  code indexes 4-slot tables by it.
- CPU weapon aiming 0x41F880 kept three `[4]` arrays on the stack. Its frame is enlarged to
  `[6]` (`sub esp,44h` → `5Ch` and the ebp offsets).
- Lifts / teleporters (0x417790, 0x41AE50) keep the cars they carry in 4 slots per hazard as
  car index (| 0x8000 = down) and read them back with `and eax,3`. Car 5 therefore moved the
  player to the lift and launched it, and car 6 moved car 2. The 8 decode sites now mask with
  0x7F. The `and …,3` instructions followed by `shl eax,8` are directions and stay as they are.
- HUD portraits are 34 px apart instead of 40 when 6 cars race. The HUD overlay is 320×240,
  but its sprite drawer 0x4075B0 clips at y 200 unless the hi-res flag is set. So
  `and esi,28h` at 0x407672 becomes `push 28h; pop esi` (clip at 240) during such races.
  The game still lays the timer out for 200 rows, so the edge-anchored HUD scaler
  (`hud_blt_inner`) ends the timer column at row 200 when the hi-res flag is off.
- Finishing rosettes: the HUD draws sprite *(0x488564) + place (0x402925). Only 1st–4th exist
  (0x50–0x53); 5th/6th showed the laurel and "LAP" sprites.
  - Race sprites are 4-bit images in the slot table 0x48F378 (0x410 per slot: header, pixels,
    16-entry 565 palette, width, height).
  - Two rosettes are built from the 4th in slots 0x1C0/0x1C1: purple and grey ribbons, and the
    digit redrawn in the medallion.
  - A stub at 0x402925 uses them for places 5 and 6.
- The race-load hook on 0x401070 is debugmenu.h's, which calls `six_prepare`.
- Sample table 0x5076C0 is not per car (about 60 entries). The sound loader overwrites entry i with
  car i's character; entries 4–5 are restored.
- Tested: 6-car and 5-car Amazon races (all cars racing and ranked, race end, back to the
  menu), a 4-car race with the tables moved, the new menu slots (navigation, cycling, None,
  saving), and League hiding them. Not yet tested: every world, the results screen
  with an extra car winning, and sound with 6 cars.

## Game speed (difficulty) (2026-11-23) — `tools/mmcda/interp.h`
Controller Setup → "Game speed": Easy (20), Medium (25, default), Hard (30) ticks per second
(`mmcda.ini [race] speed = easy | medium | hard`).
- The game runs one logic tick per frame, capped at ~33 ms by its limiter 0x406B70, and never
  compensates for slow frames. On period PCs that couldn't hold 30 fps, the whole game ran slower;
  20 is about what was typical.
- The PS1 PAL version runs one race tick per frame with `VSync(2)` (the main loop 0x8001764C:
  race update 0x80024E0C, then render and `VSync(2)` at 0x80017D5C), so 25 ticks/s at 50 Hz. It
  keeps the 30-tick race clock (+3 hundredths per tick), so its timer ran slow.
- During races the tick is stretched to 33·30/ticks ms. Cars, CPU drivers, weapons, the race
  clock and the countdown slow together. Handling and jumps are unchanged, and interpolated frames
  fill the time. Steering reacts once per tick. Menus always run at full speed.
- Measured game seconds per real second: 0.65 (Easy), 0.81 (Medium), 0.97 (Hard).
- Frame pacing: when the refresh rate allows, the smoothing locks each tick to a whole number of
  vsyncs. Hard needs an exact match (120 Hz: 4). Easy and Medium snap to the nearest whole
  number within 10% (120 Hz: Medium 5 = 24 ticks/s, Easy 6 = 20). Otherwise the blend is
  time-based, which judders.
- Pooled object frames (0x486070, handed out again every tick) used to be excluded from the
  smoothing. Cars hit by a weapon are drawn with one (car states 1–4 in 0x403230), so they
  jumped while everything else glided. A pooled frame is now blended when it was used last tick
  and moved under 40 units. A car moves ~19 per tick, and a different object of the same type is
  at least a tile (127) away.
- Car state +0x440: 1–4 draw the car as a pool model (firing the machine gun, weapon hits), and
  0x403230 then leaves the body frame unplaced. `place_hook` (car.h) now gives the invisible body
  frame the pose the game would have used, so the race cameras that follow it don't shudder.
  With "Lowered", the pool model (found as the in-use pool frame at the car's spot) is lowered
  like the body. Shadows are now lowered by 2 (was 4), leaving them 4 above the ground. City's
  shadows are dark blue on a dark road and mostly hidden under the car, as in the original.
- Track unlocking: a track is selectable once it has a lap record. The record byte at
  0x463A30 + track·0x34 is below 0x0D; Clock.ini holds the records. A new game starts with 12
  (0x43F408): West 1/2, Ice 1/6, Atlantis 1/2, Nightmare 2/8, Amazon 3/8, City 2/3. Records are set
  at the end of a lap (0x4089A0), except in Practice. Controller Setup → "Tracks: All unlocked"
  (`[race] tracks = all`) turns the two `jae` lock skips in the course picker (0x40A6C1,
  0x40A8D6) into no-ops and never touches the records.
- Speeds differ per track by design (same data on PS1): only tiles with surface code 13 in the
  track's `.att` file allow full top speed; elsewhere the cap is 5/6 of it (0x4241F0 via
  0x41F760). Atlantis has no code-13 tiles, and its CPU cars also lose 100 top speed
  (0x420600, world 2). Measured top speed: West and Nightmare 600 (150 units/tick), Ice 509 (126).
- World ids: 0 West, 1 Ice, 2 Atlantis, 3 Nightmare, 4 Amazon, 5 City (track index = world·16 +
  track−1; `MMCDA_TRACK=<index>` pre-selects the debug menu's track override, for testing).

## Unknowns / not yet verified
- 2026-11-21: the game is **playable on Win11**, including races, with fixes 1–3 above (user-confirmed).
- The `Ocean2.avi` codec and the playback path (CreateProcessA?) haven't been traced.
- The data formats (`.DAT`, `.MAT`, `.x`, `.tex`, `.anm`) haven't been analysed. `.x` is probably the DirectX mesh format that D3DRM loads.
