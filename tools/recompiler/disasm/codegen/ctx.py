"""Emission context: operand access, flags and boundaries for one instruction.

The same code generators run twice. In record mode the context collects what an instruction reads and writes
(registers, flags) for the dataflow analyses; in emit mode it produces C++ using their results.
"""

from capstone import x86 as X

# Tracked flags. fc is the x87 condition code set (C0, C2, C3), kept in cpu.fpu.status.
FLAGS = ('cf', 'pf', 'af', 'zf', 'sf', 'of', 'fc')
FLAG_BIT = {f: 1 << i for i, f in enumerate(FLAGS)}
ALL_FLAGS = (1 << len(FLAGS)) - 1
CPU_FLAGS = FLAGS[:6]
CPU_FLAG_MASK = sum(FLAG_BIT[f] for f in CPU_FLAGS)

REGS = ('eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi')
REG_BIT = {r: 1 << i for i, r in enumerate(REGS)}
ALL_REGS = (1 << len(REGS)) - 1

# name: (full register, bits, shift)
SUBREGS = {}
for _r in ('a', 'c', 'd', 'b'):
    SUBREGS['e%sx' % _r] = ('e%sx' % _r, 32, 0)
    SUBREGS['%sx' % _r] = ('e%sx' % _r, 16, 0)
    SUBREGS['%sl' % _r] = ('e%sx' % _r, 8, 0)
    SUBREGS['%sh' % _r] = ('e%sx' % _r, 8, 8)
for _r in ('sp', 'bp', 'si', 'di'):
    SUBREGS['e' + _r] = ('e' + _r, 32, 0)
    SUBREGS[_r] = ('e' + _r, 16, 0)
SEGMENTS = ('cs', 'ds', 'es', 'fs', 'gs', 'ss')

# Condition codes: flags read and the expression over flag values.
CONDITIONS = {
    'o': ('of',), 'no': ('of',), 'b': ('cf',), 'ae': ('cf',), 'e': ('zf',), 'ne': ('zf',),
    'be': ('cf', 'zf'), 'a': ('cf', 'zf'), 's': ('sf',), 'ns': ('sf',), 'p': ('pf',), 'np': ('pf',),
    'l': ('sf', 'of'), 'ge': ('sf', 'of'), 'le': ('zf', 'sf', 'of'), 'g': ('zf', 'sf', 'of'),
}
CC_ALIASES = {
    'o': 'o', 'no': 'no', 'b': 'b', 'c': 'b', 'nae': 'b', 'ae': 'ae', 'nb': 'ae', 'nc': 'ae',
    'e': 'e', 'z': 'e', 'ne': 'ne', 'nz': 'ne', 'be': 'be', 'na': 'be', 'a': 'a', 'nbe': 'a',
    's': 's', 'ns': 'ns', 'p': 'p', 'pe': 'p', 'np': 'np', 'po': 'np',
    'l': 'l', 'nge': 'l', 'ge': 'ge', 'nl': 'ge', 'le': 'le', 'ng': 'le', 'g': 'g', 'nle': 'g',
}


def generic_condition(cc, f):
    """Condition from flag expressions f[name]."""
    return {
        'o': f.get('of'), 'no': '!%s' % f.get('of'),
        'b': f.get('cf'), 'ae': '!%s' % f.get('cf'),
        'e': f.get('zf'), 'ne': '!%s' % f.get('zf'),
        'be': '(%s || %s)' % (f.get('cf'), f.get('zf')), 'a': '(!%s && !%s)' % (f.get('cf'), f.get('zf')),
        's': f.get('sf'), 'ns': '!%s' % f.get('sf'),
        'p': f.get('pf'), 'np': '!%s' % f.get('pf'),
        'l': '(%s != %s)' % (f.get('sf'), f.get('of')), 'ge': '(%s == %s)' % (f.get('sf'), f.get('of')),
        'le': '(%s || %s != %s)' % (f.get('zf'), f.get('sf'), f.get('of')),
        'g': '(!%s && %s == %s)' % (f.get('zf'), f.get('sf'), f.get('of')),
    }[cc]


def T(bits):
    return 'x86::reg%d' % bits


def S(bits):
    return 'x86::sreg%d' % bits


def mask(bits):
    return (1 << bits) - 1


def lit(value, bits):
    value &= mask(bits)
    if bits == 32:
        return '0x%xu' % value
    if bits == 64:
        return 'x86::reg64(0x%xull)' % value
    return '%s(0x%x)' % (T(bits), value)


# Lazy producers keep their operands; readers rebuild the flags they need from them.
# kind: (operand names, flags it defines, flags readers may rebuild lazily)
LAZY_KINDS = {
    'sub': (('a', 'b'), ('cf', 'zf', 'sf', 'of'), ('cf', 'zf', 'sf', 'of')),
    'add': (('a', 'b'), ('cf', 'zf', 'sf', 'of'), ('cf', 'zf', 'sf', 'of')),
    'logic': (('r',), ('cf', 'zf', 'sf', 'of'), ('cf', 'zf', 'sf', 'of')),
    'inc': (('r',), ('zf', 'sf', 'of'), ('zf', 'sf', 'of')),
    'dec': (('r',), ('zf', 'sf', 'of'), ('zf', 'sf', 'of')),
    'neg': (('a',), ('cf', 'zf', 'sf', 'of'), ('cf', 'zf', 'sf', 'of')),
    'fcmp': (('fa', 'fb'), ('cf', 'pf', 'af', 'zf', 'sf'), ('cf', 'pf', 'zf')),
}


def lazy_flags(kind, bits, v):
    """Flag expressions of a lazy producer; v maps its operand names to C++ expressions."""
    t, s = T(bits), S(bits)

    def sign(e):
        return '(%s(%s(%s)) < 0)' % (s, t, e)

    if kind in ('sub', 'add'):
        a, b = '%s(%s)' % (t, v['a']), '%s(%s)' % (t, v['b'])
        op = '-' if kind == 'sub' else '+'
        r = '%s(%s %s %s)' % (t, a, op, b)
        if kind == 'sub':
            return {'cf': '(%s < %s)' % (a, b), 'zf': '(%s == %s)' % (a, b), 'sf': sign(r),
                    'of': sign('(%s ^ %s) & (%s ^ %s)' % (a, b, a, r))}
        return {'cf': '(%s < %s)' % (r, a), 'zf': '(%s == 0)' % r, 'sf': sign(r),
                'of': sign('~(%s ^ %s) & (%s ^ %s)' % (a, b, a, r))}
    if kind == 'logic':
        r = '%s(%s)' % (t, v['r'])
        return {'cf': 'false', 'of': 'false', 'zf': '(%s == 0)' % r, 'sf': sign(r)}
    if kind in ('inc', 'dec'):
        r = '%s(%s)' % (t, v['r'])
        edge = (1 << (bits - 1)) if kind == 'inc' else mask(bits - 1)
        return {'zf': '(%s == 0)' % r, 'sf': sign(r), 'of': '(%s == %s)' % (r, lit(edge, bits))}
    if kind == 'neg':
        a = '%s(%s)' % (t, v['a'])
        return {'cf': '(%s != 0)' % a, 'zf': '(%s == 0)' % a, 'sf': sign('0u - %s' % a),
                'of': '(%s == %s)' % (a, lit(1 << (bits - 1), bits))}
    if kind == 'fcmp':
        a, b = v['fa'], v['fb']
        return {'cf': '!(%s >= %s)' % (a, b), 'zf': '!(%s < %s || %s > %s)' % (a, b, a, b),
                'pf': '__builtin_isunordered(%s, %s)' % (a, b)}
    raise AssertionError(kind)


def lazy_condition(kind, bits, v, cc):
    """Direct comparison for cc when every flag it reads comes from one lazy producer, or None."""
    t, s = T(bits), S(bits)
    if kind == 'sub':
        a, b = '%s(%s)' % (t, v['a']), '%s(%s)' % (t, v['b'])
        sa, sb = '%s(%s)' % (s, a), '%s(%s)' % (s, b)
        direct = {'b': (a, '<', b), 'ae': (a, '>=', b), 'e': (a, '==', b), 'ne': (a, '!=', b),
                  'be': (a, '<=', b), 'a': (a, '>', b),
                  'l': (sa, '<', sb), 'ge': (sa, '>=', sb), 'le': (sa, '<=', sb), 'g': (sa, '>', sb)}
        if cc in direct:
            return '(%s %s %s)' % direct[cc]
        return None
    if kind == 'logic':
        r = '%s(%s)' % (t, v['r'])
        sr = '%s(%s)' % (s, r)
        direct = {'e': '(%s == 0)' % r, 'ne': '(%s != 0)' % r, 'be': '(%s == 0)' % r, 'a': '(%s != 0)' % r,
                  'b': 'false', 'ae': 'true', 'o': 'false', 'no': 'true',
                  's': '(%s < 0)' % sr, 'ns': '(%s >= 0)' % sr, 'l': '(%s < 0)' % sr, 'ge': '(%s >= 0)' % sr,
                  'le': '(%s <= 0)' % sr, 'g': '(%s > 0)' % sr}
        return direct.get(cc)
    if kind == 'fcmp':
        a, b = v['fa'], v['fb']
        direct = {'b': '!(%s >= %s)' % (a, b), 'ae': '(%s >= %s)' % (a, b),
                  'e': '!__builtin_islessgreater(%s, %s)' % (a, b), 'ne': '__builtin_islessgreater(%s, %s)' % (a, b),
                  'be': '!(%s > %s)' % (a, b), 'a': '(%s > %s)' % (a, b),
                  'p': '__builtin_isunordered(%s, %s)' % (a, b), 'np': '!__builtin_isunordered(%s, %s)' % (a, b)}
        return direct.get(cc)
    return None


def fpu_slot(j):
    """x87 register j above the entry stack (j <= 0: the caller's registers, in cpu.fpu)."""
    if j >= 1:
        return 'fp%d' % j
    return 'cpu.fpu.regs[(fpu_count - %d) & 7]' % -j if j else 'cpu.fpu.regs[fpu_count & 7]'


def lazy_local(kind, address, name):
    return 'lz%s_%x' % (name, address)


def lazy_local_type(name):
    return 'double' if name in ('fa', 'fb') else 'x86::reg32'


class Effects:
    """What one instruction reads and writes, gathered in record mode."""
    __slots__ = ('reg_use', 'reg_def', 'reg_partial', 'flag_use', 'flag_sync', 'flag_def', 'flag_maydef', 'lazy',
                 'boundary')

    def __init__(self):
        self.reg_use = 0        # registers read
        self.reg_def = 0        # registers written as a whole
        self.reg_partial = 0    # registers partly written (the rest is kept)
        self.flag_use = 0       # read as values
        self.flag_sync = 0      # stored to cpu.flags for code reading it there (pushfd)
        self.flag_def = 0       # always written
        self.flag_maydef = 0    # written on some paths only
        self.lazy = None        # (kind, bits) of a lazy producer
        self.boundary = None    # 'call', 'dyncall', 'runtime', 'flags'


class Ctx:
    """Per-function emission state; set ins before generating each instruction."""

    def __init__(self, fn, function_names, record):
        self.fn = fn                    # analysis.Function
        self.function_names = function_names
        self.record = record
        self.ins = None
        self.index = None
        self.effects = None

    # ---- set-up -------------------------------------------------------------------------------------------
    def begin(self, index):
        self.index = index
        self.ins = self.fn.instructions[index]
        self.effects = Effects() if self.record else None
        # x87 depth above the entry stack while generating this instruction; None: the stack stays in cpu.fpu.
        self.fdepth = None if self.record else self.fn.fpu_depth(index)

    @property
    def bounds(self):
        return self.fn.bounds

    # ---- registers ----------------------------------------------------------------------------------------
    def reg(self, name):
        """Rvalue of a general, sub- or segment register."""
        if name in SEGMENTS:
            return 'cpu.%s' % name
        full, bits, shift = SUBREGS[name]
        if self.record:
            self.effects.reg_use |= REG_BIT[full]
        if not self.fn.local_registers:
            return 'cpu.%s' % name
        if bits == 32:
            return full
        if shift:
            return '%s(%s >> %d)' % (T(bits), full, shift)
        return '%s(%s)' % (T(bits), full)

    def set_reg(self, name, value):
        if name in SEGMENTS:
            return ['cpu.%s = %s;' % (name, value)]
        full, bits, shift = SUBREGS[name]
        if self.record:
            if bits == 32:
                self.effects.reg_def |= REG_BIT[full]
            else:
                self.effects.reg_partial |= REG_BIT[full]
        if not self.fn.local_registers:
            return ['cpu.%s = %s;' % (name, value)]
        if bits == 32:
            return ['%s = %s;' % (full, value)]
        keep = 0xffffffff & ~(mask(bits) << shift)
        part = '%s(%s)' % (T(bits), value)
        if shift:
            part = '(x86::reg32(%s) << %d)' % (part, shift)
        return ['%s = (%s & 0x%xu) | %s;' % (full, full, keep, part)]

    def edx_eax(self):
        return '((x86::reg64(%s) << 32) | %s)' % (self.reg('edx'), self.reg('eax'))

    # ---- operands -----------------------------------------------------------------------------------------
    def address(self, op):
        """Effective address of a memory operand (or lea source)."""
        m = op.mem
        parts = []
        if m.segment:
            seg = self.ins.reg_name(m.segment)
            parts.append('cpu.e%s' % seg)
        if m.base:
            parts.append(self.reg(self.ins.reg_name(m.base)))
        if m.index:
            index = self.reg(self.ins.reg_name(m.index))
            parts.append(index if m.scale == 1 else '%s * %d' % (index, m.scale))
        if m.disp or not parts:
            disp = m.disp & 0xffffffff
            if parts and disp >= 0x80000000:
                return '%s - 0x%xu' % (' + '.join(parts), (-disp) & 0xffffffff)
            parts.append('0x%xu' % disp)
        return ' + '.join(parts)

    def load(self, bits, address):
        return 'rc::ld%d(%s)' % (bits, address)

    def store(self, bits, address, value):
        return ['rc::st%d(%s, %s);' % (bits, address, value)]

    def val(self, op, bits=None):
        """Rvalue of an operand, of type reg<bits> (default: the operand size)."""
        bits = bits or op.size * 8
        if op.type == X.X86_OP_IMM:
            return lit(op.imm, bits)
        if op.type == X.X86_OP_REG:
            name = self.ins.reg_name(op.reg)
            value = self.reg(name)
            if op.size * 8 != bits:
                value = '%s(%s)' % (T(bits), value)
            return value
        if op.type == X.X86_OP_MEM:
            value = self.load(op.size * 8, self.address(op))
            if op.size * 8 != bits:
                value = '%s(%s)' % (T(bits), value)
            return value
        raise AssertionError(op.type)

    def sval(self, op, bits):
        """Operand sign-extended to bits."""
        if op.type == X.X86_OP_IMM:
            return lit(op.imm, bits)
        if op.size * 8 == bits:
            return self.val(op)
        return '%s(%s(%s))' % (T(bits), S(op.size * 8), self.val(op))

    def put(self, op, value):
        """Statements storing value into a register or memory operand."""
        if op.type == X.X86_OP_REG:
            return self.set_reg(self.ins.reg_name(op.reg), value)
        if op.type == X.X86_OP_MEM:
            return self.store(op.size * 8, self.address(op), value)
        raise AssertionError(op.type)

    def bind(self, op, name='dst'):
        """For read-modify-write: (setup lines, rvalue, writer) that compute a memory address once."""
        if op.type == X.X86_OP_MEM:
            a = 'a_' + name
            setup = ['const x86::reg32 %s = %s;' % (a, self.address(op))]
            bits = op.size * 8
            return setup, self.load(bits, a), lambda v: self.store(bits, a, v)
        return [], self.val(op), lambda v: self.put(op, v)

    # ---- x87 stack ----------------------------------------------------------------------------------------
    def st(self, i):
        """Lvalue of st(i)."""
        if self.fdepth is None:
            return 'cpu.fpu.st(%d)' % i
        return fpu_slot(self.fdepth - i)

    def fpush(self, value):
        if self.fdepth is None:
            return ['cpu.fpu.push(%s);' % value]
        self.fdepth += 1
        return ['%s = %s;' % (fpu_slot(self.fdepth), value),
                'NFS2_ASSERT(fpu_count + %d <= 8);' % self.fdepth]

    def fpop(self):
        if self.fdepth is None:
            return ['cpu.fpu.pop();']
        self.fdepth -= 1
        return []

    def rndint(self):
        if self.fdepth is None:
            return 'cpu.fpu.rndint()'
        return 'x86::Float(x86::roundIntegral(double(%s), cpu.fpu.control.rc))' % self.st(0)

    # ---- flags --------------------------------------------------------------------------------------------
    def flag(self, f):
        """Rvalue of a flag at the current instruction."""
        if self.record:
            self.effects.flag_use |= FLAG_BIT[f]
            return 'f_' + f
        return self.fn.flag_source(self.index, f)

    def cond(self, cc):
        cc = CC_ALIASES[cc]
        flags = CONDITIONS[cc]
        if self.record:
            for f in flags:
                self.effects.flag_use |= FLAG_BIT[f]
            return 'cond'
        return self.fn.condition(self.index, cc)

    def set_flags(self, values, kind=None, bits=32, operands=None, may=False):
        """Defines flags. values: flag -> C++ expression (computed from the instruction's temporaries).
        kind/operands make this a lazy producer: operands maps its operand names to temporaries."""
        if kind:
            values = dict(lazy_flags(kind, bits, operands), **values)
            defined = LAZY_KINDS[kind][1]
            values = {f: values[f] for f in defined}
        if self.record:
            for f in values:
                if may:
                    self.effects.flag_maydef |= FLAG_BIT[f]
                else:
                    self.effects.flag_def |= FLAG_BIT[f]
            if kind and not may:
                self.effects.lazy = (kind, bits)
            return []
        lines = []
        if kind and kind != 'fcmp' and not may and self.fn.saves_operands(self.index):
            for name in LAZY_KINDS[kind][0]:
                lines.append('%s = %s;' % (lazy_local(kind, self.ins.address, name), operands[name]))
        for f, expr in values.items():
            if f == 'fc':
                continue
            if self.fn.needs_local(self.index, f):
                lines.append('f_%s = %s;' % (f, expr))
        return lines

    def set_fc(self, lines):
        """Defines the x87 condition codes with the given statements (emitted when they are read later)."""
        if self.record:
            self.effects.flag_def |= FLAG_BIT['fc']
            return []
        return lines if self.fn.flag_live_out(self.index, 'fc') else []

    def use_fc(self):
        if self.record:
            self.effects.flag_use |= FLAG_BIT['fc']

    def sync_flags_to_cpu(self):
        """Stores changed flags into cpu.flags, for code that reads cpu.flags itself."""
        if self.record:
            self.effects.flag_sync |= CPU_FLAG_MASK
            self.effects.boundary = 'flags'
            return []
        return self.fn.store_lines(self.index, 'flags')

    def load_flags_from_cpu(self):
        """Reloads the flags after code that wrote cpu.flags itself."""
        if self.record:
            self.effects.flag_def |= CPU_FLAG_MASK
            return []
        return self.fn.reload_lines(self.index)

    # ---- boundaries ---------------------------------------------------------------------------------------
    def boundary(self, kind):
        if self.record:
            self.effects.boundary = kind

    def exit_sync(self, edge):
        """Statements before leaving the function or calling out on the given edge ('call', 'taken', 'exit')."""
        if self.record:
            return []
        return self.fn.store_lines(self.index, edge, self.fdepth)

    def after_call(self):
        if self.record:
            return []
        return self.fn.reload_lines(self.index)

    def function_name(self, address):
        return self.function_names.get(address, 'sub_%x' % address)
