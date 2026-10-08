p = r'D:\Xbox Games Ports\WWE Crush Hour\PS2Recomp\ps2xRuntime\src\lib\vu\ps2_vu1_core.cpp'
s = open(p, encoding='utf-8').read()
old = '''void VU1Interpreter::queueStore(uint32_t address, const uint32_t words[4], uint8_t laneMask)
{'''
new = '''void VU1Interpreter::queueStore(uint32_t address, const uint32_t words[4], uint8_t laneMask)
{
    {
        static const long s_watch = std::getenv("PS2X_VU_WATCH") ? std::strtol(std::getenv("PS2X_VU_WATCH"), nullptr, 16) : -1;
        static int s_watchCount = 0;
        if (s_watch >= 0 && m_unit == Unit::VU1 && address / 16u == static_cast<uint32_t>(s_watch) && s_watchCount < 40)
        {
            ++s_watchCount;
            std::printf("[vu-watch] exec=%llu pc=0x%x mask=%x words=%08x %08x %08x %08x\\n",
                        static_cast<unsigned long long>(g_vuExecCount), m_state.pc, laneMask, words[0], words[1], words[2], words[3]);
        }
    }'''
assert old in s
s = s.replace(old, new)
# g_vuExecCount is declared in an anonymous namespace later in the file; move declaration up.
s = s.replace('''    uint64_t g_vuExecCount = 0;
    int64_t g_vuTraceIndex = -2;''', '''    int64_t g_vuTraceIndex = -2;''')
s = s.replace('#include <cstdio>\n', '#include <cstdio>\nnamespace { uint64_t g_vuExecCount = 0; }\n', 1)
open(p, 'w', encoding='utf-8').write(s)
print('ok')
