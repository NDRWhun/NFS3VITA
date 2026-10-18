"""x87 stack depth relative to function entry.

When every x87 instruction of a function runs at a statically known depth, the registers it pushes live in C++
locals (fp1, fp2, ...; fpN is the N-th value above the entry stack) and the entry stack stays in cpu.fpu. Calls
need the callee's net push count (fpu delta); a function whose depth is not known everywhere keeps the in-memory
stack (cpu.fpu.push/pop/st).
"""

PUSH = {'fld', 'fild', 'fld1', 'fldz', 'fldpi', 'fldl2e', 'fldln2', 'fptan'}
POP = {'fstp', 'fistp', 'faddp', 'fsubp', 'fsubrp', 'fmulp', 'fdivp', 'fdivrp', 'fcomp', 'fpatan', 'fyl2x'}
POP2 = {'fcompp'}
RESET = {'fninit', 'finit', 'frstor', 'fnsave', 'fsave', 'fldenv', 'fnstenv', 'fstenv', 'ffree', 'fdecstp',
         'fincstp'}


NEVER = 'never'     # delta of a function that never returns


def effect(mnemonic):
    if mnemonic in PUSH:
        return 1
    if mnemonic in POP:
        return -1
    if mnemonic in POP2:
        return -2
    return 0


def is_x87(mnemonic):
    return mnemonic.startswith('f') and mnemonic not in ('fwait',) or mnemonic == 'wait'


class Depths:
    def __init__(self):
        self.depth = None       # per instruction, depth before it (None: unknown)
        self.local = False      # the function keeps pushed values in locals
        self.delta = None       # net pushes from entry to return, when every path agrees


def analyze_function(fn, deltas, indirect_delta):
    n = fn.n
    depth = [None] * n
    known = [False] * n
    conflict = [False] * n
    depth[fn.entry] = 0
    known[fn.entry] = True
    work = [fn.entry]
    returns = set()
    reset = False
    while work:
        i = work.pop()
        d = None if conflict[i] else depth[i]
        mnemonic = fn.instructions[i].mnemonic.split()[-1]
        if mnemonic in RESET and fn.effects[i].boundary != 'trap':
            reset = True
        after = d
        kind = fn.branch[i][0]
        if kind == 'call' and deltas.get(fn.branch[i][1]) == NEVER:
            continue
        if d is not None:
            if kind == 'call':
                delta = deltas.get(fn.branch[i][1])
                after = d + delta if delta is not None else None
            elif kind == 'dyncall':
                after = d + indirect_delta if indirect_delta is not None else None
            else:
                after = d + effect(mnemonic)
        for x in fn.exits[i]:
            if x.edge == 'call':
                continue
            if after is None:
                returns.add(None)
            elif x.kind == 'ret':
                returns.add(after)
            elif x.kind == 'tail':
                delta = deltas.get(x.target)
                if delta != NEVER:
                    returns.add(after + delta if delta is not None else None)
            else:
                returns.add(after + indirect_delta if indirect_delta is not None else None)
        for s in fn.succ[i]:
            if not known[s]:
                known[s] = True
                depth[s] = after
                work.append(s)
            elif not conflict[s] and depth[s] != after:
                conflict[s] = True
                depth[s] = None
                work.append(s)
    result = Depths()
    result.depth = [None if conflict[i] or not known[i] else depth[i] for i in range(n)]
    if len(returns) == 1:
        (result.delta,) = returns
    elif not returns:
        result.delta = NEVER
    if reset:
        # fninit and the like set an absolute depth: nothing is relative to the entry any more.
        result.delta = None
        result.depth = [None] * n
        return result
    # Paths reaching one instruction at different depths (data-dependent pops): no single mapping of st(i) to
    # locals, and cpu.fpu.count could not be written at the boundaries after it.
    if any(conflict[i] for i in fn.reachable):
        return result
    uses = False
    for i in fn.reachable:
        mnemonic = fn.instructions[i].mnemonic.split()[-1]
        if fn.branch[i][0] == 'skip' or not is_x87(mnemonic):
            continue
        uses = True
        if result.depth[i] is None:
            return result
    if uses and fn.program.local_x87:
        result.local = True
    return result


def fixpoint(program, fns, indirect):
    # Least fixpoint: every function starts as never returning; deltas only move up to a value, then unknown.
    deltas = dict((fn.entry_address, NEVER) for fn in fns)
    infos = {}
    for _ in range(500):
        changed = False
        for fn in fns:
            info = analyze_function(fn, deltas, indirect)
            infos[fn.entry_address] = info
            if deltas.get(fn.entry_address) != info.delta:
                deltas[fn.entry_address] = info.delta
                changed = True
        if not changed:
            return infos, deltas
    # The optimistic start (never returning) is only sound once the deltas are stable.
    raise AssertionError('x87 depth analysis did not converge')


def analyze(program, fns):
    """Depths per function entry. An indirect call is taken to keep the depth (host shims never touch the x87
    stack). When some address-taken function does not provably return at its entry depth, the code after each
    indirect call checks the depth at run time (indirect_fpu_checked)."""
    infos, deltas = fixpoint(program, fns, 0)
    program.indirect_fpu_delta = 0
    program.indirect_fpu_checked = not all(deltas.get(a) in (0, NEVER) for a in program.address_taken)
    return infos
