# NFS3VITA — Need for Speed III: Hot Pursuit on PS Vita

A PlayStation Vita port of the [nfs-recompiled](https://github.com/motor-dev/nfs-recompiled)
static recompilation of *Need for Speed III: Hot Pursuit* (Win32, 1998). The recompiled game
code runs on a virtual x86 CPU; this repository replaces the SDL3/OpenGL runtime with a native
Vita backend: libgxm rendering, SceAudioOut audio, SceCtrl input and SceKernel threads.

## What is and is not here

- Tracked: the runtime (`include/`, `src/lib`, `src/platform`), shaders, build glue.
- Not tracked: the generated recompilation (`src/nfs3hp/disassembly/`, ~85 MB) and any game
  binary or data. Import the generated code with `tools\import-upstream.ps1` from a clone of
  the upstream repository at the commit pinned in `upstream.lock`.
- You need your own copy of the game. Copy the installed game directory (including the
  `fedata` and `gamedata` folders from the CD) to `ux0:data/nfs3vita/` on the Vita.

## Building

Requirements: VitaSDK, CMake 3.16+, Ninja, and the official PSP2 SDK for its shader compiler
(`host_tools/bin/psp2cgc.exe`). Paths are configured in `tools\env.ps1`
(override locally in `tools\env.local.ps1`).

```powershell
tools\import-upstream.ps1
tools\build.ps1            # -> build-release\NFS3VITA.vpk
```

## Layout

```
cmake/          shader compilation, embedding, options
include/        virtual x86 CPU (cpu/fpu/mmx), lib/ runtime headers, winapi/ shim headers, platform/
shaders/        Cg programs compiled with psp2cgc
src/lib/        runtime: guest memory, threads, Win32 shims, Glide and DirectDraw presentation
src/platform/   Vita backend: gxm device and render thread, audio, input, sync, logging
src/nfs3hp/     entry point (+ imported disassembly)
tools/          environment, import and build scripts
```
