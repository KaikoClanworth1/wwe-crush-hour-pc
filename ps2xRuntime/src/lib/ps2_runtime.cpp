#include "ps2_runtime.h"
#include "runtime/ps2_prof.h"
#include "ps2_log.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "game_overrides.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ee_scheduler.h"
#include "runtime/native_rw.h"
#include "ThreadNaming.h"
#include "Kernel/Stubs/Audio.h"
#include "Kernel/Stubs/GS.h"
#include "Kernel/Stubs/MPEG.h"
#include "ps2_host_backend.h"
#include <cstdlib>
#if defined(PS2X_NATIVE_HOST)
#include "runtime/gs/gs_d3d11_backend.h"
#include "runtime/native_seng.h"
#endif
#include "ps2_iop_host.h"
#include "ps2x/iop/iop_subsystem.h"

#include <iostream>
#include <stdexcept>
#include <fstream>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <chrono>
#include <functional>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <sstream>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>

namespace ps2_stubs
{
    void resetSifState();
}


// ---- GS worker thread (PS2X_GS_THREAD=1) ------------------------------------------------------------
// GIF packets leaving the arbiter are copied into a FIFO and processed (frontend + D3D11 backend) on a
// dedicated thread, so VU1/VIF/DMA work on the EE side overlaps with draw submission. Packet order is
// preserved; EE-side code that touches the GS directly calls ps2GsThreadDrain() first (hooked into the
// existing asyncDmaDrain() ordering points in ps2_memory.cpp).
namespace
{
    struct GsThreadState
    {
        std::mutex m;
        std::condition_variable cv, idleCv;
        std::deque<std::vector<uint8_t>> jobs; // an empty job runs the next entry of calls
        std::deque<std::function<void()>> calls;
        std::vector<std::vector<uint8_t>> pool;
        bool busy = false;
        bool started = false;
        uint32_t queuedFlips = 0; // DISPFB writes queued and not yet applied (ps2GsThreadDeferFlip)
        GS *gs = nullptr;
    };
    GsThreadState g_gsThread;
    thread_local bool t_gsWorker = false;

    void gsThreadStart(GS &gs)
    {
        g_gsThread.gs = &gs;
        g_gsThread.started = true;
        std::thread([]()
                    {
                        t_gsWorker = true;
                        ThreadNaming::SetCurrentThreadName("GsThread");
#if defined(PS2X_NATIVE_HOST)
                        if (std::getenv("PS2X_PROFILE_GS")) // PS2X_PROFILE samples this thread instead
                            ps2host::profileCurrentThread();
#endif
                        for (;;)
                        {
                            std::vector<uint8_t> job;
                            std::function<void()> call;
                            {
                                std::unique_lock lock(g_gsThread.m);
                                g_gsThread.cv.wait(lock, [] { return !g_gsThread.jobs.empty(); });
                                job = std::move(g_gsThread.jobs.front());
                                g_gsThread.jobs.pop_front();
                                if (job.empty())
                                {
                                    call = std::move(g_gsThread.calls.front());
                                    g_gsThread.calls.pop_front();
                                }
                                g_gsThread.busy = true;
                            }
                            if (job.empty())
                                call();
                            else
                                nrw::dispatchPacket(*g_gsThread.gs, job.data(), static_cast<uint32_t>(job.size()));
                            {
                                std::lock_guard lock(g_gsThread.m);
                                job.clear();
                                if (g_gsThread.pool.size() < 256u)
                                    g_gsThread.pool.push_back(std::move(job));
                                g_gsThread.busy = false;
                                if (g_gsThread.jobs.empty())
                                    g_gsThread.idleCv.notify_all();
                            }
                        }
                    })
            .detach();
    }
}

void ps2NoteUnaligned128(uint32_t addr)
{
    static const int s_limit = []()
    {
        const char *v = std::getenv("PS2X_UNALIGNED_LOG");
        return v ? std::atoi(v) : 0;
    }();
    static std::atomic<int> s_n{0};
    const int n = s_n.fetch_add(1, std::memory_order_relaxed);
    if (n < s_limit)
    {
        std::printf("[unaligned128] #%d addr=0x%08x\n", n, addr);
        std::fflush(stdout);
    }
}

bool ps2GsThreadEnabled()
{
    static const bool s_on = []()
    {
        const char *v = std::getenv("PS2X_GS_THREAD"); // default on; PS2X_GS_THREAD=0 processes GIF inline
        return !(v && *v == '0');
    }();
    return s_on;
}

void ps2GsThreadSubmit(GS &gs, const uint8_t *data, uint32_t size)
{
    if (t_gsWorker || !data || size == 0u)
    {
        gs.processGIFPacket(data, size);
        return;
    }
    std::vector<uint8_t> buf;
    {
        std::lock_guard lock(g_gsThread.m);
        if (!g_gsThread.started)
            gsThreadStart(gs);
        if (!g_gsThread.pool.empty())
        {
            buf = std::move(g_gsThread.pool.back());
            g_gsThread.pool.pop_back();
        }
    }
    buf.assign(data, data + size);
    {
        std::lock_guard lock(g_gsThread.m);
        g_gsThread.jobs.push_back(std::move(buf));
    }
    g_gsThread.cv.notify_one();
}

// Runs fn on the GS worker in stream order (after every packet queued so far) without waiting for it.
// Returns false -- the caller does the work itself -- on the worker or when the worker is not running.
bool ps2GsThreadDefer(std::function<void()> fn)
{
    if (t_gsWorker || !ps2GsThreadEnabled())
        return false;
    {
        std::lock_guard lock(g_gsThread.m);
        if (!g_gsThread.started)
            return false;
        g_gsThread.calls.push_back(std::move(fn));
        g_gsThread.jobs.emplace_back();
    }
    g_gsThread.cv.notify_one();
    return true;
}

// A DISPFB write (buffer flip), deferred like ps2GsThreadDefer. The flip is also the back-pressure point:
// the EE may run at most one frame ahead of the worker. Unbounded, it queued several frames, the worker
// monopolised the D3D lock and the host stalled for seconds (frame rate swinging between 0 and 118).
bool ps2GsThreadDeferFlip(std::function<void()> fn)
{
    if (t_gsWorker || !ps2GsThreadEnabled())
        return false;
    // PS2X_HITCH_LOG=1: frames (flip to flip) longer than 25 ms, with how long the EE waited here for the GS worker.
    static const bool s_hitchLog = std::getenv("PS2X_HITCH_LOG") != nullptr;
    static auto s_lastFlip = std::chrono::steady_clock::now();
    const auto t0 = std::chrono::steady_clock::now();
    {
        std::unique_lock lock(g_gsThread.m);
        if (!g_gsThread.started)
            return false;
        g_gsThread.idleCv.wait(lock, [] { return g_gsThread.queuedFlips < 1u; });
        ++g_gsThread.queuedFlips;
        g_gsThread.calls.push_back([f = std::move(fn)]()
                                   {
                                       f();
                                       std::lock_guard l(g_gsThread.m);
                                       --g_gsThread.queuedFlips;
                                       g_gsThread.idleCv.notify_all();
                                   });
        g_gsThread.jobs.emplace_back();
    }
    g_gsThread.cv.notify_one();
    if (s_hitchLog)
    {
        extern void ps2HitchSamplerStart();
        extern void ps2HitchReport(double fromT, double toT);
        ps2HitchSamplerStart();
        const auto t1 = std::chrono::steady_clock::now();
        const double frameMs = std::chrono::duration<double, std::milli>(t1 - s_lastFlip).count();
        const double waitMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        s_lastFlip = t1;
        extern std::atomic<double> g_hitchLastFlip;
        g_hitchLastFlip.store(ps2host::timeSeconds(), std::memory_order_relaxed);
        if (frameMs > 25.0)
        {
            const double now = ps2host::timeSeconds();
            std::printf("[hitch] t=%.2f frame=%.1f ms (waited for GS worker %.1f ms, queue %zu)\n", now, frameMs,
                        waitMs, g_gsThread.jobs.size());
            // PS2X_HITCH_SAMPLE_MS: frames at least this long also list where the EE was (default 45).
            static const double s_sampleMs = std::getenv("PS2X_HITCH_SAMPLE_MS") ? std::atof(std::getenv("PS2X_HITCH_SAMPLE_MS")) : 45.0;
            if (frameMs > s_sampleMs)
                ps2HitchReport(now - frameMs / 1000.0, now);
            std::fflush(stdout);
        }
    }
    return true;
}

// Host presentation with the worker: latching from the host thread races the stream -- the EE can flip
// DISPFB and the worker draw the start of the next frame into the buffer being copied (half-drawn menu
// frames: sky / HUD missing). Queue the latch instead; the worker runs it between packets, and DISPFB
// writes are queued the same way (ps2_memory.cpp), so it sees the display registers that belong to the
// frames drawn so far. Returns false when the worker is not running.
bool ps2GsThreadLatch()
{
    // At most one latch queued: a slow one (CPU present path while no render target is displayed, e.g.
    // DISPFB 0 at match start) must not pile up one per vsync -- the EE's flips queue behind them and the
    // game slowed to one frame every few seconds.
    static std::atomic<bool> s_pending{false};
    if (s_pending.exchange(true, std::memory_order_acq_rel))
        return ps2GsThreadEnabled() && g_gsThread.started;
    if (ps2GsThreadDefer([]
                         {
                             s_pending.store(false, std::memory_order_release);
                             g_gsThread.gs->latchHostPresentationFrame();
                         }))
        return true;
    s_pending.store(false, std::memory_order_release);
    return false;
}

void ps2GsThreadDrain()
{
    if (t_gsWorker || !g_gsThread.started)
        return;
    std::unique_lock lock(g_gsThread.m);
    g_gsThread.idleCv.wait(lock, [] { return g_gsThread.jobs.empty() && !g_gsThread.busy; });
}

#define ELF_MAGIC 0x464C457F // "\x7FELF" in little endian
#define ET_EXEC 2            // Executable file
#define EM_MIPS 8            // MIPS architecture
#define PT_LOAD 1            // Loadable segment

static constexpr int FB_WIDTH = 640;
static constexpr int FB_HEIGHT = 512;
static constexpr int DEFAULT_DISPLAY_HEIGHT = 448;
static constexpr uint32_t DEFAULT_FB_SIZE = FB_WIDTH * FB_HEIGHT * 4;
static constexpr uint32_t DEFAULT_FB_ADDR = (PS2_RAM_SIZE - DEFAULT_FB_SIZE - 0x10000u);
#if defined(PLATFORM_VITA)
static constexpr int HOST_WINDOW_WIDTH = 960;
static constexpr int HOST_WINDOW_HEIGHT = 544;
#else
static constexpr int HOST_WINDOW_WIDTH = FB_WIDTH;
static constexpr int HOST_WINDOW_HEIGHT = DEFAULT_DISPLAY_HEIGHT;
#endif
struct ElfHeader
{
    uint32_t magic;
    uint8_t elf_class;
    uint8_t endianness;
    uint8_t version;
    uint8_t os_abi;
    uint8_t abi_version;
    uint8_t padding[7];
    uint16_t type;
    uint16_t machine;
    uint32_t version2;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct ProgramHeader
{
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
};

namespace
{
    constexpr uint32_t kGuestHeapDefaultBase = 0x00100000u;
    constexpr uint32_t kGuestHeapDefaultAlignment = 16u;
    constexpr uint32_t kGuestHeapSafetyPad = 0x1000u;
    // Real PS2: SetupHeap(-1) runs the heap up to the main stack (0x1FF0000). A 1 MB shorter heap made
    // the game's memory regions (sized at boot by probing malloc) run out during match load.
    constexpr uint32_t kGuestHeapHardLimit = 0x01FF0000u;

    constexpr uint32_t COP0_CAUSE_EXCCODE_MASK = 0x0000007Cu;
    constexpr uint32_t COP0_CAUSE_BD = 0x80000000u;
    constexpr uint32_t COP0_STATUS_EXL = 0x00000002u;
    constexpr uint32_t COP0_STATUS_BEV = 0x00400000u;
    constexpr uint32_t EXCEPTION_VECTOR_GENERAL = 0x80000080u;
    constexpr uint32_t EXCEPTION_VECTOR_TLB_REFILL = 0x80000000u;
    constexpr uint32_t EXCEPTION_VECTOR_BOOT = 0xBFC00200u;

    struct DispatchHistory
    {
        std::array<uint32_t, 64> pcs{};
        uint32_t next = 0u;
        bool wrapped = false;
    };

    thread_local DispatchHistory g_dispatchHistory;

    bool computeFileCrc32(const std::string &path, uint32_t &crcOut)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        static const std::array<uint32_t, 256> table = []
        {
            std::array<uint32_t, 256> values{};
            for (uint32_t i = 0; i < values.size(); ++i)
            {
                uint32_t value = i;
                for (uint32_t bit = 0; bit < 8; ++bit)
                {
                    value = (value & 1u) ? (0xEDB88320u ^ (value >> 1u)) : (value >> 1u);
                }
                values[i] = value;
            }
            return values;
        }();

        uint32_t crc = 0xFFFFFFFFu;
        std::array<uint8_t, 16 * 1024> buffer{};
        while (file.good())
        {
            file.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = file.gcount();
            for (std::streamsize i = 0; i < count; ++i)
            {
                crc = table[(crc ^ buffer[static_cast<size_t>(i)]) & 0xFFu] ^ (crc >> 8u);
            }
        }
        if (file.bad())
        {
            return false;
        }
        crcOut = ~crc;
        return true;
    }

    void pushDispatchPc(uint32_t pc)
    {
        DispatchHistory &h = g_dispatchHistory;
        h.pcs[h.next] = pc;
        h.next = (h.next + 1u) % static_cast<uint32_t>(h.pcs.size());
        if (h.next == 0u)
        {
            h.wrapped = true;
        }
    }

    std::string formatDispatchHistory()
    {
        const DispatchHistory &h = g_dispatchHistory;
        const uint32_t count = h.wrapped ? static_cast<uint32_t>(h.pcs.size()) : h.next;
        if (count == 0u)
        {
            return "(empty)";
        }

        std::ostringstream oss;
        bool first = true;
        for (uint32_t i = 0u; i < count; ++i)
        {
            const uint32_t idx = (h.next + h.pcs.size() - count + i) % static_cast<uint32_t>(h.pcs.size());
            if (!first)
            {
                oss << " -> ";
            }
            first = false;
            oss << "0x" << std::hex << h.pcs[idx];
        }
        return oss.str();
    }

    uint32_t selectExceptionVector(const R5900Context *ctx, bool tlbRefill)
    {
        if (ctx->cop0_status & COP0_STATUS_BEV)
        {
            return EXCEPTION_VECTOR_BOOT;
        }
        return tlbRefill ? EXCEPTION_VECTOR_TLB_REFILL : EXCEPTION_VECTOR_GENERAL;
    }

    void seedVu0IdleSuccess(R5900Context *ctx)
    {
        if (!ctx)
        {
            return;
        }

        ctx->vu0_clip_flags = 0;
        ctx->vu0_clip_flags2 = 0;
        ctx->vu0_mac_flags = 0;
        ctx->vu0_status = 0;
        ctx->vu0_q = 1.0f;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));
        ctx->vu0_vpu_stat = 0;
        ctx->vu0_vpu_stat2 = 0;
    }

    void copyVu0ContextToState(const R5900Context *ctx, VU1State &state)
    {
        std::memset(&state, 0, sizeof(state));

        for (uint32_t i = 0; i < 32u; ++i)
        {
            _mm_storeu_ps(state.vf[i], ctx->vu0_vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            state.vi[i] = static_cast<int16_t>(ctx->vi[i]);
        }

        _mm_storeu_ps(state.acc, ctx->vu0_acc);
        state.q = ctx->vu0_q;
        state.p = ctx->vu0_p;
        state.i = ctx->vu0_i;
        alignas(16) uint32_t rWords[4]{};
        _mm_storeu_si128(reinterpret_cast<__m128i *>(rWords), _mm_castps_si128(ctx->vu0_r));
        state.r = 0x3F800000u | (rWords[0] & 0x007FFFFFu);
        state.pc = ctx->vu0_pc;
        state.mac = ctx->vu0_mac_flags;
        state.clip = ctx->vu0_clip_flags;
        state.status = ctx->vu0_status;
        state.itop = ctx->vu0_itop;
        state.dBitEnabled = (ctx->vu0_fbrst & (1u << 2)) != 0u;
        state.tBitEnabled = (ctx->vu0_fbrst & (1u << 3)) != 0u;

        state.vf[0][0] = 0.0f;
        state.vf[0][1] = 0.0f;
        state.vf[0][2] = 0.0f;
        state.vf[0][3] = 1.0f;
        state.vi[0] = 0;
    }

    void copyVu0StateToContext(const VU1State &state, R5900Context *ctx)
    {
        for (uint32_t i = 0; i < 32u; ++i)
        {
            ctx->vu0_vf[i] = _mm_loadu_ps(state.vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            ctx->vi[i] = static_cast<uint16_t>(state.vi[i]);
        }

        ctx->vu0_acc = _mm_loadu_ps(state.acc);
        ctx->vu0_q = state.q;
        ctx->vu0_p = state.p;
        ctx->vu0_i = state.i;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int32_t>(state.r)));
        ctx->vu0_mac_flags = state.mac;
        ctx->vu0_clip_flags = state.clip;
        ctx->vu0_clip_flags2 = state.clip;
        ctx->vu0_status = static_cast<uint16_t>(state.status);
        ctx->vu0_itop = state.itop;
        ctx->vu0_pc = state.pc;
        ctx->vu0_tpc = state.pc;
        ctx->vu0_vpu_stat = (ctx->vu0_vpu_stat & 0xFF00u) | (state.stoppedByD ? (1u << 1) : 0u) | (state.stoppedByT ? (1u << 2) : 0u);
        ctx->vu0_vpu_stat2 = 0;

        ctx->vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
        ctx->vi[0] = 0;
    }

    void raiseCop0Exception(R5900Context *ctx, uint32_t exceptionCode, bool tlbRefill = false)
    {
        if (ctx->in_delay_slot)
        {
            ctx->cop0_epc = ctx->branch_pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~COP0_CAUSE_EXCCODE_MASK) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK) |
                              COP0_CAUSE_BD;
        }
        else
        {
            ctx->cop0_epc = ctx->pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~(COP0_CAUSE_EXCCODE_MASK | COP0_CAUSE_BD)) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK);
        }

        ctx->cop0_status |= COP0_STATUS_EXL;
        ctx->pc = selectExceptionVector(ctx, tlbRefill);
        ctx->in_delay_slot = false;
    }

    std::filesystem::path normalizeAbsolutePath(const std::filesystem::path &path)
    {
        if (path.empty())
        {
            return {};
        }

#if defined(PLATFORM_VITA)
        const std::string generic = path.generic_string();
        const std::size_t colon = generic.find(':');
        if (colon != std::string::npos && colon != 0u)
        {
            const std::size_t slash = generic.find_first_of("/\\");
            if (slash == std::string::npos || colon < slash)
            {
                return path.lexically_normal();
            }
        }
#endif

        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
        if (ec)
        {
            return path.lexically_normal();
        }
        return absolute.lexically_normal();
    }

    PS2Runtime::IoPaths &runtimeIoPaths()
    {
        static PS2Runtime::IoPaths paths = []()
        {
            PS2Runtime::IoPaths defaults;
            std::error_code ec;
            const std::filesystem::path cwd = std::filesystem::current_path(ec);
            defaults.elfDirectory = ec ? std::filesystem::path(".") : cwd.lexically_normal();
            defaults.hostRoot = defaults.elfDirectory;
            defaults.cdRoot = defaults.elfDirectory;
            defaults.mcRoot = defaults.elfDirectory / "mc0";
            return defaults;
        }();

        return paths;
    }

    std::string readGuestPrintableString(const uint8_t *rdram, uint32_t addr, size_t maxLen)
    {
        std::string out;
        if (!rdram || maxLen == 0)
        {
            return out;
        }

        out.reserve(std::min<size_t>(maxLen, 64));
        for (size_t i = 0; i < maxLen; ++i)
        {
            const char ch = static_cast<char>(rdram[(addr + static_cast<uint32_t>(i)) & PS2_RAM_MASK]);
            if (ch == '\0')
            {
                break;
            }
            if (ch >= 0x20 && ch < 0x7F)
            {
                out.push_back(ch);
            }
            else
            {
                out.push_back('.');
            }
        }
        return out;
    }
}


#if defined(PS2X_NATIVE_HOST)
// Debug aid: guest backtraces by scanning the stack for return addresses (PS2X_SYMBOLS = symbols.txt).
namespace
{
    struct GuestSym { uint32_t addr, size; std::string name; };
    const std::vector<GuestSym> &guestSymbols()
    {
        static std::vector<GuestSym> syms = []()
        {
            std::vector<GuestSym> out;
            const char *path = std::getenv("PS2X_SYMBOLS");
            if (!path)
                return out;
            std::ifstream f(path);
            std::string line;
            while (std::getline(f, line))
            {
                unsigned a = 0, z = 0;
                char type[16]{}, name[512]{};
                if (std::sscanf(line.c_str(), "%x %x %15s %511s", &a, &z, type, name) == 4 && std::strcmp(type, "FUNC") == 0)
                    out.push_back({a, z, name});
            }
            std::sort(out.begin(), out.end(), [](const GuestSym &l, const GuestSym &r) { return l.addr < r.addr; });
            return out;
        }();
        return syms;
    }
    const char *guestSymbolName(uint32_t addr)
    {
        const auto &syms = guestSymbols();
        auto it = std::upper_bound(syms.begin(), syms.end(), addr, [](uint32_t a, const GuestSym &s) { return a < s.addr; });
        if (it == syms.begin())
            return "?";
        --it;
        return (addr < it->addr + it->size) ? it->name.c_str() : "?";
    }
    void printGuestBacktrace(const uint8_t *rdram, uint32_t pc, uint32_t ra, uint32_t sp)
    {
        std::printf("    pc %08x %s\n    ra %08x %s\n", pc, guestSymbolName(pc), ra, guestSymbolName(ra));
        sp &= 0x01FFFFF0u;
        int found = 0;
        for (uint32_t off = 0; off < 0x1000u && found < 14; off += 4)
        {
            const uint32_t a = sp + off;
            if (a + 4 > 0x02000000u)
                break;
            uint32_t v;
            std::memcpy(&v, rdram + a, 4);
            if (v < 0x00100008u || v >= 0x00480000u || (v & 3u))
                continue;
            uint32_t call;
            std::memcpy(&call, rdram + (v - 8u), 4);
            if ((call >> 26) != 3u && (call & 0xFC00003Fu) != 9u) // jal / jalr
                continue;
            std::printf("    .. %08x %s\n", v, guestSymbolName(v));
            ++found;
        }
    }
}
#endif

#if defined(PS2X_NATIVE_HOST)
struct HostFrameTexture
{
};
static void hostPresentUpload(const std::vector<uint8_t> &buffer, uint32_t width, uint32_t height)
{
    if (GSD3D11Backend::IsActive() && GSD3D11Backend::PresentLatchedToHost())
        return;
    width = std::clamp<uint32_t>(width, 1u, static_cast<uint32_t>(FB_WIDTH));
    height = std::clamp<uint32_t>(height, 1u, static_cast<uint32_t>(FB_HEIGHT));
    ps2host::presentRgba(buffer.data(), width, height, static_cast<uint32_t>(FB_WIDTH) * 4u);
}
#define PS2X_FRAME_TEX_T HostFrameTexture
#else
#define PS2X_FRAME_TEX_T Texture2D
#endif

static void UploadFrame(PS2X_FRAME_TEX_T &tex, PS2Runtime *rt, uint32_t &outWidth, uint32_t &outHeight)
{
    static uint64_t s_lastPresentationTick = std::numeric_limits<uint64_t>::max();
    static bool s_hasLatchedInitialFrame = false;
    static uint32_t s_lastDisplayFbp = std::numeric_limits<uint32_t>::max();
    static uint32_t s_lastSourceFbp = std::numeric_limits<uint32_t>::max();
    static bool s_lastPreferred = false;
    static uint32_t s_lastWidth = 0u;
    static uint32_t s_lastHeight = 0u;
    static bool s_hasUploadedFrame = false;
    static std::vector<uint8_t> s_scratch;
    static std::vector<uint8_t> s_uploadBuffer(DEFAULT_FB_SIZE, 0u);

    const uint64_t currentTick = rt->eeScheduler().currentVSyncTick();
    const bool needsLatch = !s_hasLatchedInitialFrame || currentTick != s_lastPresentationTick;
    if (needsLatch)
    {
        if (!ps2GsThreadLatch())
            rt->gs().latchHostPresentationFrame();
        s_lastPresentationTick = currentTick;
        s_hasLatchedInitialFrame = true;
    }
    else if (s_hasUploadedFrame)
    {
        outWidth = (s_lastWidth != 0u) ? s_lastWidth : FB_WIDTH;
        outHeight = (s_lastHeight != 0u) ? s_lastHeight : DEFAULT_DISPLAY_HEIGHT;
#if defined(PS2X_NATIVE_HOST)
        hostPresentUpload(s_uploadBuffer, outWidth, outHeight);
#endif
        return;
    }

    s_scratch.clear();
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t displayFbp = 0u;
    uint32_t sourceFbp = 0u;
    bool usedPreferredDisplaySource = false;
    if (!rt->gs().copyLatchedHostPresentationFrame(s_scratch,
                                                   width,
                                                   height,
                                                   &displayFbp,
                                                   &sourceFbp,
                                                   &usedPreferredDisplaySource))
    {
#if defined(PS2X_NATIVE_HOST)
        (void)tex;
        std::fill(s_uploadBuffer.begin(), s_uploadBuffer.end(), 0u);
        hostPresentUpload(s_uploadBuffer, FB_WIDTH, DEFAULT_DISPLAY_HEIGHT);
#else
        Image blank = GenImageColor(FB_WIDTH, FB_HEIGHT, MAGENTA);
        UpdateTexture(tex, blank.data);
        UnloadImage(blank);
#endif
        outWidth = FB_WIDTH;
        outHeight = DEFAULT_DISPLAY_HEIGHT;
        s_lastWidth = outWidth;
        s_lastHeight = outHeight;
        s_hasUploadedFrame = true;
        return;
    }

    PS2_IF_AGRESSIVE_LOGS({
        static uint32_t s_uploadDebugCount = 0u;
        if (s_uploadDebugCount < 128u ||
            displayFbp != s_lastDisplayFbp ||
            sourceFbp != s_lastSourceFbp ||
            usedPreferredDisplaySource != s_lastPreferred ||
            width != s_lastWidth ||
            height != s_lastHeight)
        {
            std::cout << "[frame:upload] idx=" << s_uploadDebugCount
                      << " tick=" << currentTick
                      << " displayFbp=" << displayFbp
                      << " sourceFbp=" << sourceFbp
                      << " size=" << width << "x" << height
                      << " preferred=" << static_cast<uint32_t>(usedPreferredDisplaySource ? 1u : 0u)
                      << std::endl;
        }
        ++s_uploadDebugCount;
    });
    s_lastDisplayFbp = displayFbp;
    s_lastSourceFbp = sourceFbp;
    s_lastPreferred = usedPreferredDisplaySource;
    s_lastWidth = width;
    s_lastHeight = height;

    std::fill(s_uploadBuffer.begin(), s_uploadBuffer.end(), 0u);
    if (!s_scratch.empty() && width != 0u && height != 0u)
    {
        const uint32_t copyWidth = std::min<uint32_t>(width, FB_WIDTH);
        const uint32_t copyHeight = std::min<uint32_t>(height, FB_HEIGHT);
        const size_t srcRowBytes = static_cast<size_t>(width) * 4u;
        const size_t dstRowBytes = static_cast<size_t>(FB_WIDTH) * 4u;
        const size_t copyRowBytes = static_cast<size_t>(copyWidth) * 4u;
        for (uint32_t y = 0; y < copyHeight; ++y)
        {
            const size_t srcOffset = static_cast<size_t>(y) * srcRowBytes;
            const size_t dstOffset = static_cast<size_t>(y) * dstRowBytes;
            if (srcOffset + copyRowBytes > s_scratch.size() ||
                dstOffset + copyRowBytes > s_uploadBuffer.size())
            {
                break;
            }
            std::memcpy(s_uploadBuffer.data() + dstOffset, s_scratch.data() + srcOffset, copyRowBytes);
        }
    }

#if !defined(PS2X_NATIVE_HOST)
    UpdateTexture(tex, s_uploadBuffer.data());
#else
    (void)tex;
    hostPresentUpload(s_uploadBuffer, width, height);
#endif
    outWidth = width;
    outHeight = height;
    s_hasUploadedFrame = true;
}

// Interrupt/callback invocation stacks are carved downward from here. The top of RAM holds the
// game's main-thread stack (crt0: _stack = 0x1FF0000, _stack_size = 0x10000 for this title), so
// keep clear of it; handlers running on top of live main-thread frames corrupted saved $ra.
// Interrupt/callback invocation stacks (16 KB per guest thread and nesting depth). On a PS2 the
// kernel runs INTC/DMAC handlers on its own memory; the game owns all RAM from its ELF up to _stack
// and allocates thread stacks at the top (the MPEG thread's sits at 0x1FEBxxx). Stacks carved below
// 0x1FF0000 overlapped those, so a DMA handler overwrote saved return addresses and the main thread
// jumped into the heap (0x8a80c0) and froze. Use the unused EE kernel range 0x20000-0x80000 instead
// (the runtime's syscall table mirror ends near 0x12400). PS2X_CALLBACK_STACK_HIGH=1: old placement.
static bool callbackStacksHigh()
{
    static const bool s_high = std::getenv("PS2X_CALLBACK_STACK_HIGH") != nullptr;
    return s_high;
}

static uint32_t asyncCallbackStackCeiling()
{
    if (!callbackStacksHigh())
        return 0x00080000u;
    uint32_t reserve = 0x10000u;
    if (const char *v = std::getenv("PS2X_MAIN_STACK_RESERVE"))
        reserve = static_cast<uint32_t>(std::strtoul(v, nullptr, 0));
    return PS2_RAM_SIZE - std::min<uint32_t>(reserve, 0x80000u);
}

static uint32_t asyncCallbackStackFloor(uint32_t highFloor)
{
    return callbackStacksHigh() ? highFloor : 0x00020000u;
}


PS2Runtime::PS2Runtime()
{
    m_iopHost = std::make_unique<PS2IopHostAdapter>(*this);
    m_iopSubsystem = std::make_unique<ps2x::iop::IopSubsystem>(*m_iopHost);

    m_eeScheduler = std::make_unique<EeScheduler>(*this);

    // Assign rather than memset: R5900Context's constructor zeroes itself and
    // then applies the COP0 reset values, which a memset here would discard.
    m_cpuContext = R5900Context{};

    // R0 is always zero in MIPS
    m_cpuContext.r[0] = _mm_set1_epi32(0);
    m_cpuContext.vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    m_cpuContext.vu0_q = 1.0f;
    m_cpuContext.vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));

    // Stack pointer (SP) and global pointer (GP) will be set by the loaded ELF

    m_loadedModules.clear();
    m_guestHeapBlocks.clear();
    m_guestHeapBase = kGuestHeapDefaultBase;
    m_guestHeapEnd = kGuestHeapDefaultBase;
    m_guestHeapLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    m_guestHeapSuggestedBase = kGuestHeapDefaultBase;
    m_guestHeapConfigured = false;
    m_asyncCallbackStackFloor = asyncCallbackStackFloor(std::min(kGuestHeapHardLimit, PS2_RAM_SIZE));
    m_asyncCallbackStackTop = asyncCallbackStackCeiling();
}

void PS2Runtime::setDebugUiCallbacks(DebugUiCallback initCallback,
                                     DebugUiCallback drawCallback,
                                     DebugUiCallback shutdownCallback,
                                     void *userData)
{
    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }

    m_debugUiInitCallback = initCallback;
    m_debugUiDrawCallback = drawCallback;
    m_debugUiShutdownCallback = shutdownCallback;
    m_debugUiUserData = userData;
}

PS2Runtime::~PS2Runtime()
{
    try
    {
        requestStop();
        m_iopSubsystem.reset();
        m_iopHost.reset();
#if defined(PLATFORM_VITA) || defined(PS2X_NATIVE_HOST)
        m_audioBackend.stopAll();
        m_audioBackend.setAudioReady(false);
#else
        if (IsAudioDeviceReady())
        {
            CloseAudioDevice();
            m_audioBackend.setAudioReady(false);
        }
#endif
        if (m_debugUiInitialized && m_debugUiShutdownCallback)
        {
            m_debugUiShutdownCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = false;
        }

#if defined(PS2X_NATIVE_HOST)
        ps2host::shutdown();
#else
        if (IsWindowReady())
        {
            CloseWindow();
        }
#endif

        m_loadedModules.clear();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: unknown" << std::endl;
    }
}

ps2x::iop::ModuleLoadResult PS2Runtime::loadIopModule(std::string_view path, const void *arguments, uint32_t argumentSize)
{
#if defined(PS2X_NATIVE_HOST)
    {
        std::string upperPath(path);
        std::transform(upperPath.begin(), upperPath.end(), upperPath.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (upperPath.find("SENG.IRX") != std::string::npos && ps2native::NativeSeng::enabled())
        {
            // Served natively (see NativeSeng); report a successfully started resident module.
            ps2x::iop::ModuleLoadResult fake;
            fake.handled = true;
            fake.moduleId = 0x5E;
            fake.startResult = 0;
            std::cout << "[runtime] SENG.IRX replaced by native XAudio2 sound engine" << std::endl;
            return fake;
        }
        // Controller, memory card and sound-library modules: the game only reaches them through the
        // EE-side libpad / libmc / sceSdRemote APIs, which are served natively (Kernel/Stubs/Pad.cpp,
        // MemoryCard.cpp, Audio.cpp). Nothing talks to the IRXs, so report them resident without
        // running MIPS IOP code. PS2X_IOP_EMULATE=1 loads them into the IOP interpreter again.
        static const bool s_emulate = std::getenv("PS2X_IOP_EMULATE") != nullptr;
        static const char *const kNativeModules[] = {"SIO2MAN.IRX", "MTAPMAN.IRX", "PADMAN.IRX", "MCMAN.IRX",
                                                      "MCSERV.IRX", "LIBSD.IRX", "SDRDRV.IRX"};
        if (!s_emulate)
        {
            for (const char *name : kNativeModules)
            {
                if (upperPath.find(name) == std::string::npos)
                    continue;
                static int32_t s_nextId = 0x60;
                ps2x::iop::ModuleLoadResult fake;
                fake.handled = true;
                fake.moduleId = s_nextId++;
                fake.startResult = 0;
                std::cout << "[runtime] " << name << " served natively (no IOP emulation)" << std::endl;
                return fake;
            }
        }
    }
#endif
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->loadModule(path, arguments, argumentSize);
}

ps2x::iop::ModuleLoadResult PS2Runtime::loadIopModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->loadModuleBuffer(guestAddress, arguments, argumentSize);
}

bool PS2Runtime::stopIopModule(int32_t moduleId, int32_t *result)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->stopModule(moduleId, result);
}

ps2x::iop::RpcAbi PS2Runtime::selectIopRpcAbi(const ps2x::iop::RpcAbiRequest &request) const
{
    return m_iopSubsystem->selectRpcAbi(request);
}

bool PS2Runtime::canBindIopRpc(uint32_t sid) const noexcept
{
#if defined(PS2X_NATIVE_HOST)
    if (sid == ps2native::kSengSid && ps2native::NativeSeng::enabled())
        return true;
#endif
    return m_iopSubsystem->canBindRpc(sid);
}

ps2x::iop::RpcResult PS2Runtime::handleIopRpc(uint8_t *rdram, R5900Context *ctx, ps2x::iop::RpcRequest request)
{
#if defined(PS2X_NATIVE_HOST)
    if (request.sid == ps2native::kSengSid && ps2native::NativeSeng::enabled())
    {
        // Native XAudio2 sound engine replaces SENG.IRX (no IOP emulation for game audio).
        ps2native::nativeSeng().handleBatch(rdram, request.send.address, request.send.size,
                                            request.receive.address, request.receive.size);
        ps2x::iop::RpcResult result;
        result.handled = true;
        result.resultAddress = request.receive.address;
        return result;
    }
#endif
    auto scope = m_iopHost->enterCall(ctx, rdram);
    request.callToken = scope.token();
    return m_iopSubsystem->handleRpc(request);
}

void PS2Runtime::notifyIopSifTransfer(uint8_t *rdram, const ps2x::iop::SifTransfer &transfer)
{
    auto scope = m_iopHost->enterCall(nullptr, rdram);
    m_iopSubsystem->onSifTransfer(transfer);
}

void PS2Runtime::advanceIopEeCycles(uint64_t eeCycles) noexcept
{
    m_iopSubsystem->runEeCycles(eeCycles);
}

void PS2Runtime::resetIop()
{
    m_iopSubsystem->reset();
}

ps2x::iop::DebugSnapshot PS2Runtime::iopDebugSnapshot() const
{
    return m_iopSubsystem->debugSnapshot();
}

uint32_t PS2Runtime::allocateIopMemory(uint32_t size, uint32_t alignment)
{
    return m_iopSubsystem ? m_iopSubsystem->allocateMemory(size, alignment) : 0u;
}

bool PS2Runtime::freeIopMemory(uint32_t address)
{
    return m_iopSubsystem && m_iopSubsystem->freeMemory(address);
}

bool PS2Runtime::readIopMemory(uint32_t address, void *destination, size_t size) const
{
    return m_iopSubsystem && m_iopSubsystem->readMemory(address, destination, size);
}

bool PS2Runtime::writeIopMemory(uint32_t address, const void *source, size_t size)
{
    return m_iopSubsystem && m_iopSubsystem->writeMemory(address, source, size);
}

bool PS2Runtime::zeroIopMemory(uint32_t address, size_t size)
{
    return m_iopSubsystem && m_iopSubsystem->zeroMemory(address, size);
}

bool PS2Runtime::isIopMemoryRange(uint32_t address, size_t size) const
{
    return m_iopSubsystem && m_iopSubsystem->isMemoryRange(address, size);
}

bool PS2Runtime::syncCoreSubsystems()
{
    uint8_t *const rdram = m_memory.getRDRAM();
    uint8_t *const gsVram = m_memory.getGSVRAM();
    if (!rdram || !gsVram)
    {
        return false;
    }

    if (m_boundRdram == rdram && m_boundGSVram == gsVram)
    {
        return true;
    }

    m_gs.init(gsVram, static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &m_memory.gs());
    nrw::setGs(&m_gs);
    nrw::setRdram(rdram);
    m_gifArbiter.setProcessPacketFn([this](const uint8_t *data, uint32_t size)
                                    {
                                        if (ps2GsThreadEnabled())
                                            ps2GsThreadSubmit(m_gs, data, size);
                                        else
                                            m_gs.processGIFPacket(data, size);
                                    });
    m_memory.setGifArbiter(&m_gifArbiter);
    m_memory.setVu1MscalCallback([this](uint32_t startPC, uint32_t top, uint32_t itop)
                                 {
                                     R5900Context *cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                     if (!cpuContext)
                                     {
                                         cpuContext = &m_cpuContext;
                                     }
                                     m_vu1.state().dBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 11)) != 0u;
                                     ps2prof::Scope profScope(ps2prof::kVu1);
                                     m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                   m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                   m_gs, &m_memory, startPC, top, itop, 65536);
                                     cpuContext->vu0_vpu_stat =
                                         (cpuContext->vu0_vpu_stat & ~0x0600u) |
                                         (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                         (m_vu1.state().stoppedByT ? 0x0400u : 0u); });
    m_memory.setVu1MscntCallback([this](uint32_t top, uint32_t itop)
                                 {
                                     R5900Context *cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                     if (!cpuContext)
                                     {
                                         cpuContext = &m_cpuContext;
                                     }
                                     m_vu1.state().dBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 11)) != 0u;
                                     ps2prof::Scope profScope(ps2prof::kVu1);
                                     m_vu1.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                  m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                  m_gs, &m_memory, top, itop, 65536);
                                     cpuContext->vu0_vpu_stat =
                                         (cpuContext->vu0_vpu_stat & ~0x0600u) |
                                         (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                         (m_vu1.state().stoppedByT ? 0x0400u : 0u); });
    resetIop();
    m_vu0.reset();
    m_vu1.reset();

    m_boundRdram = rdram;
    m_boundGSVram = gsVram;
    return true;
}

bool PS2Runtime::initialize(const char *title)
{
    try
    {
        if (!m_memory.initialize())
        {
            std::cerr << "Failed to initialize PS2 memory" << std::endl;
            return false;
        }

        if (!syncCoreSubsystems())
        {
            std::cerr << "Failed to bind runtime core subsystems" << std::endl;
            return false;
        }
#if defined(PS2X_NATIVE_HOST)
        ps2host::loadConfig("ps2x_settings.ini");
        if (!ps2host::init(title))
        {
            std::cerr << "Failed to initialize the Direct3D 11 host" << std::endl;
            return false;
        }
        m_audioBackend.setAudioReady(ps2host::audioReady());
        if (!std::getenv("PS2X_GS_CPU"))
        {
            m_gs.setRasterBackend(std::make_unique<GSD3D11Backend>());
            std::cout << "[runtime] GS: Direct3D 11 hardware renderer" << std::endl;
        }
#elif defined(PLATFORM_VITA)
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title); // raylib vita does not support audio
#else
        SetConfigFlags(FLAG_WINDOW_RESIZABLE);
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title);
        InitAudioDevice();
        m_audioBackend.setAudioReady(IsAudioDeviceReady());
#endif
#if !defined(PS2X_NATIVE_HOST)
        SetTargetFPS(60);
#endif
        if (m_debugUiInitCallback)
        {
            m_debugUiInitCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = true;
        }

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Failed to initialize PS2 runtime: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "Failed to initialize PS2 runtime: unknown exception" << std::endl;
    }

    return false;
}

bool PS2Runtime::loadELF(const std::string &elfPath)
{
    configureIoPathsFromElf(elfPath);

    std::ifstream file(elfPath, std::ios::binary);
    if (!file)
    {
        std::cerr << "Failed to open ELF file: " << elfPath << std::endl;
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    if (fileSize < static_cast<std::streamoff>(sizeof(ElfHeader)))
    {
        std::cerr << "ELF file is too small: " << elfPath << std::endl;
        return false;
    }
    file.seekg(0, std::ios::beg);

    ElfHeader header{};
    if (!file.read(reinterpret_cast<char *>(&header), sizeof(header)))
    {
        std::cerr << "Failed to read ELF header from: " << elfPath << std::endl;
        return false;
    }

    if (header.magic != ELF_MAGIC)
    {
        std::cerr << "Invalid ELF magic number" << std::endl;
        return false;
    }

    if (header.elf_class != 1u || header.endianness != 1u)
    {
        std::cerr << "Unsupported ELF format (expected 32-bit little-endian)." << std::endl;
        return false;
    }

    if (header.machine != EM_MIPS || header.type != ET_EXEC)
    {
        std::cerr << "Not a MIPS executable ELF file" << std::endl;
        return false;
    }

    if (header.phnum != 0u && header.phentsize < sizeof(ProgramHeader))
    {
        std::cerr << "Unsupported ELF program-header entry size: " << header.phentsize << std::endl;
        return false;
    }

    const uint64_t programHeaderTableEnd =
        static_cast<uint64_t>(header.phoff) +
        static_cast<uint64_t>(header.phnum) * static_cast<uint64_t>(header.phentsize);
    if (programHeaderTableEnd > static_cast<uint64_t>(fileSize))
    {
        std::cerr << "ELF program-header table is out of range." << std::endl;
        return false;
    }

    m_cpuContext.pc = header.entry;
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);

    uint32_t maxLoadedRdramEnd = kGuestHeapDefaultBase;
    uint32_t moduleBase = std::numeric_limits<uint32_t>::max();
    uint32_t moduleEnd = 0u;
    bool loadedAnySegment = false;

    for (uint16_t i = 0; i < header.phnum; i++)
    {
        const uint64_t phOffset =
            static_cast<uint64_t>(header.phoff) +
            static_cast<uint64_t>(i) * static_cast<uint64_t>(header.phentsize);
        if (phOffset + sizeof(ProgramHeader) > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF program header " << i << " is out of range." << std::endl;
            return false;
        }

        ProgramHeader ph{};
        file.seekg(static_cast<std::streamoff>(phOffset), std::ios::beg);
        if (!file.read(reinterpret_cast<char *>(&ph), sizeof(ph)))
        {
            std::cerr << "Failed to read ELF program header " << i << std::endl;
            return false;
        }

        if (ph.type != PT_LOAD || ph.memsz == 0u)
        {
            continue;
        }

        if (ph.filesz > ph.memsz)
        {
            std::cerr << "ELF segment " << i << " has filesz > memsz." << std::endl;
            return false;
        }

        const uint64_t segmentFileEnd = static_cast<uint64_t>(ph.offset) + static_cast<uint64_t>(ph.filesz);
        if (segmentFileEnd > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF segment " << i << " exceeds file bounds." << std::endl;
            return false;
        }

        const bool scratch =
            ph.vaddr >= PS2_SCRATCHPAD_BASE &&
            ph.vaddr < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE);

        uint32_t physAddr = 0u;
        try
        {
            physAddr = m_memory.translateAddress(ph.vaddr);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Failed to translate ELF segment " << i
                      << " virtual address 0x" << std::hex << ph.vaddr
                      << std::dec << ": " << e.what() << std::endl;
            return false;
        }
        const uint64_t regionSize = scratch ? static_cast<uint64_t>(PS2_SCRATCHPAD_SIZE)
                                            : static_cast<uint64_t>(PS2_RAM_SIZE);
        const uint64_t segmentMemEnd = static_cast<uint64_t>(physAddr) + static_cast<uint64_t>(ph.memsz);
        if (segmentMemEnd > regionSize)
        {
            std::cerr << "ELF segment " << i << " exceeds "
                      << (scratch ? "scratchpad" : "RDRAM")
                      << " bounds (vaddr=0x" << std::hex << ph.vaddr
                      << " memsz=0x" << ph.memsz << std::dec << ")." << std::endl;
            return false;
        }

        uint8_t *destBase = scratch ? m_memory.getScratchpad() : m_memory.getRDRAM();
        if (!destBase)
        {
            std::cerr << "ELF segment " << i << " has no destination memory backing." << std::endl;
            return false;
        }

        uint8_t *dest = destBase + physAddr;
        if (ph.filesz > 0u)
        {
            file.seekg(static_cast<std::streamoff>(ph.offset), std::ios::beg);
            if (!file.read(reinterpret_cast<char *>(dest), ph.filesz))
            {
                std::cerr << "Failed to read ELF segment " << i << " payload." << std::endl;
                return false;
            }
        }

        if (ph.memsz > ph.filesz)
        {
            std::memset(dest + ph.filesz, 0, ph.memsz - ph.filesz);
        }

        RUNTIME_LOG("Loading segment: 0x" << std::hex << ph.vaddr
                                          << " - 0x" << (static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz))
                                          << " (filesz: 0x" << ph.filesz
                                          << ", memsz: 0x" << ph.memsz << ")"
                                          << std::dec << std::endl);

        if (!scratch)
        {
            maxLoadedRdramEnd = std::max(maxLoadedRdramEnd, static_cast<uint32_t>(segmentMemEnd));
        }

        if (ph.flags & 0x1u) // PF_X
        {
            const uint64_t execEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.filesz);
            if (execEnd <= std::numeric_limits<uint32_t>::max())
            {
                m_memory.registerCodeRegion(ph.vaddr, static_cast<uint32_t>(execEnd));
            }
        }

        loadedAnySegment = true;
        moduleBase = std::min(moduleBase, ph.vaddr);
        const uint64_t segmentVirtualEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz);
        const uint32_t clampedVirtualEnd =
            (segmentVirtualEnd > std::numeric_limits<uint32_t>::max())
                ? std::numeric_limits<uint32_t>::max()
                : static_cast<uint32_t>(segmentVirtualEnd);
        moduleEnd = std::max(moduleEnd, clampedVirtualEnd);
    }

    if (!loadedAnySegment)
    {
        std::cerr << "ELF contains no loadable PT_LOAD segments." << std::endl;
        return false;
    }

    if (maxLoadedRdramEnd > PS2_RAM_SIZE)
    {
        maxLoadedRdramEnd = PS2_RAM_SIZE;
    }

    const uint32_t paddedEnd = (maxLoadedRdramEnd > (PS2_RAM_SIZE - kGuestHeapSafetyPad))
                                   ? PS2_RAM_SIZE
                                   : (maxLoadedRdramEnd + kGuestHeapSafetyPad);
    const uint32_t suggestedHeapBase = alignGuestHeapValue(paddedEnd, kGuestHeapDefaultAlignment);
    {
        std::lock_guard<std::mutex> lock(m_guestHeapMutex);
        if (!m_guestHeapConfigured)
        {
            const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
            m_guestHeapSuggestedBase = std::min(suggestedHeapBase, hardLimit);
            m_guestHeapBase = m_guestHeapSuggestedBase;
            m_guestHeapEnd = m_guestHeapSuggestedBase;
            m_guestHeapLimit = hardLimit;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
        const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
        m_asyncCallbackStackFloor = asyncCallbackStackFloor(std::min(std::max(hardLimit, suggestedHeapBase), PS2_RAM_SIZE));
        m_asyncCallbackStackTop = asyncCallbackStackCeiling();
    }

    LoadedModule module;
    module.name = elfPath.substr(elfPath.find_last_of("/\\") + 1);
    module.baseAddress = (moduleBase == std::numeric_limits<uint32_t>::max()) ? 0x00100000u : moduleBase;
    module.size = (moduleEnd > module.baseAddress) ? static_cast<size_t>(moduleEnd - module.baseAddress) : 0u;
    module.active = true;

    m_loadedModules.push_back(module);

    uint32_t elfCrc32 = 0u;
    const bool elfCrc32Valid = computeFileCrc32(elfPath, elfCrc32);
    if (!elfCrc32Valid)
    {
        std::cerr << "[ps2xIOP] failed to compute ELF CRC32 for '" << elfPath << "'" << std::endl;
    }
    ps2x::iop::GameIdentity identity;
    identity.elfName = module.name;
    identity.entryPoint = m_cpuContext.pc;
    identity.crc32 = elfCrc32;
    std::string romError;
    if (!m_romDevice.configure(identity, &romError))
    {
        std::cerr << "[ROM0] failed to configure profile: " << romError << std::endl;
        return false;
    }

    m_iopSubsystem->reset();

    ps2_game_overrides::applyMatching(*this,
                                      elfPath,
                                      m_cpuContext.pc,
                                      elfCrc32,
                                      elfCrc32Valid);

    RUNTIME_LOG("ELF file loaded successfully. Entry point: 0x" << std::hex << m_cpuContext.pc << std::dec);
    return true;
}

const PS2Runtime::IoPaths &PS2Runtime::getIoPaths()
{
    return runtimeIoPaths();
}

void PS2Runtime::setIoPaths(const IoPaths &paths)
{
    IoPaths normalized = paths;
    normalized.elfPath = normalizeAbsolutePath(normalized.elfPath);
    normalized.elfDirectory = normalizeAbsolutePath(normalized.elfDirectory);
    normalized.hostRoot = normalizeAbsolutePath(normalized.hostRoot);
    normalized.cdRoot = normalizeAbsolutePath(normalized.cdRoot);
    normalized.mcRoot = normalizeAbsolutePath(normalized.mcRoot);
    normalized.cdImage = normalizeAbsolutePath(normalized.cdImage);

    if (normalized.elfDirectory.empty() && !normalized.elfPath.empty())
    {
        normalized.elfDirectory = normalized.elfPath.parent_path();
    }

    if (normalized.hostRoot.empty())
    {
        normalized.hostRoot = normalized.elfDirectory;
    }
    if (normalized.cdRoot.empty())
    {
        normalized.cdRoot = normalized.elfDirectory;
    }
    if (normalized.mcRoot.empty())
    {
        normalized.mcRoot = normalized.elfDirectory / "mc0";
    }

    runtimeIoPaths() = normalized;
}

void PS2Runtime::configureIoPathsFromElf(const std::string &elfPath)
{
    IoPaths paths = runtimeIoPaths();
    paths.elfPath = normalizeAbsolutePath(std::filesystem::path(elfPath));
    if (!paths.elfPath.empty())
    {
        paths.elfDirectory = paths.elfPath.parent_path();
    }

    if (!paths.elfDirectory.empty())
    {
        paths.hostRoot = paths.elfDirectory;
        paths.cdRoot = paths.elfDirectory;
        paths.mcRoot = paths.elfDirectory / "mc0";
    }

    setIoPaths(paths);
}

namespace
{
    bool generatedFunctionTableSlot(uint32_t address, uint32_t &slot)
    {
        if ((address & 3u) != 0u || g_ps2RecompiledFunctionTableSlotCount == 0u)
        {
            return false;
        }

        if (address < g_ps2RecompiledFunctionTableBase || address >= g_ps2RecompiledFunctionTableEnd)
        {
            return false;
        }

        const uint32_t offset = address - g_ps2RecompiledFunctionTableBase;
        slot = offset >> 2;
        return slot < g_ps2RecompiledFunctionTableSlotCount;
    }
}

bool PS2Runtime::replaceFunction(uint32_t address, RecompiledFunction func)
{
    uint32_t slot = 0u;
    if (!generatedFunctionTableSlot(address, slot))
    {
        std::cerr << "[function-table] cannot replace guest PC 0x" << std::hex << address
                  << ": outside generated dense table [0x" << g_ps2RecompiledFunctionTableBase
                  << ", 0x" << g_ps2RecompiledFunctionTableEnd << ")"
                  << std::dec << std::endl;
        return false;
    }

    g_ps2RecompiledFunctionTable[slot] = func;
    return true;
}

bool PS2Runtime::registerFunction(uint32_t address, RecompiledFunction func)
{
    return replaceFunction(address, func);
}

bool PS2Runtime::hasFunction(uint32_t address) const
{
    uint32_t slot = 0u;
    return generatedFunctionTableSlot(address, slot) && g_ps2RecompiledFunctionTable[slot] != nullptr;
}

const char *describeGuestBranchKind(PS2Runtime::GuestBranchKind kind)
{
    switch (kind)
    {
    case PS2Runtime::GuestBranchKind::DirectJump:
        return "DirectJump";
    case PS2Runtime::GuestBranchKind::DirectCall:
        return "DirectCall";
    case PS2Runtime::GuestBranchKind::IndirectJump:
        return "IndirectJump";
    case PS2Runtime::GuestBranchKind::IndirectCall:
        return "IndirectCall";
    case PS2Runtime::GuestBranchKind::Return:
        return "Return";
    default:
        return "Unknown";
    }
}

PS2Runtime::RecompiledFunction PS2Runtime::lookupFunction(uint32_t address)
{
    pushDispatchPc(address);

    uint32_t slot = 0u;
    if (generatedFunctionTableSlot(address, slot))
    {
        RecompiledFunction fn = g_ps2RecompiledFunctionTable[slot];
        if (fn != nullptr)
        {
            return fn;
        }
    }

    std::cerr << "Error: No exact recompiled function for guest PC 0x" << std::hex << address
              << " tableBase=0x" << g_ps2RecompiledFunctionTableBase
              << " tableEnd=0x" << g_ps2RecompiledFunctionTableEnd
              << " codeRegion=" << (m_memory.isCodeAddress(address) ? "yes" : "no")
              << " trace=" << formatDispatchHistory()
              << std::dec << std::endl;

    static RecompiledFunction missingFunction = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t badPc = ctx->pc;
        runtime->reportMissingFunction(rdram,
                                       ctx,
                                       badPc,
                                       0u,
                                       PS2Runtime::GuestBranchKind::IndirectJump,
                                       "dispatch");
    };

    return missingFunction;
}

void PS2Runtime::setMissingFunctionPolicy(MissingFunctionPolicy policy)
{
    m_missingFunctionPolicy.store(static_cast<uint32_t>(policy), std::memory_order_release);
}

PS2Runtime::MissingFunctionPolicy PS2Runtime::missingFunctionPolicy() const
{
    return static_cast<MissingFunctionPolicy>(m_missingFunctionPolicy.load(std::memory_order_acquire));
}

void PS2Runtime::resetMissingFunctionReportOnce()
{
    m_missingFunctionReported.store(false, std::memory_order_release);
}

void PS2Runtime::reportMissingFunction(uint8_t *rdram,
                                       R5900Context *ctx,
                                       uint32_t targetPc,
                                       uint32_t sourcePc,
                                       GuestBranchKind kind,
                                       const char *debugName)
{
#if defined(PS2X_NATIVE_HOST)
    {
        extern void ps2DumpBranchRing();
        ps2DumpBranchRing();
    }
#endif
    const MissingFunctionPolicy policy = missingFunctionPolicy();
    const bool firstReport = !m_missingFunctionReported.exchange(true, std::memory_order_acq_rel);

    const uint32_t pc = ctx->pc;
    const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
    const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
    const uint32_t gp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0));
    const uint32_t a0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0));
    const uint32_t a1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0));
    const uint32_t a2 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[6], 0));
    const uint32_t a3 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[7], 0));
    const uint32_t s0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[16], 0));
    const uint32_t s1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[17], 0));
    const uint32_t v0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[2], 0));
    const uint32_t v1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[3], 0));

    auto readGuestU32At = [rdram](uint32_t addr, uint32_t &out) -> bool
    {
        // TODO this !rdram exist only because of test fix those test later
        if (!rdram || addr > PS2_RAM_SIZE - sizeof(uint32_t))
        {
            out = 0u;
            return false;
        }

        std::memcpy(&out, rdram + addr, sizeof(uint32_t));
        return true;
    };

    auto readGuestU32Offset = [&readGuestU32At](uint32_t base, uint32_t offset, uint32_t &out) -> bool
    {
        if (base > PS2_RAM_SIZE - sizeof(uint32_t) || offset > PS2_RAM_SIZE - sizeof(uint32_t) - base)
        {
            out = 0u;
            return false;
        }

        return readGuestU32At(base + offset, out);
    };

    uint32_t a0Word0 = 0u;
    uint32_t a0Word4 = 0u;
    uint32_t a0Word8 = 0u;
    uint32_t a0WordC = 0u;
    const bool a0Readable =
        readGuestU32Offset(a0, 0x00u, a0Word0) &&
        readGuestU32Offset(a0, 0x04u, a0Word4) &&
        readGuestU32Offset(a0, 0x08u, a0Word8) &&
        readGuestU32Offset(a0, 0x0cu, a0WordC);

    uint32_t s0Word0 = 0u;
    uint32_t s0Word4 = 0u;
    uint32_t s0Word8 = 0u;
    uint32_t s0WordC = 0u;
    const bool s0Readable =
        readGuestU32Offset(s0, 0x00u, s0Word0) &&
        readGuestU32Offset(s0, 0x04u, s0Word4) &&
        readGuestU32Offset(s0, 0x08u, s0Word8) &&
        readGuestU32Offset(s0, 0x0cu, s0WordC);

    uint32_t recordWord0 = 0u;
    uint32_t recordWord4 = 0u;
    uint32_t recordWord8 = 0u;
    uint32_t recordWordC = 0u;
    const bool recordReadable =
        s0Readable && s0Word4 != 0u &&
        readGuestU32Offset(s0Word4, 0x00u, recordWord0) &&
        readGuestU32Offset(s0Word4, 0x04u, recordWord4) &&
        readGuestU32Offset(s0Word4, 0x08u, recordWord8) &&
        readGuestU32Offset(s0Word4, 0x0cu, recordWordC);

    uint32_t vtableSlot0 = 0u;
    uint32_t vtableSlot4 = 0u;
    uint32_t vtableSlot8 = 0u;
    uint32_t vtableSlotC = 0u;
    const bool vtableReadable =
        a0Readable && a0Word0 != 0u &&
        readGuestU32Offset(a0Word0, 0x00u, vtableSlot0) &&
        readGuestU32Offset(a0Word0, 0x04u, vtableSlot4) &&
        readGuestU32Offset(a0Word0, 0x08u, vtableSlot8) &&
        readGuestU32Offset(a0Word0, 0x0cu, vtableSlotC);

    if (firstReport)
    {
        std::ostringstream oss;
        oss << "[guest-branch:missing-target] kind=" << describeGuestBranchKind(kind)
            << " op=" << (debugName ? debugName : "<unknown>")
            << " source=0x" << std::hex << sourcePc
            << " target=0x" << targetPc
            << " pc=0x" << pc
            << " ra=0x" << ra
            << " sp=0x" << sp
            << " gp=0x" << gp
            << " a0=0x" << a0
            << " a1=0x" << a1
            << " a2=0x" << a2
            << " a3=0x" << a3
            << " s0=0x" << s0
            << " s1=0x" << s1
            << " v0=0x" << v0
            << " v1=0x" << v1
            << " a0Readable=" << (a0Readable ? "yes" : "no")
            << " a0[0]=0x" << a0Word0
            << " a0[4]=0x" << a0Word4
            << " a0[8]=0x" << a0Word8
            << " a0[c]=0x" << a0WordC
            << " s0Readable=" << (s0Readable ? "yes" : "no")
            << " s0[0]=0x" << s0Word0
            << " s0[4]=0x" << s0Word4
            << " s0[8]=0x" << s0Word8
            << " s0[c]=0x" << s0WordC
            << " recordReadable=" << (recordReadable ? "yes" : "no")
            << " record[0]=0x" << recordWord0
            << " record[4]=0x" << recordWord4
            << " record[8]=0x" << recordWord8
            << " record[c]=0x" << recordWordC
            << " vtableReadable=" << (vtableReadable ? "yes" : "no")
            << " vtbl[0]=0x" << vtableSlot0
            << " vtbl[4]=0x" << vtableSlot4
            << " vtbl[8]=0x" << vtableSlot8
            << " vtbl[c]=0x" << vtableSlotC
            << " codeRegion=" << (m_memory.isCodeAddress(targetPc) ? "yes" : "no")
            << " policy=" << static_cast<uint32_t>(policy)
            << " trace=" << formatDispatchHistory()
            << std::dec;

        static std::mutex s_missingFunctionLogMutex;
        {
            std::lock_guard<std::mutex> lock(s_missingFunctionLogMutex);
            std::cerr << oss.str() << std::endl;
        }
    }

    if (firstReport && policy == MissingFunctionPolicy::BreakOnce)
    {
#if defined(_MSC_VER)
        __debugbreak();
#endif // TODO others breakpoints
    }

    if (ctx)
    {
        ctx->pc = targetPc;
    }

    if (policy == MissingFunctionPolicy::Stop)
    {
        requestStop();
    }
}

// Time of the first call of PS2X_TRACE_ANCHOR_PC (-1 until then); see dispatchGuestBranch.
std::atomic<double> g_ps2xTraceAnchorAt{-1.0};
#if defined(PS2X_NATIVE_HOST)
namespace ps2host
{
    extern uint32_t g_autoAnchorPc;
    extern int g_autoAnchorCount;
    extern std::atomic<double> g_autoAnchorAt[32];
    extern uint32_t g_autoWatchPc[8];
    extern int g_autoWatchN;
}
#endif
bool PS2Runtime::dispatchGuestBranch(uint8_t *rdram,
                                     R5900Context *ctx,
                                     uint32_t targetPc,
                                     uint32_t sourcePc,
                                     uint32_t fallthroughPc,
                                     GuestBranchKind kind,
                                     const char *debugName)
{
    ctx->pc = targetPc;
    const bool isCall = (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall);
#if defined(PS2X_NATIVE_HOST)
    {
        extern void ps2RecordBranch(uint32_t source, uint32_t target, uint32_t ra, uint32_t sp);
        extern uint8_t *g_dbgRdram;
        g_dbgRdram = rdram;
        ps2RecordBranch(sourcePc, targetPc, GPR_U32(ctx, 31), GPR_U32(ctx, 29));
        // PS2X_AUTOINPUT "@t:BUTTON" entries count from the first call of PS2X_AUTOINPUT_ANCHOR.
        // PS2X_TRACE_ANCHOR_PC=<hex pc>: the first call stamps g_ps2xTraceAnchorAt, which
        // PS2X_GS_TRACE_ANCHOR=<sec> / PS2X_RAM_DUMP_ANCHOR=<sec> windows count from.
        static const uint32_t s_traceAnchorPc = std::getenv("PS2X_TRACE_ANCHOR_PC") ? static_cast<uint32_t>(std::strtoul(std::getenv("PS2X_TRACE_ANCHOR_PC"), nullptr, 16)) : 0u;
        if (s_traceAnchorPc != 0u && targetPc == s_traceAnchorPc && g_ps2xTraceAnchorAt.load() < 0.0)
        {
            g_ps2xTraceAnchorAt.store(ps2host::timeSeconds());
            std::printf("[trace-anchor] %08x first call at t=%.1f\n", targetPc, ps2host::timeSeconds());
        }
        if (ps2host::g_autoAnchorPc != 0u || ps2host::g_autoWatchN != 0)
        {
            static int s_anchorHits = 0;
            if (targetPc == ps2host::g_autoAnchorPc && s_anchorHits < 32)
            {
                ps2host::g_autoAnchorAt[s_anchorHits++].store(ps2host::timeSeconds());
                std::printf("[autoinput] anchor %08x hit #%d at t=%.1f\n", targetPc, s_anchorHits, ps2host::timeSeconds());
            }
            for (int i = 0; i < ps2host::g_autoWatchN; ++i)
            {
                if (targetPc != ps2host::g_autoWatchPc[i])
                    continue;
                static int s_left[8] = {40, 40, 40, 40, 40, 40, 40, 40};
                if (s_left[i]-- > 0)
                    std::printf("[autoinput] watch %08x call at t=%.1f\n", targetPc, ps2host::timeSeconds());
            }
        }
    }
#endif

    // Every inter-function transfer is also a deterministic EE safe point.
    // Backward edges inside generated functions use eeCheckpointDue(), while
    // this charge bounds straight-line call chains that have no local loop.
    if (m_eeScheduler && m_eeScheduler->checkpointDue(EeScheduler::kGuestDispatchCycles))
    {
        return false;
    }

    if (!isCall)
    {
        if (!hasFunction(targetPc))
        {
            reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);
        }

        ctx->pc = targetPc;
        return false;
    }

    if (!hasFunction(targetPc))
    {
        reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);

        const MissingFunctionPolicy policy = missingFunctionPolicy();

        if (policy == MissingFunctionPolicy::SkipCallDebug && isCall)
        {
            ctx->pc = fallthroughPc;
            return true;
        }

        if (policy == MissingFunctionPolicy::ContinueToTarget)
        {
            ctx->pc = targetPc;
            // if you need the app to keep open to open debug pannel change this to false
            return false;
        }

        return false;
    }

#if defined(PS2X_NATIVE_HOST)
    {
        // PS2X_CALL_TRACE=<substr>[,<substr>...]: log guest calls whose symbol matches (a0-a3, and
        // argv strings for (this, argc, argv) handlers such as the shell's On*_Tag callbacks).
        static const char *s_filter = std::getenv("PS2X_CALL_TRACE");
        if (s_filter && *s_filter)
        {
            static std::unordered_map<uint32_t, bool> s_match;
            static int s_budget = std::getenv("PS2X_CALL_TRACE_MAX") ? std::atoi(std::getenv("PS2X_CALL_TRACE_MAX")) : 4000;
            auto it = s_match.find(targetPc);
            if (it == s_match.end())
            {
                const char *name = guestSymbolName(targetPc);
                bool m = false;
                if (name)
                {
                    std::string f(s_filter);
                    size_t start = 0;
                    while (start <= f.size())
                    {
                        const size_t comma = f.find(',', start);
                        const std::string part = f.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                        if (!part.empty() && std::strstr(name, part.c_str()))
                            m = true;
                        if (comma == std::string::npos)
                            break;
                        start = comma + 1;
                    }
                }
                it = s_match.emplace(targetPc, m).first;
            }
            if (it->second && s_budget > 0)
            {
                --s_budget;
                const uint32_t a0 = GPR_U32(ctx, 4), a1 = GPR_U32(ctx, 5), a2 = GPR_U32(ctx, 6), a3 = GPR_U32(ctx, 7);
                std::printf("[call] %s a0=%x a1=%x a2=%x a3=%x", guestSymbolName(targetPc), a0, a1, a2, a3);
                static const bool s_wide = std::getenv("PS2X_CALL_TRACE_WIDE") != nullptr;
                if (s_wide)
                    std::printf(" A0=%llx A1=%llx A2=%llx A3=%llx f12=%08x",
                                static_cast<unsigned long long>(GPR_U64(ctx, 4)), static_cast<unsigned long long>(GPR_U64(ctx, 5)),
                                static_cast<unsigned long long>(GPR_U64(ctx, 6)), static_cast<unsigned long long>(GPR_U64(ctx, 7)),
                                [&]() { uint32_t b; std::memcpy(&b, &ctx->f[12], 4); return b; }());
                auto str = [&](uint32_t p) -> const char * {
                    p &= 0x1FFFFFFFu;
                    if (p == 0u || p >= PS2_RAM_SIZE - 1u)
                        return nullptr;
                    const char *s = reinterpret_cast<const char *>(rdram + p);
                    for (uint32_t i = 0; i < 64u && p + i < PS2_RAM_SIZE; ++i)
                    {
                        if (s[i] == 0)
                            return i > 0 ? s : nullptr;
                        if (static_cast<unsigned char>(s[i]) < 32u || static_cast<unsigned char>(s[i]) > 126u)
                            return nullptr;
                    }
                    return nullptr;
                };
                if (a1 > 0u && a1 < 8u && (a2 & 0x1FFFFFFFu) + a1 * 4u < PS2_RAM_SIZE)
                {
                    for (uint32_t i = 0; i < a1; ++i)
                    {
                        uint32_t sp;
                        std::memcpy(&sp, rdram + (a2 & 0x1FFFFFFFu) + i * 4u, 4);
                        if (const char *sv = str(sp))
                            std::printf(" argv%u=\"%.40s\"", i, sv);
                    }
                }
                else if (const char *s1 = str(a1))
                    std::printf(" a1s=\"%.48s\"", s1);
                else if (const char *s0 = str(a0))
                    std::printf(" a0s=\"%.48s\"", s0);
                std::printf("\n");
                std::fflush(stdout); // tests kill the process; keep the trace
            }
        }
    }
#endif
    RecompiledFunction targetFn = lookupFunction(targetPc);
    const uint32_t entryPc = ctx->pc;
    const uint32_t spBefore = GPR_U32(ctx, 29);
    extern uint64_t g_eeTransferSeq;
    const uint64_t transferSeqBefore = g_eeTransferSeq;
    targetFn(rdram, ctx, this);

    if (isStopRequested() || ctx->pc == 0u)
    {
        return false;
    }

    // A callee that leaves pc at its own entry normally returned without setting pc (HLE stubs).
    // But if it (or anything it called) suspended at a checkpoint, pc == entry can also mean "resume
    // a *nested* call of this same function" (e.g. _rwDMAAddPkt -> _rwDMAClosePkt -> rwDMAFlushPURef
    // -> _rwDMAAddPkt): treating that as a return skipped the rest of the call chain with the inner
    // frame's sp, and the next return jumped into the heap (title-screen freeze at 0x8a80c0).
    if (ctx->pc == entryPc && g_eeTransferSeq == transferSeqBefore)
    {
        ctx->pc = fallthroughPc;
    }

#if defined(PS2X_NATIVE_HOST)
    {
        // PS2X_SPCHECK=1: a call that returns normally must leave sp as it found it.
        static const bool s_spCheck = std::getenv("PS2X_SPCHECK") != nullptr;
        static int s_spReports = 0;
        if (s_spCheck && ctx->pc == fallthroughPc && GPR_U32(ctx, 29) != spBefore && s_spReports < 10)
        {
            ++s_spReports;
            std::printf("[spcheck] call %08x (%s) from %08x (%s) returned sp=0x%x, was 0x%x (ra=0x%x)\n", targetPc,
                        guestSymbolName(targetPc), sourcePc, guestSymbolName(sourcePc), GPR_U32(ctx, 29), spBefore,
                        GPR_U32(ctx, 31));
            std::fflush(stdout);
        }
    }
#endif

    return ctx->pc == fallthroughPc;
}

void PS2Runtime::SignalException(R5900Context *ctx, PS2Exception exception)
{
    if (exception == EXCEPTION_INTEGER_OVERFLOW)
    {
        HandleIntegerOverflow(ctx);
        return;
    }

    raiseCop0Exception(ctx, static_cast<uint32_t>(exception),
                       exception == EXCEPTION_TLB_REFILL);
}

std::atomic<uint64_t> g_vu0MicroRuns{0}; // PS2X_VU_STATS
std::atomic<uint32_t> g_vu0LastPc{0};

void PS2Runtime::executeVU0Microprogram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    (void)rdram;
    g_vu0MicroRuns.fetch_add(1, std::memory_order_relaxed);
    g_vu0LastPc.store(address, std::memory_order_relaxed);
    {
        // PS2X_VU_STATS: VU0 micro programs run on the VU interpreter -- report how often and where from.
        static const bool s_stats = std::getenv("PS2X_VU_STATS") != nullptr;
        static std::unordered_map<uint64_t, uint64_t> s_pcs;
        static auto s_last = std::chrono::steady_clock::now();
        if (s_stats)
        {
            ++s_pcs[(static_cast<uint64_t>(address) << 32) | GPR_U32(ctx, 31)];
            const auto now = std::chrono::steady_clock::now();
            if (now - s_last >= std::chrono::seconds(2))
            {
                s_last = now;
                std::printf("[vu0-micro] calls/2s by (pc, ra):");
                for (const auto &[k, n] : s_pcs)
                    std::printf(" %x/%x:%llu", static_cast<uint32_t>(k >> 32), static_cast<uint32_t>(k), (unsigned long long)n);
                std::printf("%c", 10);
                s_pcs.clear();
            }
        }
    }

    uint8_t *const vu0Code = m_memory.getVU0Code();
    uint8_t *const vu0Data = m_memory.getVU0Data();
    const uint32_t startPC = address & ~0x7u;

    if (!vu0Code || !vu0Data || startPC + 8u > PS2_VU0_CODE_SIZE)
    {
        seedVu0IdleSuccess(ctx);
        return;
    }

    m_vu0.reset();
    copyVu0ContextToState(ctx, m_vu0.state());
    m_vu0.execute(vu0Code, PS2_VU0_CODE_SIZE,
                  vu0Data, PS2_VU0_DATA_SIZE,
                  m_gs, &m_memory,
                  startPC, 0u, ctx->vu0_itop, 4096);
    copyVu0StateToContext(m_vu0.state(), ctx);
}

void PS2Runtime::vu0StartMicroProgram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    // VCALLMS and VCALLMSR both route here.
    executeVU0Microprogram(rdram, ctx, address);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx)
{
    handleSyscall(rdram, ctx, 0);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx, uint32_t encodedSyscallId)
{
    if (ctx->in_delay_slot)
    {
        throw std::runtime_error("Attempted to execute a syscall inside a branch delay slot! "
                                 "This breaks the atomic basic block model and is structurally unsupported by the emulator.");
    }

    const uint32_t syscallId = (encodedSyscallId != 0u)
                                   ? encodedSyscallId
                                   : getRegU32(ctx, 3); // $v1 / $3 is the EE kernel syscall number

    if (ps2_syscalls::dispatchNumericSyscall(syscallId, rdram, ctx, this))
    {
        return;
    }

    // God help you
    ps2_syscalls::TODO(rdram, ctx, this, encodedSyscallId);
}

void PS2Runtime::handleBreak(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_BREAKPOINT);
}

void PS2Runtime::drainCompletedDmacHandlers(uint8_t *rdram)
{
    for (uint32_t cause : m_memory.consumeCompletedDmacCauses())
    {
        ps2_syscalls::dispatchDmacHandlersForCause(rdram, this, cause);
    }
}

void PS2Runtime::handleTrap(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_TRAP);
}

void PS2Runtime::handleTLBR(uint8_t *rdram, R5900Context *ctx)
{
    uint32_t vpn = 0;
    uint32_t pfn = 0;
    uint32_t mask = 0;
    bool valid = false;

    const uint32_t index = ctx->cop0_index & 0x3Fu;
    if (!m_memory.tlbRead(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Preserve low ASID bits in EntryHi.
    ctx->cop0_entryhi = (ctx->cop0_entryhi & 0x00000FFFu) | (vpn & 0xFFFFF000u);
    ctx->cop0_entrylo0 = (ctx->cop0_entrylo0 & ~0x03FFFFC2u) |
                         ((pfn & 0x000FFFFFu) << 6) |
                         (valid ? 0x2u : 0u);
    ctx->cop0_pagemask = mask & 0x01FFE000u;
}

void PS2Runtime::handleTLBWI(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t index = ctx->cop0_index & 0x3Fu;
    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
    }
}

void PS2Runtime::handleTLBWR(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t entryCount = static_cast<uint32_t>(m_memory.tlbEntryCount());
    if (entryCount == 0)
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    const uint32_t wired = std::min(ctx->cop0_wired, entryCount - 1);
    uint32_t random = ctx->cop0_random % entryCount;
    if (random < wired)
    {
        random = wired;
    }

    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(random, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Keep COP0 bookkeeping in sync with the selected slot.
    ctx->cop0_index = (ctx->cop0_index & ~0x3Fu) | (random & 0x3Fu);
    ctx->cop0_random = (random <= wired) ? (entryCount - 1) : (random - 1);
}

void PS2Runtime::handleTLBP(uint8_t *rdram, R5900Context *ctx)
{
    const int32_t index = m_memory.tlbProbe(ctx->cop0_entryhi & 0xFFFFF000u);
    if (index >= 0)
    {
        ctx->cop0_index = (ctx->cop0_index & ~0x8000003Fu) |
                          (static_cast<uint32_t>(index) & 0x3Fu);
    }
    else
    {
        // MIPS sets probe failure bit (P) in Index[31].
        ctx->cop0_index |= 0x80000000u;
    }
}

void PS2Runtime::clearLLBit(R5900Context *ctx)
{
    // LL/SC reservation is tracked separately from COP0 Status.
    ctx->llbit = 0;
    ctx->lladdr = 0;
}

uint32_t PS2Runtime::alignGuestHeapValue(uint32_t value, uint32_t alignment)
{
    if (alignment == 0)
    {
        return value;
    }

    const uint32_t mask = alignment - 1u;
    if (value > (std::numeric_limits<uint32_t>::max() - mask))
    {
        return std::numeric_limits<uint32_t>::max();
    }
    return (value + mask) & ~mask;
}

bool PS2Runtime::isGuestHeapAlignmentValid(uint32_t alignment)
{
    return alignment != 0u && (alignment & (alignment - 1u)) == 0u;
}

uint32_t PS2Runtime::normalizeGuestHeapAlignment(uint32_t alignment)
{
    if (!isGuestHeapAlignmentValid(alignment))
    {
        return kGuestHeapDefaultAlignment;
    }
    return std::max(alignment, kGuestHeapDefaultAlignment);
}

uint32_t PS2Runtime::clampGuestHeapBase(uint32_t guestBase) const
{
    uint32_t normalized = guestBase;
    if (normalized >= PS2_RAM_SIZE)
    {
        normalized &= PS2_RAM_MASK;
    }
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    return std::min(normalized, hardLimit);
}

uint32_t PS2Runtime::clampGuestHeapLimit(uint32_t guestLimit) const
{
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    if (guestLimit == 0u || guestLimit > hardLimit)
    {
        return hardLimit;
    }
    return guestLimit;
}

void PS2Runtime::resetGuestHeapLocked(uint32_t guestBase, uint32_t guestLimit)
{
    uint32_t base = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    uint32_t limit = clampGuestHeapLimit(guestLimit);
    if (base == 0u)
    {
        const uint32_t fallbackBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
        base = alignGuestHeapValue(clampGuestHeapBase(fallbackBase), kGuestHeapDefaultAlignment);
    }

    if (limit <= base)
    {
        base = alignGuestHeapValue(clampGuestHeapBase(m_guestHeapSuggestedBase), kGuestHeapDefaultAlignment);
        limit = clampGuestHeapLimit(0u);
    }

    if (limit <= base)
    {
        base = 0u;
        limit = 0u;
    }

    m_guestHeapBlocks.clear();
    if (limit > base)
    {
        m_guestHeapBlocks.push_back({base, limit - base, true});
    }

    m_guestHeapBase = base;
    m_guestHeapEnd = base;
    m_guestHeapLimit = limit;
    m_guestHeapConfigured = true;
}

void PS2Runtime::ensureGuestHeapInitializedLocked()
{
    if (m_guestHeapConfigured)
    {
        return;
    }

    const uint32_t suggested = (m_guestHeapSuggestedBase == 0u) ? kGuestHeapDefaultBase : m_guestHeapSuggestedBase;
    resetGuestHeapLocked(suggested, clampGuestHeapLimit(0u));
}

int32_t PS2Runtime::findGuestHeapBlockIndexLocked(uint32_t guestAddr) const
{
    const uint32_t normalizedAddr = guestAddr & PS2_RAM_MASK;
    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock &block = m_guestHeapBlocks[i];
        if (!block.free && block.addr == normalizedAddr)
        {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

uint32_t PS2Runtime::allocateGuestBlockLocked(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    if (size > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock block = m_guestHeapBlocks[i];
        if (!block.free)
        {
            continue;
        }

        const uint64_t blockStart = block.addr;
        const uint64_t blockEnd = blockStart + static_cast<uint64_t>(block.size);
        const uint32_t alignedAddr = alignGuestHeapValue(block.addr, normalizedAlignment);
        if (alignedAddr < block.addr)
        {
            continue;
        }

        const uint64_t alignedStart = alignedAddr;
        if (alignedStart > blockEnd)
        {
            continue;
        }

        const uint64_t allocEnd = alignedStart + static_cast<uint64_t>(allocSize);
        if (allocEnd > blockEnd)
        {
            continue;
        }

        const uint32_t prefixSize = static_cast<uint32_t>(alignedStart - blockStart);
        const uint32_t suffixSize = static_cast<uint32_t>(blockEnd - allocEnd);

        std::vector<GuestHeapBlock> replacement;
        replacement.reserve(3);
        if (prefixSize > 0u)
        {
            replacement.push_back({block.addr, prefixSize, true});
        }
        replacement.push_back({alignedAddr, allocSize, false});
        if (suffixSize > 0u)
        {
            replacement.push_back({static_cast<uint32_t>(allocEnd), suffixSize, true});
        }

        m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
        m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i),
                                 replacement.begin(),
                                 replacement.end());

        m_guestHeapEnd = std::max(m_guestHeapEnd, static_cast<uint32_t>(allocEnd));
        return alignedAddr;
    }

    return 0u;
}

void PS2Runtime::coalesceGuestHeapLocked()
{
    if (m_guestHeapBlocks.empty())
    {
        return;
    }

    size_t i = 1;
    while (i < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &prev = m_guestHeapBlocks[i - 1];
        GuestHeapBlock &curr = m_guestHeapBlocks[i];
        const uint64_t prevEnd = static_cast<uint64_t>(prev.addr) + static_cast<uint64_t>(prev.size);
        if (prev.free && curr.free && prevEnd == curr.addr)
        {
            prev.size += curr.size;
            m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

void PS2Runtime::freeGuestBlockLocked(uint32_t guestAddr)
{
    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return;
    }

    m_guestHeapBlocks[static_cast<size_t>(index)].free = true;
    coalesceGuestHeapLocked();
}

void PS2Runtime::configureGuestHeap(uint32_t guestBase, uint32_t guestLimit)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    uint32_t normalizedBase = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    if (normalizedBase == 0u)
    {
        normalizedBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
    }
    m_guestHeapSuggestedBase = normalizedBase;
    resetGuestHeapLocked(normalizedBase, guestLimit);
}

uint32_t PS2Runtime::guestMalloc(uint32_t size, uint32_t alignment)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    return allocateGuestBlockLocked(size, alignment);
}

uint32_t PS2Runtime::guestCalloc(uint32_t count, uint32_t size, uint32_t alignment)
{
    if (count == 0u || size == 0u)
    {
        return 0u;
    }
    if (count > (std::numeric_limits<uint32_t>::max() / size))
    {
        return 0u;
    }

    const uint32_t totalSize = count * size;
    const uint32_t guestAddr = guestMalloc(totalSize, alignment);
    if (guestAddr != 0u)
    {
        uint8_t *rdram = m_memory.getRDRAM();
        if (rdram)
        {
            uint32_t physAddr = guestAddr & PS2_RAM_MASK;
            if (physAddr + totalSize <= PS2_RAM_SIZE)
                std::memset(rdram + physAddr, 0, totalSize);
        }
    }

    return guestAddr;
}

uint32_t PS2Runtime::guestRealloc(uint32_t guestAddr, uint32_t newSize, uint32_t alignment)
{
    if (guestAddr == 0u)
    {
        return guestMalloc(newSize, alignment);
    }
    if (newSize == 0u)
    {
        guestFree(guestAddr);
        return 0u;
    }

    if (newSize > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t requestedSize = alignGuestHeapValue(newSize, kGuestHeapDefaultAlignment);

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();

    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return 0u;
    }

    const size_t blockIndex = static_cast<size_t>(index);
    const uint32_t oldAddr = m_guestHeapBlocks[blockIndex].addr;
    const uint32_t oldSize = m_guestHeapBlocks[blockIndex].size;

    if (requestedSize <= oldSize)
    {
        if (requestedSize < oldSize)
        {
            const uint32_t tailAddr = oldAddr + requestedSize;
            const uint32_t tailSize = oldSize - requestedSize;
            m_guestHeapBlocks[blockIndex].size = requestedSize;
            m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u),
                                     GuestHeapBlock{tailAddr, tailSize, true});
            coalesceGuestHeapLocked();
        }
        return oldAddr;
    }

    if (blockIndex + 1u < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &next = m_guestHeapBlocks[blockIndex + 1u];
        const uint64_t blockEnd = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].addr) +
                                  static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size);
        if (next.free && blockEnd == next.addr)
        {
            const uint64_t combined = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size) +
                                      static_cast<uint64_t>(next.size);
            if (combined >= requestedSize)
            {
                const uint32_t extraNeeded = requestedSize - m_guestHeapBlocks[blockIndex].size;
                m_guestHeapBlocks[blockIndex].size = requestedSize;
                if (next.size == extraNeeded)
                {
                    m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u));
                }
                else
                {
                    next.addr += extraNeeded;
                    next.size -= extraNeeded;
                }
                m_guestHeapEnd = std::max(m_guestHeapEnd, oldAddr + requestedSize);
                return oldAddr;
            }
        }
    }

    const uint32_t newAddr = allocateGuestBlockLocked(newSize, normalizedAlignment);
    if (newAddr == 0u)
    {
        return 0u;
    }

    uint8_t *rdram = m_memory.getRDRAM();
    if (rdram)
    {
        const uint32_t copyBytes = std::min(oldSize, newSize);
        uint32_t dstPhys = newAddr & PS2_RAM_MASK;
        uint32_t srcPhys = oldAddr & PS2_RAM_MASK;
        if (dstPhys + copyBytes <= PS2_RAM_SIZE && srcPhys + copyBytes <= PS2_RAM_SIZE)
            std::memmove(rdram + dstPhys, rdram + srcPhys, copyBytes);
    }

    freeGuestBlockLocked(oldAddr);
    return newAddr;
}

void PS2Runtime::guestFree(uint32_t guestAddr)
{
    if (guestAddr == 0u)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    freeGuestBlockLocked(guestAddr);
}

uint32_t PS2Runtime::guestHeapBase() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapBase : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapEnd() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapEnd : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapLimit() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapLimit : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::reserveAsyncCallbackStack(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
    uint32_t top = m_asyncCallbackStackTop;
    if (top > PS2_RAM_SIZE)
    {
        top = PS2_RAM_SIZE;
    }
    top &= ~(kGuestHeapDefaultAlignment - 1u);

    if (top <= allocSize)
    {
        return 0u;
    }

    uint32_t base = top - allocSize;
    base &= ~(normalizedAlignment - 1u);
    if (base < m_asyncCallbackStackFloor || base >= top)
    {
        return 0u;
    }

    m_asyncCallbackStackTop = base;
    return top - 0x10u;
}

uint8_t PS2Runtime::Load8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read8(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint16_t PS2Runtime::Load16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read16(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint32_t PS2Runtime::Load32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read32(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint64_t PS2Runtime::Load64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read64(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

__m128i PS2Runtime::Load128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read128(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return _mm_setzero_si128();
    }
}


// Debug aid: PS2X_EE_WATCH=<hex vaddr> logs guest stores (special-address path) that cover it.
// Frame limiter threshold used by the patched ps2_wait_for_vsync (tools/patch_60fps.py): number of
// VSyncs a game frame waits for. 1 = 60 fps when TargetFPS >= 60 (PS2X_FPS30=1 restores 30 fps).
#if defined(PS2X_NATIVE_HOST)
// Last guest control transfers (calls via dispatchGuestBranch; scheduler dispatches have source
// 0xEEEEEEEE), dumped when a branch lands on an unknown address.
namespace
{
    struct BranchRec { uint32_t source, target, ra, sp; };
    BranchRec g_branchRing[64];
    uint32_t g_branchRingPos = 0;

    // PS2X_HITCH_LOG: the last guest transfer, sampled every millisecond (racy reads; debug only).
    struct HitchSample { double t; uint32_t source, target, pos; };
    constexpr uint32_t kHitchSamples = 8192;
    HitchSample g_hitchSamples[kHitchSamples];
    std::atomic<uint32_t> g_hitchPos{0};
}
const EeScheduler *g_hitchScheduler = nullptr;
std::atomic<double> g_hitchLastFlip{0.0};

// A stall in progress (no flip for 100 ms): what every guest thread waits on.
static void ps2HitchDumpThreads(double stallMs)
{
    if (!g_hitchScheduler)
        return;
    static const char *const kStatus[] = {"run", "ready", "wait", "waitsusp", "susp", "dormant"};
    static const char *const kWait[] = {"-", "sleep", "sema", "evflag", "vsync", "external", "mpeg"};
    const EeKernelSnapshot snap = g_hitchScheduler->snapshot();
    std::printf("[hitch-threads] stall %.0f ms, running thread %d\n", stallMs, snap.runningThreadId);
    for (const EeThreadSnapshot &t : snap.threads)
    {
        if (t.status == EeThreadStatus::Dormant)
            continue;
        std::printf("[hitch-threads]   thread %d prio %d %s/%s id %d pc 0x%x ra 0x%x entry 0x%x\n", t.id, t.currentPriority,
                    kStatus[static_cast<int>(t.status) % 6], kWait[static_cast<int>(t.waitReason) % 7], t.waitId, t.pc, t.ra, t.entry);
    }
    for (const EeSemaphoreSnapshot &sm : snap.semaphores)
        if (sm.waiters)
            std::printf("[hitch-threads]   sema %d count %d/%d waiters %u\n", sm.id, sm.count, sm.maxCount, sm.waiters);
    std::fflush(stdout);
}
void ps2HitchSamplerStart()
{
    static std::once_flag s_once;
    std::call_once(s_once, []()
    {
        std::thread([]()
        {
            for (;;)
            {
                const uint32_t pos = *reinterpret_cast<volatile uint32_t *>(&g_branchRingPos);
                const BranchRec r = g_branchRing[(pos - 1u) & 63u];
                const uint32_t i = g_hitchPos.load(std::memory_order_relaxed);
                g_hitchSamples[i % kHitchSamples] = {ps2host::timeSeconds(), r.source, r.target, pos};
                g_hitchPos.store(i + 1u, std::memory_order_release);
                static double s_dumpedFlip = -1.0;
                const double lastFlip = g_hitchLastFlip.load(std::memory_order_relaxed);
                const double stallMs = (ps2host::timeSeconds() - lastFlip) * 1000.0;
                if (lastFlip > 0.0 && stallMs > 100.0 && s_dumpedFlip != lastFlip)
                {
                    s_dumpedFlip = lastFlip;
                    ps2HitchDumpThreads(stallMs);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }).detach();
    });
}
// Where a long frame went: the most frequent (call site -> target) pairs sampled in [fromT, toT].
void ps2HitchReport(double fromT, double toT)
{
    std::unordered_map<uint64_t, uint32_t> hist;
    uint32_t total = 0, still = 0, prevPos = 0;
    const uint32_t end = g_hitchPos.load(std::memory_order_acquire);
    for (uint32_t k = 1; k < kHitchSamples && k <= end; ++k)
    {
        const HitchSample &s = g_hitchSamples[(end - k) % kHitchSamples];
        if (s.t < fromT)
            break;
        if (s.t > toT)
            continue;
        ++hist[(static_cast<uint64_t>(s.source) << 32) | s.target];
        if (total && s.pos == prevPos)
            ++still; // no guest transfer for 1 ms: host code (I/O, wait) or a call-free loop
        prevPos = s.pos;
        ++total;
    }
    std::vector<std::pair<uint32_t, uint64_t>> top;
    for (const auto &[key, n] : hist)
        top.emplace_back(n, key);
    std::sort(top.begin(), top.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    std::printf("[hitch]   %u samples, %u without guest transfers\n", total, still);
    for (size_t k = 0; k < top.size() && k < 6u; ++k)
        std::printf("[hitch]   %3u/%u samples: last transfer 0x%x -> 0x%x\n", top[k].first, total,
                    static_cast<uint32_t>(top[k].second >> 32), static_cast<uint32_t>(top[k].second));
}
void ps2RecordBranch(uint32_t source, uint32_t target, uint32_t ra, uint32_t sp)
{
    // Called for every guest call/jump: without the debug options below only the ring is written.
    static const bool s_extra = std::getenv("PS2X_RING_SKIP") || std::getenv("PS2X_STACKSCAN") || std::getenv("PS2X_GLOBALWATCH");
    if (!s_extra)
    {
        g_branchRing[g_branchRingPos++ & 63u] = {source, target, ra, sp};
        return;
    }
    // PS2X_RING_SKIP=lo-hi (hex): don't record transfers from/to that code range (e.g. a polling loop
    // that would otherwise flush everything interesting out of the ring).
    static uint32_t s_skipLo = 0, s_skipHi = 0;
    static const bool s_skipInit = []()
    {
        if (const char *v = std::getenv("PS2X_RING_SKIP"))
        {
            char *end = nullptr;
            s_skipLo = static_cast<uint32_t>(std::strtoul(v, &end, 16));
            if (end && *end == '-')
                s_skipHi = static_cast<uint32_t>(std::strtoul(end + 1, nullptr, 16));
        }
        return true;
    }();
    (void)s_skipInit;
    if (s_skipHi && ((source >= s_skipLo && source < s_skipHi) || (target >= s_skipLo && target < s_skipHi)))
        return;
    g_branchRing[g_branchRingPos++ & 63u] = {source, target, ra, sp};

    // PS2X_STACKSCAN=lo-hi:value (hex): after every transfer, report the first time <value> appears
    // as a word in guest memory [lo,hi) (finds who writes a bad return address into a stack frame).
    extern uint8_t *g_dbgRdram;
    static uint32_t s_scanLo = 0, s_scanHi = 0, s_scanVal = 0;
    static bool s_scanDone = false;
    static const bool s_scanInit = []()
    {
        if (const char *v = std::getenv("PS2X_STACKSCAN"))
            std::sscanf(v, "%x-%x:%x", &s_scanLo, &s_scanHi, &s_scanVal);
        return true;
    }();
    (void)s_scanInit;
    // PS2X_GLOBALWATCH=addr:lo-hi (hex): report the first transfer after which the 32-bit guest word at
    // addr lies inside [lo,hi) (e.g. a DMA write pointer that wanders into the stack).
    static uint32_t s_gwAddr = 0, s_gwLo = 0, s_gwHi = 0;
    static bool s_gwDone = false;
    static uint32_t s_gwPrev = 0, s_gwPrevSrc = 0, s_gwPrevTgt = 0;
    static const bool s_gwInit = []()
    {
        if (const char *v = std::getenv("PS2X_GLOBALWATCH"))
            std::sscanf(v, "%x:%x-%x", &s_gwAddr, &s_gwLo, &s_gwHi);
        return true;
    }();
    (void)s_gwInit;
    if (s_gwHi && !s_gwDone && g_dbgRdram)
    {
        uint32_t w;
        std::memcpy(&w, g_dbgRdram + (s_gwAddr & 0x1FFFFFCu), 4);
        if (w >= s_gwLo && w < s_gwHi)
        {
            s_gwDone = true;
            std::printf("[globalwatch] [0x%x]=0x%x (was 0x%x before %08x->%08x); this transfer %08x -> %08x (%s -> %s) ra=%08x sp=%08x\n",
                        s_gwAddr, w, s_gwPrev, s_gwPrevSrc, s_gwPrevTgt, source, target, guestSymbolName(source),
                        guestSymbolName(target), ra, sp);
            for (uint32_t a = (s_gwAddr & ~0xFu) - 0x20u; a < (s_gwAddr & ~0xFu) + 0x40u; a += 16)
            {
                uint32_t q[4];
                std::memcpy(q, g_dbgRdram + a, 16);
                std::printf("  %08x: %08x %08x %08x %08x\n", a, q[0], q[1], q[2], q[3]);
            }
            extern void ps2DumpBranchRing();
            ps2DumpBranchRing();
        }
        s_gwPrev = w;
        s_gwPrevSrc = source;
        s_gwPrevTgt = target;
    }
    if (s_scanHi && !s_scanDone && g_dbgRdram)
    {
        for (uint32_t a = s_scanLo & ~3u; a + 4 <= s_scanHi; a += 4)
        {
            uint32_t w;
            std::memcpy(&w, g_dbgRdram + a, 4);
            if (w == s_scanVal)
            {
                s_scanDone = true;
                std::printf("[stackscan] 0x%x appeared at 0x%x; this transfer %08x -> %08x (%s -> %s) ra=%08x sp=%08x\n", s_scanVal,
                            a, source, target, guestSymbolName(source), guestSymbolName(target), ra, sp);
                extern void ps2DumpBranchRing();
                ps2DumpBranchRing();
                break;
            }
        }
    }
}
uint8_t *g_dbgRdram = nullptr;
void ps2DumpBranchRing()
{
    static int s_dumps = 0;
    if (s_dumps++ >= 2)
        return;
    std::printf("[branch-ring] last guest transfers (oldest first):\n");
    for (uint32_t i = 0; i < 64u; ++i)
    {
        const BranchRec &r = g_branchRing[(g_branchRingPos + i) & 63u];
        if (!r.target)
            continue;
        std::printf("  %08x %-40.40s -> %08x %-40.40s ra=%08x sp=%08x\n", r.source,
                    r.source == 0xEEEEEEEEu ? "<scheduler>" : guestSymbolName(r.source), r.target,
                    guestSymbolName(r.target), r.ra, r.sp);
    }
    std::fflush(stdout);
}
#endif

// COP0 condition for BC0F/BC0T (CPCOND0): true when every DMA channel selected in D_PCR.CPC has its
// D_STAT channel-end bit set. Games spin on it (bc0f loop) while a DMA transfer is running.
extern void ps2AsyncDmaApplyCompletions();
extern "C" bool ps2x_cpcond0(PS2Runtime *runtime)
{
    ps2AsyncDmaApplyCompletions();
    const uint32_t pcr = runtime->memory().readIORegister(0x1000E020u);
    const uint32_t stat = runtime->memory().readIORegister(0x1000E010u);
    return ((~pcr | stat) & 0x3FFu) == 0x3FFu;
}

extern "C" uint32_t ps2x_frame_vsync_threshold()
{
    static const uint32_t s_threshold = []() -> uint32_t
    {
        if (const char *v = std::getenv("PS2X_FPS30"); v && *v && *v != '0')
            return 2u;
#if defined(PS2X_NATIVE_HOST)
        return ps2host::config().targetFps >= 60u ? 1u : 2u;
#else
        return 2u;
#endif
    }();
    return s_threshold;
}

// Widescreen=1: the game's screen cameras get a 16:9 view window, called from RwCameraSetViewWindow's entry
// (tools/patch_widescreen.py). Only calls from the game's camera API (pcpl_set_camera_fov / _width_fov /
// _aspect_ratio / _view_window, 0x26f740-0x26fa6c) for a camera drawing to a screen-sized camera raster are
// widened: texture cameras (env maps, the arena screen feed) keep theirs. The 4:3 frame is shown stretched to
// 16:9, so the picture keeps its proportions and shows more at the sides.
extern "C" void ps2x_widescreen_view_window(uint8_t *rdram, uint32_t camera, uint32_t viewWindow, uint32_t ra)
{
#if defined(PS2X_NATIVE_HOST)
    static const bool s_on = ps2host::config().widescreen;
    static const bool s_log = std::getenv("PS2X_WS_LOG") != nullptr;
    if (!s_on || !rdram)
        return;
    if (ra < 0x26f740u || ra >= 0x26fa6cu)
        return;
    auto rd32 = [rdram](uint32_t a)
    {
        uint32_t v = 0;
        std::memcpy(&v, rdram + (a & 0x01FFFFFCu), 4);
        return v;
    };
    const uint32_t raster = rd32(camera + 0x60u); // RwCamera::frameBuffer
    const uint32_t projection = rd32(camera + 0x14u);
    const uint32_t type = raster ? (rdram[(raster + 0x20u) & 0x01FFFFFFu] & 7u) : 0u; // RwRaster::cType
    const uint32_t width = raster ? rd32(raster + 0x0Cu) : 0u;
    float x = 0.0f, y = 0.0f;
    std::memcpy(&x, rdram + (viewWindow & 0x01FFFFFCu), 4);
    std::memcpy(&y, rdram + ((viewWindow + 4u) & 0x01FFFFFCu), 4);
    const bool screen = raster != 0u && type == 2u && width >= 512u; // rwRASTERTYPECAMERA, full screen
    if (s_log)
    {
        static int s_shown = 0;
        if (s_shown++ < 64)
            std::printf("[widescreen] camera 0x%x proj %u raster 0x%x type %u width %u view %.3f x %.3f ra 0x%x -> %s\n",
                        camera, projection, raster, type, width, x, y, ra, screen ? "16:9" : "kept");
    }
    if (!screen)
        return;
    x *= (16.0f / 9.0f) / (4.0f / 3.0f);
    std::memcpy(rdram + (viewWindow & 0x01FFFFFCu), &x, 4);
#else
    (void)rdram, (void)camera, (void)viewWindow, (void)ra;
#endif
}

static void eeWatchStore(const R5900Context *ctx, uint32_t vaddr, uint32_t size, uint64_t lo, uint64_t hi)
{
    static const long long s_watch = std::getenv("PS2X_EE_WATCH") ? std::strtoll(std::getenv("PS2X_EE_WATCH"), nullptr, 16) : -1;
    static int s_count = 0;
    if (s_watch < 0 || s_count >= 60)
        return;
    const uint32_t w = static_cast<uint32_t>(s_watch);
    if (w < vaddr || w >= vaddr + size)
        return;
    ++s_count;
#if defined(PS2X_NATIVE_HOST)
    const char *fn = guestSymbolName(ctx ? ctx->pc : 0u);
#else
    const char *fn = "?";
#endif
    std::printf("[ee-watch] pc=0x%x (%s) addr=0x%x size=%u lo=%016llx hi=%016llx\n", ctx ? ctx->pc : 0u, fn, vaddr, size,
                static_cast<unsigned long long>(lo), static_cast<unsigned long long>(hi));
}

void PS2Runtime::Store8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint8_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 1u, value, 0u, "WRITE8", ctx);
    try
    {
        m_memory.write8(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint16_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 2u, value, 0u, "WRITE16", ctx);
    try
    {
        m_memory.write16(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint32_t value)
{
    eeWatchStore(ctx, vaddr, 4u, value, 0u);
    ps2TraceGuestWrite(rdram, vaddr, 4u, value, 0u, "WRITE32", ctx);
    try
    {
        m_memory.write32(vaddr, value);
        drainCompletedDmacHandlers(rdram);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint64_t value)
{
    eeWatchStore(ctx, vaddr, 8u, value, 0u);
    ps2TraceGuestWrite(rdram, vaddr, 8u, value, 0u, "WRITE64", ctx);
    try
    {
        m_memory.write64(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, __m128i value)
{
    alignas(16) uint64_t _parts[2];
    _mm_storeu_si128(reinterpret_cast<__m128i *>(_parts), value);
    eeWatchStore(ctx, vaddr, 16u, _parts[0], _parts[1]);
    ps2TraceGuestWrite(rdram, vaddr, 16u, _parts[0], _parts[1], "WRITE128", ctx);
    try
    {
        m_memory.write128(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::kickGifDmaChainFromMMIO(uint8_t *rdram,
                                         R5900Context *ctx,
                                         uint32_t dPcrValue,
                                         uint32_t dStatValue,
                                         uint32_t tadr,
                                         uint32_t chcr)
{
    constexpr uint32_t D_PCR = 0x1000E020u;
    constexpr uint32_t D_STAT = 0x1000E010u;
    constexpr uint32_t GIF_TADR = 0x1000A030u;
    constexpr uint32_t GIF_CHCR = 0x1000A000u;

    ps2TraceGuestWrite(rdram, D_PCR, 4u, dPcrValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_PCR, dPcrValue);
    ps2TraceGuestWrite(rdram, D_STAT, 4u, dStatValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_STAT, dStatValue);
    ps2TraceGuestWrite(rdram, GIF_TADR, 4u, tadr, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(GIF_TADR, tadr);
    ps2TraceGuestWrite(rdram, GIF_CHCR, 4u, chcr, 0u, "WRITE32", ctx);
    // The native GIF chain shortcuts drain the GS worker and then write the GS straight from the EE thread.
    // With the worker that raced it (texture garbage on texture-streaming arenas: HARDCORE's jumbotron,
    // car, HUD) and gained nothing measurable, so they only run without the worker (or PS2X_NATIVE_GIF=1).
    static const bool s_noNativeGif = std::getenv("PS2X_NO_NATIVE_GIF") != nullptr ||
                                      (ps2GsThreadEnabled() && std::getenv("PS2X_NATIVE_GIF") == nullptr);
    if (!s_noNativeGif && m_memory.tryProcessNativeGifImageUploadChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    if (!s_noNativeGif && m_memory.tryProcessNativeGifPackedChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    m_memory.writeIORegister(GIF_CHCR, chcr);
    m_memory.processPendingTransfers();
    drainCompletedDmacHandlers(rdram);
}

void PS2Runtime::requestStop()
{
    m_stopRequested.store(true, std::memory_order_relaxed);
    if (m_eeScheduler)
    {
        m_eeScheduler->requestStop();
    }
}

bool PS2Runtime::isStopRequested() const
{
    return m_stopRequested.load(std::memory_order_relaxed);
}

EeScheduler &PS2Runtime::eeScheduler()
{
    return *m_eeScheduler;
}

const EeScheduler &PS2Runtime::eeScheduler() const
{
    return *m_eeScheduler;
}

void PS2Runtime::postEeEvent(EeEvent event)
{
    m_eeScheduler->postEvent(event);
}

bool PS2Runtime::eeCheckpointDue(uint32_t cycles) noexcept
{
    return m_eeScheduler->checkpointDue(cycles);
}

[[noreturn]] void PS2Runtime::eeWaitVSyncTicks(uint32_t ticks, uint32_t resumePc)
{
    const uint64_t currentTick = m_eeScheduler->currentVSyncTick();
    const uint64_t waitTicks = std::max<uint64_t>(1u, ticks);
    m_eeScheduler->waitVSync(currentTick + waitTicks - 1u,
                             0,
                             [resumePc](R5900Context &context)
                             {
                                 context.pc = resumePc;
                             });
}

void PS2Runtime::addEeExitHandler(int threadId, uint32_t function, uint32_t argument)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers[threadId].push_back({function, argument});
}

std::vector<PS2Runtime::EeExitHandlerRegistration> PS2Runtime::takeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    auto it = m_eeExitHandlers.find(threadId);
    if (it == m_eeExitHandlers.end())
    {
        return {};
    }
    auto handlers = std::move(it->second);
    m_eeExitHandlers.erase(it);
    return handlers;
}

void PS2Runtime::removeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers.erase(threadId);
}

bool PS2Runtime::findEeSyscallOverride(uint32_t syscallNumber, uint32_t &handler) const
{
    std::lock_guard lock(m_eeKernelStateMutex);
    const auto it = m_eeSyscallOverrides.find(syscallNumber);
    if (it == m_eeSyscallOverrides.end())
    {
        return false;
    }
    handler = it->second;
    return true;
}

void PS2Runtime::setEeSyscallOverride(uint8_t *rdram, uint32_t syscallNumber, uint32_t handler)
{
    constexpr uint32_t kTableBase = 0x80011F80u & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
    const int64_t address = static_cast<int64_t>(kTableBase) + offset;

    std::lock_guard lock(m_eeKernelStateMutex);
    if (handler == 0u)
    {
        m_eeSyscallOverrides.erase(syscallNumber);
    }
    else
    {
        m_eeSyscallOverrides[syscallNumber] = handler;
    }
    if (!rdram || address < 0 || address + 4 > kMirrorLimit)
    {
        return;
    }
    const uint32_t guestAddress = static_cast<uint32_t>(address);
    std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
    if (handler == 0u)
    {
        m_eeSyscallMirrorAddresses.erase(guestAddress);
    }
    else
    {
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::initializeEeKernelState(uint8_t *rdram)
{
    if (!rdram)
    {
        return;
    }
    constexpr uint32_t kTableGuestBase = 0x80011F80u;
    constexpr uint32_t kTableBase = kTableGuestBase & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    constexpr uint32_t kProbeBase = 0x000002F0u;

    std::lock_guard lock(m_eeKernelStateMutex);
    for (const uint32_t address : m_eeSyscallMirrorAddresses)
    {
        const uint32_t zero = 0u;
        std::memcpy(rdram + address, &zero, sizeof(zero));
    }
    m_eeSyscallMirrorAddresses.clear();
    const uint32_t high = kTableGuestBase >> 16;
    const uint32_t low = kTableGuestBase & 0xFFFFu;
    std::memcpy(rdram + kProbeBase, &high, sizeof(high));
    std::memcpy(rdram + kProbeBase + 8u, &low, sizeof(low));
    m_eeSyscallMirrorAddresses.insert(kProbeBase);
    m_eeSyscallMirrorAddresses.insert(kProbeBase + 8u);

    for (const auto &[syscallNumber, handler] : m_eeSyscallOverrides)
    {
        const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
        const int64_t address = static_cast<int64_t>(kTableBase) + offset;
        if (address < 0 || address + 4 > kMirrorLimit)
        {
            continue;
        }
        const uint32_t guestAddress = static_cast<uint32_t>(address);
        std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::HandleIntegerOverflow(R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_INTEGER_OVERFLOW);
}

void PS2Runtime::run()
{
    m_stopRequested.store(false, std::memory_order_relaxed);
    ps2_stubs::resetSifState();
    resetIop();
    ps2_stubs::resetAudioStubState();
    ps2_stubs::resetMpegStubState();
    initializeEeKernelState(m_memory.getRDRAM());
    m_cpuContext.r[4] = _mm_setzero_si128();
    m_cpuContext.r[5] = _mm_setzero_si128();
    m_cpuContext.r[29] = _mm_set_epi64x(0, static_cast<int64_t>(PS2_RAM_SIZE - 0x10u));
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);
    m_debugRa.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)), std::memory_order_relaxed);
    m_debugSp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[29], 0)), std::memory_order_relaxed);
    m_debugGp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[28], 0)), std::memory_order_relaxed);

    RUNTIME_LOG("Starting execution at address 0x" << std::hex << m_cpuContext.pc << std::dec);

#if defined(PS2X_NATIVE_HOST)
    HostFrameTexture frameTex{};
#else
    // A blank image to use as a framebuffer
    Image blank = GenImageColor(FB_WIDTH, FB_HEIGHT, BLANK);
    Texture2D frameTex = LoadTextureFromImage(blank);
    UnloadImage(blank);
#endif

    std::atomic<bool> gameThreadFinished{false};

    std::thread gameThread([&]()
                           {
        ThreadNaming::SetCurrentThreadName("GameThread");
#if defined(PS2X_NATIVE_HOST)
        if (!std::getenv("PS2X_PROFILE_WORKER") && !std::getenv("PS2X_PROFILE_GS"))
            ps2host::profileCurrentThread();
#endif
        try
        {
            m_eeScheduler->reset(m_memory.getRDRAM(), m_cpuContext);
            m_eeScheduler->run();
            uint32_t pc = m_debugPc.load(std::memory_order_relaxed);
            RUNTIME_LOG("Game thread returned. PC=0x" << std::hex << pc
                      << " RA=0x" << static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)) << std::dec << std::endl);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error during program execution: " << e.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "Error during program execution: unknown exception" << std::endl;
        }
        gameThreadFinished.store(true, std::memory_order_release); });

    uint64_t tick = 0;
    while (!isStopRequested() && !gameThreadFinished.load(std::memory_order_acquire))
    {
        PS2_IF_AGRESSIVE_LOGS({
            tick++;
            if ((tick % 120) == 0)
            {
                uint64_t curDma = m_memory.dmaStartCount();
                uint64_t curGif = m_memory.gifCopyCount();
                uint64_t curGs = m_memory.gsWriteCount();
                uint64_t curVif = m_memory.vifWriteCount();
                const GSRegisters &gs = m_memory.gs();
                const uint32_t dbgPc = m_debugPc.load(std::memory_order_relaxed);
                const uint32_t dbgRa = m_debugRa.load(std::memory_order_relaxed);
                const uint32_t dbgSp = m_debugSp.load(std::memory_order_relaxed);
                const uint32_t dbgGp = m_debugGp.load(std::memory_order_relaxed);
                const auto eeSnapshot = m_eeScheduler->snapshot();

                RUNTIME_LOG("[run:tick] tick=" << tick
                                               << " pc=0x" << std::hex << dbgPc
                                               << " ra=0x" << dbgRa
                                               << " sp=0x" << dbgSp
                                               << " gp=0x" << dbgGp
                                               << " dispfb1=0x" << gs.dispfb1
                                               << " display1=0x" << gs.display1
                                               << std::dec
                                               << " activeThreads=" << eeSnapshot.threads.size()
                                               << " dma=" << curDma
                                               << " gif=" << curGif
                                               << " gsw=" << curGs
                                               << " vif=" << curVif
                                               << std::endl);

            }
        });
        uint32_t presentWidth = FB_WIDTH;
        uint32_t presentHeight = DEFAULT_DISPLAY_HEIGHT;
#if defined(PS2X_NATIVE_HOST)
        {
            extern const EeScheduler *g_hitchScheduler;
            g_hitchScheduler = m_eeScheduler.get();
            static const bool s_status = std::getenv("PS2X_STATUS") != nullptr;
            static double s_nextStatus = 0.0;
            if (s_status && ps2host::timeSeconds() >= s_nextStatus)
            {
                s_nextStatus = ps2host::timeSeconds() + 2.0;
                const GSRegisters &gsr = m_memory.gs();
                std::printf("[status] t=%.1f pc=0x%x ra=0x%x vsync=%llu dma=%llu gif=%llu gsw=%llu vif=%llu pmode=0x%llx dispfb1=0x%llx display1=0x%llx\n",
                            ps2host::timeSeconds(), m_debugPc.load(), m_debugRa.load(),
                            static_cast<unsigned long long>(m_eeScheduler->currentVSyncTick()),
                            static_cast<unsigned long long>(m_memory.dmaStartCount()),
                            static_cast<unsigned long long>(m_memory.gifCopyCount()),
                            static_cast<unsigned long long>(m_memory.gsWriteCount()),
                            static_cast<unsigned long long>(m_memory.vifWriteCount()),
                            static_cast<unsigned long long>(gsr.pmode), static_cast<unsigned long long>(gsr.dispfb1),
                            static_cast<unsigned long long>(gsr.display1));
                {
                    extern uint64_t ps2Vif1KickCount();
                    static uint64_t s_lastKicks = 0;
                    const uint64_t kicks = ps2Vif1KickCount();
                    std::printf("[frames] vif1kicks=%llu rate=%.1f/s\n", static_cast<unsigned long long>(kicks),
                                static_cast<double>(kicks - s_lastKicks) / 2.0);
                    s_lastKicks = kicks;
                    extern void ps2AsyncDmaStats(uint64_t &, uint64_t &, uint64_t &);
                    uint64_t sub = 0, done = 0, applied = 0;
                    ps2AsyncDmaStats(sub, done, applied);
                    std::printf("[async] submitted=%llu done=%llu applied=%llu\n", (unsigned long long)sub,
                                (unsigned long long)done, (unsigned long long)applied);
                }
                if (const char *peek = std::getenv("PS2X_STATUS_PEEK"))
                {
                    // PS2X_STATUS_PEEK=<hex addr>[,<hex addr>...]: print guest words each status tick.
                    std::printf("[peek]");
                    for (const char *p = peek; *p;)
                    {
                        char *end = nullptr;
                        const uint32_t addr = static_cast<uint32_t>(std::strtoul(p, &end, 16));
                        if (end == p)
                            break;
                        uint32_t v = 0;
                        if (addr + 4u <= PS2_RAM_SIZE)
                            std::memcpy(&v, m_memory.getRDRAM() + addr, 4);
                        std::printf(" %x=%08x", addr, v);
                        p = (*end == ',') ? end + 1 : end;
                    }
                    std::printf("\n");
                }
                if (const char *dir = std::getenv("PS2X_RAM_DUMP"))
                {
                    // PS2X_RAM_DUMP=<dir> with PS2X_RAM_DUMP_FROM=<sec>, PS2X_RAM_DUMP_COUNT=<n>: snapshot guest
                    // RAM on status ticks (offline physics/object inspection).
                    static const double s_from = std::getenv("PS2X_RAM_DUMP_FROM") ? std::atof(std::getenv("PS2X_RAM_DUMP_FROM")) : 0.0;
                    static int s_left = std::getenv("PS2X_RAM_DUMP_COUNT") ? std::atoi(std::getenv("PS2X_RAM_DUMP_COUNT")) : 8;
                    static int s_n = 0;
                    // PS2X_RAM_DUMP_BANK1=<sec>: count from the arena sound-bank load (match start) instead.
                    static const double s_bank1 = std::getenv("PS2X_RAM_DUMP_BANK1") ? std::atof(std::getenv("PS2X_RAM_DUMP_BANK1")) : -1.0;
                    extern std::atomic<double> g_ps2xBank1LoadedAt;
                    const double bank1At = g_ps2xBank1LoadedAt.load();
                    // PS2X_RAM_DUMP_ANCHOR=<sec>: count from the first call of PS2X_TRACE_ANCHOR_PC instead.
                    static const double s_anch = std::getenv("PS2X_RAM_DUMP_ANCHOR") ? std::atof(std::getenv("PS2X_RAM_DUMP_ANCHOR")) : -1.0;
                    const double anchAt = g_ps2xTraceAnchorAt.load();
                    const bool due = s_anch >= 0.0    ? (anchAt >= 0.0 && ps2host::timeSeconds() - anchAt >= s_anch)
                                     : s_bank1 >= 0.0 ? (bank1At >= 0.0 && ps2host::timeSeconds() - bank1At >= s_bank1)
                                                      : ps2host::timeSeconds() >= s_from;
                    if (s_left > 0 && due)
                    {
                        --s_left;
                        char path[512];
                        std::snprintf(path, sizeof(path), "%s\\ram_%03d_t%.0f.bin", dir, s_n++, ps2host::timeSeconds());
                        if (FILE *fp = std::fopen(path, "wb"))
                        {
                            std::fwrite(m_memory.getRDRAM(), 1, PS2_RAM_SIZE, fp);
                            std::fclose(fp);
                        }
                    }
                }
                if (std::getenv("PS2X_SYMBOLS"))
                {
                    const auto snap = m_eeScheduler->snapshot();
                    for (const auto &t : snap.threads)
                    {
                        std::printf("  thread %d status=%d wait=%d prio=%d\n", t.id, static_cast<int>(t.status),
                                    static_cast<int>(t.waitReason), t.currentPriority);
                        printGuestBacktrace(m_memory.getRDRAM(), t.pc, t.ra, t.sp);
                    }
                }
                {
                    static uint64_t s_lastNs[ps2prof::kCount]{};
                    static uint64_t s_lastCalls[ps2prof::kCount]{};
                    std::printf("[prof]");
                    for (int c = 0; c < ps2prof::kCount; ++c)
                    {
                        const uint64_t ns = ps2prof::g_ns[c].load();
                        const uint64_t calls = ps2prof::g_calls[c].load();
                        std::printf(" %s=%.0fms/%llu", ps2prof::name(c), (ns - s_lastNs[c]) / 1.0e6,
                                    static_cast<unsigned long long>(calls - s_lastCalls[c]));
                        s_lastNs[c] = ns;
                        s_lastCalls[c] = calls;
                    }
                    std::printf("\n");
                }
                std::fflush(stdout);
            }
        }
        if (!ps2host::pumpMessages())
        {
            RUNTIME_LOG("[run] window close requested, breaking out of loop");
            requestStop();
            break;
        }
        UploadFrame(frameTex, this, presentWidth, presentHeight);
#else
        BeginDrawing();
        ClearBackground(BLACK);
        const float srcWidth = static_cast<float>(std::max<uint32_t>(1u, presentWidth));
        const float srcHeight = static_cast<float>(std::max<uint32_t>(1u, presentHeight));
        const float screenWidth = static_cast<float>(GetScreenWidth());
        const float screenHeight = static_cast<float>(GetScreenHeight());
        const float scale = std::min(screenWidth / srcWidth, screenHeight / srcHeight);
        const float dstWidth = srcWidth * scale;
        const float dstHeight = srcHeight * scale;
        const Rectangle srcRect{0.0f, 0.0f, srcWidth, srcHeight};
        const Rectangle dstRect{
            (screenWidth - dstWidth) * 0.5f,
            (screenHeight - dstHeight) * 0.5f,
            dstWidth,
            dstHeight};
        DrawTexturePro(frameTex, srcRect, dstRect, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
        if (m_debugUiInitialized && m_debugUiDrawCallback)
        {
            m_debugUiDrawCallback(*this, m_debugUiUserData);
        }
        EndDrawing();

        if (WindowShouldClose())
        {
            RUNTIME_LOG("[run] window close requested, breaking out of loop");
            requestStop();
            break;
        }
#endif
    }

    requestStop();
    if (gameThread.joinable())
    {
        gameThread.join();
    }

    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }
#if defined(PS2X_NATIVE_HOST)
    ps2host::shutdown();
#else
    UnloadTexture(frameTex);
    CloseWindow();
#endif

    RUNTIME_LOG("[run] exiting loop");
}
