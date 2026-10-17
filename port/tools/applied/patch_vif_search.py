import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
p = runtime('src', 'lib', 'ps2_vif1_interpreter.cpp')
s = open(p, encoding='utf-8').read()
old = '''                            std::printf("[vu-const] vec=0x%x vn=%u vl=%u ee=0x%llx -> %08x %08x %08x %08x\\n", destVec, vn, vl, ee,
                                        lanes[0], lanes[1], lanes[2], lanes[3]);'''
new = old + '''
                            if (destVec == 0x3FBu && lanes[1] == 0u && lanes[3] == 0u && lanes[0] != 0u)
                            {
                                static int s_searches = 0;
                                if (s_searches++ < 3)
                                {
                                    for (uint32_t a = 0; a + 16u <= PS2_RAM_SIZE; a += 4u)
                                        if (std::memcmp(m_rdram + a, srcVec, 16) == 0)
                                            std::printf("[vu-const]   found in rdram at 0x%x\\n", a);
                                    for (uint32_t a = 0; a + 16u <= PS2_SCRATCHPAD_SIZE; a += 4u)
                                        if (std::memcmp(m_scratchpad + a, srcVec, 16) == 0)
                                            std::printf("[vu-const]   found in spr at 0x%x\\n", 0x70000000u + a);
                                }
                            }'''
assert old in s
s = s.replace(old, new)
open(p, 'w', encoding='utf-8').write(s)
print('ok')
