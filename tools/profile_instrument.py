#!/usr/bin/env python3
"""Writes the NFS3_PROFILE copy of one generated source file (src/nfs3hp/disassembly/*.cpp).

Each guest function stores its address in the thread's profiler slot (include/platform/prof.h) when it starts and
again after every call it makes returns; an indirect call first stores its target. Lines stay 1:1 with the input,
so compiler messages point at the same line numbers. Fails on any call form it does not know.

Usage: profile_instrument.py <input.cpp> <output.cpp>
"""

import os
import re
import sys

FUNCTION = re.compile(r'^(?:static )?void (?:\w+::)*sub_([0-9a-f]+)(?:_guest)?\((?:win32::)?WinApplication\* app, x86::CPU& cpu\)$')
CALL = re.compile(r'^\s+(?:\w+::)*sub_[0-9a-f]+\(app, cpu\);$')
TAIL_CALL = re.compile(r'^\s+return (?:\w+::)*sub_[0-9a-f]+\(app, cpu\);$')
DYNAMIC = 'app->dynamic_call('
DYNAMIC_CALL = re.compile(r'^\s+app->dynamic_call\(.+, cpu\);$')
DYNAMIC_TAIL = re.compile(r'^\s+return app->dynamic_call\(.+, cpu\);$')
PROFILED_DYNAMIC = '::pf::prof::dynamicCall(app, '


def fail(path, number, message):
    sys.exit('%s:%d: %s' % (path, number, message))


def instrument(path, lines):
    output = []
    header = None    # address of a function whose opening brace comes next
    function = None  # address of the function being copied
    for number, line in enumerate(lines, 1):
        text = line.rstrip('\r\n')
        ending = line[len(text):]
        if header is not None:
            if text != '{':
                fail(path, number, 'expected "{" after the header of sub_%s' % header)
            function, header = header, None
            output.append('{ ::pf::prof::set(cpu, 0x%s);%s' % (function, ending))
            continue
        match = FUNCTION.match(text)
        if match:
            if function is not None:
                fail(path, number, 'function header inside sub_%s' % function)
            header = match.group(1)
            output.append(line)
            continue
        if text == '}':
            function = None
            output.append(line)
            continue
        calls = '(app, cpu)' in text or DYNAMIC in text
        if not calls or text.lstrip().startswith('//'):
            output.append(line)
            continue
        if function is None:
            fail(path, number, 'call outside a guest function')
        restore = ' ::pf::prof::set(cpu, 0x%s);' % function
        if CALL.match(text):
            output.append(text + restore + ending)
        elif TAIL_CALL.match(text):
            output.append(line)
        elif DYNAMIC_CALL.match(text):
            output.append(text.replace(DYNAMIC, PROFILED_DYNAMIC, 1) + restore + ending)
        elif DYNAMIC_TAIL.match(text):
            output.append(text.replace(DYNAMIC, PROFILED_DYNAMIC, 1) + ending)
        else:
            fail(path, number, 'unknown call form: %s' % text.strip())
    if header is not None or function is not None:
        fail(path, len(lines), 'unterminated function')
    return output


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    source, target = sys.argv[1], sys.argv[2]
    # latin-1 maps every byte to one character, so the copy is byte-exact outside the inserted text.
    with open(source, encoding='latin-1', newline='') as file:
        lines = file.readlines()
    output = instrument(source, lines)
    os.makedirs(os.path.dirname(os.path.abspath(target)), exist_ok=True)
    temporary = target + '.tmp'
    with open(temporary, 'w', encoding='latin-1', newline='') as file:
        file.writelines(output)
    os.replace(temporary, target)


if __name__ == '__main__':
    main()
