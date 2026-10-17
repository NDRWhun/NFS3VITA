# Third-party code

## nfs-recompiled

https://github.com/motor-dev/nfs-recompiled — commit pinned in `upstream.lock`.

The virtual CPU (`include/cpu.h`, `include/fpu.h`), the Win32 shim layer (`include/winapi`,
`src/lib/winapi`) and the runtime skeleton (`include/lib`, `src/lib`) originate from this project
and were modified for the Vita backend. The generated game code is not distributed here.

`tools/recompiler/` is a fork of the upstream generator (`disassemble_nfs3hp.py` and `disasm/`) at the
pinned commit. The disassembly front end (`disassembler.py`, `module.py`, `subroutine.py`, `dll.py`,
`application.py`) keeps the upstream logic; the code generators (`disasm/codegen/`), `analysis.py` and
`emit.py` are rewritten for this port. `disasm/pefile.py` and `disasm/ordlookup/` are from the pefile project
(Ero Carrera, MIT license), as bundled upstream.

The upstream repository does not carry a license file. Its author has been credited here; the
terms under which that code may be redistributed must be clarified with the upstream author
before publishing this port.

## Need for Speed III: Hot Pursuit

Game code, binaries and assets are the property of Electronic Arts and are not part of this
repository.
