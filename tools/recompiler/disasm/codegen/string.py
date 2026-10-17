"""String instructions, with and without rep prefixes.

REP loops keep esi/edi/ecx in temporaries and write them back once; guest memory cannot alias the CPU state.
"""

from .ctx import T


def _segment(ctx, op):
    """Segment base of a string operand ('' when none)."""
    if op.mem.segment:
        return 'cpu.e%s + ' % ctx.ins.reg_name(op.mem.segment)
    return ''


def _step(size):
    return '(cpu.flags.df ? 0u - %du : %du)' % (size, size)


def _single(ctx, body, size, regs):
    lines = ['const x86::reg32 step = %s;' % _step(size)]
    lines += body
    for r in regs:
        lines += ctx.set_reg(r, '%s + step' % ctx.reg(r))
    return ['{'] + ['    ' + l for l in lines] + ['}']


def cg_movs(ctx, dst, src, prefix=None):
    bits = dst.size * 8
    load = ctx.load(bits, _segment(ctx, src) + 'si')
    store = ctx.store(bits, _segment(ctx, dst) + 'di', load)
    if prefix is None:
        return _single(ctx, ['const x86::reg32 si = %s;' % ctx.reg('esi'), 'const x86::reg32 di = %s;' % ctx.reg('edi')] + store,
                       dst.size, ('esi', 'edi'))
    # rep and repne both copy ecx elements: movs does not test ZF.
    lines = ['const x86::reg32 step = %s;' % _step(dst.size),
             'x86::reg32 si = %s;' % ctx.reg('esi'), 'x86::reg32 di = %s;' % ctx.reg('edi'),
             'for (x86::reg32 n = %s; n != 0; --n)' % ctx.reg('ecx'), '{'] + \
        ['    ' + l for l in store] + ['    si += step;', '    di += step;', '}']
    lines += ctx.set_reg('esi', 'si') + ctx.set_reg('edi', 'di') + ctx.set_reg('ecx', '0')
    return ['{'] + ['    ' + l for l in lines] + ['}']


def cg_stos(ctx, dst, value, prefix=None):
    bits = dst.size * 8
    store = ctx.store(bits, _segment(ctx, dst) + 'di', 'v')
    if prefix is None:
        return _single(ctx, ['const %s v = %s;' % (T(bits), ctx.val(value)), 'const x86::reg32 di = %s;' % ctx.reg('edi')] + store,
                       dst.size, ('edi',))
    lines = ['const x86::reg32 step = %s;' % _step(dst.size),
             'const %s v = %s;' % (T(bits), ctx.val(value)),
             'x86::reg32 di = %s;' % ctx.reg('edi'),
             'for (x86::reg32 n = %s; n != 0; --n)' % ctx.reg('ecx'), '{'] + \
        ['    ' + l for l in store] + ['    di += step;', '}']
    lines += ctx.set_reg('edi', 'di') + ctx.set_reg('ecx', '0')
    return ['{'] + ['    ' + l for l in lines] + ['}']


def cg_lods(ctx, value, src, prefix=None):
    if prefix is not None:
        raise AssertionError('rep lods')
    load = ctx.load(src.size * 8, _segment(ctx, src) + 'si')
    return _single(ctx, ['const x86::reg32 si = %s;' % ctx.reg('esi')] + ctx.put(value, load), src.size, ('esi',))


def _compare(ctx, left, right, size, prefix, regs):
    """scas/cmps: left and right are (load expression builder over the temporaries)."""
    bits = size * 8
    t = T(bits)
    if prefix is None:
        lines = ['const x86::reg32 step = %s;' % _step(size)]
        lines += ['const x86::reg32 %s = %s;' % (r[1:], ctx.reg(r)) for r in regs]
        lines += ['const %s a = %s;' % (t, left), 'const %s b = %s;' % (t, right)]
        lines += ctx.set_flags({}, kind='sub', bits=bits, operands={'a': 'a', 'b': 'b'})
        for r in regs:
            lines += ctx.set_reg(r, '%s + step' % r[1:])
        return ['{'] + ['    ' + l for l in lines] + ['}']
    stop = '==' if prefix == 'repne' else '!='
    body = ['const %s a = %s;' % (t, left), 'const %s b = %s;' % (t, right)]
    body += ctx.set_flags({}, kind='sub', bits=bits, operands={'a': 'a', 'b': 'b'}, may=True)
    body += ['%s += step;' % r[1:] for r in regs]
    body += ['--n;', 'if (a %s b)' % stop, '    break;']
    lines = ['const x86::reg32 step = %s;' % _step(size)]
    lines += ['x86::reg32 %s = %s;' % (r[1:], ctx.reg(r)) for r in regs]
    lines += ['x86::reg32 n = %s;' % ctx.reg('ecx'), 'while (n != 0)', '{'] + ['    ' + l for l in body] + ['}']
    for r in regs:
        lines += ctx.set_reg(r, r[1:])
    lines += ctx.set_reg('ecx', 'n')
    return ['{'] + ['    ' + l for l in lines] + ['}']


def cg_scas(ctx, value, dst, prefix=None):
    return _compare(ctx, ctx.val(value), ctx.load(dst.size * 8, _segment(ctx, dst) + 'di'), dst.size, prefix, ('edi',))


def cg_cmps(ctx, src, dst, prefix=None):
    return _compare(ctx, ctx.load(src.size * 8, _segment(ctx, src) + 'si'),
                    ctx.load(dst.size * 8, _segment(ctx, dst) + 'di'), src.size, prefix, ('esi', 'edi'))
