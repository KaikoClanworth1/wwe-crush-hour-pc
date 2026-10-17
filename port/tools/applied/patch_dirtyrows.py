import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
p = runtime('src', 'lib', 'gs', 'gs_d3d11_backend.cpp')
s = open(p, encoding='utf-8').read()

s = s.replace('''        bool gpuDirty = false;
        bool needsUpload = false;''', '''        bool gpuDirty = false;
        bool needsUpload = false;
        // Native pixel rows written on the GPU since the last sync (inclusive range).
        uint32_t dirtyY0 = 0xFFFFFFFFu, dirtyY1 = 0u;''', 1)

s = s.replace('''        rt.gpuDirty = false;''', '''        rt.gpuDirty = false;
        rt.dirtyY0 = 0xFFFFFFFFu;
        rt.dirtyY1 = 0u;''', 1)

old = '''    void syncOverlapping(uint32_t first, uint32_t last)
    {
        for (auto &rt : rts)
        {
            if (!rt.gpuDirty)
                continue;
            uint32_t a, b;
            rtPageSpan(rt, a, b);
            if (spansOverlap(first, last, a, b))
                syncRtToVram(rt);
        }
    }'''
new = '''    void syncOverlapping(uint32_t first, uint32_t last)
    {
        for (auto &rt : rts)
        {
            if (!rt.gpuDirty)
                continue;
            uint32_t a, b;
            rtPageSpan(rt, a, b);
            if (!spansOverlap(first, last, a, b))
                continue;
            // Only sync when the requested pages intersect rows the GPU actually wrote. Games keep
            // CLUTs and small textures in the unused lines below the visible framebuffer.
            uint32_t pw, ph;
            pageSize(rt.psm, pw, ph);
            const uint32_t pagesPerRow = std::max<uint32_t>(1u, (rt.fbw * 64u + pw - 1u) / pw);
            const uint32_t basePage = rt.fbpBlock / 32u;
            const uint32_t lo = std::max(first, basePage) - basePage;
            const uint32_t hi = std::min(last, b) - basePage;
            const uint32_t rowY0 = (lo / pagesPerRow) * ph;
            const uint32_t rowY1 = (hi / pagesPerRow + 1u) * ph - 1u;
            if (rt.dirtyY0 != 0xFFFFFFFFu && (rowY1 < rt.dirtyY0 || rowY0 > rt.dirtyY1))
                continue;
            syncRtToVram(rt);
        }
    }'''
assert old in s
s = s.replace(old, new, 1)

# Record dirty rows when a batch is drawn (first occurrence = flushPending).
old = '''        rt->gpuDirty = true;
        ++statDraws;'''
assert old in s
new = '''        rt->gpuDirty = true;
        {
            float minY = 1e9f, maxY = -1e9f;
            for (const Vtx &v : pending)
            {
                minY = std::min(minY, v.y);
                maxY = std::max(maxY, v.y);
            }
            const int y0 = std::max<int>(static_cast<int>(c.scissor.y0), static_cast<int>(std::floor(minY)));
            const int y1 = std::min<int>(static_cast<int>(c.scissor.y1), static_cast<int>(std::ceil(maxY)));
            if (y1 >= y0)
            {
                rt->dirtyY0 = std::min<uint32_t>(rt->dirtyY0, static_cast<uint32_t>(std::max(0, y0)));
                rt->dirtyY1 = std::max<uint32_t>(rt->dirtyY1, static_cast<uint32_t>(y1));
            }
        }
        ++statDraws;'''
s = s.replace(old, new, 1)

# ClearFramebuffer marks the whole target.
s = s.replace('''        d.ctx->ClearRenderTargetView(rt->rtv.Get(), col);
        rt->gpuDirty = true;''', '''        d.ctx->ClearRenderTargetView(rt->rtv.Get(), col);
        rt->gpuDirty = true;
        rt->dirtyY0 = 0u;
        rt->dirtyY1 = rt->h ? rt->h - 1u : 0u;''', 1)
open(p, 'w', encoding='utf-8').write(s)
print('ok')
