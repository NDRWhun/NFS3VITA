import os
from .module import Module
from . import emit

class Application(Module):
    def __init__(self, application_name, exe_path, rebase_after):
        Module.__init__(self, application_name, exe_path, rebase_after)

    def write(self, out_dir, program, dlls=[], function_names={}):
        with open(os.path.join(out_dir, '%s.cpp' % self.application_name), 'w', newline='\n') as src:
            with open(os.path.join(out_dir, '%s.h' % self.application_name), 'w', newline='\n') as h:
                app_name = self.application_name
                APP_NAME = self.application_name.upper()
                src.write('#include "%s.h"\n'
                          '#include <x86.h>\n'
                          '#include <winapi/wrapper.h>\n' % (app_name))
                for dll in dlls:
                    src.write('#include "%s.h"\n' % (dll.application_name))
                for dll_name, dll_methods in self.dll_imports:
                    src.write('#include <winapi/%s.h>\n' % dll_name.decode())
                for dll in dlls:
                    for dll_name, dll_methods in dll.dll_imports:
                        src.write('#include <winapi/%s.h>\n' % dll_name.decode())
                src.write('\nnamespace %s\n'
                            '{\n\n' % (app_name))
                h.write('#ifndef %s_H_\n'
                        '#define %s_H_\n'
                        '#include <x86.h>\n'
                        '#include <lib/winapp.h>\n'
                        '#include <lib/memmap.h>\n'
                        '#include <map>\n'  % (APP_NAME, APP_NAME))
                h.write('\nnamespace %s\n'
                        '{\n\n'
                        'class Application : public win32::WinApplication\n'
                        '{\n'
                        'public:\n'
                        '    Application(const char* appName);\n'
                        '    void execute();\n'
                        'private:\n' % (app_name))
                for data, address, length, name, ro, code in self.sections:
                    if data:
                        self._dump_raw_section(out_dir, name, data, length)
                        h.write('    static const x86::reg8 s_%sSegment[];\n' % name.decode())
                section_segments = ['{ %s, 0x%x, %d }' % (('s_%sSegment'%name.decode()) if data else 'nullptr',
                                                           address,
                                                           length) for data, address, length, name, ro, code in self.sections]
                for dll in dlls:
                    section_segments += ['{ %s, 0x%x, %d }' % (('%s::s_%sSegment'%(dll.application_name, name.decode())) if data else 'nullptr',
                                                                address,
                                                                length) for data, address, length, name, ro, code in dll.sections]
                sections = ', '.join(section_segments)
                src.write('Application::Application(const char* appName)\n'
                            '    :   WinApplication(appName, 0x%x, { %s })\n'
                            '{\n' % (self.sections[0][1], sections))
                for dll in dlls:
                    src.write('        %s::s_registry->registerSymbols(this);\n' % dll.application_name)
                for subroutine in self.subroutines:
                    function_entry = subroutine.get_entry_point()
                    name = function_names.get(function_entry, 'sub_%x' % function_entry)
                    src.write('    registerMethod(0x%08x, {"<Application>%s", &Application::%s});\n' % (function_entry, name, name))
                for dll in dlls:
                    for subroutine in dll.subroutines:
                        function_entry = subroutine.get_entry_point()
                        name = function_names.get(function_entry, 'sub_%x' % function_entry)
                        src.write('    registerMethod(0x%08x, {"<%s>%s", &%s::%s});\n' % (function_entry, dll.application_name, name, dll.application_name, name))
                for dll_name, dll_methods in self.dll_imports:
                    for method, address in dll_methods:
                        src.write('    registerMethod(0x%08x, {"<%s>%s", &win32::Wrapper<decltype(&win32::%s::%s), &win32::%s::%s>::stdcall});\n' % (address, dll_name.decode(), method.decode(), dll_name.decode(), method.decode(), dll_name.decode(), method.decode()))
                for dll in dlls:
                    for dll_name, dll_methods in dll.dll_imports:
                        for method, address in dll_methods:
                            src.write('    registerMethod(0x%08x, {"<%s>%s", &win32::Wrapper<decltype(&win32::%s::%s), &win32::%s::%s>::stdcall});\n' % (address, dll_name.decode(), method.decode(), dll_name.decode(), method.decode(), dll_name.decode(), method.decode()))

                src.write('}\n\n')
                src.write('void Application::execute()\n'
                            '{\n'
                            '    runThread(m_cpu, 0x%x);\n'
                            '}\n\n' % (self.entry_point))
                src.write('}\n')

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
                            name = function_names.get(function_entry, 'sub_%x' % function_entry)
                            emit.write_entry(methods, h, self, program, program.functions[function_entry], name,
                                             'Application::', '    static ', 'WinApplication', function_names)
                        methods.write('}\n')
                h.write('};\n\n'
                        '}\n\n'
                        '#endif /* !%s_H_ */\n' % (APP_NAME))
        for dll in dlls:
            dll.write(out_dir, program, function_names)