"""Generates the C++ recompilation of NFS3 (nfs3.exe and its three DLLs) into one directory.

Usage: generate.py --binaries <dir> --out <dir>

<dir> must hold the original nfs3.exe, eacsnd.dll, softtria.dll and voodoo2a.dll; their SHA-256 is checked
because the address tables below only fit these exact files.
"""

import argparse
import hashlib
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'disasm'))
from disasm import analysis, disassembler, dll


BINARIES = {
    'nfs3.exe': 'b390be67925b2ad0857fc67dd6431ce68be597253eaa47c30789dc885a19d51b',
    'eacsnd.dll': '30288653eb2e0b59a3173530433a4adb8ea0dc66cdbc46aadf742dc53ab62b46',
    'softtria.dll': '8ea15aac0a7095b7d750cd75bbc9095553b6f708d1b89ca807864294a1a12b02',
    'voodoo2a.dll': 'f65f00e6fa6a5a67e11ff38d661b3d493474d0e075c9183a7b244b1143cea88e',
}

DATA_SEGMENTS = [(0x401004, 0x401010),
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
                 (0x493360, 0x493380),
                 (0x493370, 0x493380),
                 (0x49e060, 0x49e080),
                 (0x4975b0, 0x497620),
                 (0x49c3e0, 0x49c410),
                 (0x49e060, 0x49e080),
                 (0x4a2f50, 0x4a3050),
                 (0x4a4050, 0x4a4060),
                 (0x4a6f80, 0x4a7080),
                 (0x4af3b0, 0x4b0700),
                 (0x4b5580, 0x4b56d0),
                 (0x4b6a10, 0x4b6a10),
                 (0x4b7a10, 0x4b7a60),
                 (0x4c9a70, 0x4c9a80),
                 (0x4d0650, 0x4d06f0),
                 (0x4d5740, 0x4d5840),
                 (0x4dfae5, 0x4dfaf0),
                 (0x4f7fa5, 0x4f8020),
                 (0x51679d, 0x5167b3),
                 (0x52efb0, 0x52f318),
                 (0x533ef0, 0x534000),]


MERGE_ROUTINES = [(0x405740, 0x405791),
                  (0x405740, 0x4057a0),
                  (0x420840, 0x42088a),
                  (0x420840, 0x420a71),
                  (0x420840, 0x420a7a),
                  (0x420840, 0x420a83),
                  (0x420840, 0x420aac),
                  (0x420840, 0x420b53),]

KNOWN_SUBROUTINES = [0x4ff3d0,]

SKIP_INSTRUCTIONS = [0x4a3aec]


SPLIT_INSTRUCTIONS = [0x4e08af, 0x4e087a, 0x4eee0f]


# CPUID probe: pushfd on entry, popfd before ret, so the caller's flags pass through unchanged.
FLAGS_PRESERVED = [0x50ea68]

# Data decoded as a function: a dword table read with mov esi, 0x47bbb8 (sub_485680), never executed.
NOT_CODE = [0x47bbb8]

# Guest functions replaced by hand-written native code (native::sub_<address>); the translated body stays as
# sub_<address>_guest for NFS3_NATIVE_VERIFY builds.
NATIVE_OVERRIDES = [0x4ea4f0, 0x5033e4, 0x5033e5, 0x4e070c, 0x4e0716, 0x4e0721,
                    0x4bf4c0, 0x4e03b0, 0x4e0430, 0x4e0210, 0x4e0280,
                    0x4c12e0, 0x4c2cd0]

THREAD_ROUTINES = []
THREAD_SEGMENTS = [0x43ac67, 0x43ac97, 0x46785e, 0x4f06f7, 0x4f2191, 0x495b68, 0x4d9158, 0x4c4e84, 0x4f7d0e]


def check_binaries(directory):
    for name, expected in BINARIES.items():
        path = os.path.join(directory, name)
        if not os.path.isfile(path):
            sys.exit('missing %s' % path)
        with open(path, 'rb') as f:
            digest = hashlib.sha256(f.read()).hexdigest()
        if digest != expected:
            sys.exit('%s: SHA-256 %s, expected %s (only the original file fits the tables)' % (path, digest, expected))


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
                              'live_in': program.live_in[a], 'preserved': info.preserved if info else 0})
    sections = []
    for module in modules:
        for data, address, length, name, ro, code in module.sections:
            sections.append({'module': module.application_name, 'name': name.decode(), 'address': address,
                             'length': length, 'data': bool(data)})
    with open(path, 'w') as f:
        json.dump({'functions': functions, 'sections': sections}, f)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--binaries', required=True, help='directory with the original game binaries')
    parser.add_argument('--out', required=True, help='output directory for the generated sources')
    parser.add_argument('--analysis', help='also write the analysis summary (JSON) for test tools')
    parser.add_argument('--memory-registers', action='store_true',
                        help='keep x86 registers in cpu instead of C++ locals (for comparisons)')
    parser.add_argument('--memory-functions', metavar='FILE',
                        help='file of hex function entries that keep registers in cpu (for bisecting)')
    args = parser.parse_args()
    check_binaries(args.binaries)
    os.makedirs(args.out, exist_ok=True)

    def binary(name):
        return os.path.join(args.binaries, name)

    application = disassembler.disassemble('nfs3hp', binary('nfs3.exe'), DATA_SEGMENTS,
                                           KNOWN_SUBROUTINES, SPLIT_INSTRUCTIONS,
                                           THREAD_ROUTINES, THREAD_SEGMENTS, MERGE_ROUTINES)
    eacsnd = disassembler.disassemble('eacsnd', binary('eacsnd.dll'), type=dll.DLL,
                                      data_segments=[(0xa35749, 0xa3575f),
                                                     (0xa372bf, 0xa372dd)], rebase_after=application)
    softtria = disassembler.disassemble('softtria', binary('softtria.dll'), type=dll.DLL,
                                        data_segments=[(0xa73e87, 0xa73f00),
                                                       (0xa78c09, 0xa78c1f),
                                                       (0xa3fc04, 0xa3fc10),], rebase_after=eacsnd)
    voodoo2a = disassembler.disassemble('voodoo2a', binary('voodoo2a.dll'), type=dll.DLL,
                                        data_segments=[(0xa83204, 0xa83210),
                                                       (0xA86833, 0xA868A2)],
                                        known_subroutines=[0xA847D0, 0xA84810], rebase_after=softtria)
    dlls = [eacsnd, softtria, voodoo2a]
    program = analysis.Program([application] + dlls, THREAD_SEGMENTS, SKIP_INSTRUCTIONS, {}, FLAGS_PRESERVED, NOT_CODE,
                               NATIVE_OVERRIDES, local_registers=not args.memory_registers,
                               memory_functions=read_addresses(args.memory_functions))
    program.analyze()
    application.write(args.out, program, dlls=dlls)
    if args.analysis:
        write_analysis(args.analysis, program, [application] + dlls)


if __name__ == '__main__':
    main()
