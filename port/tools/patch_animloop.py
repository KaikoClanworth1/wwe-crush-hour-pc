# Generated code: RtAnimInterpolatorSubAnimTime wraps a negative time with
#     while (time < 0) time += animation->duration;
# A zero-length animation played backwards with a tiny non-zero step (4.5e-15 here, 0 on the PS2)
# loops forever -> the game freezes at wrestler select. Guard the loop: if duration <= 0, clamp
# time to 0 and leave the loop (what a zero step would have produced).
import re

path = r'D:\Xbox Games Ports\WWE Crush Hour\recomp\output\RtAnimInterpolatorSubAnimTime_0x304e80.cpp'
s = open(path, encoding='utf-8').read()

# Drop earlier debug instrumentation.
s = re.sub(r'    \{ // TEMP-ANIMDBG.*?\n    \}\n', '', s, flags=re.S)

guard = '''label_304f50:
    { // patch_animloop: zero-length animation would never leave the wrap loop
        const uint32_t durBits = READ32(ADD32(GPR_U32(ctx, 29), 124));
        float dur;
        std::memcpy(&dur, &durBits, sizeof(dur));
        if (!(dur > 0.0f))
        {
            const uint32_t interp = READ32(ADD32(GPR_U32(ctx, 29), 128));
            WRITE32(ADD32(interp, 4), 0u);
            ctx->pc = 0x304F84u;
            goto label_304f84;
        }
    }
'''
if 'patch_animloop' in s:
    print('already patched')
else:
    assert s.count('label_304f50:\n') == 1 and 'label_304f84:' in s
    s = s.replace('label_304f50:\n', guard, 1)
    open(path, 'w', encoding='utf-8', newline='').write(s)
    print('patched')

# Same wrap loop in RtAnimInterpolatorAddAnimTime (forward play): while (time > duration) time -= duration.
path2 = r'D:\Xbox Games Ports\WWE Crush Hour\recomp\output\RtAnimInterpolatorAddAnimTime_0x305140.cpp'
s2 = open(path2, encoding='utf-8').read()
guard2 = '''label_30523c:
    { // patch_animloop: zero-length animation would never leave the wrap loop
        const uint32_t durBits = READ32(ADD32(GPR_U32(ctx, 29), 164));
        float dur;
        std::memcpy(&dur, &durBits, sizeof(dur));
        if (!(dur > 0.0f))
        {
            const uint32_t interp = READ32(ADD32(GPR_U32(ctx, 29), 176));
            WRITE32(ADD32(interp, 4), 0u);
            ctx->pc = 0x3052C4u;
            goto label_3052c4;
        }
    }
'''
if 'patch_animloop' in s2:
    print('AddAnimTime already patched')
else:
    assert s2.count('label_30523c:\n') == 1 and 'label_3052c4:' in s2
    s2 = s2.replace('label_30523c:\n', guard2, 1)
    open(path2, 'w', encoding='utf-8', newline='').write(s2)
    print('AddAnimTime patched')

# Root cause of both loops: the step (dt * speed) is sometimes built from an uninitialised float
# (0xAAAAAAAA = -3e-13) and comes out tiny but non-zero. The game handles a zero step (SubAnimTime
# leaves time alone, AddAnimTime returns early), so flush |step| < 1e-10 to exactly zero on entry.
FLUSH = '''    { // patch_animloop: flush garbage-tiny animation steps to zero
        if (std::fabs(ctx->f[12]) < 1e-10f)
            ctx->f[12] = 0.0f;
    }
'''
for p, label in ((path, 'label_304e80:\n'), (path2, 'label_305140:\n')):
    t = open(p, encoding='utf-8').read()
    if 'flush garbage-tiny animation steps' in t:
        print(p.rsplit(chr(92), 1)[-1], 'flush already present')
        continue
    assert t.count(label) == 1
    t = t.replace(label, label + FLUSH, 1)
    if '#include <cmath>' not in t:
        t = t.replace('#include <stdexcept>', '#include <stdexcept>\n#include <cmath>', 1)
    open(p, 'w', encoding='utf-8', newline='').write(t)
    print(p.rsplit(chr(92), 1)[-1], 'flush added')
