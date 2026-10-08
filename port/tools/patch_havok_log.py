# Log Havok's own diagnostics: MyHavokError::{assert,error,warning}Message(const char*, const char*, const char*, int)
# (a1..a3 strings, t0 line). Printed when PS2X_HAVOK_LOG is set; at most 200 lines.
import os

out = r'D:\Xbox Games Ports\WWE Crush Hour\recomp\output'
TAG = '// patch_havok_log'
for fn, kind in [('assertMessage__12MyHavokErrorFPCcPCcPCci_0x1960d0.cpp', 'assert'),
                 ('errorMessage__12MyHavokErrorFPCcPCcPCci_0x196100.cpp', 'error'),
                 ('warningMessage__12MyHavokErrorFPCcPCcPCci_0x196130.cpp', 'warning')]:
    p = os.path.join(out, fn)
    s = open(p, encoding='utf-8').read()
    if TAG in s:
        print(fn, 'already patched')
        continue
    addr = fn.split('_0x')[1].split('.')[0]
    anchor = f'    ctx->pc = 0x{addr}u;\n\n'
    assert s.count(anchor) == 1, fn
    block = f'''    {{ {TAG}
        static const bool s_on = std::getenv("PS2X_HAVOK_LOG") != nullptr;
        static int s_n = 0;
        if (s_on && s_n < 200)
        {{
            ++s_n;
            auto str = [&](int r) -> const char * {{
                const uint32_t a = GPR_U32(ctx, r) & 0x1FFFFFFu;
                return a ? reinterpret_cast<const char *>(rdram + a) : "";
            }};
            std::printf("[havok-{kind}] %.120s | %.120s | %.80s:%d\\n", str(5), str(6), str(7), (int)GPR_U32(ctx, 8));
            std::fflush(stdout);
        }}
    }}
'''
    s = s.replace(anchor, anchor + block, 1)
    open(p, 'w', encoding='utf-8', newline='').write(s)
    print(fn, 'patched')
