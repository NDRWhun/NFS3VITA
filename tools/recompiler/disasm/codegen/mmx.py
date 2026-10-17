"""MMX instructions as SSE2 intrinsics on cpu.mmx (include/mmx.h)."""

from capstone import x86 as X


def mval(ctx, op):
    if op.type == X.X86_OP_IMM:
        return '%d' % op.imm
    if op.type == X.X86_OP_REG:
        return 'cpu.mmx.%s' % ctx.ins.reg_name(op.reg)
    return 'x86::from_reg64(rc::ld64(%s))' % ctx.address(op)


def cg_emms(ctx):
    return ['cpu.mmx.init();']


def cg_movd(ctx, dst, src):
    if src.type == X.X86_OP_MEM:
        return ['%s = { _mm_cvtsi32_si128(x86::sreg32(%s)) };' % (mval(ctx, dst), ctx.val(src))]
    if dst.type == X.X86_OP_REG and ctx.ins.reg_name(dst.reg).startswith('mm'):
        return ['%s = { _mm_cvtsi32_si128(x86::sreg32(%s)) };' % (mval(ctx, dst), ctx.val(src, 32))]
    return ctx.put(dst, 'x86::reg32(_mm_cvtsi128_si32(%s))' % mval(ctx, src))


def cg_movq(ctx, dst, src):
    if dst.type == X.X86_OP_MEM:
        return ctx.store(64, ctx.address(dst), 'x86::reg64(%s)' % mval(ctx, src))
    return ['%s = %s;' % (mval(ctx, dst), mval(ctx, src))]


def _binary(intrinsic):
    def cg(ctx, dst, src):
        d = mval(ctx, dst)
        return ['%s = { %s(%s, %s) };' % (d, intrinsic, d, mval(ctx, src))]
    return cg


def _shift(immediate, register):
    def cg(ctx, dst, count):
        d = mval(ctx, dst)
        if count.type == X.X86_OP_IMM:
            return ['%s = { %s(%s, %d) };' % (d, immediate, d, count.imm)]
        return ['%s = { %s(%s, %s) };' % (d, register, d, mval(ctx, count))]
    return cg


cg_pxor = _binary('_mm_xor_si128')
cg_pand = _binary('_mm_and_si128')
cg_por = _binary('_mm_or_si128')
cg_paddw = _binary('_mm_add_epi16')
cg_paddd = _binary('_mm_add_epi32')
cg_paddsb = _binary('_mm_adds_epi8')
cg_paddsw = _binary('_mm_adds_epi16')
cg_pmulhw = _binary('_mm_mulhi_epi16')
cg_pmaddwd = _binary('_mm_madd_epi16')
cg_punpcklbw = _binary('_mm_unpacklo_epi8')
cg_punpcklwd = _binary('_mm_unpacklo_epi16')
cg_punpckldq = _binary('_mm_unpacklo_epi32')
cg_psllw = _shift('_mm_slli_epi16', '_mm_sll_epi16')
cg_pslld = _shift('_mm_slli_epi32', '_mm_sll_epi32')
cg_psllq = _shift('_mm_slli_epi64', '_mm_sll_epi64')
cg_psrlw = _shift('_mm_srli_epi16', '_mm_srl_epi16')
cg_psrld = _shift('_mm_srli_epi32', '_mm_srl_epi32')
cg_psrlq = _shift('_mm_srli_epi64', '_mm_srl_epi64')
cg_psraw = _shift('_mm_srai_epi16', '_mm_sra_epi16')
cg_psrad = _shift('_mm_srai_epi32', '_mm_sra_epi32')


def _unpack_high(intrinsic):
    # MMX high unpacks take bytes 4-7 of the 64-bit value: shift them down, then unpack low.
    def cg(ctx, dst, src):
        d = mval(ctx, dst)
        return ['%s = { %s(_mm_srli_epi64(%s, 32), _mm_srli_epi64(%s, 32)) };' % (d, intrinsic, d, mval(ctx, src))]
    return cg


cg_punpckhbw = _unpack_high('_mm_unpacklo_epi8')
cg_punpckhwd = _unpack_high('_mm_unpacklo_epi16')


def _pack(intrinsic):
    # SSE packs place the second operand at byte 8; MMX wants it right after the first (byte 4).
    def cg(ctx, dst, src):
        d = mval(ctx, dst)
        return ['{ const __m128i p = %s(%s, %s); %s = { _mm_unpacklo_epi32(p, _mm_srli_si128(p, 8)) }; }'
                % (intrinsic, d, mval(ctx, src), d)]
    return cg


cg_packuswb = _pack('_mm_packus_epi16')
cg_packssdw = _pack('_mm_packs_epi32')
cg_packsswb = _pack('_mm_packs_epi16')
