# VU1 micro-program disassembler (mnemonic listing) for images dumped with PS2X_VU_DUMP.
# Usage: python tools/vu1_disasm.py <vu1_<hash>.bin | ELF address of a VIF MPG upload chain> [start_pc] [end_pc]
#   pc values are byte addresses in micro memory (8 bytes per instruction pair).
import struct, sys

XYZW = 'xyzw'


def dest(d):
    return ''.join(c for k, c in enumerate(XYZW) if d & (8 >> k))


def sx(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


UPPER = {0x1C: 'mulq', 0x1D: 'maxi', 0x1E: 'muli', 0x1F: 'minii', 0x20: 'addq', 0x21: 'maddq', 0x22: 'addi',
         0x23: 'maddi', 0x24: 'subq', 0x25: 'msubq', 0x26: 'subi', 0x27: 'msubi', 0x28: 'add', 0x29: 'madd',
         0x2A: 'mul', 0x2B: 'max', 0x2C: 'sub', 0x2D: 'msub', 0x2E: 'opmsub', 0x2F: 'mini'}
BC_OPS = ['add', 'sub', 'madd', 'msub', 'max', 'mini', 'mul']
SPECIAL_U = {0x10: 'itof0', 0x11: 'itof4', 0x12: 'itof12', 0x13: 'itof15', 0x14: 'ftoi0', 0x15: 'ftoi4',
             0x16: 'ftoi12', 0x17: 'ftoi15', 0x1C: 'mulaq', 0x1D: 'abs', 0x1E: 'mulai', 0x1F: 'clip',
             0x20: 'addaq', 0x21: 'maddaq', 0x22: 'addai', 0x23: 'maddai', 0x24: 'subaq', 0x25: 'msubaq',
             0x26: 'subai', 0x27: 'msubai', 0x28: 'adda', 0x29: 'madda', 0x2A: 'mula', 0x2C: 'suba',
             0x2D: 'msuba', 0x2E: 'opmula', 0x2F: 'nop'}
BCA_OPS = ['adda', 'suba', 'madda', 'msuba']


def upper(u):
    d = dest((u >> 21) & 0xF)
    ft, fs, fd = (u >> 16) & 31, (u >> 11) & 31, (u >> 6) & 31
    op = u & 0x3F
    flags = ''.join(f for f, b in (('[I]', 31), ('[E]', 30), ('[M]', 29), ('[D]', 28), ('[T]', 27)) if u >> b & 1)
    if op < 0x1C:
        bc = XYZW[op & 3]
        name = BC_OPS[op >> 2] + bc
        return f'{name}.{d} vf{fd:02}, vf{fs:02}, vf{ft:02}{bc}', flags
    if op < 0x3C:
        name = UPPER.get(op, f'?u{op:02x}')
        src = 'q' if name.endswith('q') else ('i' if name.endswith('i') else f'vf{ft:02}')
        if name in ('add', 'madd', 'mul', 'max', 'sub', 'msub', 'opmsub', 'mini'):
            return f'{name}.{d} vf{fd:02}, vf{fs:02}, vf{ft:02}', flags
        return f'{name}.{d} vf{fd:02}, vf{fs:02}, {src}', flags
    sop = ((u >> 6) & 0x1F) << 2 | (u & 3)
    if sop < 0x10:
        bc = XYZW[sop & 3]
        return f'{BCA_OPS[sop >> 2]}{bc}.{d} acc, vf{fs:02}, vf{ft:02}{bc}', flags
    if 0x18 <= sop <= 0x1B:
        bc = XYZW[sop & 3]
        return f'mula{bc}.{d} acc, vf{fs:02}, vf{ft:02}{bc}', flags
    name = SPECIAL_U.get(sop, f'?us{sop:02x}')
    if name == 'nop':
        return 'nop', flags
    if name.startswith(('itof', 'ftoi', 'abs')):
        return f'{name}.{d} vf{ft:02}, vf{fs:02}', flags
    if name == 'clip':
        return f'clipw.xyz vf{fs:02}, vf{ft:02}w', flags
    src = 'q' if name.endswith('q') else ('i' if name.endswith('i') else f'vf{ft:02}')
    return f'{name}.{d} acc, vf{fs:02}, {src}', flags


LOWER = {0x00: 'lq', 0x01: 'sq', 0x04: 'ilw', 0x05: 'isw', 0x08: 'iaddiu', 0x09: 'isubiu', 0x10: 'fceq',
         0x11: 'fcset', 0x12: 'fcand', 0x13: 'fcor', 0x14: 'fseq', 0x15: 'fsset', 0x16: 'fsand', 0x17: 'fsor',
         0x18: 'fmeq', 0x1A: 'fmand', 0x1B: 'fmor', 0x1C: 'fcget', 0x20: 'b', 0x21: 'bal', 0x24: 'jr',
         0x25: 'jalr', 0x28: 'ibeq', 0x29: 'ibne', 0x2C: 'ibltz', 0x2D: 'ibgtz', 0x2E: 'iblez', 0x2F: 'ibgez'}
SPECIAL_L = {0x30: 'move', 0x31: 'mr32', 0x34: 'lqi', 0x35: 'sqi', 0x36: 'lqd', 0x37: 'sqd', 0x38: 'div',
             0x39: 'sqrt', 0x3A: 'rsqrt', 0x3B: 'waitq', 0x3C: 'mtir', 0x3D: 'mfir', 0x3E: 'ilwr', 0x3F: 'iswr',
             0x40: 'rnext', 0x41: 'rget', 0x42: 'rinit', 0x43: 'rxor', 0x64: 'mfp', 0x68: 'xtop', 0x69: 'xitop',
             0x6C: 'xgkick', 0x70: 'esadd', 0x71: 'ersadd', 0x72: 'eleng', 0x73: 'erleng', 0x74: 'eatanxy',
             0x75: 'eatanxz', 0x76: 'esum', 0x78: 'esqrt', 0x79: 'ersqrt', 0x7A: 'ercpr', 0x7B: 'waitp',
             0x7C: 'esin', 0x7D: 'eatan', 0x7E: 'eexp'}


def lower(l, pc):
    op = l >> 25
    d = dest((l >> 21) & 0xF)
    ft, fs = (l >> 16) & 31, (l >> 11) & 31
    fsf, ftf = XYZW[(l >> 21) & 3], XYZW[(l >> 23) & 3]
    if op == 0x40:
        sub = l & 0x3F
        idr = (l >> 6) & 31
        if sub == 0x30:
            return f'iadd vi{idr:02}, vi{fs:02}, vi{ft:02}'
        if sub == 0x31:
            return f'isub vi{idr:02}, vi{fs:02}, vi{ft:02}'
        if sub == 0x32:
            return f'iaddi vi{ft:02}, vi{fs:02}, {sx(idr, 5)}'
        if sub == 0x34:
            return f'iand vi{idr:02}, vi{fs:02}, vi{ft:02}'
        if sub == 0x35:
            return f'ior vi{idr:02}, vi{fs:02}, vi{ft:02}'
        if sub < 0x3C:
            return f'?l40.{sub:02x}'
        s2 = ((l >> 6) & 0x1F) << 2 | (l & 3)
        name = SPECIAL_L.get(s2, f'?ls{s2:02x}')
        if name == 'nop' or (s2 == 0x2F):
            return 'nop'
        if name in ('move', 'mr32'):
            return f'{name}.{d} vf{ft:02}, vf{fs:02}' if (l & 0x7FF) != 0x2FF or True else 'nop'
        if name in ('lqi', 'lqd'):
            return f'{name}.{d} vf{ft:02}, (vi{fs:02}{"++" if name == "lqi" else ""})' if name == 'lqi' else f'lqd.{d} vf{ft:02}, (--vi{fs:02})'
        if name == 'sqi':
            return f'sqi.{d} vf{fs:02}, (vi{ft:02}++)'
        if name == 'sqd':
            return f'sqd.{d} vf{fs:02}, (--vi{ft:02})'
        if name == 'div':
            return f'div q, vf{fs:02}{fsf}, vf{ft:02}{ftf}'
        if name == 'sqrt':
            return f'sqrt q, vf{ft:02}{ftf}'
        if name == 'rsqrt':
            return f'rsqrt q, vf{fs:02}{fsf}, vf{ft:02}{ftf}'
        if name in ('waitq', 'waitp'):
            return name
        if name == 'mtir':
            return f'mtir vi{ft:02}, vf{fs:02}{fsf}'
        if name == 'mfir':
            return f'mfir.{d} vf{ft:02}, vi{fs:02}'
        if name in ('ilwr', 'iswr'):
            return f'{name}.{d} vi{ft:02}, (vi{fs:02})'
        if name in ('rnext', 'rget'):
            return f'{name}.{d} vf{ft:02}, r'
        if name in ('rinit', 'rxor'):
            return f'{name} r, vf{fs:02}{fsf}'
        if name == 'mfp':
            return f'mfp.{d} vf{ft:02}, p'
        if name in ('xtop', 'xitop'):
            return f'{name} vi{ft:02}'
        if name == 'xgkick':
            return f'xgkick vi{fs:02}'
        if name in ('esadd', 'ersadd', 'eleng', 'erleng', 'esum'):
            return f'{name} p, vf{fs:02}'
        if name in ('esqrt', 'ersqrt', 'ercpr', 'esin', 'eatan', 'eexp'):
            return f'{name} p, vf{fs:02}{fsf}'
        return name
    name = LOWER.get(op)
    if name is None:
        return f'?l{op:02x} {l:08x}'
    imm11 = sx(l & 0x7FF, 11)
    if name == 'lq':
        return f'lq.{d} vf{ft:02}, {imm11}(vi{fs:02})'
    if name == 'sq':
        return f'sq.{d} vf{fs:02}, {imm11}(vi{ft:02})'
    if name in ('ilw', 'isw'):
        return f'{name}.{d} vi{ft:02}, {imm11}(vi{fs:02})'
    if name in ('iaddiu', 'isubiu'):
        imm15 = ((l >> 10) & 0x7800) | (l & 0x7FF)
        return f'{name} vi{ft:02}, vi{fs:02}, 0x{imm15:x}'
    if name in ('fcset', 'fceq', 'fcand', 'fcor'):
        return f'{name} 0x{l & 0xFFFFFF:06x}'
    if name in ('fsand', 'fseq', 'fsor'):
        imm12 = ((l >> 10) & 0x800) | (l & 0x7FF)
        return f'{name} vi{ft:02}, 0x{imm12:x}'
    if name == 'fsset':
        imm12 = ((l >> 10) & 0x800) | (l & 0x7FF)
        return f'fsset 0x{imm12:x}'
    if name in ('fmeq', 'fmand', 'fmor'):
        return f'{name} vi{ft:02}, vi{fs:02}'
    if name == 'fcget':
        return f'fcget vi{ft:02}'
    target = pc + 8 + imm11 * 8
    if name == 'b':
        return f'b L{target:04x}'
    if name == 'bal':
        return f'bal vi{ft:02}, L{target:04x}'
    if name == 'jr':
        return f'jr vi{fs:02}'
    if name == 'jalr':
        return f'jalr vi{ft:02}, vi{fs:02}'
    if name in ('ibeq', 'ibne'):
        return f'{name} vi{ft:02}, vi{fs:02}, L{target:04x}'
    return f'{name} vi{fs:02}, L{target:04x}'


def main():
    data = open(sys.argv[1], 'rb').read()
    start = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
    end = int(sys.argv[3], 16) if len(sys.argv) > 3 else len(data)
    for pc in range(start, min(end, len(data)), 8):
        l, u = struct.unpack_from('<II', data, pc)
        if l == 0 and u == 0:
            continue
        un, flags = upper(u)
        if u & 0x80000000:
            f = struct.unpack('<f', struct.pack('<I', l))[0]
            lo = f'loi {f:g}'
        else:
            lo = lower(l, pc)
        print(f'{pc:04x}: {un:34} {lo:36} {flags}')


if __name__ == '__main__':
    main()
