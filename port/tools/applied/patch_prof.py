import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
root = runtime('src', 'lib')

def patch(path, pairs, include=None):
    s = open(path, encoding='utf-8').read()
    for a, b in pairs:
        assert a in s, (path, a[:60])
        s = s.replace(a, b, 1)
    if include and include not in s:
        # after the first #include line
        i = s.index('#include')
        j = s.index('\n', i) + 1
        s = s[:j] + include + '\n' + s[j:]
    open(path, 'w', encoding='utf-8').write(s)

# VU1 execution time (MSCAL + MSCNT callbacks)
patch(os.path.join(root, 'ps2_runtime.cpp'), [
    ('''                                     m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,''',
     '''                                     ps2prof::Scope profScope(ps2prof::kVu1);
                                     m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,'''),
    ('''                                     m_vu1.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,''',
     '''                                     ps2prof::Scope profScope(ps2prof::kVu1);
                                     m_vu1.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,'''),
    ('''                std::fflush(stdout);
            }
        }
        if (!ps2host::pumpMessages())''',
     '''                {
                    static uint64_t s_lastNs[ps2prof::kCount]{};
                    static uint64_t s_lastCalls[ps2prof::kCount]{};
                    std::printf("[prof]");
                    for (int c = 0; c < ps2prof::kCount; ++c)
                    {
                        const uint64_t ns = ps2prof::g_ns[c].load();
                        const uint64_t calls = ps2prof::g_calls[c].load();
                        std::printf(" %s=%.0fms/%llu", ps2prof::name(c), (ns - s_lastNs[c]) / 1.0e6,
                                    static_cast<unsigned long long>(calls - s_lastCalls[c]));
                        s_lastNs[c] = ns;
                        s_lastCalls[c] = calls;
                    }
                    std::printf("\\n");
                }
                std::fflush(stdout);
            }
        }
        if (!ps2host::pumpMessages())'''),
], '#include "runtime/ps2_prof.h"')

# GS backend
patch(os.path.join(root, 'gs', 'gs_d3d11_backend.cpp'), [
    ('''void GSD3D11Backend::Submit(const GSPrimitiveBatch &batch)
{
    std::lock_guard lock(ps2host::d3dLock());''',
     '''void GSD3D11Backend::Submit(const GSPrimitiveBatch &batch)
{
    ps2prof::Scope profScope(ps2prof::kGsSubmit);
    std::lock_guard lock(ps2host::d3dLock());'''),
    ('''    void flushPending()
    {
        if (!hasPending)
            return;''',
     '''    void flushPending()
    {
        if (!hasPending)
            return;
        ps2prof::Scope profScope(ps2prof::kGsFlush);'''),
    ('''PresentationFrame GSD3D11Backend::Present(const GSPresentationRequest &request)
{''',
     '''PresentationFrame GSD3D11Backend::Present(const GSPresentationRequest &request)
{
    ps2prof::Scope profScope(ps2prof::kGsPresent);'''),
], '#include "runtime/ps2_prof.h"')

# MPEG
patch(os.path.join(root, 'Kernel', 'Stubs', 'MPEG.cpp'), [
    ('''            writeDecodedFrameToGuest(rdram, imageAddr, frame);''',
     '''            ps2prof::Scope profScope(ps2prof::kMpegFrameWrite);
            writeDecodedFrameToGuest(rdram, imageAddr, frame);'''),
    ('''                                std::vector<MpegStreamCallbackEvent> &callbackEvents)
        {
            size_t copied = 0u;
            while (copied < size)''',
     '''                                std::vector<MpegStreamCallbackEvent> &callbackEvents)
        {
            ps2prof::Scope profScope(ps2prof::kMpegDecode);
            size_t copied = 0u;
            while (copied < size)'''),
], '#include "runtime/ps2_prof.h"')
print('ok')
