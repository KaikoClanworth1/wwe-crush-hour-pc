p = r'D:\Xbox Games Ports\WWE Crush Hour\PS2Recomp\ps2xRuntime\src\lib\vu\ps2_vu1_core.cpp'
s = open(p, encoding='utf-8').read()

s = s.replace('''void VU1Interpreter::finishXgkick()
{
    if (!m_xgkick.active)
        return;
''', '''namespace
{
    uint32_t g_vuTraceProgramStart = 0;
    int g_vuTraceRemaining = -1;
    uint32_t g_vuTracePc = 0xFFFFFFFFu;
}

void VU1Interpreter::finishXgkick()
{
    if (!m_xgkick.active)
        return;
    {
        static bool s_reported = false;
        if (!s_reported && std::getenv("PS2X_VU1_FIND"))
        {
            for (uint32_t off = 0; off + 16u <= m_xgkick.totalBytes; off += 16u)
            {
                uint64_t lo, hi;
                std::memcpy(&lo, m_xgkick.packet.data() + off, 8);
                std::memcpy(&hi, m_xgkick.packet.data() + off + 8, 8);
                if (lo == 0x4bull && hi == 0x4bull)
                {
                    s_reported = true;
                    std::printf("[vu1-find] program start pc=0x%x kick src=0x%x qword=%u\\n", g_vuTraceProgramStart,
                                m_xgkick.sourceAddress, off / 16u);
                    break;
                }
            }
        }
    }
''')

s = s.replace('''    resetScheduler();
    m_state.pc = startPC & microAddressMask();''', '''    resetScheduler();
    m_state.pc = startPC & microAddressMask();
    g_vuTraceProgramStart = m_state.pc;
    if (g_vuTracePc == 0xFFFFFFFFu)
    {
        const char *t = std::getenv("PS2X_VU1_TRACE_PC");
        g_vuTracePc = t ? static_cast<uint32_t>(std::strtoul(t, nullptr, 16)) : 0xFFFFFFFEu;
    }
    if (g_vuTraceRemaining < 0 && m_state.pc == g_vuTracePc && m_unit == Unit::VU1)
    {
        g_vuTraceRemaining = 600;
        std::printf("[vu1-trace] begin pc=0x%x top=%u itop=%u\\n", m_state.pc, top, itop);
        for (int r = 0; r < 32; ++r)
        {
            uint32_t w[4];
            std::memcpy(w, m_state.vf[r], 16);
            std::printf("[vu1-trace]   vf%02d %08x %08x %08x %08x\\n", r, w[0], w[1], w[2], w[3]);
        }
        for (int r = 0; r < 16; ++r)
            std::printf("[vu1-trace]   vi%02d %04x\\n", r, static_cast<uint32_t>(m_state.vi[r]) & 0xFFFFu);
    }''')

s = s.replace('''        const DecodedInstructionPair decoded = getDecodedInstructionPairForPc(vuCode, codeSize, memory, m_state.pc);''', '''        const DecodedInstructionPair decoded = getDecodedInstructionPairForPc(vuCode, codeSize, memory, m_state.pc);
        float traceBefore[32][4];
        int32_t traceViBefore[16];
        const bool tracing = g_vuTraceRemaining > 0 && m_unit == Unit::VU1;
        const uint32_t tracePc = m_state.pc;
        if (tracing)
        {
            std::memcpy(traceBefore, m_state.vf, sizeof(traceBefore));
            for (int r = 0; r < 16; ++r)
                traceViBefore[r] = m_state.vi[r];
        }''')

# Emit diff after the pair has been executed: hook at the top of the next loop iteration is simpler;
# place it right after the branch/pipeline bookkeeping by wrapping the commit call.
s = s.replace('''    while (m_cycle < budgetEnd && !m_stopRequested)
    {
        commitReadyPipelines();''', '''    struct TraceState
    {
        bool active = false;
        uint32_t pc = 0, upper = 0, lower = 0;
        float vf[32][4];
        int32_t vi[16];
    } trace;
    auto flushTrace = [&]()
    {
        if (!trace.active)
            return;
        trace.active = false;
        std::printf("[vu1-trace] %04x: %08x %08x", trace.pc, trace.upper, trace.lower);
        for (int r = 1; r < 32; ++r)
            if (std::memcmp(trace.vf[r], m_state.vf[r], 16) != 0)
            {
                uint32_t w[4];
                std::memcpy(w, m_state.vf[r], 16);
                std::printf(" vf%d=[%08x %08x %08x %08x]", r, w[0], w[1], w[2], w[3]);
                std::memcpy(trace.vf[r], m_state.vf[r], 16);
            }
        for (int r = 1; r < 16; ++r)
            if (trace.vi[r] != m_state.vi[r])
                std::printf(" vi%d=%04x", r, static_cast<uint32_t>(m_state.vi[r]) & 0xFFFFu);
        std::printf("\\n");
        if (--g_vuTraceRemaining == 0)
            std::printf("[vu1-trace] end\\n");
    };
    while (m_cycle < budgetEnd && !m_stopRequested)
    {
        commitReadyPipelines();
        flushTrace();''')

s = s.replace('''        m_viBranchBackupValid = false;
''', '''        m_viBranchBackupValid = false;
        if (tracing)
        {
            trace.active = true;
            trace.pc = tracePc;
            trace.upper = decoded.upper;
            trace.lower = decoded.lower;
            std::memcpy(trace.vf, traceBefore, sizeof(traceBefore));
            std::memcpy(trace.vi, traceViBefore, sizeof(traceViBefore));
        }
''', 1)

if '#include <cstdlib>' not in s:
    s = '#include <cstdlib>\n#include <cstdio>\n' + s
open(p, 'w', encoding='utf-8').write(s)
print('patched')
