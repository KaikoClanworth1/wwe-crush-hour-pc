#include "runtime/vu1_native.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/ps2_memory.h"

#include <cstdio>
#include <cstdlib>
#include <vector>
#include <xmmintrin.h>
#include <pmmintrin.h>

namespace ps2vu1n
{
#if !defined(PS2X_HAVE_VU1_NATIVE)
    const ProgramEntry *programs(uint32_t &count)
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
        struct KickUser
        {
            uint8_t *mem;
            uint32_t size;
            PS2Memory *memory;
        };

        // XGKICK: the whole packet (GIFtags up to EOP) is read from VU1 data memory at issue.
        void kick(void *user, uint32_t qwAddr)
        {
            KickUser &u = *static_cast<KickUser *>(user);
            static thread_local std::vector<uint8_t> packet;
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
                for (uint64_t q = 0; q < qw; ++q)
                {
                    const size_t at = packet.size();
                    packet.resize(at + 16u);
                    std::memcpy(packet.data() + at, u.mem + addr, 16u);
                    addr = (addr + 16u) % u.size;
                }
                if (eop)
                    break;
            }
            if (!packet.empty() && u.memory)
                u.memory->submitGifPacket(GifPathId::Path1, packet.data(), static_cast<uint32_t>(packet.size()));
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
}

// Returns true when the native program ran; `ended` tells whether it reached the E-bit (false =
// continue in the interpreter at state.pc).
bool ps2vu1nTryRun(VU1State &state, uint8_t *vuData, uint32_t dataSize, PS2Memory *memory,
                   uint64_t codeHash, uint32_t maxCycles, bool &ended)
{
    using namespace ps2vu1n;
    static const bool s_disabled = std::getenv("PS2X_VU1_INTERP") != nullptr;
    if (s_disabled)
        return false;
    const ProgramFn fn = lookup(codeHash);
    if (!fn)
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

    // VU arithmetic: round toward zero, denormals flushed.
    const unsigned int savedCsr = _mm_getcsr();
    _mm_setcsr((savedCsr & ~0x6000u) | 0x6000u | 0x8040u);
    const uint32_t entryPc = state.pc;
    const int r = fn(c, state.pc);
    _mm_setcsr(savedCsr);
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
