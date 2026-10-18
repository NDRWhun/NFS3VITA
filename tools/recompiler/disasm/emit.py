"""Writes one recompiled function as C++."""

import re

from . import codegen
from .codegen import ctx as C


def raw(module, fn, ins):
    if not ins.size:
        return ins.__class__.__name__
    data = module.read_section_data(fn.sub.section, ins.address, ins.address + ins.size)
    return '%08x  %s%s %s %s' % (ins.address, ''.join('%02x' % c for c in data), ' ' * (23 - ins.size * 2),
                                 ins.mnemonic, ins.op_str)


def function_body(module, fn, function_names):
    """Body lines of the function after its local declarations."""
    ctx = C.Ctx(fn, function_names, record=False)
    sub = fn.sub
    lines = []
    if sub.dynamic_labels:
        lines += ['  goto start;', 'dynamic_jump:', '  switch(cpu.ip)', '  {', 'start:']
    if fn.entry != 0:
        lines.append('    goto L_entry_0x%08x;' % fn.entry_address)
    fallthrough = False
    for i, ins in enumerate(fn.instructions):
        if ins.address in fn.dynamic_targets:
            if fallthrough:
                lines.append('  [[fallthrough]];')
            lines.append('  case 0x%08x:' % ins.address)
        if ins.address in fn.static_targets:
            lines.append('L_0x%08x:' % ins.address)
        if fn.entry != 0 and i == fn.entry:
            lines.append('L_entry_0x%08x:' % ins.address)
        if ins.address in fn.thread_segments:
            lines += ['    app->unlockContext(cpu);', '    win32::Thread::sleep(0);', '    app->lockContext(cpu);']
        lines.append('    // %s' % raw(module, fn, ins))
        if fn.branch[i][0] == 'skip':
            lines.append('    // skipped')
            code = []
        else:
            ctx.begin(i)
            code = codegen.generate(ctx, ins)
        lines += ['    ' + l for l in code]
        fallthrough = ins.mnemonic not in ('jmp', 'ret')
    last = fn.n - 1
    if fn.branch[last][0] not in ('ret',) and not (fn.branch[last][0] in ('label', 'tail', 'dyntail', 'switch')
                                                   and fn.instructions[last].mnemonic == 'jmp'):
        ctx.begin(last)
        ctx.fdepth = fn.fpu_exit_depth(last)
        lines += ['    ' + l for l in ctx.exit_sync('exit')]
    if sub.dynamic_labels:
        lines += ['  default:', '    NFS2_ASSERT(false);', '  }']
    return lines


def declarations(fn, body):
    """Locals used by the body: flags (loaded when live on entry) and saved operands of lazy producers."""
    text = '\n'.join(body)
    used = set(re.findall(r'\b(f_[a-z]{2}|lz[a-z]+_[0-9a-f]+)\b', text))
    lines = []
    if fn.local_registers:
        words = set(re.findall(r'\b(e[a-d]x|e[sd]i|e[sb]p)\b', text))
        for r in C.REGS:
            if r in words:
                lines.append('  [[maybe_unused]] x86::reg32 %s = cpu.%s;' % (r, r))
    if re.search(r'\bfpu_count\b', text):
        lines.append('  const x86::reg32 fpu_count = cpu.fpu.count;')
    for n in sorted(set(int(x) for x in re.findall(r'\bfp(\d+)\b', text))):
        lines.append('  [[maybe_unused]] x86::Float fp%d = 0;' % n)
    entry = fn.entry_loads()
    for f in C.CPU_FLAGS:
        name = 'f_' + f
        if name in used:
            value = 'cpu.flags.%s' % f if f in entry else 'false'
            lines.append('  bool %s = %s;' % (name, value))
    for type_, name in fn.lazy_locals():
        if name in used:
            lines.append('  %s %s = 0;' % (type_, name))
    return lines


def write_entry(out, header, module, program, fn, name, qualifier, declare, app_type, function_names):
    """Declares and writes one guest function. With a native override the translated body becomes <name>_guest,
    and the entry calls native::<name> through NFS3_NATIVE (include/native.h)."""
    parameters = '(%s* app, x86::CPU& cpu)' % app_type
    header.write('%svoid %s%s;\n' % (declare, name, parameters))
    if fn.entry_address in program.native_overrides:
        header.write('%svoid %s_guest%s;\n' % (declare, name, parameters))
        out.write('namespace native { void %s(win32::WinApplication* app, x86::CPU& cpu); }\n' % name)
        out.write('void %s%s%s\n{\n    NFS3_NATIVE(0x%x, native::%s, %s_guest);\n}\n\n'
                  % (qualifier, name, parameters, fn.entry_address, name, name))
        name += '_guest'
    write_function(out, module, fn, 'void %s%s%s' % (qualifier, name, parameters), function_names)


def write_function(out, module, fn, signature, function_names):
    sub = fn.sub
    out.write('/* align: skip %s */\n' % (' '.join(['0x%02x' % c for c in sub.skipped_blob])))
    if sub.data_blob:
        out.write('/* data blob: %s */\n' % (''.join(['%02x' % c for c in sub.data_blob])))
    for jump_entry in sub.jump_table:
        out.write('/* jump table: 0x%08x */\n' % jump_entry)
    body = function_body(module, fn, function_names)
    out.write('%s\n{\n  NFS2_USE(cpu);\n  NFS2_USE(app);\n' % signature)
    for line in declarations(fn, body) + body:
        out.write(line + '\n')
    out.write('}\n\n')
