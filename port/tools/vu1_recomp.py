# Static recompiler for VU1 microcode -> C++ (see PS2Recomp/ps2xRuntime/include/runtime/vu1_native.h).
#
# Input : a directory written by the runtime with PS2X_VU_DUMP=<dir>
#         (vu1_<hash>.bin = 16 KiB micro memory image, vu1_entries.txt = "<hash> <pc>" lines).
# Output: recomp/vu1/vu1_native.cpp (one function per image + the lookup table).
#
# Semantics mirror the runtime's VU1 interpreter (ps2_vu1_upper.cpp / ps2_vu1_lower.cpp):
#  * pair semantics: the lower instruction sees VF values from before the upper instruction; when
#    both write the same VF register the upper result wins.
#  * VF/VI hazards interlock, so results are written immediately; per-lane ready cycles keep the
#    cycle counter (which drives Q/P/flag visibility) in step with the interpreter.
#  * a conditional branch / JR reads the pre-write value of a VI register written by the integer
#    instruction immediately before it.
import os, re, sys, glob, collections

CODE_SIZE = 0x4000
PC_MASK = 0x3FFF

def code_range_hash(code, lo, hi):
    """FNV-1a over 32-bit words of code[lo:hi] (runtime: ps2vu1n::codeRangeHash)."""
    h = 0xCBF29CE484222325
    for a in range(lo, hi, 4):
        w = int.from_bytes(code[a & 0x3FFC:(a & 0x3FFC) + 4], 'little')
        h = ((h ^ w) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def lanes_of(dest):
    return [k for k in range(4) if dest & (1 << (3 - k))]

def lmask(L):
    return sum(1 << k for k in L)


def sx(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v & (1 << (bits - 1)) else v

C = 'xyzw'

class Unsupported(Exception):
    pass

# ------------------------------------------------------------------------------------------ upper
class Upper:
    def __init__(self):
        self.reads = []          # (reg, lanes list)
        self.write = None        # (reg, lanes) for VF writes
        self.acc_write = None    # lanes
        self.uses_q = False
        self.fmac = False        # produces MAC/status flags
        self.clip = False
        self.lines = []          # code computing into r0..r3 (for vf/acc writes) or side effects
        self.nop = False
        self.vec = False         # result is the __m128 R (lanes stored with a blend)

def decode_upper(u):
    up = Upper()
    op = u & 0x3F
    dest = (u >> 21) & 0xF
    ft = (u >> 16) & 0x1F
    fs = (u >> 11) & 0x1F
    fd = (u >> 6) & 0x1F
    L = lanes_of(dest)

    def vs(k): return f's.vf[{fs}][{k}]'
    def vt(k): return f's.vf[{ft}][{k}]'
    def acc(k): return f's.acc[{k}]'

    VS = f'V(s.vf[{fs}])'
    VT = f'V(s.vf[{ft}])'
    ACC = 'V(s.acc)'

    def vk(kind, x):
        return {'+': f'_mm_add_ps({VS}, {x})', '-': f'_mm_sub_ps({VS}, {x})', '*': f'_mm_mul_ps({VS}, {x})',
                'madd': f'_mm_add_ps({ACC}, _mm_mul_ps({VS}, {x}))',
                'msub': f'_mm_sub_ps({ACC}, _mm_mul_ps({VS}, {x}))'}[kind]

    def fmac(target, exprs, acc_dest=False, clamp=True, flags=True, reads=(), vexpr=None):
        for reg, ln in reads:
            up.reads.append((reg, ln))
        if vexpr is not None:
            up.vec = True
            up.lines.append(f'const __m128 R = {"clampv(" + vexpr + ")" if clamp else vexpr};')
        else:
            for k in L:
                e = exprs(k)
                up.lines.append(f'const float r{k} = {"clampf(" + e + ")" if clamp else e};')
        if acc_dest:
            up.acc_write = L
        else:
            up.write = (target, L)
        up.fmac = flags and bool(L)

    if dest == 0 and op < 0x3C:
        up.nop = True
        return up

    bcops = {0x00: '+', 0x04: '-', 0x08: 'madd', 0x0C: 'msub', 0x18: '*'}
    if op < 0x10 or 0x18 <= op <= 0x1B:
        base = op & ~3
        bc = op & 3
        b = vt(bc)
        kind = bcops[base]
        rd = [(fs, L), (ft, [bc])]
        fmac(fd, None, reads=rd, vexpr=vk(kind, f'B({b})'))
        return up
    if 0x10 <= op <= 0x17:
        bc = op & 3
        fn = 'maxRaw' if op < 0x14 else 'minRaw'
        fmac(fd, lambda k: f'{fn}({vs(k)}, {vt(bc)})', clamp=False, flags=False, reads=[(fs, L), (ft, [bc])])
        return up
    qi = {0x1C: ('*', 'q'), 0x1E: ('*', 'i'), 0x20: ('+', 'q'), 0x21: ('madd', 'q'), 0x22: ('+', 'i'),
          0x23: ('madd', 'i'), 0x24: ('-', 'q'), 0x25: ('msub', 'q'), 0x26: ('-', 'i'), 0x27: ('msub', 'i')}
    if op in qi:
        kind, src = qi[op]
        sv = 's.q' if src == 'q' else 's.i'
        if src == 'q':
            up.uses_q = True
        rd = [(fs, L)]
        fmac(fd, None, reads=rd, vexpr=vk(kind, f'B({sv})'))
        return up
    if op in (0x1D, 0x1F):
        fn = 'maxRaw' if op == 0x1D else 'minRaw'
        fmac(fd, lambda k: f'{fn}({vs(k)}, s.i)', clamp=False, flags=False, reads=[(fs, L)])
        return up
    full = {0x28: lambda k: f'{vs(k)} + {vt(k)}', 0x29: lambda k: f'{acc(k)} + {vs(k)} * {vt(k)}',
            0x2A: lambda k: f'{vs(k)} * {vt(k)}', 0x2C: lambda k: f'{vs(k)} - {vt(k)}',
            0x2D: lambda k: f'{acc(k)} - {vs(k)} * {vt(k)}'}
    fullk = {0x28: '+', 0x29: 'madd', 0x2A: '*', 0x2C: '-', 0x2D: 'msub'}
    if op in full:
        fmac(fd, None, reads=[(fs, L), (ft, L)], vexpr=vk(fullk[op], VT))
        return up
    if op in (0x2B, 0x2F):
        fn = 'maxRaw' if op == 0x2B else 'minRaw'
        fmac(fd, lambda k: f'{fn}({vs(k)}, {vt(k)})', clamp=False, flags=False, reads=[(fs, L), (ft, L)])
        return up
    if op == 0x2E:  # OPMSUB
        left = [1, 2, 0]; right = [2, 0, 1]
        fmac(fd, None, reads=[(fs, [0, 1, 2]), (ft, [0, 1, 2])],
             vexpr=f'_mm_and_ps(_mm_sub_ps({ACC}, opm({VS}, {VT})), _mm_castsi128_ps(_mm_setr_epi32(-1, -1, -1, 0)))')
        return up
    if op < 0x3C:
        raise Unsupported(f'upper op {op:#x}')

    sop = (u & 3) | ((u >> 4) & 0x7C)
    if sop in (0x2F, 0x30):
        up.nop = True
        return up
    if dest == 0 and sop != 0x1F:
        up.nop = True
        return up
    if sop < 0x10 or 0x18 <= sop <= 0x1B:
        base = sop & ~3
        bc = sop & 3
        b = vt(bc)
        rd = [(fs, L), (ft, [bc])]
        kind = {0x00: '+', 0x04: '-', 0x08: 'madd', 0x0C: 'msub', 0x18: '*'}[base]
        fmac(0, None, acc_dest=True, reads=rd, vexpr=vk(kind, f'B({b})'))
        return up
    if 0x10 <= sop <= 0x13:
        scale = {0x10: '1.0f', 0x11: '0.0625f', 0x12: '(1.0f / 4096.0f)', 0x13: '(1.0f / 32768.0f)'}[sop]
        fmac(ft, None, clamp=False, flags=False, reads=[(fs, L)], vexpr=f'itofv({VS}, {scale})')
        return up
    if 0x14 <= sop <= 0x17:
        scale = {0x14: '1.0f', 0x15: '16.0f', 0x16: '4096.0f', 0x17: '32768.0f'}[sop]
        fmac(ft, None, clamp=False, flags=False, reads=[(fs, L)], vexpr=f'ftoiv({VS}, {scale})')
        return up
    if sop == 0x1D:
        fmac(ft, None, clamp=False, flags=False, reads=[(fs, L)], vexpr=f'absv({VS})')
        return up
    if sop == 0x1F:  # CLIP
        up.reads += [(fs, [0, 1, 2]), (ft, [3])]
        up.clip = True
        up.lines.append(f'const uint32_t clipf = clipFlags(s.vf[{fs}], s.vf[{ft}][3]);')
        return up
    qa = {0x1C: ('*', 'q'), 0x1E: ('*', 'i'), 0x20: ('+', 'q'), 0x21: ('madd', 'q'), 0x22: ('+', 'i'),
          0x23: ('madd', 'i'), 0x24: ('-', 'q'), 0x25: ('msub', 'q'), 0x26: ('-', 'i'), 0x27: ('msub', 'i')}
    if sop in qa:
        kind, src = qa[sop]
        sv = 's.q' if src == 'q' else 's.i'
        if src == 'q':
            up.uses_q = True
        fmac(0, None, acc_dest=True, reads=[(fs, L)], vexpr=vk(kind, f'B({sv})'))
        return up
    fulla = {0x28: lambda k: f'{vs(k)} + {vt(k)}', 0x29: lambda k: f'{acc(k)} + {vs(k)} * {vt(k)}',
             0x2A: lambda k: f'{vs(k)} * {vt(k)}', 0x2C: lambda k: f'{vs(k)} - {vt(k)}',
             0x2D: lambda k: f'{acc(k)} - {vs(k)} * {vt(k)}'}
    fullak = {0x28: '+', 0x29: 'madd', 0x2A: '*', 0x2C: '-', 0x2D: 'msub'}
    if sop in fulla:
        fmac(0, None, acc_dest=True, reads=[(fs, L), (ft, L)], vexpr=vk(fullak[sop], VT))
        return up
    if sop == 0x2E:  # OPMULA
        left = [1, 2, 0]; right = [2, 0, 1]
        fmac(0, None, acc_dest=True, reads=[(fs, [0, 1, 2]), (ft, [0, 1, 2])], vexpr=f'opm({VS}, {VT})')
        return up
    raise Unsupported(f'upper special {sop:#x}')

# ------------------------------------------------------------------------------------------ lower
class Lower:
    def __init__(self):
        self.reads = []           # VF reads (reg, lanes)
        self.vi_reads = []
        self.vf_write = None      # (reg, lanes, latency)
        self.vi_write = None      # (reg, latency)
        self.backup_writer = False  # branch after this reads pre-write VI
        self.branch = None        # dict
        self.lines = []           # code (may reference VI_R(n) for branch VI reads)
        self.q = None             # 'div' / 'waitq'
        self.efu = None           # (lat) or 'waitp'
        self.reads_p = False
        self.reads_mac = False
        self.reads_status = False
        self.reads_clip = False
        self.fsset = False
        self.fcset = False
        self.special = None       # 'xgkick'

def decode_lower(l, pc):
    lo = Lower()
    if l == 0 or l == 0x8000033C:
        return lo
    opHi = (l >> 25) & 0x7F
    dest = (l >> 21) & 0xF
    L = lanes_of(dest)
    ft = (l >> 16) & 0x1F
    fs = (l >> 11) & 0x1F
    it = (l >> 16) & 0xF
    is_ = (l >> 11) & 0xF
    id_ = (l >> 6) & 0xF
    imm11 = sx(l, 11)

    def addr(expr):
        return f'((static_cast<uint32_t>({expr}) * 16u) & mm)'

    def vi(n):
        return '0' if n == 0 else f's.vi[{n}]'

    def setvi(n, expr, lat=1, backup=False):
        if n != 0:
            lo.lines.append(f's.vi[{n}] = {expr};')
            lo.vi_write = (n, lat)
            lo.backup_writer = backup

    if opHi == 0x00:  # LQ
        lo.vi_reads.append(is_)
        if ft and L:
            src = addr(f"{vi(is_)} + ({imm11})")
            lo.lines.append(f'stb<{lmask(L)}>(s.vf[{ft}], V(reinterpret_cast<const float *>(mem + {src})));')
            lo.vf_write = (ft, L, 4)
        return lo
    if opHi == 0x01:  # SQ
        lo.vi_reads.append(it)
        lo.reads.append((fs, L))
        if L:
            dst = addr(f"{vi(it)} + ({imm11})")
            lo.lines.append(f'stb<{lmask(L)}>(reinterpret_cast<float *>(mem + {dst}), V(s.vf[{fs}]));')
        return lo
    if opHi in (0x04, 0x05):
        comp = 0 if dest & 8 else 1 if dest & 4 else 2 if dest & 2 else 3
        lo.vi_reads.append(is_)
        a = addr(f'{vi(is_)} + ({imm11})')
        if opHi == 0x04:
            if it:
                lo.lines.append(f'{{ uint32_t w; std::memcpy(&w, mem + {a} + {comp * 4}, 4); s.vi[{it}] = static_cast<int16_t>(w & 0xFFFFu); }}')
                lo.vi_write = (it, 4)
        else:
            lo.vi_reads.append(it)
            lo.lines.append(f'{{ const uint32_t v = static_cast<uint16_t>({vi(it)}); uint8_t *dst = mem + {a};')
            for k in L:
                lo.lines.append(f'  std::memcpy(dst + {k * 4}, &v, 4);')
            lo.lines.append('}')
        return lo
    if opHi in (0x08, 0x09):
        imm = (l & 0x7FF) | ((l >> 10) & 0x7800)
        op = '+' if opHi == 0x08 else '-'
        lo.vi_reads.append(is_)
        setvi(it, f'static_cast<int16_t>({vi(is_)} {op} {imm})', backup=True)
        return lo
    if opHi in (0x10, 0x12, 0x13, 0x1C):
        lo.reads_clip = True
        imm24 = l & 0xFFFFFF
        if opHi == 0x10: setvi(1, f'(s.clip & 0xFFFFFFu) == {imm24}u ? 1 : 0')
        elif opHi == 0x12: setvi(1, f'(s.clip & {imm24}u) != 0u ? 1 : 0')
        elif opHi == 0x13: setvi(1, f'((s.clip | {imm24}u) & 0xFFFFFFu) == 0xFFFFFFu ? 1 : 0')
        else: setvi(it, 'static_cast<int32_t>(s.clip & 0xFFFu)')
        return lo
    if opHi == 0x11:
        lo.fcset = True
        lo.lines.append(f'c.workingClip = {l & 0xFFFFFF}u; pushClip(c, c.workingClip);')
        return lo
    if opHi in (0x14, 0x15, 0x16, 0x17):
        imm12 = (((l >> 21) & 1) << 11) | (l & 0x7FF)
        if opHi == 0x15:
            lo.fsset = True
            lo.lines.append(f's.status = (s.status & 0x03Fu) | {imm12 & 0xFC0}u;')
            return lo
        lo.reads_status = True
        if opHi == 0x14: setvi(it, f'(s.status & 0xFFFu) == {imm12}u ? 1 : 0')
        elif opHi == 0x16: setvi(it, f'static_cast<int32_t>((s.status & 0xFFFu) & {imm12}u)')
        else: setvi(it, f'static_cast<int32_t>((s.status & 0xFFFu) | {imm12}u)')
        return lo
    if opHi in (0x18, 0x1A, 0x1B):
        lo.reads_mac = True
        lo.vi_reads.append(is_)
        m = f'static_cast<uint32_t>(static_cast<uint16_t>({vi(is_)}))'
        if opHi == 0x18: setvi(it, f'(s.mac & 0xFFFFu) == {m} ? 1 : 0')
        elif opHi == 0x1A: setvi(it, f'static_cast<int32_t>(s.mac & {m})')
        else: setvi(it, f'static_cast<int32_t>(s.mac | {m})')
        return lo
    if opHi in (0x20, 0x21):
        tgt = (pc + 8 + imm11 * 8) & PC_MASK
        lo.branch = {'kind': 'b', 'target': tgt, 'link': it if opHi == 0x21 else 0}
        return lo
    if opHi in (0x24, 0x25):
        lo.vi_reads.append(is_)
        lo.branch = {'kind': 'jr', 'reg': is_, 'link': it if opHi == 0x25 else 0}
        return lo
    if opHi in (0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F):
        tgt = (pc + 8 + imm11 * 8) & PC_MASK
        cond = {0x28: '==', 0x29: '!=', 0x2C: '<0', 0x2D: '>0', 0x2E: '<=0', 0x2F: '>=0'}[opHi]
        regs = [is_, it] if opHi in (0x28, 0x29) else [is_]
        lo.vi_reads += regs
        lo.branch = {'kind': 'cond', 'target': tgt, 'cond': cond, 'regs': regs}
        return lo
    if opHi != 0x40:
        raise Unsupported(f'lower opHi {opHi:#x}')

    funct = l & 0x3F
    if funct in (0x30, 0x31, 0x34, 0x35):
        lo.vi_reads += [is_, it]
        expr = {0x30: f'static_cast<int16_t>({vi(is_)} + {vi(it)})', 0x31: f'static_cast<int16_t>({vi(is_)} - {vi(it)})',
                0x34: f'({vi(is_)} & {vi(it)})', 0x35: f'({vi(is_)} | {vi(it)})'}[funct]
        setvi(id_, expr, backup=True)
        return lo
    if funct == 0x32:
        imm5 = sx((l >> 6) & 0x1F, 5)
        lo.vi_reads.append(is_)
        setvi(it, f'static_cast<int16_t>({vi(is_)} + ({imm5}))', backup=True)
        return lo
    if funct < 0x3C:
        raise Unsupported(f'lower funct {funct:#x}')

    f2 = (l & 3) | ((l >> 4) & 0x7C)
    if f2 in (0x30, 0x31):
        if f2 == 0x30:
            lo.reads.append((fs, L))
            src = lambda k: f's.vf[{fs}][{k}]'
        else:
            lo.reads.append((fs, [0, 1, 2, 3]))
            src = lambda k: f's.vf[{fs}][{(k + 1) & 3}]'
        if ft and L:
            v = f'V(s.vf[{fs}])' if f2 == 0x30 else f'mr32(V(s.vf[{fs}]))'
            lo.lines.append(f'stb<{lmask(L)}>(s.vf[{ft}], {v});')
            lo.vf_write = (ft, L, 4)
        return lo
    if f2 in (0x34, 0x36):  # LQI / LQD
        lo.vi_reads.append(is_)
        if f2 == 0x36 and is_:
            lo.lines.append(f's.vi[{is_}] = static_cast<int16_t>(s.vi[{is_}] - 1);')
        if ft and L:
            src = addr(f"static_cast<uint16_t>({vi(is_)})")
            lo.lines.append(f'stb<{lmask(L)}>(s.vf[{ft}], V(reinterpret_cast<const float *>(mem + {src})));')
            lo.vf_write = (ft, L, 4)
        if f2 == 0x34 and is_:
            lo.lines.append(f's.vi[{is_}] = static_cast<int16_t>(s.vi[{is_}] + 1);')
        if is_:
            lo.vi_write = (is_, 1)
            lo.backup_writer = True
        return lo
    if f2 in (0x35, 0x37):  # SQI / SQD
        lo.vi_reads.append(it)
        lo.reads.append((fs, L))
        if f2 == 0x37 and it:
            lo.lines.append(f's.vi[{it}] = static_cast<int16_t>(s.vi[{it}] - 1);')
        if L:
            dst = addr(f"static_cast<uint16_t>({vi(it)})")
            lo.lines.append(f'stb<{lmask(L)}>(reinterpret_cast<float *>(mem + {dst}), V(s.vf[{fs}]));')
        if f2 == 0x35 and it:
            lo.lines.append(f's.vi[{it}] = static_cast<int16_t>(s.vi[{it}] + 1);')
        if it:
            lo.vi_write = (it, 1)
            lo.backup_writer = True
        return lo
    if f2 in (0x38, 0x39, 0x3A):
        fsf = (l >> 21) & 3
        ftf = (l >> 23) & 3
        lo.q = 'div'
        if f2 == 0x38:
            lo.reads += [(fs, [fsf]), (ft, [ftf])]
            lo.lines.append(f'{{ uint32_t di; const float r = vdiv(s.vf[{fs}][{fsf}], s.vf[{ft}][{ftf}], di); c.qPend = true; c.qNew = r; c.qDi = di; c.qReady = c.cyc + 7u; }}')
        elif f2 == 0x39:
            lo.reads += [(ft, [ftf])]
            lo.lines.append(f'{{ const float v = clampf(s.vf[{ft}][{ftf}]); c.qPend = true; c.qNew = std::sqrt(std::fabs(v)); c.qDi = v < 0.0f ? 0x10u : 0u; c.qReady = c.cyc + 7u; }}')
        else:
            lo.reads += [(fs, [fsf]), (ft, [ftf])]
            lo.lines.append(f'{{ uint32_t di; const float r = vrsqrt(s.vf[{fs}][{fsf}], s.vf[{ft}][{ftf}], di); c.qPend = true; c.qNew = r; c.qDi = di; c.qReady = c.cyc + 13u; }}')
        return lo
    if f2 == 0x3B:
        lo.q = 'waitq'
        return lo
    if f2 == 0x3C:  # MTIR
        comp = (l >> 21) & 3
        lo.reads.append((fs, [comp]))
        setvi(it, f'static_cast<int16_t>(fbits(s.vf[{fs}][{comp}]) & 0xFFFFu)', backup=True)
        return lo
    if f2 == 0x3D:  # MFIR
        lo.vi_reads.append(is_)
        if ft and L:
            lo.lines.append(f'{{ const float v = ifloat(static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>({vi(is_)}))));')
            for k in L:
                lo.lines.append(f'  s.vf[{ft}][{k}] = v;')
            lo.lines.append('}')
            lo.vf_write = (ft, L, 4)
        return lo
    if f2 in (0x3E, 0x3F):
        comp = 0 if dest & 8 else 1 if dest & 4 else 2 if dest & 2 else 3
        lo.vi_reads.append(is_)
        a = addr(f'static_cast<uint16_t>({vi(is_)})')
        if f2 == 0x3E:
            if it:
                lo.lines.append(f'{{ uint32_t w; std::memcpy(&w, mem + {a} + {comp * 4}, 4); s.vi[{it}] = static_cast<int16_t>(w & 0xFFFFu); }}')
                lo.vi_write = (it, 4)
        else:
            lo.vi_reads.append(it)
            lo.lines.append(f'{{ const uint32_t v = static_cast<uint16_t>({vi(it)}); uint8_t *dst = mem + {a};')
            for k in L:
                lo.lines.append(f'  std::memcpy(dst + {k * 4}, &v, 4);')
            lo.lines.append('}')
        return lo
    if f2 in (0x40, 0x41):
        if f2 == 0x40:
            lo.lines.append('s.r = (((s.r << 1) ^ ((s.r >> 4) & 1u) ^ ((s.r >> 22) & 1u)) & 0x007FFFFFu) | 0x3F800000u;')
        if ft and L:
            lo.lines.append('{ const float v = ifloat(s.r);')
            for k in L:
                lo.lines.append(f'  s.vf[{ft}][{k}] = v;')
            lo.lines.append('}')
            lo.vf_write = (ft, L, 4)
        return lo
    if f2 in (0x42, 0x43):
        comp = (l >> 21) & 3
        lo.reads.append((fs, [comp]))
        if f2 == 0x42:
            lo.lines.append(f's.r = 0x3F800000u | (fbits(s.vf[{fs}][{comp}]) & 0x007FFFFFu);')
        else:
            lo.lines.append(f's.r = 0x3F800000u | ((s.r ^ fbits(s.vf[{fs}][{comp}])) & 0x007FFFFFu);')
        return lo
    if f2 == 0x64:  # MFP
        lo.reads_p = True
        if ft and L:
            for k in L:
                lo.lines.append(f's.vf[{ft}][{k}] = s.p;')
            lo.vf_write = (ft, L, 4)
        return lo
    if f2 in (0x68, 0x69):
        setvi(it, 'static_cast<int32_t>(s.top & 0x3FFu)' if f2 == 0x68 else 'static_cast<int32_t>(s.itop & 0x3FFu)')
        return lo
    if f2 == 0x6C:
        lo.vi_reads.append(is_)
        lo.lines.append(f'c.xgkick(c.user, static_cast<uint16_t>({vi(is_)}));')
        return lo
    efu_lat = {0x70: 11, 0x71: 18, 0x72: 18, 0x73: 24, 0x74: 54, 0x75: 54, 0x76: 12, 0x77: 18, 0x78: 12,
               0x79: 29, 0x7A: 12, 0x7C: 54, 0x7D: 44}
    if f2 in efu_lat:
        comp = (l >> 21) & 3
        lo.reads.append((fs, [0, 1, 2, 3]))
        lat = efu_lat[f2]
        lo.efu = lat
        lo.lines.append(f'c.pPend = true; c.pNew = efu({f2:#x}u, s.vf[{fs}], {comp}u); c.pReady = c.cyc + {lat}u; c.efuFree = c.cyc + {lat - 1}u;')
        return lo
    if f2 == 0x7B:
        lo.efu = 'waitp'
        return lo
    raise Unsupported(f'lower special {f2:#x}')

# ------------------------------------------------------------------------------------------ pairs
class Pair:
    pass

def decode_pair(code, pc):
    p = Pair()
    p.pc = pc
    p.lower = int.from_bytes(code[pc:pc + 4], 'little')
    p.upper = int.from_bytes(code[pc + 4:pc + 8], 'little')
    u = p.upper
    p.ibit = bool(u & 0x80000000); p.ebit = bool(u & 0x40000000)
    p.dbit = bool(u & 0x10000000); p.tbit = bool(u & 0x08000000)
    p.error = None
    try:
        p.up = decode_upper(u)
        p.lo = Lower() if p.ibit else decode_lower(p.lower, pc)
    except Unsupported as e:
        p.error = str(e)
    return p

def is_plain(p):
    return p.error is None and p.lo.branch is None and not p.ebit and not p.dbit and not p.tbit

# ------------------------------------------------------------------------------------------ codegen
class Gen:
    def __init__(self, code, entries, name):
        self.code = code
        self.entries = sorted(set(entries))
        self.name = name
        self.pairs = {}
        self.labels = set(self.entries)
        self.reach = set()
        self.fallback = set()     # pcs emitted as interpreter fallback stubs
        self.resume_points = set()
        self.sim_reset()

    def sim_reset(self):
        self.sim = {'t': 0, 'vf': {}, 'vi': {}, 'issue': 0}

    def pair(self, pc):
        pc &= PC_MASK
        if pc not in self.pairs:
            self.pairs[pc] = decode_pair(self.code, pc)
        return self.pairs[pc]

    def discover(self):
        work = list(self.entries)
        while work:
            pc = work.pop()
            if pc in self.reach:
                continue
            self.reach.add(pc)
            p = self.pair(pc)
            if p.error or p.dbit or p.tbit:
                self.fallback.add(pc)
                continue
            nxt = (pc + 8) & PC_MASK
            if p.lo.branch or p.ebit:
                slot = self.pair(nxt)
                if not is_plain(slot) or (p.lo.branch and p.ebit):
                    self.fallback.add(pc)
                    continue
                if p.ebit:
                    # MSCNT continues right after the E-bit delay slot.
                    resume = (pc + 16) & PC_MASK
                    self.labels.add(resume); work.append(resume)
                    continue
                b = p.lo.branch
                after = (pc + 16) & PC_MASK
                if b['kind'] in ('b', 'cond'):
                    self.labels.add(b['target']); work.append(b['target'])
                if b['kind'] == 'cond' or b.get('link'):
                    self.labels.add(after); work.append(after)
                continue
            work.append(nxt)

        # Fallthrough edges into a pc that is not emitted right after need a label.
        order = sorted(self.reach)
        self.order = order

    def succs(self, pc):
        """Pair-level successors (None = unknown / leaves the program)."""
        p = self.pairs.get(pc)
        if p is None or pc in self.fallback:
            return [None]
        nxt = (pc + 8) & PC_MASK
        if p.lo.branch:
            return [('slot', pc)]
        if p.ebit:
            return [('eslot', pc)]
        return [nxt]

    def compute_mac_live(self):
        """An FMAC's MAC result can only be observed if a MAC read is reachable from it through
        fewer than four other flag-producing FMACs (the fourth newer one is always ready first)."""
        def produces(pc):
            p = self.pairs.get(pc)
            return p is not None and p.error is None and p.up.fmac
        def reads(pc):
            p = self.pairs.get(pc)
            return p is not None and p.error is None and p.lo.reads_mac
        def expand(node):
            # node: pc or ('slot', branch_pc) / ('eslot', ebit_pc)
            if node is None:
                return [None]
            if isinstance(node, tuple):
                kind, bpc = node
                slot = (bpc + 8) & PC_MASK
                return [('exec', slot, kind, bpc)]
            return [('exec', node, None, None)]
        def after(kind, bpc, slotpc):
            if kind == 'eslot':
                return []
            b = self.pairs[bpc].lo.branch
            out = []
            if b['kind'] in ('b', 'cond'):
                out.append(b['target'])
            if b['kind'] == 'cond':
                out.append((bpc + 16) & PC_MASK)
            if b['kind'] == 'jr':
                out.append(None)
            return out
        live = set()
        for start in self.reach:
            if not produces(start):
                continue
            # BFS over (execution item, fmac count)
            seen = set()
            work = [(n, 0) for n in self.succs(start)]
            found = False
            while work and not found:
                node, k = work.pop()
                for item in expand(node):
                    if item is None:
                        found = True
                        break
                    _, pc, kind, bpc = item
                    key = (pc, kind, bpc, k)
                    if key in seen:
                        continue
                    seen.add(key)
                    if reads(pc):
                        found = True
                        break
                    k2 = k + (1 if produces(pc) else 0)
                    if k2 >= 4:
                        continue
                    if kind is None:
                        nxts = self.succs(pc)
                    else:
                        nxts = after(kind, bpc, pc)
                    for n in nxts:
                        work.append((n, k2))
            if found:
                live.add(start)
        self.mac_live = live

    def flags_needed(self):
        mac = any(self.pairs[pc].error is None and (self.pairs[pc].lo.reads_mac or self.pairs[pc].lo.reads_status)
                  for pc in self.reach)
        clip = any(self.pairs[pc].error is None and self.pairs[pc].lo.reads_clip for pc in self.reach)
        return mac, clip

    # Emits one pair. vi_override: {reg: c++ expr} for branch reads of backed-up VI values.
    def emit_pair(self, out, p, ind, vi_override=None, want_mac=False, want_clip=False):
        up, lo = p.up, p.lo
        out.append(f'{ind}// {p.pc:04x}: {p.upper:08x} {p.lower:08x}')
        if TRACE_HASH and TRACE_HASH in self.name:
            out.append(f'{ind}VU1N_PC({p.pc:#x}u);')
        # --- stalls: VF/VI hazards are resolved statically within the basic block (registers are
        # assumed ready at block entry); only the latency-visible Q/P pipelines stay dynamic.
        sim = self.sim
        need = sim['t']
        for reg, ln in up.reads + lo.reads:
            if reg == 0:
                continue
            for k in ln:
                need = max(need, sim['vf'].get((reg, k), 0))
        for r in set(lo.vi_reads):
            if r:
                need = max(need, sim['vi'].get(r, 0))
        stall = need - sim['t']
        sim['issue'] = need
        if stall > 0:
            out.append(f'{ind}c.cyc += {stall}u;')
        pre = []
        if lo.q in ('div', 'waitq'):
            pre.append('if (c.qPend && c.cyc < c.qReady) c.cyc = c.qReady;')
        if isinstance(lo.efu, int):
            pre.append('if (c.cyc < c.efuFree) c.cyc = c.efuFree;')
        if lo.efu == 'waitp':
            pre.append('if (c.pPend && c.cyc < c.pReady) c.cyc = c.pReady;')
        for ln in pre:
            out.append(f'{ind}{ln}')
        if up.uses_q or lo.q or lo.reads_status:
            out.append(f'{ind}commitQ(c);')
        if lo.reads_p or isinstance(lo.efu, int) or lo.efu == 'waitp':
            if isinstance(lo.efu, int):
                out.append(f'{ind}if (c.pPend) {{ c.s->p = c.pNew; c.pPend = false; }}')
            out.append(f'{ind}commitP(c);')
        if lo.reads_mac or lo.reads_status:
            out.append(f'{ind}commitMac(c);')
        if lo.reads_clip:
            out.append(f'{ind}commitClip(c);')

        # --- upper
        shadow = False
        if up.write and up.write[0] != 0:
            reg = up.write[0]
            if any(r == reg for r, _ in lo.reads) or (lo.vf_write and lo.vf_write[0] == reg):
                shadow = True
        out.append(f'{ind}{{')
        i2 = ind + '  '
        for ln in up.lines:
            out.append(i2 + ln)
        if up.fmac and want_mac and p.pc in self.mac_live:
            lanes = up.write[1] if up.write else up.acc_write
            if up.vec:
                out.append(f'{i2}macFlagsV(c, R, {lmask(lanes)}u);')
            else:
                vals = ', '.join(f'r{k}' if k in lanes else '0.0f' for k in range(4))
                dmask = sum(1 << (3 - k) for k in lanes)
                out.append(f'{i2}macFlags(c, {vals}, {dmask}u);')
        if up.clip and want_clip and not lo.fcset:
            out.append(f'{i2}c.workingClip = ((c.workingClip << 6) | (clipf & 0x3Fu)) & 0xFFFFFFu; pushClip(c, c.workingClip);')
        def store_upper():
            reg = up.write[0]
            if up.vec:
                out.append(f'{i2}stb<{lmask(up.write[1])}>(s.vf[{reg}], R);')
            else:
                for k in up.write[1]:
                    out.append(f'{i2}s.vf[{reg}][{k}] = r{k};')
        if up.acc_write:
            if up.vec:
                out.append(f'{i2}stb<{lmask(up.acc_write)}>(s.acc, R);')
            else:
                for k in up.acc_write:
                    out.append(f'{i2}s.acc[{k}] = r{k};')
        if up.write and up.write[0] != 0 and not shadow:
            store_upper()

        # --- lower
        if p.ibit:
            out.append(f'{i2}s.i = ifloat({p.lower:#010x}u);')
        else:
            for ln in lo.lines:
                out.append(i2 + ln)
        if shadow:
            store_upper()
        # --- ready times (static)
        t0 = sim['issue']
        if up.write and up.write[0] != 0:
            for k in up.write[1]:
                sim['vf'][(up.write[0], k)] = t0 + 4
        if lo.vf_write and lo.vf_write[0] != 0 and not (shadow and up.write and lo.vf_write[0] == up.write[0]):
            for k in lo.vf_write[1]:
                sim['vf'][(lo.vf_write[0], k)] = t0 + lo.vf_write[2]
        if lo.vi_write:
            sim['vi'][lo.vi_write[0]] = t0 + lo.vi_write[1]
        sim['t'] = t0 + 1
        out.append(f'{ind}}}')
        out.append(f'{ind}++c.cyc;')

    def branch_code(self, out, p, ind, override, want_mac, want_clip):
        """Branch pair p, its delay slot, then the jump."""
        b = p.lo.branch
        def rv(r):
            if r == 0:
                return '0'
            if override and r in override:
                return override[r]
            return f's.vi[{r}]'
        # the branch pair itself (upper op + condition)
        self.emit_pair(out, p, ind, want_mac=want_mac, want_clip=want_clip)
        out.append(f'{ind}{{')
        i2 = ind + '  '
        if b['kind'] == 'cond':
            regs = b['regs']
            a = f'static_cast<int16_t>({rv(regs[0])})'
            if b['cond'] in ('==', '!='):
                cond = f'{a} {b["cond"]} static_cast<int16_t>({rv(regs[1])})'
            else:
                cond = f'{a} {b["cond"][0:-1]} 0'
            out.append(f'{i2}const bool taken = {cond};')
        elif b['kind'] == 'jr':
            out.append(f'{i2}const uint32_t jt = (static_cast<uint32_t>(static_cast<uint16_t>({rv(b["reg"])})) * 8u) & {PC_MASK:#x}u;')
        if b.get('link'):
            out.append(f'{i2}s.vi[{b["link"]}] = {((p.pc + 16) // 8)};')
        slot = self.pair((p.pc + 8) & PC_MASK)
        self.emit_pair(out, slot, i2, want_mac=want_mac, want_clip=want_clip)
        after = (p.pc + 16) & PC_MASK
        if b['kind'] == 'b':
            tgt = b['target']
            if tgt <= p.pc:
                out.append(f'{i2}if (c.cyc >= c.budget) {{ s.pc = {tgt:#x}u; return kFallback; }}')
            out.append(f'{i2}goto L_{tgt:04x};')
        elif b['kind'] == 'cond':
            tgt = b['target']
            if tgt <= p.pc:
                out.append(f'{i2}if (taken && c.cyc >= c.budget) {{ s.pc = {tgt:#x}u; return kFallback; }}')
            out.append(f'{i2}if (taken) goto L_{tgt:04x};')
            out.append(f'{i2}goto L_{after:04x};')
        else:
            out.append(f'{i2}if (c.cyc >= c.budget) {{ s.pc = jt; return kFallback; }}')
            out.append(f'{i2}pcv = jt; goto dispatch;')
        out.append(f'{ind}}}')

    def region_succs(self, pc):
        """Successors used for region partitioning (delay slots are emitted inline)."""
        p = self.pairs.get(pc)
        if p is None or pc in self.fallback:
            return []
        nxt = (pc + 8) & PC_MASK
        if p.ebit:
            return []
        if p.lo.branch:
            b = p.lo.branch
            out = []
            if b['kind'] in ('b', 'cond'):
                out.append(b['target'])
            if b['kind'] == 'cond' or b.get('link'):
                out.append((pc + 16) & PC_MASK)
            return out
        return [nxt]

    def emit_region(self, region, want_mac, want_clip):
        """Body lines for the pcs in `region` (sorted emission, gotos may leave the region)."""
        body = []
        emitted = set()
        order = sorted(region)
        i = 0
        fell_through = False
        while i < len(order):
            pc = order[i]
            p = self.pairs[pc]
            nxt = (pc + 8) & PC_MASK
            if pc in self.labels or not fell_through:
                self.sim_reset()
            fell_through = False
            body.append(f'L_{pc:04x}:')
            emitted.add(pc)
            if pc in self.fallback:
                body.append(f'    s.pc = {pc:#x}u; return kFallback;')
                i += 1
                continue
            if p.lo.branch:
                self.branch_code(body, p, '    ', None, want_mac, want_clip)
                i += 1
                continue
            if p.ebit:
                self.emit_pair(body, p, '    ', want_mac=want_mac, want_clip=want_clip)
                self.emit_pair(body, self.pair(nxt), '    ', want_mac=want_mac, want_clip=want_clip)
                body.append(f'    s.pc = {(pc + 16) & PC_MASK:#x}u; return kEnded;')
                i += 1
                continue
            q = self.pair(nxt)
            if p.lo.backup_writer and p.lo.vi_write and nxt in self.reach and nxt not in self.fallback and \
                    q.error is None and q.lo.branch:
                reg = p.lo.vi_write[0]
                reads = q.lo.branch.get('regs', []) + ([q.lo.branch['reg']] if q.lo.branch['kind'] == 'jr' else [])
                if reg in reads:
                    body.append(f'    {{ const int32_t bk = s.vi[{reg}];')
                    self.emit_pair(body, p, '      ', want_mac=want_mac, want_clip=want_clip)
                    self.branch_code(body, q, '      ', {reg: 'bk'}, want_mac, want_clip)
                    body.append('    }')
                    i += 1
                    continue
            self.emit_pair(body, p, '    ', want_mac=want_mac, want_clip=want_clip)
            if i + 1 < len(order) and order[i + 1] == nxt:
                fell_through = True
                i += 1
                continue
            body.append(f'    goto L_{nxt:04x};')
            i += 1
        return body, emitted

    def generate(self):
        import re
        self.discover()
        want_mac, want_clip = self.flags_needed()
        self.compute_mac_live() if want_mac else setattr(self, 'mac_live', set())

        # Region entries: MSCAL/MSCNT/JR entry points and post-E-bit resume points.
        entries = set(self.entries)
        for pc in self.reach:
            p = self.pairs[pc]
            if p.error is None and p.ebit:
                r = (pc + 16) & PC_MASK
                if r in self.reach:
                    entries.add(r)
        regions = []
        for e in sorted(entries):
            if e not in self.reach:
                continue
            seen = {e}
            work = [e]
            while work:
                pc = work.pop()
                for n in self.region_succs(pc):
                    if n in seen or (n in entries and n != e) or n not in self.reach:
                        continue
                    seen.add(n)
                    work.append(n)
            regions.append((e, seen))

        out = []
        label_owner = {}
        region_names = []
        self.region_table = []  # (entry pc, region fn, lo, hi, code hash) -> RegionEntry
        for idx, (e, region) in enumerate(regions):
            rname = f'{self.name}_r{idx}'
            region_names.append(rname)
            body, emitted = self.emit_region(region, want_mac, want_clip)
            # Leaving the region: hand the pc to the image trampoline.
            def exit_goto(m):
                t = int(m.group(1), 16)
                if t in emitted:
                    return m.group(0)
                return f'{{ s.pc = {t:#x}u; return kContinue; }}'
            body = [re.sub(r'goto L_([0-9a-f]{4});', exit_goto, ln) for ln in body]
            used = {e}
            for ln in body:
                for m in re.finditer(r'goto L_([0-9a-f]{4})', ln):
                    used.add(int(m.group(1), 16))
            # JR targets and entries need dispatch cases even without a local goto.
            used |= (self.labels & emitted)
            body = [ln for ln in body if not (ln.startswith('L_') and int(ln[2:6], 16) not in used)]
            cases = sorted(l for l in emitted if l in used)
            for l in cases:
                label_owner.setdefault(l, rname)
            # Code the region was compiled from: its pairs plus a delay slot after the last one.
            lo, hi = min(emitted), min(max(emitted) + 16, CODE_SIZE)
            rh = code_range_hash(self.code, lo, hi)
            for l in cases:
                self.region_table.append((l, rname, lo, hi, rh))
            fn = [f'static int {rname}(Ctx &c, uint32_t pc)', '{', '    VU1State &s = *c.s;',
                  '    uint8_t *const mem = c.mem;', '    const uint32_t mm = c.memMask;', '    uint32_t pcv = pc;',
                  '    (void)mem; (void)mm;', 'dispatch:', '    switch (pcv)', '    {']
            fn += [f'    case {l:#x}: goto L_{l:04x};' for l in cases]
            fn += ['    default: s.pc = pcv; return kContinue;', '    }']
            fn += body
            fn.append('}')
            if '--no-local-cyc' not in sys.argv:
                helpers = ('commitQ(c)', 'commitP(c)', 'commitMac(c)', 'commitClip(c)', 'macFlags(c,', 'macFlagsV(c,',
                           'pushClip(c,')
                fixed = []
                for ln in fn:
                    ln = ln.replace('c.cyc', 'cyc')
                    if any(h in ln for h in helpers):
                        stripped = ln.lstrip()
                        ln = ln[:len(ln) - len(stripped)] + 'c.cyc = cyc; ' + stripped
                    for kind in ('kFallback', 'kEnded', 'kContinue'):
                        ln = ln.replace(f'return {kind};', f'c.cyc = cyc; return {kind};')
                    fixed.append(ln)
                fn = fixed
                fn.insert(fn.index('    uint32_t pcv = pc;') + 1, '    uint64_t cyc = c.cyc;')
            if '--no-vf-cache' not in sys.argv:
                fn = vf_cache(fn)
            out += fn
            out.append('')

        # Trampoline: run regions until the program ends or needs the interpreter.
        out.append(f'static int {self.name}(Ctx &c, uint32_t pc)')
        out.append('{')
        out.append('    for (uint32_t hops = 0; hops < 1000000u; ++hops)')
        out.append('    {')
        out.append('        int r;')
        out.append('        switch (pc)')
        out.append('        {')
        for l in sorted(label_owner):
            out.append(f'        case {l:#x}: r = {label_owner[l]}(c, pc); break;')
        out.append('        default: c.s->pc = pc; return kFallback;')
        out.append('        }')
        out.append('        if (r != kContinue)')
        out.append('            return r;')
        out.append('        if (c.cyc >= c.budget)')
        out.append('            return kFallback;')
        out.append('        pc = c.s->pc;')
        out.append('    }')
        out.append('    c.s->pc = pc;')
        out.append('    return kFallback;')
        out.append('}')
        self.region_count = len(regions)
        return out, want_mac, want_clip


TRACE_HASH = os.environ.get('VU1_TRACE_HASH', '')

# Register cache: inside one region function, VF1..VF31 and ACC live in __m128 locals instead of
# VU1State memory (every op used to load/store s.vf[], and stores to VU data memory made the compiler
# reload them). Values are written back to s.vf / s.acc before every return. VF0 stays in memory.
_RE_STB_VF = re.compile(r'^(\s*)stb<(\d+)>\(s\.vf\[(\d+)\], (.*)\);$')
_RE_STB_ACC = re.compile(r'^(\s*)stb<(\d+)>\(s\.acc, (.*)\);$')
_RE_SET_LANE = re.compile(r'^(\s*)s\.vf\[(\d+)\]\[(\d)\] = (.*);$')

def vf_cache(fn):
    used = set()
    acc = False
    for ln in fn:
        for m in re.finditer(r's\.vf\[(\d+)\]', ln):
            n = int(m.group(1))
            if n != 0:
                used.add(n)
        if 's.acc' in ln:
            acc = True
    if not used and not acc:
        return fn

    def sub_reads(ln):
        ln = re.sub(r'V\(s\.vf\[([1-9]\d*)\]\)', r'vf\1', ln)
        ln = ln.replace('V(s.acc)', 'accv')
        ln = re.sub(r'B\(s\.vf\[([1-9]\d*)\]\[(\d)\]\)', r'vbc<\2>(vf\1)', ln)
        ln = re.sub(r's\.vf\[([1-9]\d*)\]\[(\d)\]', r'vget<\2>(vf\1)', ln)
        ln = re.sub(r's\.vf\[([1-9]\d*)\]', r'VTmp(vf\1)', ln)  # pointer uses (clipFlags, efu)
        return ln

    out = []
    for ln in fn:
        m = _RE_STB_VF.match(ln)
        if m and m.group(3) != '0':
            ind, imm, n, expr = m.groups()
            out.append(f'{ind}vf{n} = vsel<{imm}>(vf{n}, {sub_reads(expr)});')
            continue
        m = _RE_STB_ACC.match(ln)
        if m:
            ind, imm, expr = m.groups()
            out.append(f'{ind}accv = vsel<{imm}>(accv, {sub_reads(expr)});')
            continue
        m = _RE_SET_LANE.match(ln)
        if m and m.group(2) != '0':
            ind, n, k, expr = m.groups()
            out.append(f'{ind}vf{n} = vsetl<{k}>(vf{n}, {sub_reads(expr)});')
            continue
        out.append(sub_reads(ln))
    # Declarations + write-back helper right after the prologue.
    regs = sorted(used)
    decl = [f'    __m128 vf{n} = V(s.vf[{n}]);' for n in regs]
    if acc:
        decl.append('    __m128 accv = V(s.acc);')
    stores = ' '.join(f'_mm_storeu_ps(s.vf[{n}], vf{n});' for n in regs)
    if acc:
        stores += ' _mm_storeu_ps(s.acc, accv);'
    decl.append(f'    auto wb = [&]() {{ {stores} }};')
    at = out.index('    (void)mem; (void)mm;') + 1
    out[at:at] = decl
    res = []
    for ln in out:
        for kind in ('kFallback', 'kEnded', 'kContinue'):
            ln = ln.replace(f'return {kind};', f'{{ wb(); return {kind}; }}')
        res.append(ln)
    return res

VF_CACHE_HELPERS = [
    'template <int IMM> VU1N_INLINE __m128 vsel(__m128 old, __m128 v)',
    '{',
    '    if constexpr (IMM == 0xF) return v; else return _mm_blend_ps(old, v, IMM);',
    '}',
    'template <int K> VU1N_INLINE __m128 vbc(__m128 v) { return _mm_shuffle_ps(v, v, _MM_SHUFFLE(K, K, K, K)); }',
    'template <int K> VU1N_INLINE float vget(__m128 v) { return _mm_cvtss_f32(_mm_shuffle_ps(v, v, _MM_SHUFFLE(K, K, K, K))); }',
    'template <int K> VU1N_INLINE __m128 vsetl(__m128 v, float x) { return _mm_insert_ps(v, _mm_set_ss(x), K << 4); }',
    'struct VTmp',
    '{',
    '    alignas(16) float f[4];',
    '    explicit VTmp(__m128 v) { _mm_store_ps(f, v); }',
    '    operator const float *() const { return f; }',
    '};',
]

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    dump = args[0]
    outp = args[1]
    entries = collections.defaultdict(set)
    for line in open(os.path.join(dump, 'vu1_entries.txt')):
        parts = line.split()
        if len(parts) == 2:
            entries[parts[0]].add(int(parts[1], 16))
    lines = ['// Generated by tools/vu1_recomp.py -- statically recompiled VU1 microcode. Do not edit.',
             '#include "runtime/vu1_native.h"',
             '#include <vector>',
             '',
             'namespace ps2vu1n',
             '{',
             '#if defined(_MSC_VER)',
             '#pragma warning(disable : 4102) // unreferenced label',
             '#endif',
             'extern std::vector<uint32_t> *g_vu1nPcTrace; // debug: VU1_TRACE_HASH lockstep trace',
             '#define VU1N_PC(pcv_) do { if (g_vu1nPcTrace) g_vu1nPcTrace->push_back(pcv_); } while (0)'] + VF_CACHE_HELPERS
    table = []
    region_rows = []
    stats = []
    for h, ents in sorted(entries.items()):
        path = os.path.join(dump, f'vu1_{h}.bin')
        if not os.path.exists(path):
            continue
        code = open(path, 'rb').read()
        if len(code) < CODE_SIZE:
            code += bytes(CODE_SIZE - len(code))
        name = f'vu1_{h}'
        g = Gen(code, ents, name)
        fn, want_mac, want_clip = g.generate()
        lines += fn
        lines.append('')
        table.append((h, name))
        region_rows += g.region_table
        nf = sum(1 for pc in g.reach if g.pairs[pc].error is None and g.pairs[pc].up.fmac)
        stats.append(f'{h}: entries={len(ents)} regions={g.region_count} pairs={len(g.reach)} fallback={len(g.fallback)} mac={want_mac} live={len(g.mac_live)}/{nf} clip={want_clip}'
                     + ('' if not g.fallback else ' [' + ', '.join(f"{pc:04x}:{g.pairs[pc].error or 'ctl'}" for pc in sorted(g.fallback)[:6]) + ']'))
    lines.append('static const ProgramEntry kPrograms[] = {')
    for h, name in table:
        lines.append(f'    {{0x{h}ull, {name}}},')
    if not table:
        lines.append('    {0ull, nullptr},')
    lines.append('};')
    lines.append('')
    lines.append('static const RegionEntry kRegions[] = {')
    for pc, rname, lo, hi, rh in region_rows:
        lines.append(f'    {{{pc:#x}u, {lo:#x}u, {hi:#x}u, 0x{rh:016x}ull, {rname}}},')
    if not region_rows:
        lines.append('    {0u, 0u, 0u, 0ull, nullptr},')
    lines.append('};')
    lines.append('')
    lines.append('const RegionEntry *regions(uint32_t &count)')
    lines.append('{')
    lines.append(f'    count = {len(region_rows)}u;')
    lines.append('    return kRegions;')
    lines.append('}')
    lines.append('')
    lines.append('const ProgramEntry *programs(uint32_t &count)')
    lines.append('{')
    lines.append(f'    count = {len(table)}u;')
    lines.append('    return kPrograms;')
    lines.append('}')
    lines.append('}')
    os.makedirs(os.path.dirname(outp), exist_ok=True)
    open(outp, 'w', newline='\n').write('\n'.join(lines) + '\n')
    print('\n'.join(stats))
    print(f'{len(table)} images -> {outp}')

if __name__ == '__main__':
    main()
