"""Integer, data movement and stack instructions."""

from capstone import x86 as X

from .ctx import T, S, lit


def block(lines):
    return ['{'] + ['    ' + l for l in lines] + ['}']


# ---- moves ----------------------------------------------------------------------------------------------------

def cg_mov(ctx, dst, src):
    bits = dst.size * 8
    if src.type == X.X86_OP_REG and src.size != dst.size:
        # Segment register moves: zero-extend or truncate.
        return ctx.put(dst, '%s(%s)' % (T(bits), ctx.val(src)))
    return ctx.put(dst, ctx.val(src, bits))


def cg_movzx(ctx, dst, src):
    return ctx.put(dst, '%s(%s)' % (T(dst.size * 8), ctx.val(src)))


def cg_movsx(ctx, dst, src):
    return ctx.put(dst, ctx.sval(src, dst.size * 8))


def cg_lea(ctx, dst, src):
    return ctx.put(dst, '%s(%s)' % (T(dst.size * 8), ctx.address(src)))


def cg_xchg(ctx, a, b):
    bits = a.size * 8
    sa, va, wa = ctx.bind(a, 'a')
    sb, vb, wb = ctx.bind(b, 'b')
    return block(sa + sb + ['const %s x = %s;' % (T(bits), va), 'const %s y = %s;' % (T(bits), vb)] + wa('y') + wb('x'))


def cg_cdq(ctx):
    return ctx.set_reg('edx', 'x86::reg32(x86::sreg32(%s) >> 31)' % ctx.reg('eax'))


def cg_cwde(ctx):
    return ctx.set_reg('eax', 'x86::reg32(x86::sreg16(%s))' % ctx.reg('ax'))


def cg_cbw(ctx):
    return ctx.set_reg('ax', 'x86::reg16(x86::sreg8(%s))' % ctx.reg('al'))


def cg_bswap(ctx, op):
    return ctx.put(op, '__builtin_bswap32(%s)' % ctx.val(op))


def cg_nop(ctx, *operands):
    return [';']


cg_wait = cg_nop
cg_fwait = cg_nop
cg_fclex = cg_nop
cg_fnclex = cg_nop


# ---- arithmetic -----------------------------------------------------------------------------------------------

def _binary(ctx, dst, src, kind, op, write=True):
    bits = dst.size * 8
    t = T(bits)
    setup, cur, writer = ctx.bind(dst)
    lines = setup + ['const %s a = %s;' % (t, cur), 'const %s b = %s;' % (t, ctx.sval(src, bits))]
    if kind in ('add', 'sub'):
        lines.append('const %s r = %s(a %s b);' % (t, t, op))
        lines += ctx.set_flags({}, kind=kind, bits=bits, operands={'a': 'a', 'b': 'b'})
    else:
        lines.append('const %s r = %s(a %s b);' % (t, t, op))
        lines += ctx.set_flags({}, kind='logic', bits=bits, operands={'r': 'r'})
    if write:
        lines += writer('r')
    return block(lines)


def cg_add(ctx, dst, src):
    return _binary(ctx, dst, src, 'add', '+')


def cg_sub(ctx, dst, src):
    return _binary(ctx, dst, src, 'sub', '-')


def cg_cmp(ctx, dst, src):
    return _binary(ctx, dst, src, 'sub', '-', write=False)


def cg_and(ctx, dst, src):
    return _binary(ctx, dst, src, 'logic', '&')


def cg_or(ctx, dst, src):
    return _binary(ctx, dst, src, 'logic', '|')


def cg_xor(ctx, dst, src):
    return _binary(ctx, dst, src, 'logic', '^')


def cg_test(ctx, dst, src):
    return _binary(ctx, dst, src, 'logic', '&', write=False)


def _carry(ctx, dst, src, subtract):
    bits = dst.size * 8
    t = T(bits)
    setup, cur, writer = ctx.bind(dst)
    op = '-' if subtract else '+'
    lines = setup + ['const %s a = %s;' % (t, cur), 'const %s b = %s;' % (t, ctx.sval(src, bits)),
                     'const bool c = %s;' % ctx.flag('cf'),
                     'const %s r = %s(a %s b %s %s(c));' % (t, t, op, op, t)]
    sign = lambda e: '(%s(%s(%s)) < 0)' % (S(bits), t, e)
    if subtract:
        flags = {'cf': '(c ? a <= b : a < b)', 'of': sign('(a ^ b) & (a ^ r)')}
    else:
        flags = {'cf': '(c ? r <= a : r < a)', 'of': sign('~(a ^ b) & (a ^ r)')}
    flags['zf'] = '(r == 0)'
    flags['sf'] = sign('r')
    lines += ctx.set_flags(flags)
    return block(lines + writer('r'))


def cg_adc(ctx, dst, src):
    return _carry(ctx, dst, src, False)


def cg_sbb(ctx, dst, src):
    return _carry(ctx, dst, src, True)


def _incdec(ctx, dst, kind):
    bits = dst.size * 8
    t = T(bits)
    setup, cur, writer = ctx.bind(dst)
    op = '+' if kind == 'inc' else '-'
    lines = setup + ['const %s r = %s(%s %s 1);' % (t, t, cur, op)]
    lines += ctx.set_flags({}, kind=kind, bits=bits, operands={'r': 'r'})
    return block(lines + writer('r'))


def cg_inc(ctx, dst):
    return _incdec(ctx, dst, 'inc')


def cg_dec(ctx, dst):
    return _incdec(ctx, dst, 'dec')


def cg_neg(ctx, dst):
    bits = dst.size * 8
    t = T(bits)
    setup, cur, writer = ctx.bind(dst)
    lines = setup + ['const %s a = %s;' % (t, cur)]
    lines += ctx.set_flags({}, kind='neg', bits=bits, operands={'a': 'a'})
    return block(lines + writer('%s(0u - a)' % t))


def cg_not(ctx, dst):
    bits = dst.size * 8
    setup, cur, writer = ctx.bind(dst)
    return block(setup + writer('%s(~%s)' % (T(bits), cur)))


def cg_mul(ctx, src):
    bits = src.size * 8
    if bits == 8:
        lines = ['const x86::reg16 r = x86::reg16(%s * %s);' % (ctx.reg('al'), ctx.val(src))]
        lines += ctx.set_reg('ax', 'r')
        hi = '(r >> 8)'
    elif bits == 16:
        lines = ['const x86::reg32 r = x86::reg32(%s) * %s;' % (ctx.reg('ax'), ctx.val(src))]
        lines += ctx.set_reg('ax', 'x86::reg16(r)') + ctx.set_reg('dx', 'x86::reg16(r >> 16)')
        hi = '(r >> 16)'
    else:
        lines = ['const x86::reg64 r = x86::reg64(%s) * %s;' % (ctx.reg('eax'), ctx.val(src))]
        lines += ctx.set_reg('eax', 'x86::reg32(r)') + ctx.set_reg('edx', 'x86::reg32(r >> 32)')
        hi = '(r >> 32)'
    lines += ctx.set_flags({'cf': '(%s != 0)' % hi, 'of': '(%s != 0)' % hi})
    return block(lines)


def cg_imul(ctx, *ops):
    if len(ops) == 1:
        src = ops[0]
        bits = src.size * 8
        if bits == 8:
            lines = ['const x86::sreg16 r = x86::sreg16(x86::sreg8(%s)) * x86::sreg8(%s);' % (ctx.reg('al'), ctx.val(src))]
            lines += ctx.set_reg('ax', 'x86::reg16(r)')
            over = '(r != x86::sreg8(r))'
        elif bits == 16:
            lines = ['const x86::sreg32 r = x86::sreg32(x86::sreg16(%s)) * x86::sreg16(%s);' % (ctx.reg('ax'), ctx.val(src))]
            lines += ctx.set_reg('ax', 'x86::reg16(r)') + ctx.set_reg('dx', 'x86::reg16(x86::reg32(r) >> 16)')
            over = '(r != x86::sreg16(r))'
        else:
            lines = ['const x86::sreg64 r = x86::sreg64(x86::sreg32(%s)) * x86::sreg32(%s);' % (ctx.reg('eax'), ctx.val(src))]
            lines += ctx.set_reg('eax', 'x86::reg32(r)') + ctx.set_reg('edx', 'x86::reg32(x86::reg64(r) >> 32)')
            over = '(r != x86::sreg32(r))'
        lines += ctx.set_flags({'cf': over, 'of': over})
        return block(lines)
    dst = ops[0]
    a, b = (ops[0], ops[1]) if len(ops) == 2 else (ops[1], ops[2])
    bits = dst.size * 8
    wide = S(bits * 2)
    lines = ['const %s r = %s(%s(%s)) * %s(%s(%s));' % (wide, wide, S(bits), ctx.val(a, bits), wide, S(bits), ctx.val(b, bits))]
    over = '(r != %s(r))' % S(bits)
    lines += ctx.set_flags({'cf': over, 'of': over})
    lines += ctx.put(dst, '%s(r)' % T(bits))
    return block(lines)


def cg_div(ctx, src):
    bits = src.size * 8
    if bits == 8:
        lines = ['const x86::reg16 n = %s;' % ctx.reg('ax'), 'const x86::reg8 d = %s;' % ctx.val(src)]
        lines += ctx.set_reg('al', 'x86::reg8(n / d)') + ctx.set_reg('ah', 'x86::reg8(n % d)')
    elif bits == 16:
        lines = ['const x86::reg32 n = (x86::reg32(%s) << 16) | %s;' % (ctx.reg('dx'), ctx.reg('ax')),
                 'const x86::reg16 d = %s;' % ctx.val(src)]
        lines += ctx.set_reg('ax', 'x86::reg16(n / d)') + ctx.set_reg('dx', 'x86::reg16(n % d)')
    else:
        lines = ['const x86::reg64 n = %s;' % ctx.edx_eax(), 'const x86::reg32 d = %s;' % ctx.val(src)]
        lines += ctx.set_reg('eax', 'x86::reg32(n / d)') + ctx.set_reg('edx', 'x86::reg32(n % d)')
    return block(lines)


def cg_idiv(ctx, src):
    bits = src.size * 8
    if bits == 8:
        lines = ['const x86::sreg16 n = x86::sreg16(%s);' % ctx.reg('ax'), 'const x86::sreg8 d = x86::sreg8(%s);' % ctx.val(src)]
        lines += ctx.set_reg('al', 'x86::reg8(n / d)') + ctx.set_reg('ah', 'x86::reg8(n % d)')
    elif bits == 16:
        lines = ['const x86::sreg32 n = x86::sreg32((x86::reg32(%s) << 16) | %s);' % (ctx.reg('dx'), ctx.reg('ax')),
                 'const x86::sreg16 d = x86::sreg16(%s);' % ctx.val(src)]
        lines += ctx.set_reg('ax', 'x86::reg16(n / d)') + ctx.set_reg('dx', 'x86::reg16(n % d)')
    else:
        lines = ['const x86::sreg64 n = x86::sreg64(%s);' % ctx.edx_eax(), 'const x86::sreg32 d = x86::sreg32(%s);' % ctx.val(src)]
        lines += ctx.set_reg('eax', 'x86::reg32(n / d)') + ctx.set_reg('edx', 'x86::reg32(n % d)')
    return block(lines)


# ---- shifts and rotates ---------------------------------------------------------------------------------------

def _count(ctx, op):
    """(C++ count expression, constant count or None)."""
    if op.type == X.X86_OP_IMM:
        return str(op.imm & 31), op.imm & 31
    return '(%s & 31)' % ctx.val(op, 8), None


def _shift(ctx, dst, cnt, kind):
    bits = dst.size * 8
    t, s = T(bits), S(bits)
    count, constant = _count(ctx, cnt)
    if constant == 0:
        return [';']
    setup, cur, writer = ctx.bind(dst)
    head = setup + ['const %s a = %s;' % (t, cur), 'const unsigned n = %s;' % count]
    msb = '(((r >> %d) & 1) != 0)' % (bits - 1)
    if kind == 'shl':
        body = ['const %s r = %s(x86::reg32(a) << n);' % (t, t)]
        cf = '(((x86::reg64(a) << n >> %d) & 1) != 0)' % bits
        of = '(%s != %s)' % (msb, cf)
    elif kind == 'shr':
        body = ['const %s r = %s(x86::reg32(a) >> n);' % (t, t)]
        cf = '(((x86::reg32(a) >> (n - 1)) & 1) != 0)'
        of = '(((a >> %d) & 1) != 0)' % (bits - 1)
    else:
        body = ['const %s r = %s(x86::sreg32(%s(a)) >> n);' % (t, t, s)]
        cf = '(((x86::sreg32(%s(a)) >> (n - 1)) & 1) != 0)' % s
        of = 'false'
    flags = {'cf': cf, 'zf': '(r == 0)', 'sf': '(%s(r) < 0)' % s}
    if constant is not None:
        if constant == 1:
            flags['of'] = of
        return block(head + body + ctx.set_flags(flags) + writer('r'))
    body += ctx.set_flags(flags, may=True)
    body += ['if (n == 1)', '{'] + ['    ' + l for l in ctx.set_flags({'of': of}, may=True)] + ['}']
    body += writer('r')
    return block(head + ['if (n != 0)', '{'] + ['    ' + l for l in body] + ['}'])


def cg_shl(ctx, dst, cnt):
    return _shift(ctx, dst, cnt, 'shl')


cg_sal = cg_shl


def cg_shr(ctx, dst, cnt):
    return _shift(ctx, dst, cnt, 'shr')


def cg_sar(ctx, dst, cnt):
    return _shift(ctx, dst, cnt, 'sar')


def _double_shift(ctx, dst, src, cnt, left):
    bits = dst.size * 8
    assert bits == 32, 'shld/shrd: only 32-bit operands are implemented'
    count, constant = _count(ctx, cnt)
    if constant == 0:
        return [';']
    setup, cur, writer = ctx.bind(dst)
    lines = setup + ['const x86::reg32 a = %s;' % cur, 'const x86::reg32 b = %s;' % ctx.val(src),
                     'const unsigned n = %s;' % count]
    if left:
        body = ['const x86::reg32 r = x86::reg32(((x86::reg64(a) << 32 | b) << n) >> 32);']
        cf = '(((a >> (32 - n)) & 1) != 0)'
    else:
        body = ['const x86::reg32 r = x86::reg32((x86::reg64(b) << 32 | a) >> n);']
        cf = '(((a >> (n - 1)) & 1) != 0)'
    flags = {'cf': cf, 'zf': '(r == 0)', 'sf': '(x86::sreg32(r) < 0)'}
    of = '(((r ^ a) >> 31) != 0)'
    if constant is not None:
        if constant == 1:
            flags['of'] = of
        return block(lines + body + ctx.set_flags(flags) + writer('r'))
    body += ctx.set_flags(flags, may=True)
    body += ['if (n == 1)', '{'] + ['    ' + l for l in ctx.set_flags({'of': of}, may=True)] + ['}']
    body += writer('r')
    return block(lines + ['if (n != 0)', '{'] + ['    ' + l for l in body] + ['}'])


def cg_shld(ctx, dst, src, cnt):
    return _double_shift(ctx, dst, src, cnt, True)


def cg_shrd(ctx, dst, src, cnt):
    return _double_shift(ctx, dst, src, cnt, False)


def _rotate(ctx, dst, cnt, left):
    bits = dst.size * 8
    t = T(bits)
    count, constant = _count(ctx, cnt)
    if constant is not None:
        constant %= bits
        if constant == 0:
            return [';']
        count = str(constant)
    else:
        count = '(%s %% %d)' % (count, bits)
    setup, cur, writer = ctx.bind(dst)
    lines = setup + ['const %s a = %s;' % (t, cur), 'const unsigned n = %s;' % count]
    if left:
        body = ['const %s r = %s((a << n) | (a >> (%d - n)));' % (t, t, bits)]
        cf = '((r & 1) != 0)'
        of = '((((r >> %d) ^ r) & 1) != 0)' % (bits - 1)
    else:
        body = ['const %s r = %s((a >> n) | (a << (%d - n)));' % (t, t, bits)]
        cf = '(((r >> %d) & 1) != 0)' % (bits - 1)
        of = '((((r >> %d) ^ (r >> %d)) & 1) != 0)' % (bits - 1, bits - 2)
    if constant is not None:
        flags = {'cf': cf}
        if constant == 1:
            flags['of'] = of
        return block(lines + body + ctx.set_flags(flags) + writer('r'))
    body += ctx.set_flags({'cf': cf}, may=True)
    body += ['if (n == 1)', '{'] + ['    ' + l for l in ctx.set_flags({'of': of}, may=True)] + ['}']
    body += writer('r')
    return block(lines + ['if (n != 0)', '{'] + ['    ' + l for l in body] + ['}'])


def cg_rol(ctx, dst, cnt):
    return _rotate(ctx, dst, cnt, True)


def cg_ror(ctx, dst, cnt):
    return _rotate(ctx, dst, cnt, False)


def cg_rcr(ctx, dst, cnt):
    bits = dst.size * 8
    t = T(bits)
    count, constant = _count(ctx, cnt)
    period = bits + 1
    if constant is not None:
        constant %= period
        if constant == 0:
            return [';']
    setup, cur, writer = ctx.bind(dst)
    lines = setup + ['%s v = %s;' % (t, cur), 'bool c = %s;' % ctx.flag('cf'),
                     'const bool o = (((v >> %d) & 1) != 0) != c;' % (bits - 1)]
    loop = ['for (unsigned n = %s; n != 0; --n)' % (str(constant) if constant is not None else '(%s %% %d)' % (count, period)),
            '{',
            '    const bool low = (v & 1) != 0;',
            '    v = %s((v >> 1) | (%s(c) << %d));' % (t, t, bits - 1),
            '    c = low;',
            '}']
    if constant is not None:
        flags = {'cf': 'c'}
        if constant == 1:
            flags['of'] = 'o'
        return block(lines + loop + ctx.set_flags(flags) + writer('v'))
    body = ctx.set_flags({'cf': 'c'}, may=True)
    body += ['if (%s == 1)' % count, '{'] + ['    ' + l for l in ctx.set_flags({'of': 'o'}, may=True)] + ['}']
    return block(lines + loop + ['if (%s %% %d != 0)' % (count, period), '{'] + ['    ' + l for l in body + writer('v')] + ['}'])


# ---- bit scans ------------------------------------------------------------------------------------------------

def cg_bsf(ctx, dst, src):
    lines = ['const x86::reg32 s = %s;' % ctx.val(src, 32)]
    lines += ctx.set_flags({'zf': '(s == 0)'})
    lines += ['if (s != 0)', '{'] + ['    ' + l for l in ctx.put(dst, '%s(__builtin_ctz(s))' % T(dst.size * 8))] + ['}']
    return block(lines)


def cg_bsr(ctx, dst, src):
    lines = ['const x86::reg32 s = %s;' % ctx.val(src, 32)]
    lines += ctx.set_flags({'zf': '(s == 0)'})
    lines += ['if (s != 0)', '{'] + ['    ' + l for l in ctx.put(dst, '%s(31 - __builtin_clz(s))' % T(dst.size * 8))] + ['}']
    return block(lines)


# ---- flag instructions ----------------------------------------------------------------------------------------

def cg_clc(ctx):
    return ctx.set_flags({'cf': 'false'})


def cg_cmc(ctx):
    return block(['const bool c = %s;' % ctx.flag('cf')] + ctx.set_flags({'cf': '!c'}))


def cg_cld(ctx):
    return ['cpu.flags.df = 0;']


def cg_std(ctx):
    return ['cpu.flags.df = 1;']


def cg_sahf(ctx):
    if ctx.fn.fused.get(ctx.index) == ctx.index:
        address = ctx.ins.address
        values = {'fa': 'lzfa_%x' % address, 'fb': 'lzfb_%x' % address}
        return ctx.set_flags({'sf': '(cpu.fpu.status.b != 0)', 'af': '(((cpu.fpu.status.word >> 12) & 1) != 0)'},
                             kind='fcmp', bits=32, operands=values)
    ah = ctx.reg('ah')
    return block(['const x86::reg8 h = %s;' % ah] + ctx.set_flags({
        'cf': '((h & 0x01) != 0)', 'pf': '((h & 0x04) != 0)', 'af': '((h & 0x10) != 0)',
        'zf': '((h & 0x40) != 0)', 'sf': '((h & 0x80) != 0)'}))


def cg_lahf(ctx):
    value = 'x86::reg8(0x02 | %s | %s << 2 | %s << 4 | %s << 6 | %s << 7)' % tuple(
        'x86::reg8(%s)' % ctx.flag(f) for f in ('cf', 'pf', 'af', 'zf', 'sf'))
    return ctx.set_reg('ah', value)


def cg_pushfd(ctx):
    return ctx.sync_flags_to_cpu() + block(['const x86::reg32 s = %s - 4;' % ctx.reg('esp')] +
                                                  ctx.store(32, 's', 'cpu.flags.eflags') + ctx.set_reg('esp', 's'))


def cg_popfd(ctx):
    return ['cpu.flags.eflags = %s;' % ctx.load(32, ctx.reg('esp'))] + \
        ctx.set_reg('esp', '%s + 4' % ctx.reg('esp')) + ctx.load_flags_from_cpu()


def cg_pushf(ctx):
    return ctx.sync_flags_to_cpu() + block(['const x86::reg32 s = %s - 4;' % ctx.reg('esp')] +
                                                  ctx.store(16, 's', 'cpu.flags.word') + ctx.set_reg('esp', 's'))


def cg_popf(ctx):
    return ['cpu.flags.word = %s;' % ctx.load(16, ctx.reg('esp'))] + \
        ctx.set_reg('esp', '%s + 4' % ctx.reg('esp')) + ctx.load_flags_from_cpu()


def cg_setcc(ctx, dst, cc):
    return ctx.put(dst, 'x86::reg8(%s)' % ctx.cond(cc))


# ---- stack ----------------------------------------------------------------------------------------------------

def cg_push(ctx, value):
    bits = value.size * 8
    return block(['const %s v = %s;' % (T(bits), ctx.val(value)),
                  'const x86::reg32 s = %s - 4;' % ctx.reg('esp')] + ctx.store(bits, 's', 'v') + ctx.set_reg('esp', 's'))


def cg_pop(ctx, dst):
    bits = dst.size * 8
    return block(['const %s v = %s;' % (T(bits), ctx.load(bits, ctx.reg('esp')))] +
                 ctx.set_reg('esp', '%s + 4' % ctx.reg('esp')) + ctx.put(dst, 'v'))


def cg_pushal(ctx):
    regs = ('eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi')
    lines = ['const x86::reg32 s = %s;' % ctx.reg('esp')]
    for i, r in enumerate(regs):
        lines += ctx.store(32, 's - %d' % (4 * (i + 1)), ctx.reg(r))
    return block(lines + ctx.set_reg('esp', 's - 32'))


def cg_popal(ctx):
    lines = ['const x86::reg32 s = %s;' % ctx.reg('esp')]
    for offset, r in ((0, 'edi'), (4, 'esi'), (8, 'ebp'), (16, 'ebx'), (20, 'edx'), (24, 'ecx'), (28, 'eax')):
        lines += ctx.set_reg(r, ctx.load(32, 's + %d' % offset))
    return block(lines + ctx.set_reg('esp', 's + 32'))


def cg_enter(ctx, alloc, nesting):
    level = nesting.imm & 31
    lines = ['x86::reg32 s = %s - 4;' % ctx.reg('esp'), 'x86::reg32 bp = %s;' % ctx.reg('ebp')]
    lines += ctx.store(32, 's', 'bp')
    lines += ['const x86::reg32 frame = s;']
    for _ in range(max(level - 1, 0)):
        lines += ['s -= 4;', 'bp -= 4;'] + ctx.store(32, 's', ctx.load(32, 'bp'))
    if level:
        lines += ['s -= 4;'] + ctx.store(32, 's', 'frame')
    lines += ctx.set_reg('ebp', 'frame') + ctx.set_reg('esp', 's - %s' % lit(alloc.imm, 32))
    return block(lines)


def cg_leave(ctx):
    return block(['const x86::reg32 s = %s;' % ctx.reg('ebp')] + ctx.set_reg('ebp', ctx.load(32, 's')) +
                 ctx.set_reg('esp', 's + 4'))


# ---- runtime --------------------------------------------------------------------------------------------------

def _runtime(ctx, call, reads, writes):
    """A CPU helper working on cpu: the registers it reads are stored first, the ones it writes reloaded."""
    ctx.boundary('runtime')
    lines = []
    for r in reads:
        value = ctx.reg(r)
        if ctx.fn.local_registers:
            lines.append('cpu.%s = %s;' % (r, value))
    lines.append(call)
    for r in writes:
        if ctx.fn.local_registers:
            lines += ctx.set_reg(r, 'cpu.%s' % r)
        else:
            ctx.set_reg(r, '0')
    return lines


def cg_cpuid(ctx):
    return _runtime(ctx, 'cpu.cpuid();', ('eax', 'ecx'), ('eax', 'ecx', 'edx', 'ebx'))


def cg_rdtsc(ctx):
    return _runtime(ctx, 'cpu.rdtsc();', (), ('eax', 'edx'))


def cg_int3(ctx):
    ctx.boundary('trap')
    return ['NFS2_ASSERT(false);']
