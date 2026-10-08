# Log Havok BVDataRegistry MOPP cache traffic (PS2X_BV_LOG): buildMoppCode entry, checkCache hit/miss.
import os, re

out = r'D:\Xbox Games Ports\WWE Crush Hour\recomp\output'
TAG = '// patch_bvlog'
for fn in ['buildMoppCode__Q25Havok14BVDataRegistryFPQ25Havok8Geometry_0x363c50.cpp',
           'checkCache__Q25Havok14BVDataRegistryFPQ25Havok8Geometry_0x365310.cpp',
           'readFromArchive__Q25Havok14BVDataRegistryFPQ25Havok8GeometryR10hkIArchive_0x364ea0.cpp']:
    p = os.path.join(out, fn)
    s = open(p, encoding='utf-8').read()
    if TAG in s:
        print(fn, 'already patched'); continue
    name = fn.split('__')[0]
    anchor = '    switch (ctx->pc) {'
    i = s.index(anchor)
    block = (f'    {{ {TAG}\n'
             f'        static const bool s_on = std::getenv("PS2X_BV_LOG") != nullptr;\n'
             f'        if (s_on && ctx->pc == 0x{fn.split("_0x")[1][:-4]}u) {{\n'
             f'            const uint32_t g = GPR_U32(ctx, 5);\n'
             f'            const uint32_t *gp = reinterpret_cast<const uint32_t *>(rdram + (g & 0x1FFFFF0u));\n'
             f'            std::printf("[bv] {name} geom=%08x n14=%u p18=%08x w20=%08x hits=%u miss=%u\\n", g,\n'
             f'                        gp[(g & 15) / 4 + 5], gp[(g & 15) / 4 + 6], gp[(g & 15) / 4 + 8],\n'
             f'                        *reinterpret_cast<const uint32_t *>(rdram + ((GPR_U32(ctx, 4) + 0x6C) & 0x1FFFFFFu)),\n'
             f'                        *reinterpret_cast<const uint32_t *>(rdram + ((GPR_U32(ctx, 4) + 0x70) & 0x1FFFFFFu)));\n'
             f'            std::fflush(stdout);\n'
             f'        }}\n'
             f'    }}\n')
    s = s[:i] + block + s[i:]
    open(p, 'w', encoding='utf-8').write(s)
    print('patched', fn)
