# Third-party code and licensing

NFS3VITA's own code is licensed under the MIT License ([LICENSE](LICENSE)). The parts below come from other projects
and keep their own terms.

## nfs-recompiled

https://github.com/motor-dev/nfs-recompiled (motor-dev), commit pinned in `upstream.lock`.

The virtual CPU (`include/cpu.h`, `include/fpu.h`), the Win32 shim layer (`include/winapi`, `src/lib/winapi`) and the
runtime skeleton (`include/lib`, `src/lib`) originate from this project and were modified for the Vita backend.
`tools/recompiler/` is a fork of the upstream generator (`disassemble_nfs3hp.py` and `disasm/`): the disassembly front
end (`disassembler.py`, `module.py`, `subroutine.py`, `dll.py`, `application.py`) keeps the upstream logic; the code
generators (`disasm/codegen/`), `analysis.py` and `emit.py` are rewritten for this port.

The upstream repository has no license file, so its code is not covered by this repository's MIT License; copyright
in it stays with its author. The terms for redistributing it are to be agreed with the upstream author.

## pefile

`tools/recompiler/disasm/pefile.py` and `tools/recompiler/disasm/ordlookup/`, from the pefile project (Ero Carrera,
version 2018.8.8), MIT License, as bundled upstream. The license text is in `tools/recompiler/disasm/LICENSE.pefile`.

## Need for Speed III: Hot Pursuit

Game code, binaries and assets are the property of Electronic Arts and are not part of this repository. The code
generated from them (`src/nfs3hp/disassembly/`) is not tracked.

## Modern Patch

https://gamebanana.com/mods/34590 (natko), CC BY-NC-ND 4.0. No part of it is in this repository; users supply it.
Release builds are generated from its `nfs3.exe`.
