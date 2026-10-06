#include "runtime/vu1_native.h"
#include "runtime/native_rw.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/ps2_memory.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <array>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <xmmintrin.h>
#include <pmmintrin.h>

extern bool g_vu1SuppressKick;

namespace nrw
{
    bool pgenVerifyWanted();                                                              // native_rw.cpp
    void pgenVerifyRun(const uint8_t *vuBefore, const uint8_t *kicks, uint32_t kickBytes); // native_rw.cpp
}

namespace ps2vu1n
{
    std::vector<uint32_t> *g_vu1nPcTrace = nullptr; // debug lockstep trace (see VU1_TRACE_HASH)
#if !defined(PS2X_HAVE_VU1_NATIVE)
    const ProgramEntry *programs(uint32_t &count)
    {
        count = 0;
        return nullptr;
    }
    const RegionEntry *regions(uint32_t &count)
    {
        count = 0;
        return nullptr;
    }
#endif

    namespace
    {
        float eatan(float value)
        {
            constexpr float k[] = {0.999999344348907f, -0.333298563957214f, 0.199465364217758f, -0.13085337519646f,
                                   0.096420042216778f, -0.055909886956215f, 0.021861229091883f, -0.004054057877511f};
            const float sq = value * value;
            float p = k[7];
            for (int i = 6; i >= 0; --i)
                p = k[i] + sq * p;
            return 0.785398185253143f + value * p;
        }
        float esin(float value)
        {
            constexpr float k[] = {1.0f, -0.166666567325592f, 0.008333025500178f, -0.000198074136279f, 0.000002601886990f};
            const float sq = value * value;
            float p = k[4];
            for (int i = 3; i >= 0; --i)
                p = k[i] + sq * p;
            return value * p;
        }
        float eexp(float value)
        {
            constexpr float k[] = {0.249998688697815f, 0.031257584691048f, 0.002591371303424f,
                                   0.000171562001924f, 0.000005430199963f, 0.000000690600018f};
            float p = k[5];
            for (int i = 4; i >= 0; --i)
                p = k[i] + value * p;
            p = 1.0f + value * p;
            p *= p;
            p *= p;
            return p != 0.0f ? 1.0f / p : std::numeric_limits<float>::max();
        }
    }

    // EFU op numbers are the lower-special function codes (0x70..0x7D).
    float efu(uint32_t op, const float *vsIn, uint32_t fsf)
    {
        const float x = clampf(vsIn[0]), y = clampf(vsIn[1]), z = clampf(vsIn[2]), w = clampf(vsIn[3]);
        const float v = clampf(vsIn[fsf & 3u]);
        switch (op)
        {
        case 0x70: return x * x + y * y + z * z;
        case 0x71: { const float s = x * x + y * y + z * z; return s != 0.0f ? 1.0f / s : s; }
        case 0x72: return std::sqrt(x * x + y * y + z * z);
        case 0x73: { const float l = std::sqrt(x * x + y * y + z * z); return l != 0.0f ? 1.0f / l : l; }
        case 0x74: return x != 0.0f ? eatan(y / x) : 0.0f;
        case 0x75: return x != 0.0f ? eatan(z / x) : 0.0f;
        case 0x76: return x + y + z + w;
        case 0x77: { float r = v; if (r >= 0.0f) { r = std::sqrt(r); if (r != 0.0f) r = 1.0f / r; } return r; }
        case 0x78: return v >= 0.0f ? std::sqrt(v) : v;
        case 0x79: return esin(v);
        case 0x7A: return v != 0.0f ? 1.0f / v : v;
        case 0x7C: return eatan(v);
        case 0x7D: return eexp(v);
        default: return 0.0f;
        }
    }

    namespace
    {
        FILE *g_kickDump = nullptr; // PS2X_PGEN_DUMP: packets kicked by the program being captured
        thread_local std::vector<uint8_t> *g_kickCapture = nullptr; // PS2X_NRW_PGEN=verify: same, in memory

        struct KickUser
        {
            uint8_t *mem;
            uint32_t size;
            PS2Memory *memory;
        };

        // XGKICK: the whole packet (GIFtags up to EOP) is read from VU1 data memory at issue.
        void gatherPacket(const uint8_t *mem, uint32_t size, uint32_t qwAddr, std::vector<uint8_t> &packet)
        {
            struct
            {
                const uint8_t *mem;
                uint32_t size;
            } u{mem, size};
            packet.clear();
            uint32_t addr = (qwAddr * 16u) % u.size;
            for (uint32_t guard = 0; guard < 1024u; ++guard)
            {
                uint64_t tagLo;
                std::memcpy(&tagLo, u.mem + addr, 8);
                const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
                const bool eop = ((tagLo >> 15) & 1u) != 0u;
                const uint32_t flg = static_cast<uint32_t>((tagLo >> 58) & 3u);
                uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
                if (nreg == 0u)
                    nreg = 16u;
                uint64_t qw = 1u;
                if (flg == 0u)
                    qw += static_cast<uint64_t>(nloop) * nreg;
                else if (flg == 1u)
                    qw += (static_cast<uint64_t>(nloop) * nreg + 1u) / 2u;
                else
                    qw += nloop;
                if ((packet.size() / 16u) + qw > u.size / 16u)
                    break;
                // One resize per GIF tag, then copy in at most two runs (VU memory wraps).
                size_t at = packet.size();
                packet.resize(at + static_cast<size_t>(qw) * 16u);
                uint64_t left = qw * 16u;
                while (left > 0u)
                {
                    const uint32_t run = static_cast<uint32_t>(std::min<uint64_t>(left, u.size - addr));
                    std::memcpy(packet.data() + at, u.mem + addr, run);
                    at += run;
                    left -= run;
                    addr = (addr + run) % u.size;
                }
                if (eop)
                    break;
            }
        }

        void kick(void *user, uint32_t qwAddr)
        {
            KickUser &u = *static_cast<KickUser *>(user);
            static thread_local std::vector<uint8_t> packet;
            gatherPacket(u.mem, u.size, qwAddr, packet);
            if (g_kickCapture && !packet.empty())
                g_kickCapture->insert(g_kickCapture->end(), packet.begin(), packet.end());
            if (g_kickDump && !packet.empty())
            {
                const uint32_t hdr[2] = {qwAddr, static_cast<uint32_t>(packet.size())};
                std::fwrite(hdr, sizeof(hdr), 1, g_kickDump);
                std::fwrite(packet.data(), 1, packet.size(), g_kickDump);
            }
            if (!packet.empty() && u.memory && !g_vu1SuppressKick)
            {
                nrw::observeKick(packet.data(), static_cast<uint32_t>(packet.size()));
                u.memory->submitGifPacket(GifPathId::Path1, packet.data(), static_cast<uint32_t>(packet.size()));
            }
        }

        ProgramFn lookup(uint64_t hash)
        {
            static uint64_t s_lastHash = 0;
            static ProgramFn s_lastFn = nullptr;
            static bool s_lastValid = false;
            if (s_lastValid && s_lastHash == hash)
                return s_lastFn;
            uint32_t count = 0;
            const ProgramEntry *table = programs(count);
            ProgramFn fn = nullptr;
            for (uint32_t i = 0; i < count; ++i)
                if (table[i].hash == hash)
                {
                    fn = table[i].fn;
                    break;
                }
            s_lastHash = hash;
            s_lastFn = fn;
            s_lastValid = true;
            return fn;
        }
    }

    uint64_t g_nativeRuns = 0, g_nativeFallbacks = 0, g_nativeMisses = 0, g_nativeCycles = 0;

    namespace
    {
        const uint8_t *g_code = nullptr; // VU1 micro memory (ps2vu1nNoteCode)
        uint32_t g_codeSize = 0;

        // pc -> compiled region for one resident image, built from the regions whose code bytes match.
        using RegionMap = std::array<ProgramFn, 0x4000 / 8>;

        // Region map of a 16 KB micro image: every compiled region whose code bytes are present unchanged.
        std::unique_ptr<RegionMap> buildRegionMap(const uint8_t *code, uint64_t codeHash)
        {
            uint32_t count = 0;
            const RegionEntry *table = regions(count);
            auto map = std::make_unique<RegionMap>();
            map->fill(nullptr);
            // Several entries share a region (and regions repeat across images): hash each range once.
            std::unordered_map<uint64_t, bool> verified;
            uint32_t matched = 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                const RegionEntry &e = table[i];
                ProgramFn &slot = (*map)[(e.pc & 0x3FFFu) >> 3];
                if (slot)
                    continue;
                const uint64_t key = (static_cast<uint64_t>(e.lo) << 32) ^ e.hi ^ (e.hash * 31u);
                auto v = verified.find(key);
                if (v == verified.end())
                    v = verified.emplace(key, codeRangeHash(code, e.lo, e.hi) == e.hash).first;
                if (v->second)
                {
                    slot = e.fn;
                    ++matched;
                }
            }
            static const bool s_log = std::getenv("PS2X_VU_STATS") != nullptr;
            if (s_log)
                std::printf("[vu1n] image %016llx: %u region entries match compiled code\n",
                            static_cast<unsigned long long>(codeHash), matched);
            return map;
        }

        const RegionMap *regionMap(uint64_t codeHash)
        {
            static std::unordered_map<uint64_t, std::unique_ptr<RegionMap>> s_maps;
            const auto it = s_maps.find(codeHash);
            if (it != s_maps.end())
                return it->second.get();
            if (!g_code || g_codeSize < 0x4000u)
                return nullptr;
            if (s_maps.size() > 1024u)
                s_maps.clear();
            auto map = buildRegionMap(g_code, codeHash);
            const RegionMap *out = map.get();
            s_maps.emplace(codeHash, std::move(map));
            return out;
        }

        // Generic trampoline over a region map (same contract as a generated image dispatcher).
        int runRegions(Ctx &c, uint32_t pc, const RegionMap &map)
        {
            for (uint32_t hops = 0; hops < 1000000u; ++hops)
            {
                const ProgramFn fn = map[(pc & 0x3FFFu) >> 3];
                if (!fn || (pc & 7u))
                {
                    c.s->pc = pc;
                    return kFallback;
                }
                const int r = fn(c, pc);
                if (r != kContinue)
                    return r;
                if (c.cyc >= c.budget)
                    return kFallback;
                pc = c.s->pc;
            }
            c.s->pc = pc;
            return kFallback;
        }
    }
}

void ps2vu1nNoteCode(const uint8_t *code, uint32_t size)
{
    ps2vu1n::g_code = code;
    ps2vu1n::g_codeSize = size;
}

// Returns true when the native program ran; `ended` tells whether it reached the E-bit (false =
// continue in the interpreter at state.pc).
bool ps2vu1nTryRun(VU1State &state, uint8_t *vuData, uint32_t dataSize, PS2Memory *memory,
                   uint64_t codeHash, uint32_t maxCycles, bool &ended)
{
    using namespace ps2vu1n;
    // PS2X_VU1_INTERP=1 interprets every program; PS2X_VU1_INTERP=<16-hex-digit hash> only that one.
    static const char *s_interp = std::getenv("PS2X_VU1_INTERP");
    static const uint64_t s_interpHash = (s_interp && std::strlen(s_interp) >= 8) ? std::strtoull(s_interp, nullptr, 16) : 0;
    if (s_interp && (s_interpHash == 0 || s_interpHash == codeHash))
        return false;
    const ProgramFn fn = lookup(codeHash);
    // Unknown whole image (another arena's mix of programs): run the compiled regions whose code is
    // resident unchanged. PS2X_VU1_NO_REGIONS=1 disables (exact images only).
    static const bool s_noRegions = std::getenv("PS2X_VU1_NO_REGIONS") != nullptr;
    const RegionMap *rmap = (fn || s_noRegions) ? nullptr : regionMap(codeHash);
    if (!fn && (!rmap || !(*rmap)[(state.pc & 0x3FFFu) >> 3]))
    {
        ++g_nativeMisses;
        return false;
    }

    KickUser user{vuData, dataSize, memory};
    Ctx c;
    c.s = &state;
    c.mem = vuData;
    c.memMask = (dataSize - 1u) & ~0xFu;
    c.budget = std::max<uint64_t>(maxCycles, 1ull << 24); // runaway guard only
    c.workingClip = state.clip;
    c.xgkick = kick;
    c.user = &user;

    // PS2X_PGEN_DUMP=<dir>: capture runs of the game's particle programs (micro memory starts with the PGen stub
    // "iaddiu vi14, vi00, 0x3d4", entry 0): the micro image once per program, then per run the VU state and data
    // memory before it (in_*.bin) and every packet it kicks (kick_*.bin: {qw address, bytes} + packet).
    // PS2X_VU_CAPTURE_HASH=<16 hex digits>: the same capture for every run of that micro image (any entry).
    static const char *s_pgenDir = std::getenv("PS2X_PGEN_DUMP");
    static const uint64_t s_captureHash = std::getenv("PS2X_VU_CAPTURE_HASH") ? std::strtoull(std::getenv("PS2X_VU_CAPTURE_HASH"), nullptr, 16) : 0;
    if (s_pgenDir && *s_pgenDir && g_code && g_codeSize >= 0x4000u)
    {
        uint32_t w0;
        std::memcpy(&w0, g_code, 4);
        if ((state.pc == 0u && w0 == 0x100E03D4u) || (s_captureHash && codeHash == s_captureHash))
        {
            uint64_t sig = 1469598103934665603ull;
            for (uint32_t k = 0x28u; k < 0x428u; ++k)
                sig = (sig ^ g_code[k]) * 1099511628211ull;
            static std::unordered_map<uint64_t, uint32_t> s_runs;
            uint32_t &n = s_runs[sig];
            if (n < 40u)
            {
                char path[1024];
                if (n == 0u)
                {
                    std::snprintf(path, sizeof(path), "%s/code_%016llx.bin", s_pgenDir, static_cast<unsigned long long>(sig));
                    if (FILE *f = std::fopen(path, "wb"))
                    {
                        std::fwrite(g_code, 1, 0x4000u, f);
                        std::fclose(f);
                    }
                }
                std::snprintf(path, sizeof(path), "%s/in_%016llx_%02u.bin", s_pgenDir, static_cast<unsigned long long>(sig), n);
                if (FILE *f = std::fopen(path, "wb"))
                {
                    std::fwrite(&state, sizeof(state), 1, f);
                    std::fwrite(vuData, 1, dataSize, f);
                    std::fclose(f);
                }
                std::snprintf(path, sizeof(path), "%s/kick_%016llx_%02u.bin", s_pgenDir, static_cast<unsigned long long>(sig), n);
                g_kickDump = std::fopen(path, "wb");
                ++n;
            }
        }
    }

    // PS2X_NRW_PGEN=verify: RenderWare's run of a particle program, compared with the native run of the same object.
    static thread_local std::vector<uint8_t> s_pgenBefore, s_pgenKicks;
    bool pgenVerify = false;
    if (state.pc == 0u && g_code && g_codeSize >= 0x4000u && nrw::pgenVerifyWanted())
    {
        uint32_t w0;
        std::memcpy(&w0, g_code, 4);
        if (w0 == 0x100E03D4u)
        {
            pgenVerify = true;
            s_pgenBefore.assign(vuData, vuData + std::min<uint32_t>(dataSize, 0x4000u));
            s_pgenKicks.clear();
            g_kickCapture = &s_pgenKicks;
        }
    }

    // VU arithmetic: round toward zero, denormals flushed.
    const unsigned int savedCsr = _mm_getcsr();
    _mm_setcsr((savedCsr & ~0x6000u) | 0x6000u | 0x8040u);
    const uint32_t entryPc = state.pc;
    const int r = fn ? fn(c, state.pc) : runRegions(c, state.pc, *rmap);
    _mm_setcsr(savedCsr);
    if (g_kickDump)
    {
        std::fclose(g_kickDump);
        g_kickDump = nullptr;
    }
    if (pgenVerify)
    {
        g_kickCapture = nullptr;
        if (s_pgenBefore.size() == 0x4000u)
            nrw::pgenVerifyRun(s_pgenBefore.data(), s_pgenKicks.data(), static_cast<uint32_t>(s_pgenKicks.size()));
    }
    if (r == kFallback)
    {
        static int s_log = std::getenv("PS2X_VU1_FALLBACK_LOG") ? 40 : 0;
        if (s_log > 0)
        {
            --s_log;
            std::printf("[vu1n] fallback hash=%016llx entry=%04x -> pc=%04x cyc=%llu\n",
                        static_cast<unsigned long long>(codeHash), entryPc, state.pc,
                        static_cast<unsigned long long>(c.cyc));
        }
    }
    state.cycles += c.cyc > maxCycles ? maxCycles : c.cyc;
    g_nativeCycles += c.cyc;
    flushAll(c);
    ++g_nativeRuns;
    if (r == kFallback)
    {
        ++g_nativeFallbacks;
        // Dump mode: record where native code gave up (usually a JR target) as an extra entry
        // point so the next tools/vu1_recomp.py pass compiles it.
        static const char *s_dumpDir = std::getenv("PS2X_VU_DUMP");
        if (s_dumpDir && *s_dumpDir)
        {
            static std::vector<std::pair<uint64_t, uint32_t>> s_seen;
            const auto key = std::make_pair(codeHash, state.pc);
            if (std::find(s_seen.begin(), s_seen.end(), key) == s_seen.end())
            {
                s_seen.push_back(key);
                char path[1024];
                std::snprintf(path, sizeof(path), "%s/vu1_entries.txt", s_dumpDir);
                if (FILE *f = std::fopen(path, "a"))
                {
                    std::fprintf(f, "%016llx %04x\n", static_cast<unsigned long long>(codeHash), state.pc);
                    std::fclose(f);
                }
            }
        }
    }
    ended = r == kEnded;
    return true;
}

// Runs a VU1 micro program image natively against a private data memory, outside VIF/VU1 scheduling: `image` is the
// 16 KB micro memory the program would have been uploaded to (its compiled regions are found by code hash, cached
// under `key`), `data` the 16 KB data memory it reads and writes. Every packet it kicks goes to `onKick`; nothing
// reaches the GS. Returns false when the image has no compiled code at `pc` or the program did not end.
bool ps2vu1nRunPrivate(const uint8_t *image, uint64_t key, uint8_t *data, uint32_t pc,
                       void (*onKick)(void *user, const uint8_t *packet, uint32_t bytes), void *user)
{
    using namespace ps2vu1n;
    static std::mutex s_lock;
    static std::unordered_map<uint64_t, std::unique_ptr<RegionMap>> s_maps;
    const RegionMap *map = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_lock);
        auto it = s_maps.find(key);
        if (it == s_maps.end())
            it = s_maps.emplace(key, buildRegionMap(image, key)).first;
        map = it->second.get();
    }
    if (!map || !(*map)[(pc & 0x3FFFu) >> 3])
        return false;
    struct PrivateKick
    {
        uint8_t *mem;
        void (*onKick)(void *, const uint8_t *, uint32_t);
        void *user;
        std::vector<uint8_t> packet;
    } pk{data, onKick, user, {}};
    VU1State state{};
    state.vf[0][3] = 1.0f;
    state.pc = pc;
    Ctx c;
    c.s = &state;
    c.mem = data;
    c.memMask = 0x3FF0u;
    c.budget = 1ull << 24;
    c.xgkick = [](void *u, uint32_t qwAddr)
    {
        PrivateKick &k = *static_cast<PrivateKick *>(u);
        gatherPacket(k.mem, 0x4000u, qwAddr, k.packet);
        if (!k.packet.empty() && k.onKick)
            k.onKick(k.user, k.packet.data(), static_cast<uint32_t>(k.packet.size()));
    };
    c.user = &pk;
    const unsigned int savedCsr = _mm_getcsr();
    _mm_setcsr((savedCsr & ~0x6000u) | 0x6000u | 0x8040u);
    const int r = runRegions(c, pc, *map);
    _mm_setcsr(savedCsr);
    flushAll(c);
    return r == kEnded;
}
