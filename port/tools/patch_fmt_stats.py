p = r'D:\Xbox Games Ports\WWE Crush Hour\PS2Recomp\ps2xRuntime\src\lib\gs\gs_d3d11_backend.cpp'
s = open(p, encoding='utf-8').read()
s = s.replace('''        ++statDraws;
        pending.clear();''', '''        ++statDraws;
        if (st.prim.tme)
        {
            const uint32_t k = (static_cast<uint32_t>(c.tex0.psm) << 16) | (static_cast<uint32_t>(c.tex0.cpsm) << 8) | c.tex0.csm;
            ++fmtStats[k];
        }
        pending.clear();''', 1)
s = s.replace('''    std::unordered_map<std::string, uint64_t> syncReasons;''', '''    std::unordered_map<std::string, uint64_t> syncReasons;
    std::unordered_map<uint32_t, uint64_t> fmtStats;''', 1)
s = s.replace('''        if (std::getenv("PS2X_GS_STATS"))
            for (const auto &[r, n] : d.syncReasons)''', '''        if (std::getenv("PS2X_GS_STATS"))
            for (const auto &[k, n] : d.fmtStats)
                std::printf("[gs-d3d11]   tex psm=%u cpsm=%u csm=%u draws=%llu\\n", k >> 16, (k >> 8) & 0xFF, k & 0xFF,
                            static_cast<unsigned long long>(n));
        if (std::getenv("PS2X_GS_STATS"))
            for (const auto &[r, n] : d.syncReasons)''', 1)
open(p, 'w', encoding='utf-8').write(s)
print('ok')
