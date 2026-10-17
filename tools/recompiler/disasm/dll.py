import os
from .module import Module
from . import emit


class DLL(Module):
    def __init__(self, application_name, exe_path, rebase_after):
        Module.__init__(self, application_name, exe_path, rebase_after)

    def write(self, out_dir, program, function_names={}):
        with open(os.path.join(out_dir, '%s.cpp' % self.application_name), 'w', newline='\n') as src:
            with open(os.path.join(out_dir, '%s.h' % self.application_name), 'w', newline='\n') as h:
                app_name = self.application_name
                APP_NAME = self.application_name.upper()

                src.write('#include "%s.h"\n'
                          '#include <x86.h>\n'
                          '#include <winapi/wrapper.h>\n'
                          '\nnamespace %s\n'
                          '{\n\n' % (app_name, app_name))
                h.write('#ifndef %s_H_\n'
                        '#define %s_H_\n'
                        '#include <x86.h>\n'
                        '#include <lib/winapp.h>\n'
                        '#include <lib/library.h>\n'
                        '\nnamespace %s\n'
                        '{\n\n'  % (APP_NAME, APP_NAME, app_name))
                for data, address, length, name, ro, code in self.sections:
                    if data:
                        self._dump_raw_section(out_dir, name, data, length)
                        h.write('extern const x86::reg8 s_%sSegment[];\n' % name.decode())
                for index in range(0, 1+int(len(self.subroutines)/100)):
                    with open(os.path.join(out_dir, '%s.%d.cpp' % (app_name, index)), 'w', newline='\n') as methods:
                        methods.write('#include "%s.h"\n'
                                      '#include <lib/thread.h>\n'
                                      '#include <recomp.h>\n'
                                      '#include <native.h>\n\n'
                                      'namespace %s\n'
                                      '{\n\n' % (app_name, app_name))
                        for subroutine in self.subroutines[100*index:100*(index+1)]:
                            function_entry = subroutine.get_entry_point()
                            emit.write_entry(methods, h, self, program, program.functions[function_entry],
                                             'sub_%x' % function_entry, '', '', 'win32::WinApplication', function_names)
                        methods.write('}\n')
                h.write('\nextern win32::Library* s_registry;\n'
                        '\n'
                        '}\n\n'
                        '#endif /* !%s_H_ */\n' % (APP_NAME))
                src.write('win32::Library s_library = win32::Library("%s.dll");\n'
                          'win32::Library* s_registry = &(s_library\n' % app_name)
                for address, name in self.exported_symbols:
                    src.write('        .registerSymbol("%s", &sub_%x)\n' % (name.decode(), address))
                src.write('\n    );\n}\n')

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
            src.write('const x86::reg8 s_%sSegment[] = {\n    ' % name)
            src.write(',\n    '.join(data_lines))
            src.write('\n};\n'
                      '}\n')