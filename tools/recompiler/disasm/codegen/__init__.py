"""C++ code generators, one per x86 mnemonic: cg_<mnemonic>(ctx, *operands) -> list of statements."""

from . import control, fpu, integer, mmx, string
from .ctx import CC_ALIASES

_MODULES = (integer, control, string, fpu, mmx)
_STRING = {'movs': string.cg_movs, 'stos': string.cg_stos, 'scas': string.cg_scas, 'cmps': string.cg_cmps}
_PREFIXES = ('rep', 'repe', 'repz', 'repne', 'repnz')
_unknown = set()


def _lookup(mnemonic):
    for module in _MODULES:
        cg = getattr(module, 'cg_' + mnemonic, None)
        if cg:
            return cg
    return None


def _unhandled(ctx, ins):
    ctx.boundary('trap')
    if ins.mnemonic not in _unknown:
        _unknown.add(ins.mnemonic)
        print('Unhandled instruction: %s' % ins.mnemonic)
    return ['NFS2_ASSERT(false);']


def generate(ctx, ins):
    parts = ins.mnemonic.split()
    prefix = parts[0] if len(parts) > 1 else None
    mnemonic = parts[-1]
    operands = list(getattr(ins, 'operands', []))
    if prefix is None and hasattr(ins, 'prefix') and ins.bytes[0] not in tuple(ins.prefix) + (ins.opcode[0],):
        # Capstone drops some F2/F3 prefixes on string instructions; both repeat them on real CPUs.
        if ctx.record:
            print('Warning: Capstone did not recognize instruction prefix for instruction at 0x%08x: %s %s'
                  % (ins.address, ins.mnemonic, ins.op_str))
        prefix = 'rep'
    if prefix in ('repz',):
        prefix = 'repe'
    if prefix in ('repnz',):
        prefix = 'repne'

    if mnemonic[:4] in _STRING and mnemonic not in ('movsx', 'movzx'):
        if prefix in ('repe', 'repne') and mnemonic[:4] in ('movs', 'stos'):
            prefix = 'rep'
        return _STRING[mnemonic[:4]](ctx, *operands, prefix=prefix)
    if prefix is not None:
        return _unhandled(ctx, ins)
    if mnemonic.startswith('j') and mnemonic[1:] in CC_ALIASES:
        return control.cg_jcc(ctx, mnemonic[1:])
    if mnemonic.startswith('set') and mnemonic[3:] in CC_ALIASES:
        return integer.cg_setcc(ctx, operands[0], mnemonic[3:])
    cg = _lookup(mnemonic)
    if cg is None:
        return _unhandled(ctx, ins)
    return cg(ctx, *operands)
