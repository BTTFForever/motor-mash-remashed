# Motor Mash VR (tabletop viewer)

Shows the race you're playing as a miniature diorama on a table, in an OpenXR headset
(tested runtime: Virtual Desktop / Quest 3) and in a desktop window.

## How it works
- `MMCDA.dll` (in the game folder) exports the live D3DRM scene when `mmcda.ini` has
  `[vr] export=1` (or `MMCDA_VR=1`): every logic tick it walks the scene frame tree and
  writes each MeshBuilder instance with its world matrix to shared memory
  `Local\MotorMashVR`. Meshes and textures are written once per race to
  `game\vrcache\<n>\` (`m<id>.bin`, `t<id>.bin`; formats in `../mmcda/vrshared.h`).
  Textures come from the game's own DirectDraw texture surfaces (table at 0x54def8),
  including the colour key used for transparency.
- `MotorMashVR.exe` (64-bit, D3D11 + OpenXR) reads that, interpolates the ~30 Hz ticks to
  the headset rate, and draws the track with instancing.

## Use
1. Start the game (`game\MotorMsh_dbg.exe`) and start a race.
2. Connect the Quest through Virtual Desktop.
3. Run `MotorMashVR.exe` (it opens on the other monitor and never takes focus).
4. Keep the game window focused: it pauses when it loses focus, and it needs the keyboard.

Headset controls: grip = grab and move/turn the table, right stick up/down = scale,
left stick left/right = rotate, A/X = put the table in front of you,
B/Y = cycle follow mode: off, follow your car (the map slides so the car stays at the
table centre), and follow + turn (the map also turns to match the game camera's heading).

Also shown: decals (dust, smoke and other sprites, drawn facing you). Cars and their
shadows are lowered onto the track; the game lifts them ~10 and ~5 units, which only
shows up in VR.

Desktop window: drag = orbit, wheel = zoom, right-click = orbit around your car.
`--desktop` skips OpenXR. Test switches: `MMVR_FOLLOW=1`, `MMVR_FOLLOWMODE=0..2`,
`MMVR_DIST` (orbit distance, m), `MMVR_YAW` (orbit yaw, radians). `MotorMashVR.log` is written next to the exe.

## Build
`build.bat` (VS 2022, x64). OpenXR headers/loader: `third_party/openxr`
(OpenXR.Loader 1.1.65 NuGet package from KhronosGroup/OpenXR-SDK).
