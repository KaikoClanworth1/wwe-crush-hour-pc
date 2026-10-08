root = r'D:\Xbox Games Ports\WWE Crush Hour\PS2Recomp\ps2xRuntime'

# --- host: sampling profiler ---
p = root + r'\src\lib\host\win32_host.cpp'
s = open(p, encoding='utf-8').read()
sampler = r'''
    // ---- Sampling profiler (PS2X_PROFILE=<output file>) ------------------------------------------
    namespace
    {
        std::atomic<bool> g_profRun{false};
        std::thread g_profThread;
        HANDLE g_profTarget = nullptr;
        std::string g_profPath;
    }

    void profileCurrentThread()
    {
        const char *out = std::getenv("PS2X_PROFILE");
        if (!out || !*out || g_profRun.load())
            return;
        g_profPath = out;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_profTarget,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
        g_profRun.store(true);
        g_profThread = std::thread([]()
        {
            std::unordered_map<uint64_t, uint64_t> hist;
            const uint64_t base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
            uint64_t samples = 0;
            auto dump = [&]()
            {
                if (FILE *f = std::fopen(g_profPath.c_str(), "w"))
                {
                    std::fprintf(f, "base %llx samples %llu\n", (unsigned long long)base, (unsigned long long)samples);
                    for (const auto &[rip, n] : hist)
                        std::fprintf(f, "%llx %llu\n", (unsigned long long)rip, (unsigned long long)n);
                    std::fclose(f);
                }
            };
            auto lastDump = std::chrono::steady_clock::now();
            while (g_profRun.load())
            {
                if (SuspendThread(g_profTarget) != static_cast<DWORD>(-1))
                {
                    CONTEXT c{};
                    c.ContextFlags = CONTEXT_CONTROL;
                    if (GetThreadContext(g_profTarget, &c))
                    {
                        ++hist[c.Rip];
                        ++samples;
                    }
                    ResumeThread(g_profTarget);
                }
                Sleep(1);
                if (std::chrono::steady_clock::now() - lastDump > std::chrono::seconds(5))
                {
                    dump();
                    lastDump = std::chrono::steady_clock::now();
                }
            }
            dump();
        });
        g_profThread.detach();
    }
'''
s = s.replace('''    double lastFrameMs() { return g_lastFrameMs; }''', '''    double lastFrameMs() { return g_lastFrameMs; }
''' + sampler, 1)
if '#include <unordered_map>' not in s:
    s = s.replace('#include <vector>', '#include <vector>\n#include <unordered_map>', 1)
open(p, 'w', encoding='utf-8').write(s)

p = root + r'\include\host\win32_host.h'
s = open(p, encoding='utf-8').read()
s = s.replace('''    double lastFrameMs();''', '''    double lastFrameMs();

    // Debug: start sampling the calling thread's instruction pointer (PS2X_PROFILE=<file>).
    void profileCurrentThread();''', 1)
open(p, 'w', encoding='utf-8').write(s)

p = root + r'\src\lib\ps2_runtime.cpp'
s = open(p, encoding='utf-8').read()
s = s.replace('''        ThreadNaming::SetCurrentThreadName("GameThread");''', '''        ThreadNaming::SetCurrentThreadName("GameThread");
#if defined(PS2X_NATIVE_HOST)
        ps2host::profileCurrentThread();
#endif''', 1)
open(p, 'w', encoding='utf-8').write(s)

p = root + r'\CMakeLists.txt'
s = open(p, encoding='utf-8').read()
s = s.replace('''# Work around WinAPI vs raylib symbol clash for CloseWindow on x64''', '''if(MSVC)
    # Map file lets tools/prof_report.py resolve sampled addresses to functions.
    target_link_options(ps2EntryRunner PRIVATE "/MAP")
endif()

# Work around WinAPI vs raylib symbol clash for CloseWindow on x64''', 1)
open(p, 'w', encoding='utf-8').write(s)
print('ok')
