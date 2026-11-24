# New renderer (phase 1): design notes

Goal: a modern renderer for Motor Mash (Vulkan first), chosen in `mmcda.ini`, with the
original 1998 path kept exactly as it is:

```ini
[render]
backend=original   ; original = D3DRM + DirectDraw (dgVoodoo2), as shipped
                   ; vulkan   = the new renderer (MMCDA.dll)
```

Like the original game shipping separate DOS and DirectX exes, nothing is removed: `original`
never touches the new code.

## What the game uses (API census, 2026-11-24)

Measured with `MMCDA_CENSUS=1` (`tools/mmcda/census.h`): a one-shot breakpoint on every COM
call site in the game's code (`call [reg+disp]`, 335 sites from `gen_census_sites.py`), which
records the interface table and method each one reaches. A menu session plus a Single Race hit
207 sites. The rest are in modes and error paths not played (League, Knockout, failures), so
the lists below are a minimum. Interfaces are identified from the d3drm.dll vtable sizes and
method order. (Patching d3drm.dll's vtables makes it abort, so the census never modifies a
DirectX DLL.)

Entry points: `Direct3DRMCreate` (IAT 0x43D264) and `DirectDrawCreate` (IAT 0x43D010 area).
Both can be redirected by MMCDA.dll at start-up. This is where the backend switch sits.

### Direct3D Retained Mode (d3drm.dll)
| Interface | Methods the game calls (slot) |
|---|---|
| IDirect3DRM (v1) | QueryInterface 0 (for IDirect3DRM2) |
| IDirect3DRM2 | CreateObject 3, CreateFrame 4 (14 sites), CreateMeshBuilder 6, CreateAnimationSet 9, CreateLightRGB 12, CreateDeviceFromD3D 16, CreateViewport 20, SetSearchPath 25, SetDefaultTextureColors 28, SetDefaultTextureShades 29 |
| IDirect3DRMDevice2 | Update 14, SetBufferCount 17, SetDither 19, SetShades 20, SetQuality 21, GetDither 24, GetHeight 26, GetWidth 27, GetQuality 30, GetTextureQuality 32 |
| IDirect3DRMViewport | Clear 12, Render 13, SetBack 15, SetField 16, Transform 20, ForceUpdate 23, SetPlane 24 |
| IDirect3DRMFrame | SetName 8, AddChild 11, AddLight 12, AddMoveCallback 13, AddScale 16, AddRotation 17, AddVisual 18, GetChildren 19, Move 37, DeleteLight 39, DeleteMoveCallback 40, DeleteVisual 41, 60, 61 (SetPosition / SetOrientation region) |
| IDirect3DRMMeshBuilder2 | Load 11 only: every model is loaded from a `.x` file |
| IDirect3DRMTexture2 | InitFromSurface 12, SetDecalSize 17, SetDecalOrigin 18, SetDecalScale 19 |
| IDirect3DRMAnimationSet | Load 12, SetTime 14 (animated `.x` models) |
| IDirect3DRMArray (frame/visual arrays) | GetSize 3, GetElement 4 |

MMCDA.dll also uses Frame AddTransform / AddTranslation / SetPosition / SetOrientation /
GetTransform / GetVisuals, Mesh GetGroup* / GetVertices, Texture GetDecal* and Viewport
Clear / Render / SetBack / SetPlane (interpolation, cameras, VR export).

Not called: **MeshBuilder SetPerspective**. That's why textures warp up close: D3DRM's
perspective correction is off for every model, so textures are interpolated affinely like
the PlayStation.

### DirectDraw (used directly by the game, for 2D, menus, HUD and textures)
- IDirectDraw: QueryInterface, CreatePalette, CreateSurface, EnumDisplayModes, GetCaps,
  GetDisplayMode, RestoreDisplayMode, SetCooperativeLevel, SetDisplayMode.
- IDirectDrawSurface:
  - drawing: Blt, BltFast, Lock / Unlock, GetDC / ReleaseDC (GDI text)
  - setup: AddAttachedSurface, GetAttachedSurface, SetColorKey, SetPalette
  - status: Flip, GetSurfaceDesc, IsLost, Restore
- IDirectDrawPalette: SetEntries, Release.

### Direct3D Immediate Mode (setup only)
- IDirect3D2: EnumDevices, CreateDevice. The game creates the D3D device itself, then wraps it
  with `CreateDeviceFromD3D`.
- One Direct3D device: slot 5 (AddViewport) and slot 9 (a format enumeration with a callback
  at 0x414D20).

## Plan for the Vulkan backend

1. **Our own implementations of exactly the interfaces above,** returned from the two entry
   points when `backend=vulkan`. They're written in C as plain vtables, like the census.
   Anything outside the census returns `E_NOTIMPL`, and that gets logged so gaps show up
   immediately.
2. **Scene graph:** frames (hierarchy, transforms, move callbacks), mesh builders, lights,
   textures with decals, animation sets. Kept as plain data, and later the basis of the
   Android/Quest port.
3. **`.x` file loader** for MeshBuilder Load and AnimationSet Load (text and binary X formats).
   It's our own parser, so it doesn't depend on d3dxof.dll and works on other platforms.
4. **DirectDraw as CPU surfaces:** palettes, colour keys, blits, lock, and GDI text through a
   DIB section. The menus and HUD keep working unchanged.
5. **Vulkan renderer:**
   - 3D at the full window resolution with perspective-correct texturing and a depth buffer
   - D3DRM's lighting model (ambient + directional/point, Gouraud) and fog
   - decals drawn as billboards
   - the 2D back buffer composited on top, where the game drew 2D
   - VSync pacing handled through the existing frame-pacing code
6. **Interpolation, cameras, HUD scaling and six-car support** keep working, because they sit on
   the same interfaces.

Order of work: Vulkan device/swapchain and the 2D path (menus playable), then the scene graph
and `.x` loader, then 3D rendering, then side-by-side comparison with `original`.

Tools: Vulkan headers (Khronos `Vulkan-Headers`, header-only); shaders are written in HLSL
and compiled to SPIR-V with the Windows SDK's `dxc -spirv`. The 32-bit Vulkan loader
(`SysWOW64\vulkan-1.dll`) comes with the GPU driver, and it's loaded at run time.

## Progress

**Stage 1 (2026-11-24): the game runs end to end on `MMCDA_RENDER=vulkan` / `[render] backend=vulkan`.**
- `tools/mmcda/render.h`: the switch. `original` (default) leaves everything as shipped.
- `vdd.h`: DirectDraw as CPU DIB surfaces, plus Direct3D set-up objects.
- `vrm.h`: the D3DRM scene graph (frames, transforms, move callbacks, textures, lights,
  viewports, devices).
- `vkpresent.h`: Vulkan device and swapchain, presenting the 2D frame scaled to the window.
- `com_stubs.h` (from `gen_com_stubs.py`): every other method is a logging stub with the right
  argument count.
- The game reaches the menu loop at 30 fps on the RTX 2070 Super, but the screen is still
  black: this game draws nearly everything through D3DRM. The title screen, menus and sprites
  are textures drawn as decals, and the 2D back buffer only carries the race HUD overlay.

Findings that shaped it:
- The game skips hardware Direct3D devices (flag 0x62FAB4) and runs on the software
  Ramp/RGB/MMX devices. Our IDirect3D2 reports those three with dgVoodoo's capabilities, or
  the game finds no device.
- The game passes DirectX 5-size structs (DDCAPS 364 bytes, D3DDEVICEDESC 0xCC). Write only
  what fits the caller's dwSize: the SDK's DX7 DDCAPS is larger, and overrunning it smashed the
  stack.
- The intro (0x41C2B0) makes its own DirectDraw object at 320x200x16 and releases it.

**Stage 2 (2026-11-24): the game draws through Vulkan, from the title screen to a race.**
- `xload.h`: our own loader for the binary `.x` models. It handles MeshBuilder::Load (all meshes,
  with frame transforms applied) and AnimationSet::Load (frames plus rotation, scale and
  position keys).
- `vkrender.h`: like D3DRM, the scene work happens on the CPU. Viewport::Render walks the
  frame tree, transforms and lights vertices (ambient, directional and point lights), and
  turns decals into screen quads. The GPU then draws one render pass per frame: the viewport
  clears, depth-tested textured batches (perspective-correct; colour keys become alpha), and
  the game's 2D layer on top.
- `gen_shaders.py` writes the two shaders as SPIR-V directly (the SDK's dxc has no SPIR-V
  back end), so no shader compiler is needed.
- Two more ways the game depends on what DirectDraw reports:
  - It draws its sprites (front end, HUD) with the 2D overlay only if the *software* caps
    include DDCAPS_BLTSTRETCH (flag 0x62FAFE, set at 0x40711B). Otherwise it turns every sprite
    into a D3DRM decal.
  - D3DRM's Viewport::Clear only clears the areas it drew into on the previous frame, so the
    2D layer must not be cleared by Clear (the title screen is all 2D).
- The game parks pooled objects at z = -300 relative to the camera (0x404E4E) so they're
  never drawn.
- Debug: `MMCDA_VKDEBUG=1` clears to magenta and logs batch counts. Creating `vkshot.req`
  next to mmcda.ini makes the renderer write its next frame to `vkshot.bmp` (screenshots
  can't see a borderless Vulkan window).

**Stage 3 (2026-11-24): parity and options.**
- Look: race starts compared pixel by pixel with the original renderer on Amazon. Average colours
  match within 1-3 levels; the remaining per-pixel differences are texture sampling at polygon
  edges. No track turns fog on (checked one track per world), so fog is not part of parity.
- Back faces are culled like D3DRM (meshes only; decals stay double-sided). F10 debug menu:
  "Back faces Shown" draws them too, for testing.
- Texture filtering: `[render] filter = sharp | smooth` (F10 menu: Textures). Sharp is the
  original look (neither software D3DRM nor the PS1 filtered textures). Smooth uses linear
  sampling; colour-keyed texels take a neighbour's colour so edges don't fringe. Menus and the
  HUD stay sharp.
- Interpolation works unchanged on Vulkan: 120 Hz locks one tick to 4 vsyncs. A walk of the
  scene after each tick finds no untracked movers. About one frame in 10 s misses a vsync,
  with the time spent inside Present (the game's own work is ~5 ms).
- Display menu Cancel restores the Renderer choice with the game's settings word.

**Stage 4 (2026-11-24): default renderer, v1.1.**
- Pacing: the missed vsyncs were spent waiting for the display to release an image (the
  compositor). VK_EXT_full_screen_exclusive (application controlled) is used while the window
  covers its monitor (races); misses dropped from ~9 to ~1-2 per 40 s. `[render] exclusive=off`.
- Fog extra: `[render] fog=on` sets linear D3DRM scene fog (45-95% of the far clip) in a haze
  colour per world. Only Vulkan draws it; the software D3DRM device ignores scene fog.
- Vulkan is the default backend. At start `vkp_probe` checks for a Vulkan 1.1 instance and a
  GPU with a graphics queue; without one the original renderer is used (`MMCDA_NOVULKAN=1` tests
  that path).

**Performance (2026-11-24):** `MMCDA_PERF=1` logs per-stage frame times. A race frame was
4454 batches (the game alternates textures object by object). Since nothing blends and
everything is depth-tested, batches with the same state are merged into one indexed draw,
grouped with a hash table and kept in first-drawn order (so coplanar edges resolve as before;
pixel-identical). Recording went from 2.06 to 0.39 ms; a frame is ~5.5 ms of work (game 2.9,
scene transform/lighting 1.7, present 0.9), so 120 and 144 Hz have headroom.

**Still to do:** scene transform/lighting on the GPU or threads (for 240 Hz), in-game VR (phase 2, on hold), native ports (phase 3: Android,
Quest 3, Linux).

Debug switches (environment variables):
- `MMCDA_RENDER=original|vulkan`, `MMCDA_FILTER=sharp|smooth`, `MMCDA_TRACK=n` (world*16 + track)
- `MMCDA_VKDEBUG=1`: magenta clear and batch counts; `vkshot.req` next to mmcda.ini writes the
  next frame to `vkshot.bmp` plus the 2D layers (`vkshot2d.bin`, `vkshot2u.bin`)
- `MMCDA_INTROSHOT=1`: capture the race loading screen
- `MMCDA_PACEDIAG=1`: log race frames that miss a vsync; `MMCDA_INTERPDIAG=1`: log frames that
  move without being interpolated
- `MMCDA_INPUTLOG=1`: log keys/clicks in the game window (to record a menu path)
- `MMCDA_VDDTRACE=1`, `MMCDA_VRMTRACE=1`: DirectDraw / D3DRM call traces; `MMCDA_CENSUS=1`: call census
- `MMCDA_SKIP=six,render,window,fov,camera,transition,loadscreen,interp,tiles,debugmenu,vrexport,pad,car`:
  leave those features out at start, to bisect a problem (the two crash fixes always stay on)
