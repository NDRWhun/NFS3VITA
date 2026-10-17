"""x87 instructions on the in-memory register stack (cpu.fpu)."""

from capstone import x86 as X

from .ctx import T, S


def _st(ctx, op):
    name = ctx.ins.reg_name(op.reg)
    return 'cpu.fpu.st(%s)' % (name[-2] if name.endswith(')') else name[-1])


def fval(ctx, op):
    """Operand as an x86::Float rvalue."""
    if op.type == X.X86_OP_REG:
        return _st(ctx, op)
    if op.type == X.X86_OP_MEM:
        a = ctx.address(op)
        if op.size == 4:
            return 'x86::Float(rc::ldf32(%s))' % a
        if op.size == 8:
            return 'x86::Float(rc::ldf64(%s))' % a
        if op.size == 10:
            return 'x86::Float(app->getMemory<x86::IEEEf80>(%s))' % a
    raise AssertionError(ctx.ins.op_str)


def fstore(ctx, op, value):
    if op.type == X.X86_OP_REG:
        return ['%s = x86::Float(%s);' % (_st(ctx, op), value)]
    a = ctx.address(op)
    if op.size == 4:
        return ['rc::stf32(%s, float(%s));' % (a, value)]
    if op.size == 8:
        return ['rc::stf64(%s, double(%s));' % (a, value)]
    if op.size == 10:
        return ['app->getMemory<x86::IEEEf80>(%s) = x86::Float(%s);' % (a, value)]
    raise AssertionError(ctx.ins.op_str)


POP = 'cpu.fpu.pop();'


def cg_fninit(ctx):
    return ['cpu.fpu.init();'] + ctx.set_fc([])


def cg_fld(ctx, value):
    return ['cpu.fpu.push(%s);' % fval(ctx, value)]


def cg_fild(ctx, value):
    bits = value.size * 8
    return ['cpu.fpu.push(x86::Float(%s(%s)));' % (S(bits), ctx.val(value))]


def _const(value):
    return lambda ctx: ['cpu.fpu.push(%s);' % value]


cg_fldz = _const('0.0')
cg_fld1 = _const('1.0')
cg_fldpi = _const('3.1415926535897932')
cg_fldl2e = _const('1.4426950408889634')
cg_fldln2 = _const('0.6931471805599453')


def cg_fst(ctx, dst):
    return fstore(ctx, dst, 'cpu.fpu.st(0)')


def cg_fstp(ctx, dst):
    return cg_fst(ctx, dst) + [POP]


def cg_fist(ctx, dst):
    bits = dst.size * 8
    return ctx.put(dst, '%s(%s(cpu.fpu.rndint()))' % (T(bits), S(bits)))


def cg_fistp(ctx, dst):
    return cg_fist(ctx, dst) + [POP]


def cg_frndint(ctx):
    return ['cpu.fpu.st(0) = cpu.fpu.rndint();']


def _arith(op, reverse=False):
    def one_or_two(ctx, *args):
        if len(args) == 1:
            dst, src = 'cpu.fpu.st(0)', fval(ctx, args[0])
        else:
            dst, src = fval(ctx, args[0]), fval(ctx, args[1])
        if reverse:
            return ['%s = x86::Float(%s) %s x86::Float(%s);' % (dst, src, op, dst)]
        return ['%s %s= x86::Float(%s);' % (dst, op, src)]
    return one_or_two


def _arith_pop(op, reverse=False):
    def pop(ctx, *args):
        dst = fval(ctx, args[0]) if args else 'cpu.fpu.st(1)'
        if reverse:
            return ['%s = cpu.fpu.st(0) %s x86::Float(%s);' % (dst, op, dst), POP]
        return ['%s %s= cpu.fpu.st(0);' % (dst, op), POP]
    return pop


cg_fadd = _arith('+')
cg_fsub = _arith('-')
cg_fsubr = _arith('-', reverse=True)
cg_fmul = _arith('*')
cg_fdiv = _arith('/')
cg_fdivr = _arith('/', reverse=True)
cg_faddp = _arith_pop('+')
cg_fsubp = _arith_pop('-')
cg_fsubrp = _arith_pop('-', reverse=True)
cg_fmulp = _arith_pop('*')
cg_fdivp = _arith_pop('/')
cg_fdivrp = _arith_pop('/', reverse=True)


def cg_fxch(ctx, *args):
    other = fval(ctx, args[-1]) if args else 'cpu.fpu.st(1)'
    return ['{', '    const x86::Float t = cpu.fpu.st(0);', '    cpu.fpu.st(0) = %s;' % other, '    %s = t;' % other, '}']


def _unary(expr):
    return lambda ctx: ['cpu.fpu.st(0) = %s;' % expr]


cg_fchs = _unary('-cpu.fpu.st(0)')
cg_fabs = _unary('cpu.fpu.abs(cpu.fpu.st(0))')
cg_fsqrt = _unary('cpu.fpu.sqrt(cpu.fpu.st(0))')
cg_fsin = _unary('cpu.fpu.sin(cpu.fpu.st(0))')
cg_fcos = _unary('cpu.fpu.cos(cpu.fpu.st(0))')
cg_f2xm1 = _unary('cpu.fpu.f2xm1(cpu.fpu.st(0))')
cg_fscale = _unary('cpu.fpu.scale(cpu.fpu.st(0), cpu.fpu.st(1))')
cg_fprem = _unary('cpu.fpu.rem(cpu.fpu.st(0), cpu.fpu.st(1))')


def cg_fptan(ctx):
    return ['cpu.fpu.st(0) = cpu.fpu.tan(cpu.fpu.st(0));', 'cpu.fpu.push(1.0);']


def cg_fpatan(ctx):
    return ['cpu.fpu.st(1) = cpu.fpu.atan(cpu.fpu.st(0), cpu.fpu.st(1));', POP]


def cg_fyl2x(ctx):
    return ['cpu.fpu.st(1) = cpu.fpu.log2(cpu.fpu.st(0)) * cpu.fpu.st(1);', POP]


def _compare(ctx, other, pops):
    lines = ['const double fa = cpu.fpu.st(0);', 'const double fb = %s;' % other]
    sahf = ctx.fn.fused.get(ctx.index)
    if sahf is not None and (ctx.record or ctx.fn.fcmp_saved(sahf)):
        address = ctx.fn.instructions[sahf].address
        lines += ['lzfa_%x = fa;' % address, 'lzfb_%x = fb;' % address]
    lines += ctx.set_fc(['cpu.fpu.compare(fa, fb);'])
    lines += [POP] * pops
    return ['{'] + ['    ' + l for l in lines] + ['}']


def cg_fcom(ctx, *args):
    return _compare(ctx, fval(ctx, args[0]) if args else 'cpu.fpu.st(1)', 0)


def cg_fcomp(ctx, *args):
    return _compare(ctx, fval(ctx, args[0]) if args else 'cpu.fpu.st(1)', 1)


def cg_fcompp(ctx):
    return _compare(ctx, 'cpu.fpu.st(1)', 2)


def cg_ftst(ctx):
    return _compare(ctx, '0.0', 0)


def cg_fnstsw(ctx, dst):
    if ctx.fn.fused.get(ctx.index) is not None and not ctx.record and not ctx.fn.writes_ax(ctx.index):
        return []
    ctx.use_fc()
    return ctx.put(dst, 'cpu.fpu.status.word')


cg_fstsw = cg_fnstsw


def cg_fldcw(ctx, src):
    return ['cpu.fpu.control.word = %s;' % ctx.val(src)]


def cg_fnstcw(ctx, dst):
    return ctx.put(dst, 'cpu.fpu.control.word')


cg_fstcw = cg_fnstcw
