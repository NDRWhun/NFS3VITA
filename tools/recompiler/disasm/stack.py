"""Which registers a function gives back unchanged, and how far it moves esp.

Forward symbolic execution over the function: esp and every register is either its entry value, entry esp + k,
or unknown; stack slots written at known esp offsets remember what was stored there. A register is preserved
when it holds its entry value at every return (typically push on entry, pop before ret).

Stores through registers that do not point into this frame are assumed not to hit the frame's save slots, as
any compiler assumes; stores through pointers derived from esp/ebp with an unknown offset forget all slots.
"""

from capstone import x86 as X

from .codegen import ctx as C
from .codegen.integer import stack_slot

ESP = ('esp',)


def entry_state():
    regs = dict((r, ('orig', r)) for r in C.REGS)
    regs['esp'] = ('esp', 0)
    return (regs, {})


def merge(a, b):
    if a is None:
        return b
    ra, sa = a
    rb, sb = b
    regs = dict((r, ra[r] if ra[r] == rb[r] else None) for r in C.REGS)
    slots = dict((k, v) for k, v in sa.items() if sb.get(k) == v)
    return (regs, slots)


class StackInfo:
    def __init__(self):
        self.preserved = 0      # REG_BIT mask
        self.delta = None       # esp after return - esp at entry (4 + ret immediate), when every path agrees


def reg_symbol(regs, ins, op):
    if op.type == X.X86_OP_REG:
        name = ins.reg_name(op.reg)
        if name in C.REGS:
            return regs[name]
    return None


def address_symbol(regs, ins, op):
    """('esp', k) for an esp-relative address with known offset, 'stack' for an unknown one, else None."""
    m = op.mem
    if m.segment:
        return None
    base = regs.get(ins.reg_name(m.base)) if m.base else None
    index = regs.get(ins.reg_name(m.index)) if m.index else None
    stack = (base and base[0] == 'esp') or (index and index[0] == 'esp')
    if not stack:
        return None
    if base and base[0] == 'esp' and not m.index:
        return ('esp', base[1] + m.disp)
    return 'stack'


# x87 stores: capstone reports their memory operand as read only.
X87_STORES = ('fst', 'fstp', 'fist', 'fistp', 'fisttp', 'fbstp', 'fnstsw', 'fstsw', 'fnstcw', 'fstcw', 'fnstenv',
              'fstenv', 'fnsave', 'fsave')


def written_memory(ins):
    x87_store = ins.mnemonic.split()[-1] in X87_STORES
    for op in getattr(ins, 'operands', []):
        if op.type == X.X86_OP_MEM and (op.access & 2 or x87_store):
            yield op


def store(slots, address, size, value):
    if address == 'stack':
        slots.clear()
        return
    if address is None:
        return
    k = address[1]
    for s in list(slots):
        if s < k + size and k < s + 4:
            del slots[s]
    if size == 4 and value is not None:
        slots[k] = value


def analyze_function(fn, infos):
    """StackInfo of one function, given the current infos of its callees."""
    n = fn.n
    state_in = [None] * n
    state_in[fn.entry] = entry_state()
    info = StackInfo()
    preserved = C.ALL_REGS
    deltas = set()
    work = [fn.entry]
    while work:
        i = work.pop()
        regs, slots = state_in[i]
        regs = dict(regs)
        slots = dict(slots)
        ins = fn.instructions[i]
        e = fn.effects[i]
        mnemonic = ins.mnemonic.split()[-1]
        esp = regs['esp'] if regs['esp'] and regs['esp'][0] == 'esp' else None
        regs['esp'] = esp
        kind = fn.branch[i][0]
        handled = False
        if kind == 'skip':
            handled = True
        elif mnemonic == 'ret':
            pop = 4 + (ins.operands[0].imm if ins.operands else 0)
            regs['esp'] = ('esp', esp[1] + pop) if esp and esp[1] == 0 else None
            handled = True
        elif mnemonic == 'push' and esp:
            size = stack_slot(ins)
            value = reg_symbol(regs, ins, ins.operands[0]) if ins.operands[0].size == 4 else None
            store(slots, ('esp', esp[1] - size), size, value)
            regs['esp'] = ('esp', esp[1] - size)
            handled = True
        elif mnemonic == 'pop' and esp and ins.operands[0].type == X.X86_OP_REG:
            name = ins.reg_name(ins.operands[0].reg)
            value = slots.get(esp[1])
            regs['esp'] = ('esp', esp[1] + stack_slot(ins))
            if name in C.REGS:
                regs[name] = value
            elif name in C.SUBREGS:
                regs[C.SUBREGS[name][0]] = None
            handled = True
        elif mnemonic == 'mov' and ins.operands[0].type == X.X86_OP_REG and ins.operands[0].size == 4:
            name = ins.reg_name(ins.operands[0].reg)
            src = ins.operands[1]
            regs[name] = reg_symbol(regs, ins, src) if src.type == X.X86_OP_REG and src.size == 4 else None
            handled = True
        elif mnemonic == 'lea' and ins.operands[0].size == 4:
            name = ins.reg_name(ins.operands[0].reg)
            a = address_symbol(regs, ins, ins.operands[1])
            regs[name] = a if isinstance(a, tuple) else None
            handled = True
        elif mnemonic in ('add', 'sub') and ins.operands[0].type == X.X86_OP_REG and \
                ins.operands[1].type == X.X86_OP_IMM and ins.operands[0].size == 4:
            name = ins.reg_name(ins.operands[0].reg)
            v = regs.get(name)
            if v and v[0] == 'esp':
                imm = ins.operands[1].imm
                regs[name] = ('esp', v[1] + (imm if mnemonic == 'add' else -imm))
            else:
                regs[name] = None
            handled = True
        elif mnemonic == 'leave':
            v = regs['ebp']
            if v and v[0] == 'esp':
                regs['ebp'] = slots.get(v[1])
                regs['esp'] = ('esp', v[1] + 4)
            else:
                regs['ebp'] = None
                regs['esp'] = None
            handled = True
        if not handled:
            if kind in ('call', 'dyncall'):
                for op in written_memory(ins):
                    store(slots, address_symbol(regs, ins, op), op.size, None)
                target = fn.branch[i][1] if kind == 'call' else None
                callee = infos.get(target) if target is not None else None
                changed = (fn.callee_may_def(i) >> 8) & C.ALL_REGS
                if callee:
                    changed &= ~callee.preserved
                for r in C.REGS:
                    if changed & C.REG_BIT[r] and r != 'esp':
                        regs[r] = None
                # The call pushes nothing (cpu.esp -= 4 only); the callee's ret adds 4 + its immediate.
                if esp and callee and callee.delta is not None:
                    regs['esp'] = ('esp', esp[1] - 4 + callee.delta)
                else:
                    regs['esp'] = None
            else:
                for op in written_memory(ins):
                    store(slots, address_symbol(regs, ins, op), op.size, None)
                if mnemonic in ('stos', 'stosb', 'stosw', 'stosd', 'movs', 'movsb', 'movsw', 'movsd', 'pushal',
                                'enter', 'pushfd', 'pushf'):
                    d = regs.get('edi')
                    if mnemonic in ('pushal', 'enter', 'pushfd', 'pushf') or (d and d[0] == 'esp'):
                        slots.clear()
                written = e.reg_def | e.reg_partial
                for r in C.REGS:
                    if written & C.REG_BIT[r]:
                        regs[r] = None
        # Exits.
        for x in fn.exits[i]:
            if x.edge == 'call':
                continue
            if x.kind == 'ret':
                # A real ret has applied its adjustment; falling off the end of a function has no known delta.
                at = regs['esp'] if mnemonic == 'ret' else None
                deltas.add(at[1] if at and at[0] == 'esp' else None)
                good = 0
                for r in C.REGS:
                    if r != 'esp' and regs[r] == ('orig', r):
                        good |= C.REG_BIT[r]
                preserved &= good
            elif x.kind == 'tail':
                callee = infos.get(x.target)
                good = 0
                if callee:
                    for r in C.REGS:
                        if r != 'esp' and regs[r] == ('orig', r) and callee.preserved & C.REG_BIT[r]:
                            good |= C.REG_BIT[r]
                    esp_now = regs['esp']
                    known = esp_now and esp_now[0] == 'esp' and callee.delta is not None
                    deltas.add(esp_now[1] + callee.delta if known else None)
                else:
                    deltas.add(None)
                preserved &= good
            else:
                preserved = 0
                deltas.add(None)
        out = (regs, slots)
        for s in fn.succ[i]:
            merged = merge(state_in[s], out)
            if merged != state_in[s]:
                state_in[s] = merged
                work.append(s)
    info.preserved = preserved & ~C.REG_BIT['esp']
    if len(deltas) == 1:
        (info.delta,) = deltas
    return info


def analyze(program, fns):
    """StackInfo per function entry; iterates from no knowledge until the summaries stop changing. Each round
    only refines summaries that are already sound, so stopping at the round limit is conservative."""
    infos = {}
    changed = True
    rounds = 0
    while changed and rounds < 20:
        rounds += 1
        changed = False
        for fn in fns:
            info = analyze_function(fn, infos)
            old = infos.get(fn.entry_address)
            if old is None or old.preserved != info.preserved or old.delta != info.delta:
                infos[fn.entry_address] = info
                changed = True
    return infos
