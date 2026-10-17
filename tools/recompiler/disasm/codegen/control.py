"""Jumps, calls and returns."""

from capstone import x86 as X


def _target(ctx, edge):
    """Statements transferring control to the target of the current jump."""
    kind = ctx.fn.branch[ctx.index]
    if kind[0] == 'label':
        return ['goto L_0x%08x;' % kind[1]]
    if kind[0] == 'tail':
        return ctx.exit_sync(edge) + ['return %s(app, cpu);' % ctx.function_name(kind[1])]
    op = ctx.ins.operands[0]
    if kind[0] == 'switch':
        return ['cpu.ip = %s;' % ctx.val(op, 32), 'goto dynamic_jump;']
    target = ctx.val(op, 32)
    ctx.boundary('dyncall')
    return ctx.exit_sync(edge) + ['return app->dynamic_call(%s, cpu);' % target]


def cg_jmp(ctx, *operands):
    return _target(ctx, 'exit')


def cg_jcc(ctx, cc):
    return ['if (%s)' % ctx.cond(cc), '{'] + ['    ' + l for l in _target(ctx, 'taken')] + ['}']


def cg_jcxz(ctx, *operands):
    return ['if (%s == 0)' % ctx.reg('cx'), '{'] + ['    ' + l for l in _target(ctx, 'taken')] + ['}']


def cg_jecxz(ctx, *operands):
    return ['if (%s == 0)' % ctx.reg('ecx'), '{'] + ['    ' + l for l in _target(ctx, 'taken')] + ['}']


def _loop(ctx, condition):
    """loop/loope/loopne: ecx is decremented without changing flags."""
    jump = ['if (%s != 0%s)' % (ctx.reg('ecx'), condition), '{'] + ['    ' + l for l in _target(ctx, 'taken')] + ['}']
    return ctx.set_reg('ecx', '%s - 1' % ctx.reg('ecx')) + jump


def cg_loop(ctx, *operands):
    return _loop(ctx, '')


def cg_loope(ctx, *operands):
    return _loop(ctx, ' && %s' % ctx.flag('zf'))


def cg_loopne(ctx, *operands):
    return _loop(ctx, ' && !%s' % ctx.flag('zf'))


def cg_call(ctx, destination):
    # The return address slot is not written: callees take their arguments relative to esp.
    if destination.type == X.X86_OP_IMM:
        ctx.boundary('call')
        call = ['%s(app, cpu);' % ctx.function_name(destination.imm), 'if (cpu.terminate) return;']
        return ctx.set_reg('esp', '%s - 4' % ctx.reg('esp')) + ctx.exit_sync('call') + call + ctx.after_call()
    ctx.boundary('dyncall')
    target = ['cpu.ip = %s;' % ctx.val(destination, 32)]
    call = ['app->dynamic_call(cpu.ip, cpu);', 'if (cpu.terminate) return;']
    return target + ctx.set_reg('esp', '%s - 4' % ctx.reg('esp')) + ctx.exit_sync('call') + call + ctx.after_call()


def cg_ret(ctx, *value):
    pop = 4 + (value[0].imm if value else 0)
    return ctx.set_reg('esp', '%s + %d' % (ctx.reg('esp'), pop)) + ctx.exit_sync('exit') + ['return;']
