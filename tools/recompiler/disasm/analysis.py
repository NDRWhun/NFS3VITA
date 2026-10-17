"""Control flow and dataflow analyses over the recompiled functions.

Flags live in C++ locals (f_cf...) inside a function; cpu.flags is the copy other code sees. A flag is stored to
cpu.flags at a boundary (call, return, tail call, pushfd) only when code outside can read it there (interprocedural
liveness) and this function changed it since cpu.flags was last valid (a local producer reaches the boundary).

Every flag read is served either from the local or, when exactly one lazy producer reaches it, from that producer's
saved operands (cmp a, b; jl -> a < b). Producers write their local only for reads that cannot be served lazily.

State masks: bits 0-6 are FLAGS (ctx.FLAG_BIT), bits 8-15 general registers (ctx.REG_BIT << 8).
"""

from capstone import x86 as X

from .codegen import ctx as C
from . import codegen


FCOM = ('fcom', 'fcomp', 'fcompp', 'ftst')
JCC = set('j' + cc for cc in C.CC_ALIASES) | {'jcxz', 'jecxz'}
REG_SHIFT = 8
ALL_REG_STATE = C.ALL_REGS << REG_SHIFT
ALL_STATE = C.ALL_FLAGS | ALL_REG_STATE
MEMORY_DEFS = ('popfd', 'popf')     # write cpu.flags directly


class Exit:
    """An edge leaving the function (or a call out of it) at one instruction."""
    __slots__ = ('edge', 'kind', 'target')

    def __init__(self, edge, kind, target=None):
        self.edge = edge        # 'exit' (ret, tail, fall-off), 'taken' (jcc to a tail target), 'call'
        self.kind = kind        # 'ret', 'tail', 'dyntail', 'call', 'dyncall'
        self.target = target    # entry address for 'tail' and 'call'


class Function:
    ENTRY = -1

    def __init__(self, module, subroutine, thread_segments, skip_instructions, function_names):
        self.module = module
        self.sub = subroutine
        self.instructions = subroutine.instructions
        self.entry_address = subroutine.get_entry_point()
        self.bounds = (subroutine.get_start_address(), subroutine.get_end_address())
        self.thread_segments = thread_segments
        self.skip = set(skip_instructions)
        self.function_names = function_names
        self.index_of = {}
        for i, ins in enumerate(self.instructions):
            self.index_of.setdefault(ins.address, i)
        self.static_targets = set(x for _, x in subroutine.static_labels)
        self.dynamic_targets = set(x for _, x in subroutine.dynamic_labels)
        self.entry = self.index_of[self.entry_address]
        self.n = len(self.instructions)
        self.program = None

        self.branch = [None] * self.n
        self.succ = [[] for _ in range(self.n)]
        self.pred = None
        self.exits = [[] for _ in range(self.n)]
        self.effects = [None] * self.n
        self.use = [0] * self.n     # state read (flags | regs << 8)
        self.kill = [0] * self.n    # state always written
        self.fused = {}             # index of fcom / fnstsw / sahf -> index of the sahf of its triple
        self.live_in = None
        self.live_out = None
        self._rd_cache = {}
        self.local_needed = set()   # (producer index, flag): the producer writes f_<flag>
        self.lazy_readers = set()   # producers with lazily served reads
        self.reloads = set()        # (memory producer, flag): reload f_<flag> from cpu.flags after it
        self.stores = {}            # (index, edge) -> flag mask stored to cpu.flags there

    # ---- structure ----------------------------------------------------------------------------------------
    def is_label(self, i):
        a = self.instructions[i].address
        return a in self.static_targets or a in self.dynamic_targets or a == self.entry_address

    def in_bounds(self, address):
        return self.bounds[0] <= address < self.bounds[1]

    def classify(self):
        """Control flow of every instruction: successors and exit edges."""
        switch_cases = sorted(self.index_of[a] for a in self.dynamic_targets if a in self.index_of)
        for i, ins in enumerate(self.instructions):
            nxt = [i + 1] if i + 1 < self.n else []
            fall_off = [] if nxt else [Exit('exit', 'ret')]
            mnemonic = ins.mnemonic.split()[-1]
            if ins.address in self.skip:
                self.branch[i] = ('skip',)
                self.succ[i] = nxt
                self.exits[i] = fall_off
            elif mnemonic == 'jmp' or mnemonic in JCC:
                target = self.jump_target(ins)
                conditional = mnemonic != 'jmp'
                self.branch[i] = target
                succ = list(nxt) if conditional else []
                exits = list(fall_off) if conditional else []
                edge = 'taken' if conditional else 'exit'
                if target[0] == 'label':
                    succ.append(self.index_of[target[1]])
                elif target[0] == 'tail':
                    exits.append(Exit(edge, 'tail', target[1]))
                elif target[0] == 'dyntail':
                    exits.append(Exit(edge, 'dyntail'))
                elif target[0] == 'switch':
                    succ.extend(switch_cases)
                self.succ[i] = succ
                self.exits[i] = exits
            elif mnemonic == 'ret':
                self.branch[i] = ('ret',)
                self.exits[i] = [Exit('exit', 'ret')]
            elif mnemonic == 'call':
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM:
                    self.branch[i] = ('call', op.imm)
                    self.exits[i] = [Exit('call', 'call', op.imm)] + fall_off
                else:
                    self.branch[i] = ('dyncall',)
                    self.exits[i] = [Exit('call', 'dyncall')] + fall_off
                self.succ[i] = nxt
            else:
                self.branch[i] = ('next',)
                self.succ[i] = nxt
                self.exits[i] = fall_off
        self.pred = [[] for _ in range(self.n)]
        for i, s in enumerate(self.succ):
            for j in s:
                self.pred[j].append(i)
        self.find_fused_compares()

    def jump_target(self, ins):
        op = ins.operands[0]
        if op.type == X.X86_OP_IMM:
            return ('label', op.imm) if self.in_bounds(op.imm) else ('tail', op.imm)
        if op.type == X.X86_OP_REG:
            return ('dyntail',)
        for dest in ins.potential_destinations:
            if dest < self.bounds[0] or dest > self.bounds[1]:
                return ('dyntail',)
        return ('switch',) if ins.potential_destinations else ('dyntail',)

    def find_fused_compares(self):
        """fcom*; fnstsw ax; sahf with no label inside: the flags come from the compared values."""
        for i in range(self.n - 2):
            a, b, c = self.instructions[i:i + 3]
            if a.mnemonic not in FCOM or b.mnemonic not in ('fnstsw', 'fstsw') or c.mnemonic != 'sahf':
                continue
            if b.operands[0].type != X.X86_OP_REG or b.reg_name(b.operands[0].reg) != 'ax':
                continue
            if self.is_label(i + 1) or self.is_label(i + 2):
                continue
            if any(x.address in self.thread_segments or x.address in self.skip for x in (a, b, c)):
                continue
            self.fused[i] = self.fused[i + 1] = self.fused[i + 2] = i + 2

    def is_call(self, i):
        return self.branch[i][0] in ('call', 'dyncall')

    def is_fused_fnstsw(self, i):
        return self.fused.get(i) is not None and self.instructions[i].mnemonic in ('fnstsw', 'fstsw')

    # ---- effects ------------------------------------------------------------------------------------------
    def record(self):
        ctx = C.Ctx(self, self.function_names, record=True)
        for i, ins in enumerate(self.instructions):
            ctx.begin(i)
            if self.branch[i][0] != 'skip':
                codegen.generate(ctx, ins)
            e = ctx.effects
            self.effects[i] = e
            self.use[i] = e.flag_use | e.flag_sync | e.reg_use << REG_SHIFT
            self.kill[i] = e.flag_def | e.reg_def << REG_SHIFT
            if self.is_fused_fnstsw(i) or self.masks_condition_codes(i):
                self.use[i] &= ~C.FLAG_BIT['fc']    # fused: read only when eax is live (see liveness)
        # A failed assertion traps: nothing runs after an instruction the generator cannot translate.
        for i in range(self.n):
            if self.effects[i].boundary == 'trap':
                self.succ[i] = []
                self.exits[i] = []
        self.pred = [[] for _ in range(self.n)]
        for i, s in enumerate(self.succ):
            for j in s:
                self.pred[j].append(i)
        self.reachable = set()
        stack = [self.entry]
        while stack:
            i = stack.pop()
            if i not in self.reachable:
                self.reachable.add(i)
                stack.extend(self.succ[i])

    def masks_condition_codes(self, i):
        """fnstsw ax directly followed by and eax/ax, imm clearing C0, C2 and C3."""
        ins = self.instructions[i]
        if ins.mnemonic not in ('fnstsw', 'fstsw') or i + 1 >= self.n or self.is_label(i + 1):
            return False
        op = ins.operands[0]
        if op.type != X.X86_OP_REG or ins.reg_name(op.reg) != 'ax':
            return False
        nxt = self.instructions[i + 1]
        if nxt.mnemonic != 'and' or nxt.operands[0].type != X.X86_OP_REG or nxt.operands[1].type != X.X86_OP_IMM:
            return False
        return nxt.reg_name(nxt.operands[0].reg) in ('eax', 'ax') and not (nxt.operands[1].imm & 0x4500)

    def may_def_local(self):
        """Flags this function may write, before its callees."""
        mask = 0
        for i in self.reachable:
            e = self.effects[i]
            mask |= e.flag_def | e.flag_maydef
            if self.instructions[i].mnemonic in MEMORY_DEFS:
                mask |= C.ALL_FLAGS
            for x in self.exits[i]:
                if x.kind in ('dyncall', 'dyntail'):
                    mask |= C.ALL_FLAGS
        return mask

    # ---- liveness -----------------------------------------------------------------------------------------
    def exit_use(self, x):
        p = self.program
        if x.kind == 'ret':
            return p.ret_live[self.entry_address]
        if x.kind == 'tail':
            return p.live_in.get(x.target, ALL_STATE) | p.ret_live[self.entry_address]
        if x.kind == 'dyntail':
            return p.indirect_live_in | p.ret_live[self.entry_address]
        if x.kind == 'call':
            return p.live_in.get(x.target, ALL_STATE)
        if x.kind == 'dyncall':
            return p.indirect_live_in
        raise AssertionError(x.kind)

    def liveness(self):
        """Backward liveness of flags and registers; calls pass live state through."""
        n = self.n
        live_in = self.live_in if self.live_in else [0] * n
        out = [0] * n
        eax = C.REG_BIT['eax'] << REG_SHIFT
        fc = C.FLAG_BIT['fc']
        changed = True
        while changed:
            changed = False
            for i in range(n - 1, -1, -1):
                o = 0
                for s in self.succ[i]:
                    o |= live_in[s]
                call_use = 0
                for x in self.exits[i]:
                    if x.edge == 'call':
                        call_use |= self.exit_use(x)
                    else:
                        o |= self.exit_use(x)
                use = self.use[i]
                if (o & eax) and self.is_fused_fnstsw(i):
                    use |= fc
                v = use | call_use | (o & ~self.kill[i])
                out[i] = o
                if v != live_in[i]:
                    live_in[i] = v
                    changed = True
        self.live_in = live_in
        self.live_out = out
        return live_in[self.entry]

    def writes_ax(self, i):
        return bool(self.live_out[i] & (C.REG_BIT['eax'] << REG_SHIFT))

    def flag_live_out(self, i, f):
        return bool(self.live_out[i] & C.FLAG_BIT[f])

    # ---- reaching definitions -----------------------------------------------------------------------------
    def callee_may_def(self, i):
        b = self.branch[i]
        if b[0] == 'call':
            return self.program.may_def.get(b[1], C.ALL_FLAGS)
        return C.ALL_FLAGS

    def is_memory_producer(self, p, f):
        """p leaves cpu.flags authoritative for f: entry, a call that may change f, popfd."""
        if p == self.ENTRY:
            return True
        return self.is_call(p) or self.instructions[p].mnemonic in MEMORY_DEFS

    def reaching(self, i, f):
        """Producers of flag f reaching the start of instruction i (indices or ENTRY)."""
        key = (i, f)
        cached = self._rd_cache.get(key)
        if cached is not None:
            return cached
        bit = C.FLAG_BIT[f]
        result = set()
        seen = set()
        stack = list(self.pred[i])
        if i == self.entry:
            result.add(self.ENTRY)
        while stack:
            p = stack.pop()
            if p in seen:
                continue
            seen.add(p)
            if self.is_call(p):
                if self.callee_may_def(p) & bit:
                    result.add(p)
                    continue
            elif self.instructions[p].mnemonic in MEMORY_DEFS:
                result.add(p)
                continue
            else:
                e = self.effects[p]
                if e.flag_def & bit:
                    result.add(p)
                    continue
                if e.flag_maydef & bit:
                    result.add(p)
            if p == self.entry:
                result.add(self.ENTRY)
            stack.extend(self.pred[p])
        result = frozenset(result)
        self._rd_cache[key] = result
        return result

    def lazy_producer(self, i, f):
        """The single lazy producer serving flag f at instruction i, or None."""
        rd = self.reaching(i, f)
        if len(rd) != 1:
            return None
        (p,) = rd
        if self.is_memory_producer(p, f):
            return None
        e = self.effects[p]
        if not e.lazy or e.flag_maydef:
            return None
        if f not in C.LAZY_KINDS[e.lazy[0]][2]:
            return None
        return p

    # ---- decisions ----------------------------------------------------------------------------------------
    def boundary_mask(self, i, x):
        """CPU flags code outside may read on exit edge x of instruction i."""
        if x.edge == 'call':
            after = self.live_out[i]
            return (self.exit_use(x) | (after & self.callee_may_def(i))) & C.CPU_FLAG_MASK
        return self.exit_use(x) & C.CPU_FLAG_MASK

    def need_valid(self, i, f):
        """Every producer reaching a read of f at i must provide a valid f_<flag>."""
        p = self.lazy_producer(i, f)
        if p is not None:
            self.lazy_readers.add(p)
            return
        for q in self.reaching(i, f):
            if self.is_memory_producer(q, f):
                self.reloads.add((q, f))
            else:
                self.local_needed.add((q, f))

    def dirty(self, i, f):
        return any(not self.is_memory_producer(q, f) for q in self.reaching(i, f))

    def decide(self):
        for i in range(self.n):
            mask = self.effects[i].flag_use & C.CPU_FLAG_MASK
            for f in C.CPU_FLAGS:
                if mask & C.FLAG_BIT[f]:
                    self.need_valid(i, f)
            stores = {}
            for x in self.exits[i]:
                mask = self.boundary_mask(i, x)
                for f in C.CPU_FLAGS:
                    if mask & C.FLAG_BIT[f] and self.dirty(i, f):
                        stores[x.edge] = stores.get(x.edge, 0) | C.FLAG_BIT[f]
                        self.need_valid(i, f)
            if self.effects[i].boundary == 'flags':
                for f in C.CPU_FLAGS:
                    if self.dirty(i, f):
                        stores['flags'] = stores.get('flags', 0) | C.FLAG_BIT[f]
                        self.need_valid(i, f)
            for edge, mask in stores.items():
                self.stores[(i, edge)] = mask

    # ---- queries used by the code generators --------------------------------------------------------------
    def needs_local(self, i, f):
        return (i, f) in self.local_needed

    def saves_operands(self, i):
        return i in self.lazy_readers

    def fcmp_saved(self, sahf):
        return sahf in self.lazy_readers or any(self.needs_local(sahf, f) for f in ('cf', 'pf', 'zf'))

    def lazy_values(self, p):
        kind, bits = self.effects[p].lazy
        address = self.instructions[p].address
        return kind, bits, {name: C.lazy_local(kind, address, name) for name in C.LAZY_KINDS[kind][0]}

    def flag_source(self, i, f):
        p = self.lazy_producer(i, f)
        if p is None:
            return 'f_' + f
        kind, bits, values = self.lazy_values(p)
        return C.lazy_flags(kind, bits, values)[f]

    def condition(self, i, cc):
        flags = C.CONDITIONS[cc]
        producers = set(self.lazy_producer(i, f) for f in flags)
        if len(producers) == 1:
            (p,) = producers
            if p is not None:
                kind, bits, values = self.lazy_values(p)
                direct = C.lazy_condition(kind, bits, values, cc)
                if direct:
                    return direct
        return C.generic_condition(cc, {f: self.flag_source(i, f) for f in flags})

    def store_lines(self, i, edge):
        mask = self.stores.get((i, edge), 0)
        return ['cpu.flags.%s = %s;' % (f, self.flag_source(i, f)) for f in C.CPU_FLAGS if mask & C.FLAG_BIT[f]]

    def reload_lines(self, i):
        return ['f_%s = cpu.flags.%s;' % (f, f) for f in C.CPU_FLAGS if (i, f) in self.reloads]

    def entry_loads(self):
        return [f for f in C.CPU_FLAGS if (self.ENTRY, f) in self.reloads]

    def lazy_locals(self):
        """Declarations of the saved operands of lazy producers."""
        names = []
        for p in sorted(self.lazy_readers):
            kind, _, values = self.lazy_values(p)
            if kind == 'fcmp':
                continue
            for name, local in values.items():
                names.append((C.lazy_local_type(name), local))
        for i, s in self.fused.items():
            if i == s and self.fcmp_saved(s):
                address = self.instructions[s].address
                for name in ('fa', 'fb'):
                    names.append(('double', C.lazy_local('fcmp', address, name)))
        return sorted(set(names), key=lambda x: x[1])


class Program:
    def __init__(self, modules, thread_segments, skip_instructions, function_names, flags_preserved=(), not_code=(),
                 native_overrides=()):
        self.functions = {}
        self.native_overrides = set(native_overrides)
        # Functions that save the caller's flags on entry (pushfd) and restore them before returning (popfd):
        # callers neither provide nor lose flags across a call to them.
        self.flags_preserved = set(flags_preserved)
        for module in modules:
            for sub in module.subroutines:
                fn = Function(module, sub, thread_segments, skip_instructions, function_names)
                fn.program = self
                self.functions[fn.entry_address] = fn
        self.address_taken = self.find_address_taken(modules) - set(not_code)
        self.live_in = {}
        self.ret_live = {}
        self.may_def = {}
        self.indirect_live_in = 0

    def find_address_taken(self, modules):
        """Entries that may be called indirectly: exports, module entry points, instruction immediates, and any
        dword in data or in code bytes no instruction covers. Displacements (jump table bases) do not count."""
        entries = set(self.functions)
        taken = set()
        for module in modules:
            taken.add(module.entry_point)
            for address, _ in module.exported_symbols:
                taken.add(address)
            covered = {}
            for sub in module.subroutines:
                for ins in sub.instructions:
                    if ins.size:
                        covered[ins.address] = ins.size
                    if ins.mnemonic in ('call', 'jmp') or ins.mnemonic in JCC:
                        continue
                    for op in getattr(ins, 'operands', []):
                        if op.type == X.X86_OP_IMM:
                            taken.add(op.imm & 0xffffffff)
            for data, address, length, name, ro, code in module.sections:
                if not data:
                    continue
                skip = bytearray(len(data))
                if code:
                    for a, size in covered.items():
                        if address <= a < address + len(data):
                            skip[a - address:a - address + size] = b'\x01' * size
                for offset in range(len(data) - 3):
                    if skip[offset]:
                        continue
                    value = data[offset] | data[offset + 1] << 8 | data[offset + 2] << 16 | data[offset + 3] << 24
                    if value in entries:
                        taken.add(value)
        return taken & entries

    def compute_may_def(self, fns):
        local = dict((fn.entry_address, fn.may_def_local()) for fn in fns)
        callees = dict((fn.entry_address, set(x.target for i in fn.reachable for x in fn.exits[i]
                                              if x.kind in ('call', 'tail'))) for fn in fns)
        for a in self.flags_preserved:
            local[a] = 0
            callees[a] = set()
        may_def = dict(local)
        changed = True
        while changed:
            changed = False
            for a, targets in callees.items():
                v = may_def[a]
                for t in targets:
                    v |= may_def.get(t, C.ALL_FLAGS)
                if v != may_def[a]:
                    may_def[a] = v
                    changed = True
        self.may_def = may_def

    def analyze(self, log=print):
        fns = list(self.functions.values())
        for fn in fns:
            fn.classify()
            fn.record()
        self.compute_may_def(fns)
        for fn in fns:
            self.live_in[fn.entry_address] = 0
            self.ret_live[fn.entry_address] = 0
        # Host code (threads, callbacks, window procedures) may read any register after an indirect call.
        host_ret = ALL_REG_STATE
        iteration = 0
        while True:
            iteration += 1
            changed = False
            for fn in fns:
                v = fn.liveness()
                if fn.entry_address in self.flags_preserved:
                    v &= ~C.ALL_FLAGS
                if v != self.live_in[fn.entry_address]:
                    self.live_in[fn.entry_address] = v
                    changed = True
            indirect_live_in = ALL_REG_STATE
            for a in self.address_taken:
                indirect_live_in |= self.live_in[a]
            ret_live = dict((a, 0) for a in self.functions)
            indirect_ret = host_ret
            for fn in fns:
                for i in fn.reachable:
                    for x in fn.exits[i]:
                        if x.kind == 'call':
                            if x.target in ret_live:
                                ret_live[x.target] |= fn.live_out[i]
                        elif x.kind == 'dyncall':
                            indirect_ret |= fn.live_out[i]
                        elif x.kind == 'tail':
                            if x.target in ret_live:
                                ret_live[x.target] |= self.ret_live[fn.entry_address]
                        elif x.kind == 'dyntail':
                            indirect_ret |= self.ret_live[fn.entry_address]
            for a in self.address_taken:
                ret_live[a] |= indirect_ret
            if indirect_live_in != self.indirect_live_in or ret_live != self.ret_live:
                changed = True
            self.indirect_live_in = indirect_live_in
            self.ret_live = ret_live
            if not changed:
                break
        log('liveness: %d iterations; %d address-taken functions; indirect live-in %s; indirect return %s'
            % (iteration, len(self.address_taken), state_names(self.indirect_live_in), state_names(indirect_ret)))
        for fn in fns:
            fn.decide()


def state_names(mask):
    names = [f for f in C.FLAGS if mask & C.FLAG_BIT[f]]
    names += [r for r in C.REGS if mask & (C.REG_BIT[r] << REG_SHIFT)]
    return ','.join(names) or '-'
