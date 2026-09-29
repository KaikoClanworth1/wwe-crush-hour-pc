#ifndef PS2_VU1_NATIVE_H
#define PS2_VU1_NATIVE_H

// Runtime support for statically recompiled VU1 microcode (tools/vu1_recomp.py generates the
// programs). Each resident microcode image, identified by a hash of VU1 micro memory, becomes one
// native function with an entry switch over its MSCAL/JR targets.
//
// Timing model: VF/VI hazards interlock on hardware, so results are written immediately and the
// generated code only tracks per-lane ready cycles to keep the cycle counter honest. Q, P and the
// MAC/status/clip flags are latency-visible (no interlock), so those go through small pending
// queues that commit when the cycle counter reaches their ready time.

#include "runtime/ps2_vu1.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace ps2vu1n
{
    enum Result : int
    {
        kEnded = 0,
        kFallback = 1, // continue in the interpreter at state.pc
    };

    struct FlagEntry
    {
        uint64_t ready;
        uint32_t mac;
        uint32_t status; // current Z,S,U,O bits (low 4) + sticky extras
        uint32_t kind;   // 0 = fmac, 1 = fsset, 2 = clip
    };

    struct Ctx
    {
        VU1State *s = nullptr;
        uint8_t *mem = nullptr;
        uint32_t memMask = 0x3FF0u; // byte address mask, qword aligned
        uint64_t cyc = 0;
        uint64_t budget = 0;

        uint64_t vfReady[32 * 4]{};
        uint64_t viReady[16]{};

        bool qPend = false;
        float qNew = 0.0f;
        uint32_t qDi = 0;
        uint64_t qReady = 0;

        bool pPend = false;
        float pNew = 0.0f;
        uint64_t pReady = 0;
        uint64_t efuFree = 0;

        FlagEntry flags[16]{};
        uint32_t flagCount = 0;
        uint32_t workingClip = 0;

        // MAC / clip pipelines: at most one issue per cycle and 4-cycle latency, so 4 slots hold
        // every entry that can still be pending.
        struct Pend
        {
            uint64_t ready;
            uint32_t value;
            uint32_t status;
        };
        Pend macRing[4]{};
        Pend clipRing[4]{};
        uint32_t macHead = 0, clipHead = 0;
        uint64_t macSeen = 0, clipSeen = 0; // ready time of the entry last made visible

        void (*xgkick)(void *user, uint32_t qwAddr) = nullptr;
        void *user = nullptr;
    };

    using ProgramFn = int (*)(Ctx &, uint32_t pc);

#if defined(_MSC_VER)
#define VU1N_INLINE __forceinline
#else
#define VU1N_INLINE inline __attribute__((always_inline))
#endif

    struct ProgramEntry
    {
        uint64_t hash;
        ProgramFn fn;
    };

    // Provided by the generated translation unit.
    const ProgramEntry *programs(uint32_t &count);

    // ---------------------------------------------------------------- helpers used by generated code
    inline void commitQ(Ctx &c)
    {
        if (c.qPend && c.cyc >= c.qReady)
        {
            c.s->q = c.qNew;
            const uint32_t di = c.qDi & 0x30u;
            c.s->status = (c.s->status & 0xFCFu) | di | (di << 6);
            c.qPend = false;
        }
    }
    inline void commitP(Ctx &c)
    {
        if (c.pPend && c.cyc >= c.pReady)
        {
            c.s->p = c.pNew;
            c.pPend = false;
        }
    }
    inline void commitFlags(Ctx &c)
    {
        uint32_t keep = 0;
        for (uint32_t i = 0; i < c.flagCount; ++i)
        {
            const FlagEntry &e = c.flags[i];
            if (e.ready > c.cyc)
            {
                c.flags[keep++] = e;
                continue;
            }
            if (e.kind == 0u)
            {
                c.s->mac = e.mac;
                const uint32_t cur = e.status & 0xFu;
                c.s->status = (c.s->status & 0xFF0u) | cur | ((cur | (e.status >> 4)) << 6);
            }
            else if (e.kind == 1u)
                c.s->status = (c.s->status & 0x03Fu) | (e.status & 0xFC0u);
            else if (e.kind == 3u) // FMAC issued with FSSET in the same pair: MAC only
                c.s->mac = e.mac;
            else
                c.s->clip = e.mac;
        }
        c.flagCount = keep;
    }
    inline void pushFlag(Ctx &c, uint32_t kind, uint32_t mac, uint32_t status)
    {
        if (c.flagCount == 16u)
        {
            const uint64_t saved = c.cyc;
            c.cyc = c.flags[0].ready;
            commitFlags(c);
            c.cyc = saved;
        }
        c.flags[c.flagCount++] = {c.cyc + 4u, mac, status, kind};
    }
    inline void flushAllRings(Ctx &c);
    inline void flushAll(Ctx &c)
    {
        c.cyc = ~0ull >> 1;
        commitQ(c);
        commitP(c);
        commitFlags(c);
        flushAllRings(c);
    }

    // FMAC flags for a result vector (lanes in dest mask only). MAC layout: x = bit 3 per group.
    inline void fmacFlags(Ctx &c, const float r[4], uint32_t dest)
    {
        uint32_t mac = 0, st = 0;
        for (uint32_t k = 0; k < 4; ++k)
        {
            const uint32_t lane = 1u << (3u - k);
            if (!(dest & lane))
                continue;
            uint32_t b;
            std::memcpy(&b, &r[k], 4);
            if ((b & 0x7FFFFFFFu) == 0u)
            {
                mac |= lane;
                st |= 1u;
            }
            if (b & 0x80000000u)
            {
                mac |= lane << 4;
                st |= 2u;
            }
        }
        pushFlag(c, 0u, mac, st);
    }

    VU1N_INLINE float clampf(float v)
    {
        uint32_t b;
        std::memcpy(&b, &v, 4);
        if ((b & 0x7F800000u) == 0x7F800000u)
        {
            b = (b & 0x80000000u) | 0x7F7FFFFFu;
            std::memcpy(&v, &b, 4);
        }
        return v;
    }
    VU1N_INLINE int32_t orderKey(float v)
    {
        uint32_t b;
        std::memcpy(&b, &v, 4);
        return (b & 0x80000000u) ? -static_cast<int32_t>(b & 0x7FFFFFFFu) : static_cast<int32_t>(b);
    }
    VU1N_INLINE float maxRaw(float a, float b) { return orderKey(a) >= orderKey(b) ? a : b; }
    VU1N_INLINE float minRaw(float a, float b) { return orderKey(a) <= orderKey(b) ? a : b; }
    VU1N_INLINE float ftoi(float v, float scale)
    {
        const double d = static_cast<double>(clampf(v)) * static_cast<double>(scale);
        int32_t i;
        if (d >= 2147483647.0)
            i = 2147483647;
        else if (d <= -2147483648.0)
            i = static_cast<int32_t>(0x80000000u);
        else
            i = static_cast<int32_t>(d);
        float f;
        std::memcpy(&f, &i, 4);
        return f;
    }
    VU1N_INLINE float itof(float v, float scale)
    {
        int32_t i;
        std::memcpy(&i, &v, 4);
        return static_cast<float>(i) * scale;
    }
    VU1N_INLINE uint32_t clipFlags(const float *vs, float w)
    {
        uint32_t wBits;
        std::memcpy(&wBits, &w, 4);
        const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;
        uint32_t f = 0;
        for (uint32_t k = 0; k < 3; ++k)
        {
            uint32_t b;
            std::memcpy(&b, &vs[k], 4);
            const int32_t pos = static_cast<int32_t>(b);
            const int32_t neg = static_cast<int32_t>(b ^ 0x80000000u);
            if (pos > limit)
                f |= 1u << (k * 2u);
            if (neg > limit)
                f |= 2u << (k * 2u);
        }
        return f;
    }
    VU1N_INLINE float ifloat(uint32_t bits)
    {
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    VU1N_INLINE uint32_t fbits(float f)
    {
        uint32_t b;
        std::memcpy(&b, &f, 4);
        return b;
    }
    inline float vdiv(float num, float den, uint32_t &di)
    {
        num = clampf(num);
        den = clampf(den);
        if (fbits(den) << 1 == 0u)
        {
            di = (fbits(num) << 1 == 0u) ? 0x10u : 0x20u;
            return std::signbit(num) != std::signbit(den) ? -std::numeric_limits<float>::max()
                                                          : std::numeric_limits<float>::max();
        }
        di = 0u;
        return clampf(num / den);
    }
    inline float vrsqrt(float num, float rad, uint32_t &di)
    {
        num = clampf(num);
        rad = clampf(rad);
        const float den = std::sqrt(std::fabs(rad));
        di = rad < 0.0f ? 0x10u : 0u;
        if (den != 0.0f)
            return clampf(num / den);
        di = (fbits(num) << 1 == 0u) ? 0x10u : 0x20u;
        return std::signbit(num) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
    }
    // ---- MAC ring
    VU1N_INLINE void macApply(Ctx &c, const Ctx::Pend &e)
    {
        c.s->mac = e.value;
        const uint32_t cur = e.status & 0xFu;
        c.s->status = (c.s->status & 0xFF0u) | cur | (cur << 6);
        c.macSeen = e.ready;
    }
    VU1N_INLINE void pushMac(Ctx &c, uint32_t mac, uint32_t st)
    {
        Ctx::Pend &slot = c.macRing[c.macHead & 3u];
        if (slot.ready > c.macSeen && slot.ready <= c.cyc)
            macApply(c, slot); // oldest entry is visible already; keep it before overwriting
        slot = {c.cyc + 4u, mac, st};
        ++c.macHead;
    }
    VU1N_INLINE void commitMac(Ctx &c)
    {
        const Ctx::Pend *best = nullptr;
        for (const Ctx::Pend &e : c.macRing)
            if (e.ready > c.macSeen && e.ready <= c.cyc && (!best || e.ready > best->ready))
                best = &e;
        if (best)
            macApply(c, *best);
    }
    VU1N_INLINE void pushClip(Ctx &c, uint32_t clip)
    {
        Ctx::Pend &slot = c.clipRing[c.clipHead & 3u];
        if (slot.ready > c.clipSeen && slot.ready <= c.cyc)
        {
            c.s->clip = slot.value;
            c.clipSeen = slot.ready;
        }
        slot = {c.cyc + 4u, clip, 0u};
        ++c.clipHead;
    }
    VU1N_INLINE void commitClip(Ctx &c)
    {
        const Ctx::Pend *best = nullptr;
        for (const Ctx::Pend &e : c.clipRing)
            if (e.ready > c.clipSeen && e.ready <= c.cyc && (!best || e.ready > best->ready))
                best = &e;
        if (best)
        {
            c.s->clip = best->value;
            c.clipSeen = best->ready;
        }
    }
    // MAC bits for result lanes r (x = bit 3 of each nibble); only lanes in dest count.
    VU1N_INLINE void macFlags(Ctx &c, float r0, float r1, float r2, float r3, uint32_t dest)
    {
        const float r[4] = {r0, r1, r2, r3};
        uint32_t mac = 0, st = 0;
        for (uint32_t k = 0; k < 4; ++k)
        {
            const uint32_t lane = 1u << (3u - k);
            if (!(dest & lane))
                continue;
            uint32_t b;
            std::memcpy(&b, &r[k], 4);
            if ((b & 0x7FFFFFFFu) == 0u)
            {
                mac |= lane;
                st |= 1u;
            }
            if (b & 0x80000000u)
            {
                mac |= lane << 4;
                st |= 2u;
            }
        }
        pushMac(c, mac, st);
    }

    inline void flushAllRings(Ctx &c)
    {
        commitMac(c);
        commitClip(c);
    }

    float efu(uint32_t op, const float *vs, uint32_t fsf);
}

#endif
