p = r'D:\Xbox Games Ports\WWE Crush Hour\PS2Recomp\ps2xRuntime\src\lib\host\win32_host.cpp'
s = open(p, encoding='utf-8').read()
old = '''                    CONTEXT c{};
                    c.ContextFlags = CONTEXT_CONTROL;
                    if (GetThreadContext(g_profTarget, &c))
                    {
                        ++hist[c.Rip];
                        ++samples;
                    }
                    ResumeThread(g_profTarget);'''
new = '''                    CONTEXT c{};
                    c.ContextFlags = CONTEXT_FULL;
                    if (GetThreadContext(g_profTarget, &c))
                    {
                        // Attribute samples outside the exe to the first caller frame inside it.
                        const uint64_t modBase = base;
                        const uint64_t modEnd = base + 0x10000000ull;
                        uint64_t attributed = c.Rip;
                        CONTEXT u = c;
                        for (int depth = 0; depth < 24 && !(u.Rip >= modBase && u.Rip < modEnd); ++depth)
                        {
                            DWORD64 imageBase = 0;
                            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(u.Rip, &imageBase, nullptr);
                            if (!fn)
                            {
                                u.Rip = *reinterpret_cast<DWORD64 *>(u.Rsp);
                                u.Rsp += 8;
                            }
                            else
                            {
                                PVOID handlerData = nullptr;
                                DWORD64 establisher = 0;
                                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, u.Rip, fn, &u, &handlerData, &establisher, nullptr);
                            }
                            if (u.Rip == 0)
                                break;
                        }
                        if (c.Rip < modBase || c.Rip >= modEnd)
                            attributed = (u.Rip >= modBase && u.Rip < modEnd) ? (u.Rip | 0x8000000000000000ull) : 0x7FFFFFFFFFFFFFFFull;
                        ++hist[attributed];
                        ++samples;
                    }
                    ResumeThread(g_profTarget);'''
assert old in s
s = s.replace(old, new)
open(p, 'w', encoding='utf-8').write(s)

p = r'D:\Xbox Games Ports\WWE Crush Hour\tools\prof_report.py'
s = open(p, encoding='utf-8').read()
s = s.replace('''for rip, n in samples:
    total += n
    rva = rip - base + pref''', '''for rip, n in samples:
    total += n
    ext = bool(rip & 0x8000000000000000)
    rip &= 0x7FFFFFFFFFFFFFFF
    if rip == 0x7FFFFFFFFFFFFFFF:
        agg['<external, unattributed>'] += n
        continue
    rva = rip - base + pref''')
s = s.replace('''    agg[name] += n''', '''    agg[('[ext] ' if ext else '') + name] += n''')
open(p, 'w', encoding='utf-8').write(s)
print('ok')
