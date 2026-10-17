#pragma once

// Mipmap chains for decoded game textures (RGBA8, little-endian 0xAABBGGRR, PS2 alpha 0..0x80 or full 0..0xFF).
// The PS2 renders without mipmaps; on PC at 3-6x resolution distant textures shimmer without them. Box-filtered
// levels, except that cut-out textures (fences, crowds, foliage: alpha is nearly all "off" or "on") keep the
// fraction of texels that pass the alpha test from level 0, so they do not fade away with distance.
// Header-only and platform-free so the unit tests can exercise it.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ps2gs
{
    struct MipLevel
    {
        uint32_t w = 0, h = 0;
        std::vector<uint32_t> px;
    };

    namespace mipdetail
    {
        inline uint32_t channel(uint32_t c, int shift) { return (c >> shift) & 0xFFu; }

        // Fraction of texels whose alpha is >= ref.
        inline double coverage(const std::vector<uint32_t> &px, uint32_t ref, double scale = 1.0)
        {
            if (px.empty())
                return 0.0;
            size_t n = 0;
            for (uint32_t c : px)
                n += static_cast<double>(channel(c, 24)) * scale >= static_cast<double>(ref) ? 1u : 0u;
            return static_cast<double>(n) / static_cast<double>(px.size());
        }
    }

    // Alpha reference for the coverage test: half of opaque (PS2 alpha: 0x80 = opaque).
    inline uint32_t alphaReference(const std::vector<uint32_t> &px)
    {
        uint32_t maxA = 0;
        for (uint32_t c : px)
            maxA = std::max(maxA, mipdetail::channel(c, 24));
        return maxA <= 0x80u ? 0x40u : 0x80u;
    }

    // A cut-out texture: some texels transparent, some opaque, and (nearly) nothing in between.
    inline bool isCutout(const std::vector<uint32_t> &px)
    {
        if (px.empty())
            return false;
        const uint32_t ref = alphaReference(px);
        const uint32_t opaque = ref * 2u;
        size_t off = 0, on = 0, between = 0;
        for (uint32_t c : px)
        {
            const uint32_t a = mipdetail::channel(c, 24);
            if (a <= opaque / 16u)
                ++off;
            else if (a + opaque / 16u >= opaque)
                ++on;
            else
                ++between;
        }
        return off > 0 && on > 0 && between * 10u <= px.size();
    }

    // Levels 1..n (level 0 is the source itself) down to 1x1.
    inline std::vector<MipLevel> buildMipChain(const uint32_t *src, uint32_t w, uint32_t h)
    {
        std::vector<MipLevel> levels;
        if (!src || w == 0 || h == 0)
            return levels;
        std::vector<uint32_t> base(src, src + static_cast<size_t>(w) * h);
        const bool cutout = isCutout(base);
        const uint32_t ref = alphaReference(base);
        const double targetCoverage = cutout ? mipdetail::coverage(base, ref) : 0.0;

        const std::vector<uint32_t> *prev = &base;
        uint32_t pw = w, ph = h;
        while (pw > 1u || ph > 1u)
        {
            MipLevel level;
            level.w = std::max(1u, pw / 2u);
            level.h = std::max(1u, ph / 2u);
            level.px.resize(static_cast<size_t>(level.w) * level.h);
            for (uint32_t y = 0; y < level.h; ++y)
                for (uint32_t x = 0; x < level.w; ++x)
                {
                    // 2x2 box (clamped at odd edges).
                    const uint32_t x0 = std::min(pw - 1u, x * 2u), x1 = std::min(pw - 1u, x * 2u + 1u);
                    const uint32_t y0 = std::min(ph - 1u, y * 2u), y1 = std::min(ph - 1u, y * 2u + 1u);
                    const uint32_t c[4] = {(*prev)[y0 * pw + x0], (*prev)[y0 * pw + x1], (*prev)[y1 * pw + x0],
                                           (*prev)[y1 * pw + x1]};
                    uint32_t out = 0;
                    for (int shift = 0; shift < 32; shift += 8)
                    {
                        uint32_t sum = 0;
                        for (uint32_t v : c)
                            sum += mipdetail::channel(v, shift);
                        out |= ((sum + 2u) / 4u) << shift;
                    }
                    level.px[static_cast<size_t>(y) * level.w + x] = out;
                }
            if (cutout)
            {
                // Scale alpha so the share of texels passing the test matches level 0 (binary search on the
                // scale factor; coverage only grows with it).
                double lo = 0.0, hi = 4.0;
                for (int i = 0; i < 16; ++i)
                {
                    const double mid = (lo + hi) * 0.5;
                    (mipdetail::coverage(level.px, ref, mid) < targetCoverage ? lo : hi) = mid;
                }
                // Coverage is a step function of the scale: take whichever bound lands closer to the target.
                const double scale = std::abs(mipdetail::coverage(level.px, ref, lo) - targetCoverage) <
                                             std::abs(mipdetail::coverage(level.px, ref, hi) - targetCoverage)
                                         ? lo
                                         : hi;
                const uint32_t maxA = ref * 2u;
                for (uint32_t &v : level.px)
                {
                    const uint32_t a = std::min<uint32_t>(maxA, static_cast<uint32_t>(mipdetail::channel(v, 24) * scale)); // truncated, as coverage() measures it
                    v = (v & 0x00FFFFFFu) | (a << 24);
                }
            }
            levels.push_back(std::move(level));
            prev = &levels.back().px;
            pw = levels.back().w;
            ph = levels.back().h;
        }
        return levels;
    }

    // FNV-1a over the pixels and size: the id a texture replacement is found by (Textures/<id>.png).
    inline uint64_t textureContentId(const uint32_t *px, uint32_t w, uint32_t h)
    {
        uint64_t hash = 14695981039346656037ull;
        auto mix = [&hash](uint32_t v)
        {
            for (int i = 0; i < 4; ++i)
            {
                hash ^= (v >> (i * 8)) & 0xFFu;
                hash *= 1099511628211ull;
            }
        };
        mix(w);
        mix(h);
        for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i)
            mix(px[i]);
        return hash;
    }
}
