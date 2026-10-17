# Generated code: wwfGfxAnimThreadPlayQueuedAnim shifts an animation thread's queue down one slot:
#     for (i = 1; i < thread->count; i++) entry[i] = entry[i + 1];   (count at thread+0x10)
# During wrestler select -> match load the game updates an object whose animation-thread array has
# been freed and reused (count reads as a heap pointer, e.g. 0x00ef7268). The loop then walks
# megabytes past the array and overwrote the vsync callback pointer (0x4AEDA0 -> 0x3e), crashing in
# _ps2_vsync_handler. Treat an implausible count as a one-entry queue (the loop ends at once and the
# queue becomes empty) instead of shifting through RAM.
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
path = out('wwfGfxAnimThreadPlayQueuedAnim__FP12WWFGfxObjecti_0x242ba0.cpp')
s = open(path, encoding='utf-8').read()
guard = '''label_242cb8:
    { // patch_animqueue: never shift through memory on a garbage queue count
        const uint32_t countAddr = ADD32(GPR_U32(ctx, 16), 16);
        const int32_t count = static_cast<int32_t>(READ32(countAddr));
        if (count < 0 || count > 64)
            WRITE32(countAddr, 1u);
    }
'''
if 'patch_animqueue' in s:
    print('already patched')
else:
    assert s.count('label_242cb8:\n') == 1
    s = s.replace('label_242cb8:\n', guard, 1)
    open(path, 'w', encoding='utf-8', newline='').write(s)
    print('patched')
