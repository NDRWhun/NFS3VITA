import os
from . import pefile
from .subroutine import Subroutine
import ordlookup


# Fake addresses of imported functions, one per import across all modules: the import thunks are patched to point
# there and the runtime registers its shims at them. They must stay in nfs3.exe's header page, below its code.
address = 0x400a00
FAKE_IMPORT_END = 0x401000


def _do_import(section, index, offset, base):
    global address
    # import directory
    def read_name(o):
        end = o
        while section[end] != 0: end += 1
        return section[o:end]
    def read_dword(o):
        return section[o]           + \
                section[o+1] * 2**8  + \
                section[o+2] * 2**16 + \
                section[o+3] * 2**24
    def write_dword(o, dword):
        return section[:o]               \
                + bytes([dword & 0xff, (dword >> 8) & 0xff, (dword >> 16) & 0xff, (dword >> 24) & 0xff]) \
                + section[o+4:]

    dlls = []
    while True:
        table_offset = read_dword(index + 0)
        dll_name_offset = read_dword(index + 12)
        thunk_table = read_dword(index + 16)
        index += 20
        if table_offset == 0:
            break
        dll_name_offset -= offset
        dll_name = read_name(dll_name_offset)[:-4].lower()
        table_offset -= offset
        thunk_table -= offset
        methods = []
        characteristics = read_dword(table_offset)
        while characteristics:
            if address >= FAKE_IMPORT_END:
                raise AssertionError('fake import addresses reach 0x%x' % FAKE_IMPORT_END)
            if characteristics & 0x80000000:
                funcname = ordlookup.ordLookup(dll_name + b'.dll', characteristics & 0xffff)
                print('%d->%s' % (characteristics & 0xffff, funcname))
                methods.append((funcname, address))
            else:
                name = read_name(characteristics - offset + 2)
                methods.append((name, address))
                print('0x%x -> %s' % (offset + thunk_table + base, name))
            section = write_dword(thunk_table, address)
            address += 1
            table_offset += 4
            thunk_table += 4
            characteristics = read_dword(table_offset)
        dlls.append((dll_name, methods))
    return dlls, section


def _load_exe(path, rebase_address):
    nfs2exe = pefile.PE(path)
    if rebase_address:
        nfs2exe.relocate_image(rebase_address)
    import_data = nfs2exe.OPTIONAL_HEADER.DATA_DIRECTORY[1]
    image_base = rebase_address or nfs2exe.OPTIONAL_HEADER.ImageBase
    sections = []
    for s in nfs2exe.sections:
        if s.PointerToRawData:
            data = s.get_data()
            if s.contains_rva(import_data.VirtualAddress):
                dlls, data = _do_import(data, import_data.VirtualAddress-s.VirtualAddress, s.VirtualAddress, image_base)
            if s.Characteristics & 0x02000000:
                print('Discarding %s' % s.Name.rstrip(b'\x00'))
                continue
            read_only = 0 == s.Characteristics & 0x80000000
            code = 0 != s.Characteristics & 0x20000000
            print(s.Name.rstrip(b'\x00'), 'RO:', read_only,)
            print(s.Name.rstrip(b'\x00'), 'EXE:', code,)
            sections.append((data, s.VirtualAddress + image_base, len(data), s.Name.rstrip(b'\x00').replace(b'.', b'_'), read_only, code))
        else:
            sections.append((None, s.VirtualAddress + image_base, s.SizeOfRawData, s.Name.rstrip(b'\x00').replace(b'.', b'_'), False, False))
    entry_point = nfs2exe.OPTIONAL_HEADER.AddressOfEntryPoint + image_base
    exported_symbols = [(image_base+h['RVA'],h['Name']) for h in nfs2exe.dump_dict().get('Exported symbols', [])[1:]]
    return (sections, entry_point, dlls, exported_symbols, image_base)


def _reduce(subroutines, found_subroutines, merge_hints):
    index = 0
    result = subroutines[:]
    while index < len(result)-1:
        subroutine = result[index]
        subroutine2 = result[index+1]
        for m1, m2 in merge_hints:
            if subroutine.instructions[0].address == m1 and subroutine2.instructions[0].address == m2:
                result[index] = Subroutine.merge(subroutine, subroutine2)
                del result[index+1]
                break
        else:
            if subroutine2.instructions[0].address in found_subroutines:
                # call leading there, so can't merge with subroutine
                index += 1
            elif subroutine2.skipped_blob:
                # seems to have a skip alignment prefix, so should be legit call
                # TODO: catch?
                index += 1
            elif subroutine2.data_blob or subroutine2.jump_table:
                # seems to have a data prefix, so should be legit call
                # TODO: catch?
                index += 1
            else:
                for inst, label in subroutine.static_labels + subroutine.dynamic_labels:
                    if label in subroutine2:
                        result[index] = Subroutine.merge(subroutine, subroutine2)
                        del result[index+1]
                        break
                else:
                    for inst, label in subroutine2.static_labels + subroutine2.dynamic_labels:
                        if label in subroutine:
                            result[index] = Subroutine.merge(subroutine, subroutine2)
                            del result[index+1]
                            break
                    else:
                        index += 1
    return result


class Module:
    def __init__(self, application_name, exe_path, rebase_address):
        self.application_name = application_name
        loaded = _load_exe(exe_path, rebase_address)
        self.sections, self.entry_point, self.dll_imports, self.exported_symbols, self.base_address = loaded
        self.subroutines = []
        for data, address, length, name, ro, code in self.sections:
            print('%s section 0x%08x - 0x%08x' % (name, address, address + length))
        print('Entry point: 0x%08x' % (self.entry_point))

    def _dump_raw_section(self, out_dir, name, section, length):
        name = name.decode()
        with open(os.path.join(out_dir, '%s.%s.cpp' % (self.application_name, name)), 'w', newline='\n') as src:
            data_lines = []
            for i in range(0, int((length+15)/16)):
                sub_data = section[i*16:(i+1)*16]
                data_lines.append(', '.join(['0x%02x'%d for d in sub_data]))
            src.write('#include "%s.h"\n\n'
                        'namespace %s\n'
                        '{\n\n' % (self.application_name, self.application_name))
            src.write('const x86::reg8 Application::s_%sSegment[] = {\n    ' % name)
            src.write(',\n    '.join(data_lines))
            src.write('\n};\n'
                        '}\n')

    def end_address(self):
        """End of the highest loaded section; the next module is rebased there."""
        return self.sections[-1][1] + self.sections[-1][2]

    def get_code_sections(self):
        return [s for s in self.sections if s[5]]

    def get_address(self, section):
        return section[1]

    def get_address_end(self, section):
        return section[1] + section[2]

    def add_subroutine(self, subroutine):
        self.subroutines.append(subroutine)

    def read_section_data(self, section, start, end=None):
        section_data, address, length, name, ro, code = section
        if end:
            return section_data[start-address : end-address]
        else:
            return section_data[start-address : ]

    def is_section_address(self, address):
        for section_data, section_address, section_length, section_name, ro, code in self.sections:
            if address >= section_address and address < section_address + section_length:
                return True

    def is_code_address(self, address):
        if address is None:
            return False
        for s in self.sections:
            if s[5] and s[1] <= address < s[1] + s[2]:
                return True
        return False

    def get_dword(self, address):
        """Dword at address (bytes past the end of the section read as 0), or None outside every section."""
        for section_data, section_address, section_length, section_name, ro, code in self.sections:
            if address >= section_address and address < section_address + section_length:
                if section_data:
                    offset = address - section_address
                    return int.from_bytes(section_data[offset:offset + 4].ljust(4, b'\0'), 'little')
                else:
                    return 0

    def split_subroutines(self, known_subroutines):
        addresses = { }
        splits = set([])
        split_origin = { }
        for s in self.subroutines:
            addresses[s.get_entry_point()] = s
        for s in known_subroutines:
            if s not in addresses:
                splits.add(s)
        for s in self.subroutines:
            for c_address, c_dest in s.calls:
                if c_dest not in addresses:
                    if c_dest not in splits:
                        splits.add(c_dest)
                        split_origin[c_dest] = s
            for c_address, c_dest in s.dynamic_labels + s.static_labels:
                if c_dest not in s:
                    if c_dest not in addresses:
                        if c_dest not in splits:
                            splits.add(c_dest)
                            split_origin[c_dest] = s
        for split in splits:
            for index, s in enumerate(self.subroutines):
                if split in s:
                    m1, m2 = s.split(split)
                    self.subroutines[index] = m2
                    self.subroutines.insert(index, m1)
                    break
            else:
                print('oops: unknown split 0x%08x from 0x%08x' % (split, split_origin[split].instructions[0].address))

    def reduce_subroutines(self, merge_hints):
        print(merge_hints)
        print('reducing subroutines')
        found_subroutines = []
        for subroutine in self.subroutines:
            for call_address, call_dest in subroutine.calls:
                if call_dest not in found_subroutines:
                    found_subroutines.append(call_dest)
        reduced_subroutines = _reduce(self.subroutines, found_subroutines, merge_hints)
        while len(reduced_subroutines) != len(self.subroutines):
            self.subroutines = reduced_subroutines
            reduced_subroutines = _reduce(self.subroutines, found_subroutines, merge_hints)
