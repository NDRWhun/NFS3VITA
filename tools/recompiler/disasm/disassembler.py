import sys
try:
    import capstone
except ImportError:
    print('Capstone not found; verify that the Capstone extension for Python is installed')
    print('%s -m pip show capstone' % sys.executable)
    sys.exit(1)
import ctypes
from .subroutine import Subroutine
from .application import Application

nops = (
    b'\x8d\x80\x00\x00\x00\x00\x8d\x92\x00\x00\x00\x00\x8d\x40\x00',
    b'\x8b\xc0\x8d\x80\x00\x00\x00\x00\x8d\x92\x00\x00\x00\x00',
    b'\x8d\x80\x00\x00\x00\x00\x8d\x92\x00\x00\x00\x00\x8b\xc0',
    b'\x90\x8d\x80\x00\x00\x00\x00\x8d\x92\x00\x00\x00\x00',
    b'\x8d\x80\x00\x00\x00\x00\x8d\x92\x00\x00\x00\x00\x90',
    b'\x8d\x80\x00\x00\x00\x00\x8d\x92\x00\x00\x00\x00',
    b'\x8d\x80\x00\x00\x00\x00\x8d\x52\x00\x8b\xdb',
    b'\x8d\x80\x00\x00\x00\x00\x8d\x54\x22\x00',
    b'\x8d\x80\x00\x00\x00\x00\x8d\x52\x00',
    b'\x8d\x80\x00\x00\x00\x00\x8b\xd2',
    b'\x8d\x80\x00\x00\x00\x00\x90',
    b'\x8d\x80\x00\x00\x00\x00',
    b'\x8d\x40\x00\x8b\xc9',
    b'\x8d\x44\x20\x00',
    b'\x8d\x40\x00',
    b'\x8b\xc0',
    b'\x90',
)


def skip_nop_run(application, section, current_address):
    """Skips a run of two or more single-byte nops (padding left by in-place binary patches); returns the new
    address and the skipped bytes."""
    end = application.get_address_end(section)
    address = current_address
    while address < end and application.read_section_data(section, address, address + 1) == b'\x90':
        address += 1
    if address - current_address < 2:
        return current_address, b''
    return address, application.read_section_data(section, current_address, address)


def realign(application, section, current_address, skip_nop_runs=False):
    # A nop run is not reported as skipped code (which keeps the next code a separate function): patches also leave
    # them inside functions, between a jump and its targets.
    if skip_nop_runs:
        address, nop_run = skip_nop_run(application, section, current_address)
        if nop_run:
            return (address, '')
    current_address, skipped_code = align(application, section, current_address)
    if skip_nop_runs:
        current_address, _ = skip_nop_run(application, section, current_address)
    return (current_address, skipped_code)


def align(application, section, current_address):
    new_current_address = (current_address + 15) & 0xfffffff0
    skipped_code = application.read_section_data(section, current_address, new_current_address)
    if current_address != new_current_address:
        if sum([c for c in skipped_code]) == 0:
            current_address = new_current_address
        elif skipped_code in nops:
            current_address = new_current_address
    if current_address != new_current_address:
        new_current_address = (current_address + 3) & 0xfffffffc
        skipped_code = application.read_section_data(section, current_address, new_current_address)
        if sum([c for c in skipped_code]) == 0:
            current_address = new_current_address
        elif skipped_code in nops:
            current_address = new_current_address
        else:
            new_current_address = (current_address + 1) & 0xfffffffe
            skipped_code = application.read_section_data(section, current_address, new_current_address)
            if sum([c for c in skipped_code]) == 0:
                current_address = new_current_address
            elif skipped_code in nops:
                current_address = new_current_address
            else:
                skipped_code = ''
    return (current_address, skipped_code)


def decode_subroutine(disassembler, application, section, start, split_instructions, subroutine):
    """Adds instructions from start until the subroutine ends; returns the address after it. Decodes in
    windows, so a subroutine may be any size; an undecodable byte or the section end also ends it."""
    end = application.get_address_end(section)
    address = start
    while address < end:
        window = application.read_section_data(section, address, min(address + 8192, end))
        decoded = False
        for instruction in disassembler.disasm(window, address):
            decoded = True
            address = instruction.address + instruction.size
            next_address = subroutine.add_instruction(instruction)
            if next_address:
                return next_address
            for split_instruction in split_instructions:
                if instruction.address < split_instruction < address:
                    subroutine.add_split_instruction(split_instruction)
                    data = application.read_section_data(section, split_instruction, address)
                    for extra_inst in disassembler.disasm(data, split_instruction):
                        subroutine.add_instruction(extra_inst)
        if not decoded:
            print('Invalid instruction at address 0x%08x' % address)
            return address + 1
    print('End of section at address 0x%08x' % address)
    return address


def disassemble(name, exe_path, data_segments=[], known_subroutines=[], split_instructions=[], merge_hints=[],
                type=Application, rebase_after=None, skip_nop_runs=False):
    disassembler = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    disassembler.detail = True

    rebase_address = 0
    if rebase_after:
        rebase_address = rebase_after.end_address()
    application = type(name, exe_path, rebase_address)

    for section in application.get_code_sections():
        current_address = application.get_address(section)
        while current_address < application.get_address_end(section):
            data_blob = ''
            jump_table = []

            current_address, skipped_code = realign(application, section, current_address, skip_nop_runs)
            for data_segment in data_segments:
                if data_segment[0] == current_address:
                    current_address = data_segment[1]
                    data_blob = application.read_section_data(section, data_segment[0], data_segment[1])

            x = application.get_dword(current_address)
            y = application.get_dword(current_address + 4)
            if application.is_code_address(x) and application.is_code_address(y):
                while application.is_code_address(x):
                    current_address += 4
                    x = application.get_dword(current_address)
            for data_segment in data_segments:
                if data_segment[0] == current_address:
                    current_address = data_segment[1]
                    data_blob = application.read_section_data(section, data_segment[0], data_segment[1])

            if current_address >= application.get_address_end(section):
                break

            current_address, skipped_code_2 = realign(application, section, current_address, skip_nop_runs)
            skipped_code = skipped_code_2 or skipped_code

            if current_address >= application.get_address_end(section):
                break

            subroutine = Subroutine(application, section, skipped_code, data_blob, jump_table)
            current_address = decode_subroutine(disassembler, application, section, current_address,
                                                split_instructions, subroutine)
            if subroutine.instructions:
                application.add_subroutine(subroutine)

    application.reduce_subroutines(merge_hints)
    application.split_subroutines(known_subroutines)

    return application
