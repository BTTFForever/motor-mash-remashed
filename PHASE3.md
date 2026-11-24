# Phase 3: native Linux, Android and Quest 3

Status: options for a decision (2026-11-24). Nothing here is started.

## What has to move

- **The game itself:** `MotorMsh.exe`, 32-bit x86 Windows code from 1998: 837 procedures,
  of which **789 are the game's own** (~245 KB of code; the rest are imports). It is the only
  part we don't have as source. All of them decompile cleanly with Ghidra (raw output in
  `decomp/MotorMsh/functions/`, kept out of git; see PORTING.md). The first analysis found 692;
  `tools/ghidra/RecoverMissed.java` adds the 97 it missed (19 front-end pages reached only
  through the page table, ~80 per-track trap routines) and 87 switch cases in 8 functions.
  What is left uncovered is the switch tables themselves, ~1.2 KB of unreferenced code and
  the C runtime.
- **What it calls** (its whole import table):
  - DirectDraw and Direct3D Retained Mode: **already reimplemented** by us (vdd.h, vrm.h)
    and drawn with Vulkan.
  - DirectInput (keyboard, joystick): controllers already go through our SDL3 layer.
  - DirectSound (effects), DirectPlay (network play), MCI CD audio (already ours, MMCDA).
  - About 130 basic Windows calls: window and messages, files, timers, a few GDI text and
    dialog calls, three registry reads.
- **Our code** (mmcda.dll): plain C. The Vulkan renderer, SDL3 input and audio decoding
  already build on Linux and Android with small changes; Quest 3 adds OpenXR.

So the hard part is the 789 game functions, not the platform layer.

## Options

### 1. Linux through Wine / Proton (days)
Run the patched game as it is under Wine or Proton (Steam Deck included). Vulkan goes
through winevulkan, controllers through SDL3.
- Effort: test, then fix what breaks (exclusive fullscreen, window handling, CD audio path).
- Result: works on Linux PCs and Steam Deck. Not a native build, x86 only, no Android/Quest.

### 2. Emulation on Android / Quest (days to weeks)
Box86/Box64 + Wine (as in Winlator) runs x86 Windows games on ARM.
- Result: playable on Android; on Quest only as a flat window. Not native, heavy, and no
  real VR integration. A stopgap at best.

### 3. Static recompilation to C (weeks to a few months)
Translate the 789 functions mechanically from x86 to C (one C function per game function,
registers as variables, x87 floating point emulated). Link it with our platform layer
(renderer, input, sound, file and window shims on SDL3).
- No mature off-the-shelf x86 Win32 recompiler exists; we would build the lifter on
  Ghidra's analysis. Jump tables, x87 code and self-modifying patches need care.
- Result: true native builds for Linux x86-64, Android ARM64 and Quest from one code base,
  behaving exactly like the original (same code, translated). The output is unreadable, so
  later changes still go through hooks.

### 4. Incremental decompilation to readable C (months, can start now)
Replace the game one function at a time with hand-cleaned C from Ghidra's output. Each
replaced function is hooked into the running exe and checked against the original (same
inputs, same results). The game stays playable the whole time. When all 789 are done, the
exe isn't needed: our code plus the platform layer is a native program.
- Result: real source for the game logic. Best base for in-game VR, 12 racers, Quest
  controls and later mods. Largest effort, but it starts from Ghidra output that already
  exists for every function, plus everything learned while patching (most globals and many
  functions are already identified in our hooks).
- This is the route PORTING.md ("Route B", step 4) already sketched; the API half of that
  route (D3DRM, DirectDraw, input, CD audio) is now done.
- Can be combined with option 3: decompile the parts we want to change (cars, cameras,
  menus, race logic), recompile the rest.

## Legal note

Patching a copy people own (the release model now) and loaders that run the original exe
(options 1-2) keep the game's code out of what we distribute. Options 3 and 4 produce code
derived from the game. That is fine in this private repo, but publishing it, or binaries
built from it, would be distributing a derivative of Ocean's code. Players would still need
their own CD for the data either way. Worth deciding before releasing anything from 3 or 4.

## Recommendation

1. **Now: option 1.** Make the current patch work well under Wine/Proton. That gives Linux
   and Steam Deck players the full v1.1 experience within days.
2. **Then: option 4 with option 3 as the accelerator.** Start decompiling the systems that
   matter most for later goals (race loop, cameras, cars, input, menus), hooked in one at a
   time, and recompile the rest mechanically. The result is one native code base for Linux,
   Android and Quest 3, and the base for in-game VR.
3. Skip option 2 except as a quick test on Android hardware.
