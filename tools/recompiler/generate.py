"""Generates the C++ recompilation of NFS3 (nfs3.exe and its three DLLs) into one directory.

Usage: generate.py --binaries <dir> --out <dir>

<dir> must hold nfs3.exe, eacsnd.dll, softtria.dll and voodoo2a.dll. The SHA-256 of nfs3.exe selects the binary set
(the original game or the Modern Patch 1.6.1); every file is checked against that set because the address tables
below only fit these exact files.

Files in the output directory are replaced only when their content changes; generated files that are no longer
produced are deleted, other files are left alone.
"""

import argparse
import filecmp
import hashlib
import json
import os
import re
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'disasm'))
from disasm import analysis, disassembler, dll


class BinarySet:
    def __init__(self, name, label, exe, data_segments, skip_instructions, thread_segments, known_subroutines,
                 skip_nop_runs):
        self.name = name                            # enumerator in the generated layout header
        self.label = label
        self.exe = exe                              # SHA-256 of nfs3.exe
        self.data_segments = data_segments
        self.skip_instructions = skip_instructions
        self.thread_segments = thread_segments      # busy-wait loops: the generated code yields there
        self.known_subroutines = known_subroutines
        self.skip_nop_runs = skip_nop_runs          # functions may start after runs of single-byte nops


# Data inside the original nfs3.exe code section.
ORIGINAL_DATA_SEGMENTS = [(0x401004, 0x401010),
                          (0x405734, 0x405740),
                          (0x409ff0, 0x40a010),
                          (0x40cb00, 0x40cb20),
                          (0x412920, 0x412994),
                          (0x41a880, 0x41a8c8),
                          (0x420814, 0x420840),
                          (0x4212b0, 0x421360),
                          (0x423d50, 0x423d90),
                          (0x42d550, 0x42d618),
                          (0x42dd30, 0x42dd58),
                          (0x434ea0, 0x434f14),
                          (0x43f180, 0x43f1e4),
                          (0x44e150, 0x44e170),
                          (0x4489f0, 0x448ef2),
                          (0x454f10, 0x455020),
                          (0x45cb60, 0x45cdc0),
                          (0x465d10, 0x465d60),
                          (0x46ccb0, 0x46cce0),
                          (0x472160, 0x4721f0),
                          (0x478310, 0x478320),
                          (0x47ba70, 0x47bbb8),
                          (0x488030, 0x4880b0),
                          (0x489630, 0x4896f0),
                          (0x491110, 0x491120),
                          (0x493370, 0x493380),
                          (0x4975b0, 0x497620),
                          (0x49c3e0, 0x49c410),
                          (0x49e060, 0x49e080),
                          (0x4a2f50, 0x4a3050),
                          (0x4a4050, 0x4a4060),
                          (0x4a6f80, 0x4a7080),
                          (0x4af3b0, 0x4b0700),
                          (0x4b5580, 0x4b56d0),
                          (0x4b7a10, 0x4b7a60),
                          (0x4c9a70, 0x4c9a80),
                          (0x4d0650, 0x4d06f0),
                          (0x4d5740, 0x4d5840),
                          (0x4dfae5, 0x4dfaf0),
                          (0x4f7fa5, 0x4f8020),
                          (0x51679d, 0x5167b3),
                          (0x52efb0, 0x52f318),
                          (0x533ef0, 0x534000),]

# The Modern Patch put code into three of the original data regions (0x472160 now starts with padding, 0x4a2f50 and
# 0x4b5580 hold code) and added its own tables to the code section.
MODERN_PATCH_DATA_SEGMENTS = sorted(
    [s for s in ORIGINAL_DATA_SEGMENTS if s[0] not in (0x472160, 0x4a2f50, 0x4b5580)] +
    [(0x40ff80, 0x40ff9c),      # byte tables
     (0x41d6e4, 0x41d7e4),      # word table (mov esi, 0x41d6e4)
     (0x4721b8, 0x4721f0),      # remainder of the original table after the patch's padding
     (0x472e20, 0x472e56),      # byte tables
     (0x4961f0, 0x496200),      # string pointer table
     (0x4a3938, 0x4a3984),      # path pointer table
     (0x4ccaf8, 0x4ccb84),      # key tables
     (0x4f8390, 0x4f84a8)])     # THRASH export names and pointer slots

ORIGINAL_THREAD_SEGMENTS = [0x43ac67, 0x43ac97, 0x46785e, 0x4f06f7, 0x4f2191, 0x495b68, 0x4d9158, 0x4c4e84, 0x4f7d0e]

BINARY_SETS = [
    BinarySet('Original', 'original nfs3.exe',
              'b390be67925b2ad0857fc67dd6431ce68be597253eaa47c30789dc885a19d51b',
              ORIGINAL_DATA_SEGMENTS,
              skip_instructions=[0x4a3aec],
              thread_segments=ORIGINAL_THREAD_SEGMENTS,
              known_subroutines=[0x4ff3d0],
              skip_nop_runs=False),
    # The CPU benchmark loop (0x4f2191) is gone and 0x4a3aec is a nop.
    BinarySet('ModernPatch161', 'Modern Patch 1.6.1 nfs3.exe',
              '240a083a78ac3b836f0bdf3fe643a1c6568c12eefa5a186a2f6edd808596dca6',
              MODERN_PATCH_DATA_SEGMENTS,
              skip_instructions=[],
              thread_segments=[a for a in ORIGINAL_THREAD_SEGMENTS if a != 0x4f2191],
              known_subroutines=[0x4ff3d0],
              skip_nop_runs=True),
]

# DLLs in load order, rebased one after the other above nfs3.exe. Hints are offsets from the DLL's guest base.
# eacsnd.dll of the Modern Patch only adds a version resource to the original.
DLLS = [
    ('eacsnd', 'eacsnd.dll',
     ('30288653eb2e0b59a3173530433a4adb8ea0dc66cdbc46aadf742dc53ab62b46',
      '63f3d1bff4e6526753524423be47146e82416d1b170cd7b2e0133699587b01b3'),
     [(0x3f49, 0x3f5f), (0x5abf, 0x5add)], []),
    ('softtria', 'softtria.dll',
     ('8ea15aac0a7095b7d750cd75bbc9095553b6f708d1b89ca807864294a1a12b02',),
     [(0x35287, 0x35300), (0x3a009, 0x3a01f), (0x1004, 0x1010)], []),
    ('voodoo2a', 'voodoo2a.dll',
     ('f65f00e6fa6a5a67e11ff38d661b3d493474d0e075c9183a7b244b1143cea88e',),
     [(0x1004, 0x1010), (0x4633, 0x46a2)], [0x25d0, 0x2610]),
]

MERGE_ROUTINES = [(0x405740, 0x405791),
                  (0x405740, 0x4057a0),
                  (0x420840, 0x42088a),
                  (0x420840, 0x420a71),
                  (0x420840, 0x420a7a),
                  (0x420840, 0x420a83),
                  (0x420840, 0x420aac),
                  (0x420840, 0x420b53),]

# Names of the files the generator writes: <module>.cpp/.h, <module>.<n>.cpp (functions), <module>.<section>.cpp
# (section data) and the layout header.
GENERATED_FILE = re.compile(r'^(%s)(\.\w+)?\.(cpp|h)$|^nfs3hp_layout\.h$'
                            % '|'.join(['nfs3hp'] + [name for name, _, _, _, _ in DLLS]))

SPLIT_INSTRUCTIONS = [0x4e08af, 0x4e087a, 0x4eee0f]


# CPUID probe: pushfd on entry, popfd before ret, so the caller's flags pass through unchanged.
FLAGS_PRESERVED = [0x50ea68]

# Data decoded as a function: a dword table read with mov esi, 0x47bbb8 (sub_485680), never executed.
NOT_CODE = [0x47bbb8]

# Guest functions replaced by hand-written native code (native::sub_<address>); the translated body stays as
# sub_<address>_guest, which the game does not call (test tools do).
NATIVE_OVERRIDES = [0x4ea4f0, 0x5033e4, 0x5033e5, 0x4e070c, 0x4e0716, 0x4e0721,
                    0x4bf4c0, 0x4e03b0, 0x4e0430, 0x4e0210, 0x4e0280,
                    0x4c12e0, 0x4c2cd0,
                    0x4beb20]     # view rect read: the Modern Patch loading screen (src/nfs3hp/loadingscreen.cpp)


def sha256(path):
    with open(path, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


def check_binaries(directory):
    """Returns the binary set of the nfs3.exe in directory after checking every file."""
    def path(name):
        p = os.path.join(directory, name)
        if not os.path.isfile(p):
            sys.exit('missing %s' % p)
        return p

    exe = sha256(path('nfs3.exe'))
    binary_set = next((s for s in BINARY_SETS if s.exe == exe), None)
    if not binary_set:
        sys.exit('%s: SHA-256 %s fits no known binary set (%s)'
                 % (path('nfs3.exe'), exe, ', '.join(s.label for s in BINARY_SETS)))
    for _, name, hashes, _, _ in DLLS:
        digest = sha256(path(name))
        if digest not in hashes:
            sys.exit('%s: SHA-256 %s, expected %s (the original file)' % (path(name), digest, ' or '.join(hashes)))
    print('binary set: %s' % binary_set.label)
    return binary_set


def read_addresses(path):
    if not path:
        return ()
    with open(path) as f:
        return [int(word, 16) for line in f for word in line.split('#')[0].split()]


def write_analysis(path, program, modules):
    functions = []
    for module in modules:
        for sub in module.subroutines:
            a = sub.get_entry_point()
            info = program.stack.get(a)
            functions.append({'entry': a, 'module': module.application_name, 'ret_live': program.ret_live[a],
                              'live_in': program.live_in[a], 'preserved': info.preserved if info else 0,
                              'native': a in program.native_overrides})
    sections = []
    for module in modules:
        for data, address, length, name, ro, code in module.sections:
            sections.append({'module': module.application_name, 'name': name.decode(), 'address': address,
                             'length': length, 'data': bool(data)})
    with open(path, 'w') as f:
        json.dump({'functions': functions, 'sections': sections}, f)


def write_layout(out_dir, binary_set, application, dlls):
    """nfs3hp_layout.h: the binary set and the guest addresses that depend on it, for the port code."""
    names = ', '.join(s.name for s in BINARY_SETS)
    lines = ['// Generated by tools/recompiler/generate.py: binary set and guest layout of the generated code.',
             '#ifndef NFS3HP_LAYOUT_H_',
             '#define NFS3HP_LAYOUT_H_',
             '#include <x86.h>',
             '',
             'namespace nfs3hp { namespace layout {',
             '',
             'enum class Binaries { %s };' % names,
             'constexpr Binaries binaries = Binaries::%s;' % binary_set.name,
             '',
             '// End of the highest loaded section of nfs3.exe and its DLLs.',
             'constexpr x86::reg32 imageEnd = 0x%x;' % dlls[-1].end_address(),
             '',
             '// Guest base of each DLL (its image base after rebasing; section addresses are base + RVA).']
    lines += ['constexpr x86::reg32 %sBase = 0x%x;' % (d.application_name, d.base_address) for d in dlls]
    resources = [s for s in application.sections if s[3] == b'_rsrc']
    lines += ['',
              '// nfs3.exe image base and its resource section (the RVAs in the resource directory refer to the base).',
              'constexpr x86::reg32 imageBase = 0x%x;' % application.base_address,
              'constexpr x86::reg32 resourceSection = 0x%x;' % (resources[0][1] if resources else 0)]
    lines += ['', '}}', '', '#endif /* !NFS3HP_LAYOUT_H_ */', '']
    with open(os.path.join(out_dir, 'nfs3hp_layout.h'), 'w', newline='\n') as f:
        f.write('\n'.join(lines))


def sync_output(stage, out_dir):
    """Copies the files generated in stage whose content changed (unchanged files keep their timestamps, so the build
    recompiles just the changed ones) and deletes generated files of earlier runs; other files are left alone."""
    names = set(os.listdir(stage))
    changed = removed = 0
    for name in sorted(names):
        assert GENERATED_FILE.match(name), name
        target = os.path.join(out_dir, name)
        if not os.path.isfile(target) or not filecmp.cmp(os.path.join(stage, name), target, shallow=False):
            shutil.copyfile(os.path.join(stage, name), target)
            changed += 1
    for name in os.listdir(out_dir):
        if name not in names and GENERATED_FILE.match(name) and os.path.isfile(os.path.join(out_dir, name)):
            os.remove(os.path.join(out_dir, name))
            removed += 1
    print('generated %d files into %s (%d changed, %d removed)' % (len(names), out_dir, changed, removed))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--binaries', required=True, help='directory with the game binaries')
    parser.add_argument('--out', required=True, help='output directory for the generated sources')
    parser.add_argument('--analysis', help='also write the analysis summary (JSON) for test tools')
    parser.add_argument('--memory-x87', action='store_true',
                        help='keep the x87 stack in cpu.fpu instead of C++ locals (for comparisons)')
    parser.add_argument('--memory-registers', action='store_true',
                        help='keep x86 registers in cpu instead of C++ locals (for comparisons)')
    parser.add_argument('--memory-functions', metavar='FILE',
                        help='file of hex function entries that keep registers in cpu (for bisecting)')
    args = parser.parse_args()
    binary_set = check_binaries(args.binaries)
    os.makedirs(args.out, exist_ok=True)

    def binary(name):
        return os.path.join(args.binaries, name)

    application = disassembler.disassemble('nfs3hp', binary('nfs3.exe'), binary_set.data_segments,
                                           binary_set.known_subroutines, SPLIT_INSTRUCTIONS, MERGE_ROUTINES,
                                           skip_nop_runs=binary_set.skip_nop_runs)
    dlls = []
    previous = application
    for name, file_name, _, data_segments, known_subroutines in DLLS:
        base = previous.end_address()
        module = disassembler.disassemble(name, binary(file_name), type=dll.DLL,
                                          data_segments=[(base + a, base + b) for a, b in data_segments],
                                          known_subroutines=[base + a for a in known_subroutines],
                                          rebase_after=previous)
        dlls.append(module)
        previous = module
    program = analysis.Program([application] + dlls, binary_set.thread_segments, binary_set.skip_instructions, {},
                               FLAGS_PRESERVED, NOT_CODE, NATIVE_OVERRIDES,
                               local_registers=not args.memory_registers,
                               local_x87=not args.memory_x87,
                               memory_functions=read_addresses(args.memory_functions))
    program.analyze()
    with tempfile.TemporaryDirectory(prefix='nfs3gen-') as stage:
        application.write(stage, program, dlls=dlls)
        write_layout(stage, binary_set, application, dlls)
        sync_output(stage, args.out)
    if args.analysis:
        write_analysis(args.analysis, program, [application] + dlls)


if __name__ == '__main__':
    main()
