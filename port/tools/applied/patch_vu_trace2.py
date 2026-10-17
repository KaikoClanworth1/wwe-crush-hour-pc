import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
p = runtime('src', 'lib', 'vu', 'ps2_vu1_core.cpp')
s = open(p, encoding='utf-8').read()
s = s.replace('''    uint32_t g_vuTracePc = 0xFFFFFFFFu;
}''', '''    uint32_t g_vuTracePc = 0xFFFFFFFFu;
    uint64_t g_vuExecCount = 0;
    int64_t g_vuTraceIndex = -2;
}''')
s = s.replace('''std::printf("[vu1-find] program start pc=0x%x kick src=0x%x qword=%u\\n", g_vuTraceProgramStart,''',
              '''std::printf("[vu1-find] exec=%llu program start pc=0x%x kick src=0x%x qword=%u\\n", static_cast<unsigned long long>(g_vuExecCount), g_vuTraceProgramStart,''')
s = s.replace('''    if (g_vuTraceRemaining < 0 && m_state.pc == g_vuTracePc && m_unit == Unit::VU1)''', '''    if (m_unit == Unit::VU1)
        ++g_vuExecCount;
    if (g_vuTraceIndex == -2)
    {
        const char *ti = std::getenv("PS2X_VU1_TRACE_INDEX");
        g_vuTraceIndex = ti ? std::atoll(ti) : -1;
    }
    if (g_vuTraceRemaining < 0 && m_unit == Unit::VU1 &&
        (m_state.pc == g_vuTracePc || static_cast<int64_t>(g_vuExecCount) == g_vuTraceIndex))''')
s = s.replace('g_vuTraceRemaining = 600;', 'g_vuTraceRemaining = 1500;')
open(p, 'w', encoding='utf-8').write(s)
print('ok')
