#include "runtime/gs/ps2_gif_arbiter.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

// Path of the packet currently being processed (diagnostics only).
thread_local GifPathId g_gifCurrentPath = GifPathId::Path3;

namespace
{
    // Recycled packet buffers: every GS packet (VU1 XGKICK output, PATH3 texture uploads) is copied
    // into the queue, and allocating a fresh vector for each one cost several % of the frame.
    thread_local std::vector<std::vector<uint8_t>> t_packetPool;
}

GifArbiter::GifArbiter(ProcessPacketFn processFn)
    : m_processFn(std::move(processFn))
{
}

bool GifArbiter::isImagePacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (!data || sizeBytes < 16u)
        return false;

    uint64_t tagLo = 0;
    std::memcpy(&tagLo, data, sizeof(tagLo));
    const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
    return flg == 2u;
}

void GifArbiter::submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl)
{
    if (!data || sizeBytes < 16 || !m_processFn)
        return;

    GifArbiterPacket pkt;
    pkt.pathId = pathId;
    pkt.path2DirectHl = (pathId == GifPathId::Path2) && path2DirectHl;
    pkt.path3Image = (pathId == GifPathId::Path3) && isImagePacket(data, sizeBytes);
    if (!t_packetPool.empty())
    {
        pkt.data = std::move(t_packetPool.back());
        t_packetPool.pop_back();
    }
    pkt.data.assign(data, data + sizeBytes);
    m_queue.push_back(std::move(pkt));
}

void GifArbiter::drain()
{
    if (!m_processFn)
        return;

    // Packets are processed in submission order. DMA channels are serviced in the order the guest
    // kicked them, so FIFO matches the hardware result. Re-sorting by path priority moved PATH1
    // (VU1 XGKICK) draws ahead of PATH3 texture uploads that were queued before them, so
    // RenderWare's streamed texture cache was overwritten before the draws that used it.
    static const bool s_priorityOrder = std::getenv("PS2X_GIF_PRIORITY_ORDER") != nullptr;
    if (s_priorityOrder)
    {
        std::stable_sort(m_queue.begin(), m_queue.end(),
                         [](const GifArbiterPacket &a, const GifArbiterPacket &b)
                         {
                             // DIRECTHL cannot preempt PATH3 IMAGE transfers.
                             if (a.path2DirectHl != b.path2DirectHl || a.path3Image != b.path3Image)
                             {
                                 if (a.path3Image && b.path2DirectHl)
                                     return true;
                                 if (a.path2DirectHl && b.path3Image)
                                     return false;
                             }
                             return pathPriority(a.pathId) < pathPriority(b.pathId);
                         });
    }

    for (size_t i = 0; i < m_queue.size(); ++i)
    {
        auto &pkt = m_queue[i];
        if (!pkt.data.empty())
        {
            g_gifCurrentPath = pkt.pathId;
            m_processFn(pkt.data.data(), static_cast<uint32_t>(pkt.data.size()));
        }
    }
    for (auto &pkt : m_queue)
        if (t_packetPool.size() < 512u && pkt.data.capacity() <= (4u << 20))
            t_packetPool.push_back(std::move(pkt.data));
    m_queue.clear();
}

uint8_t GifArbiter::pathPriority(GifPathId id)
{
    return static_cast<uint8_t>(id);
}
