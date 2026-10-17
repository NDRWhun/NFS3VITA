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

## Playing with the Modern Patch 1.6.1 (recommended)

The port can be built from the `nfs3.exe` of the [Modern Patch 1.6.1](https://gamebanana.com/mods/34590) instead of
the original: races run natively at 960x544 with the patch's 16:9 HUD, menus and field-of-view options. You need:

- the Modern Patch 1.6.1, extracted (its `nfs3.exe` and data files);
- from the original game: `voodoo2a.dll` and `softtria.dll` (the patch's own drivers use Glide 3, which the port
  does not have), the `fedata` and `gamedata` folders (CD or installed game);
- `eacsnd.dll` from either.

Steps (PowerShell, after setting up the build environment below):

```powershell
# 1. one folder with the four binaries
mkdir binaries
copy <patch>\nfs3.exe, <patch>\eacsnd.dll, <game>\voodoo2a.dll, <game>\softtria.dll binaries
# 2. recompile and build
tools/generate.ps1 -Binaries binaries
tools/build.ps1                       # -> build-release/NFS3VITA.vpk
# 3. game data plus the patch's data files in one folder
tools/stage-data.ps1 -Game <game> -ModernPatch <patch> -Out vita-data
```

Install `build-release/NFS3VITA.vpk` with VitaShell and copy the contents of `vita-data` to `ux0:data/nfs3vita/`.
Races start at 960x544 on the first run. The 640x480 frontend menus and the loading screens are stretched to the
full screen; `stretch=0` in `ux0:data/nfs3vita/nfs3vita.cfg` shows them in 4:3 with side bars, and `resolution=WIDTHxHEIGHT`
picks another race resolution (for example `1024x768`, shown in 4:3).

## Building

Requirements: [VitaSDK](https://vitasdk.org), CMake 3.16+, Ninja, and a clone of the upstream
repository for the generated code. The shaders ship prebuilt in `shaders/gxp/`; the official PSP2
SDK (`host_tools/bin/psp2cgc`) is only needed to change them.

Machine-specific paths are never stored in the repository. Set them as environment variables, or
in `tools/env.local.ps1` (ignored by git):

```powershell
$env:VITASDK          = 'C:\vitasdk'                    # required
$env:SCE_PSP2_SDK_DIR = 'C:\psp2-sdk'                   # optional: recompile shaders (refreshes shaders/gxp)
$env:VITA3K_EXE       = 'C:\Vita3K\Vita3K.exe'          # optional: tools/run-vita3k.ps1 (Windows)
```

With PowerShell (Windows PowerShell 5.1 or PowerShell 7 on any OS):

```powershell
tools/generate.ps1 -Binaries <dir>   # recompiles nfs3.exe + its DLLs with tools/recompiler
tools/build.ps1            # -> build-release/NFS3VITA.vpk; rebuilds only what changed
```

`tools/generate.ps1` needs Python 3 with `pip install -r tools/recompiler/requirements.txt` and a directory
holding `nfs3.exe`, `eacsnd.dll`, `softtria.dll` and `voodoo2a.dll` (their SHA-256 is checked; the upstream
repository's `nfs3hp/` folder has the original ones). `tools/import-upstream.ps1 -Upstream <path-to-nfs-recompiled>`
still imports upstream's pre-generated code instead.

Or with CMake directly:

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

A full build compiles ~85 MB of generated C++ (a few minutes on a modern multi-core CPU); it only
recurs when a header used by the generated code changes (`include/x86.h`, `cpu.h`, `fpu.h`,
`mmx.h`, `lib/winapp.h`, `lib/memmap.h`, `lib/thread.h`). Other changes rebuild a few files.

## Layout

```
cmake/          shader compilation, embedding, options
include/        virtual x86 CPU (cpu/fpu/mmx), lib/ runtime headers, winapi/ shim headers, platform/
shaders/        Cg programs; gxp/ holds their psp2cgc binaries
src/lib/        runtime: guest memory, threads, Win32 shims, Glide and DirectDraw presentation
src/platform/   Vita backend: gxm device and render thread, audio, input, sync, logging
src/nfs3hp/     entry point (+ imported disassembly)
tools/          environment, import and build scripts
```
