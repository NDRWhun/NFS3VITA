"""Recompiler checks on synthetic machine code (no game binaries): python -m unittest discover tools/recompiler/tests"""

import io
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'disasm'))

import capstone  # noqa: E402
import generate  # noqa: E402
from disasm import analysis, disassembler, emit  # noqa: E402
from disasm.module import Module  # noqa: E402
from disasm.subroutine import Subroutine  # noqa: E402

BASE = 0x401000


def module(code):
    """A module whose only section is code at BASE."""
    m = Module.__new__(Module)
    m.application_name = 'test'
    m.sections = [(bytes(code), BASE, len(code), b'_code', True, True)]
    m.entry_point = BASE
    m.exported_symbols = []
    m.subroutines = []
    return m


def decode(m):
    cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    cs.detail = True
    section = m.sections[0]
    sub = Subroutine(m, section, b'', b'', [])
    end = disassembler.decode_subroutine(cs, m, section, BASE, [], sub)
    return sub, end


def translate(code):
    """C++ of a one-function module."""
    m = module(code)
    sub, _ = decode(m)
    m.subroutines = [sub]
    program = analysis.Program([m], [], [], {})
    program.analyze(log=lambda *_: None)
    out = io.StringIO()
    emit.write_function(out, m, program.functions[BASE], 'void f()', {})
    return out.getvalue()


class Codegen(unittest.TestCase):
    def test_16bit_push_pop_move_esp_by_2(self):
        text = translate(bytes.fromhex('6650' '6658' 'c3'))     # push ax; pop ax; ret
        self.assertIn('esp - 2', text)
        self.assertIn('esp + 2', text)

    def test_segment_push_moves_esp_by_4(self):
        text = translate(bytes.fromhex('06' '07' 'c3'))         # push es; pop es; ret
        self.assertIn('esp - 4', text)
        self.assertIn('esp + 4', text)

    def test_fistp_stores_integer_indefinite(self):
        text = translate(bytes.fromhex('d9e8' 'db18' 'c3'))     # fld1; fistp dword ptr [eax]; ret
        self.assertIn('x86::fistInteger<x86::sreg32>', text)

    def test_rotate_by_width_sets_cf(self):
        text = translate(bytes.fromhex('c0c008' '0f92c0' 'c3'))  # rol al, 8; setb al; ret
        self.assertIn('const x86::reg8 r = a;', text)
        self.assertIn('f_cf = ((r & 1) != 0);', text)

    def test_parity_after_integer_producer_fails(self):
        with self.assertRaises(AssertionError):
            translate(bytes.fromhex('f6c444' '7a01' '90' 'c3'))  # test ah, 0x44; jp +1; nop; ret

    def test_parity_after_sahf(self):
        translate(bytes.fromhex('9e' '7a01' '90' 'c3'))          # sahf; jp +1; nop; ret


class Disassembler(unittest.TestCase):
    def test_subroutine_longer_than_one_window(self):
        body = bytes.fromhex('8b8078563412') * 3000             # mov eax, [eax + 0x12345678]
        m = module(body + b'\xc3')
        sub, end = decode(m)
        self.assertEqual(len(sub.instructions), 3001)
        self.assertEqual(end, BASE + len(body) + 1)

    def test_invalid_byte_ends_subroutine(self):
        m = module(bytes.fromhex('90' 'ff'))                    # nop; a lone 0xff cannot be decoded
        sub, end = decode(m)
        self.assertEqual(len(sub.instructions), 1)
        self.assertEqual(end, BASE + 2)


def write(path):
    with open(path, 'w') as f:
        f.write(os.path.basename(path))


class Output(unittest.TestCase):
    def test_sync_keeps_foreign_files(self):
        with tempfile.TemporaryDirectory() as stage, tempfile.TemporaryDirectory() as out:
            for name in ('nfs3hp.0.cpp', 'nfs3hp_layout.h'):
                write(os.path.join(stage, name))
            for name in ('nfs3hp.7.cpp', 'eacsnd._idata.cpp', 'notes.cpp', 'nfs3hp_extra.h'):
                write(os.path.join(out, name))
            generate.sync_output(stage, out)
            self.assertEqual(sorted(os.listdir(out)), ['nfs3hp.0.cpp', 'nfs3hp_extra.h', 'nfs3hp_layout.h', 'notes.cpp'])


if __name__ == '__main__':
    unittest.main()
