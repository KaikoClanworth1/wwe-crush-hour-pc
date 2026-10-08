p = r'D:\Xbox Games Ports\WWE Crush Hour\PS2Recomp\ps2xRuntime\src\lib\ps2_vif1_interpreter.cpp'
s = open(p, encoding='utf-8').read()
old = '''                        if (s_unpackTrace > 0 && vl == 2u && vn == 3u)'''
new = '''                        static int s_constTrace = std::getenv("PS2X_VU_CONST_TRACE") ? 400 : 0;
                        if (s_constTrace > 0 && destVec >= 0x3F0u && srcVec)
                        {
                            --s_constTrace;
                            const long long ee = (srcVec >= m_rdram && srcVec < m_rdram + PS2_RAM_SIZE) ? (long long)(srcVec - m_rdram) : -1;
                            std::printf("[vu-const] vec=0x%x vn=%u vl=%u ee=0x%llx -> %08x %08x %08x %08x\\n", destVec, vn, vl, ee,
                                        lanes[0], lanes[1], lanes[2], lanes[3]);
                        }
                        if (s_unpackTrace > 0 && vl == 2u && vn == 3u)'''
assert old in s
s = s.replace(old, new)
open(p, 'w', encoding='utf-8').write(s)
print('ok')
