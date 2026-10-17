#include "host/win32_host.h"
#include "host/gamepad_input.h"
#include "host/input_mapping.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#include <tlhelp32.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>
#include <xaudio2.h>
#include <Xinput.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <array>
#include <unordered_map>

extern std::atomic<double> g_hitchLastFlip; // ps2_runtime.cpp (PS2X_HITCH_LOG): host time of the last flip

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "xinput.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

uint64_t ps2Vif1KickCount(); // ps2_memory.cpp: game frames (VIF1 frame kicks) so far

// Crash report: an unhandled exception prints the faulting thread's stack as module+offset (x64 unwind
// data, no PDB needed) to stdout; tools/crashsym.py resolves the offsets with ps2EntryRunner.map.
static LONG WINAPI ps2xCrashFilter(EXCEPTION_POINTERS *ep)
{
    CONTEXT c = *ep->ContextRecord;
    std::printf("[crash] code=0x%08lx addr=%p thread=%lu\n", ep->ExceptionRecord->ExceptionCode,
                ep->ExceptionRecord->ExceptionAddress, GetCurrentThreadId());
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        std::printf("[crash] %s address 0x%llx\n", ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                    static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[1]));
    for (int i = 0; i < 48 && c.Rip; ++i)
    {
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(c.Rip), &mod))
            GetModuleFileNameA(mod, name, MAX_PATH);
        const char *base = std::strrchr(name, '\\');
        std::printf("[crash]   #%02d %s+0x%llx\n", i, base ? base + 1 : name,
                    static_cast<unsigned long long>(c.Rip - reinterpret_cast<DWORD64>(mod)));
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
        if (!fn)
        {
            c.Rip = *reinterpret_cast<DWORD64 *>(c.Rsp); // leaf function
            c.Rsp += 8;
            continue;
        }
        void *handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fn, &c, &handlerData, &establisher, nullptr);
    }
    std::fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}

namespace modmenu // mod_menu.cpp
{
    void filterInput(ps2host::PadState *pads, int count, bool hotkeyDown);
    bool panel(uint32_t outH, const uint32_t *&px, uint32_t &w, uint32_t &h, bool &changed);
}

namespace ps2host
{
    constexpr int kAutoAnchorHits = 32;
    // Anchored auto-input: the dispatcher stamps g_autoAnchorAt when the guest first calls g_autoAnchorPc.
    uint32_t g_autoAnchorPc = 0;
    int g_autoAnchorCount = 1;          // PS2X_AUTOINPUT_ANCHOR=<hex pc>[:<n>] = default call index for "@t"
    std::atomic<double> g_autoAnchorAt[kAutoAnchorHits]; // time of the 1st..32nd call of the anchor pc
    uint32_t g_autoWatchPc[8] = {};     // PS2X_AUTOINPUT_WATCH=<hex pc>,... : log call times (finding anchors)
    int g_autoWatchN = 0;
    namespace
    {
        HostConfig g_config;
        bool g_background = false;      // PS2X_BACKGROUND: hidden, never activated (automated tests)
        std::string g_shotDir;          // PS2X_SCREENSHOT_DIR: periodic back-buffer dumps
        double g_shotInterval = 5.0;    // PS2X_SCREENSHOT_SEC
        double g_nextShot = 0.0;
        int g_shotIndex = 0;
        struct AutoPress { double t, until; uint16_t button; int anchorHit; int pad; }; // anchorHit 0 = absolute time
        std::vector<AutoPress> g_autoPress; // PS2X_AUTOINPUT="12:START,@0.5:RIGHT,@3+1.0:CROSS" ('@[n+]t' = t s after anchor call n)
        HWND g_hwnd = nullptr;
        std::atomic<bool> g_quit{false};
        bool g_ready = false;

        ComPtr<ID3D11Device> g_device;
        ComPtr<ID3D11DeviceContext> g_ctx;
        ComPtr<IDXGISwapChain1> g_swap;
        HANDLE g_frameWaitable = nullptr;
        ComPtr<ID3D11RenderTargetView> g_backRtv;
        ComPtr<ID3D11VertexShader> g_blitVs;
        ComPtr<ID3D11PixelShader> g_blitPs;
        ComPtr<ID3D11SamplerState> g_linearSampler;
        ComPtr<ID3D11Buffer> g_blitCb;
        ComPtr<ID3D11Texture2D> g_uploadTex;
        ComPtr<ID3D11ShaderResourceView> g_uploadSrv;
        uint32_t g_uploadW = 0, g_uploadH = 0;
        std::recursive_mutex g_d3dMutex;
        std::atomic<uint64_t> g_d3dEpoch{1}; // bumped whenever the host changes immediate-context state

        ComPtr<IXAudio2> g_xaudio;
        IXAudio2MasteringVoice *g_master = nullptr;
        std::recursive_mutex g_audioMutex;
        std::vector<std::function<void()>> g_audioResetListeners;
        std::atomic<bool> g_audioLost{false}; // XAudio2 critical error (device removed); rebuilt in pumpMessages
        double g_nextAudioRetry = 0.0;          // no device: look for one again at this time

        // XAudio2 reports a lost device (headset unplugged, driver reset) as a critical error on its own thread;
        // the engine is unusable from then on and has to be recreated.
        struct AudioEngineCallback final : IXAudio2EngineCallback
        {
            void STDMETHODCALLTYPE OnProcessingPassStart() override {}
            void STDMETHODCALLTYPE OnProcessingPassEnd() override {}
            void STDMETHODCALLTYPE OnCriticalError(HRESULT) override { g_audioLost.store(true); }
        } g_audioEngineCallback;

        std::mutex g_inputMutex;
        // [Controls] keyboard bindings: keys per PS2 button, indexed by the button's bit (input::kButtonNames).
        std::array<std::vector<int>, 16> g_keyBindings;
        bool g_keyBindingsLoaded = false;

        void loadKeyBindings(const char *iniPath)
        {
            for (size_t bit = 0; bit < g_keyBindings.size(); ++bit)
            {
                const char *button = input::kButtonNames[bit].name;
                const char *fallback = "";
                for (const auto &[name, keys] : input::kDefaultKeys)
                    if (std::string_view(name) == button)
                        fallback = keys;
                char value[128] = {};
                if (iniPath)
                    GetPrivateProfileStringA("Controls", button, fallback, value, sizeof(value), iniPath);
                else
                    std::snprintf(value, sizeof(value), "%s", fallback);
                g_keyBindings[bit] = input::parseKeyList(value);
            }
            g_keyBindingsLoaded = true;
        }
        PadState g_pads[kMaxPads];
        bool g_keys[256]{};

        LARGE_INTEGER g_qpcFreq{};
        LARGE_INTEGER g_qpcStart{};
        double g_nextFrameTime = 0.0;
        double g_lastFrameMs = 0.0;
        double g_lastPresentTime = 0.0;

        // FPS counter: on-screen box (F1 toggles, ShowFPS in [Display]) and the window title.
        bool g_showFps = true;
        std::filesystem::path g_configPath; // settings file in use (loadConfig)
        // Swap chain created with ALLOW_TEARING: with VSync off, presents go out at once (variable refresh /
        // G-Sync / FreeSync displays follow the game's frame rate instead of tearing at a fixed one).
        bool g_tearingSupported = false;
        double g_displayHz = 60.0;

        UINT swapChainFlags()
        {
            return DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                   (g_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
        }
        std::wstring g_baseTitle;
        std::atomic<int> g_titleFps{-1}, g_titleGameFps{-1};
        int g_shownTitleFps = -2, g_shownTitleGameFps = -2;
        double g_fpsWindowStart = 0.0;
        uint32_t g_fpsPresents = 0;
        uint64_t g_fpsKickStart = 0;
        double g_presentFps = 0.0, g_gameFps = 0.0;
        ComPtr<ID3D11Texture2D> g_fpsTex;
        ComPtr<ID3D11ShaderResourceView> g_fpsSrv;
        ComPtr<ID3D11SamplerState> g_pointSampler;

        const char *kBlitShader = R"(
cbuffer Blit : register(b0) { float4 uvRect; };
Texture2D tex : register(t0);
SamplerState samp : register(s0);
struct VsOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VsOut vs_main(uint id : SV_VertexID)
{
    VsOut o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uvRect.xy + p * uvRect.zw;
    return o;
}
float4 ps_main(VsOut i) : SV_Target
{
    float4 c = tex.Sample(samp, i.uv);
    return float4(c.rgb, 1.0);
}
)";

        std::wstring widen(const char *s)
        {
            if (!s)
                return L"";
            int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
            std::wstring w(static_cast<size_t>(std::max(0, n - 1)), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
            return w;
        }

        constexpr UINT kMsgSetFullscreen = WM_APP + 1; // wParam: fullscreen on/off (setSetting)

        void applyFullscreen(HWND hwnd, bool fullscreen)
        {
            g_config.fullscreen = fullscreen;
            LONG style = fullscreen ? (WS_POPUP | WS_VISIBLE) : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
            SetWindowLongPtrW(hwnd, GWL_STYLE, style);
            if (fullscreen)
            {
                MONITORINFO mi{sizeof(mi)};
                GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
                SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                             mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                             SWP_FRAMECHANGED);
            }
            else
            {
                RECT r{0, 0, (LONG)g_config.width, (LONG)g_config.height};
                AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
                SetWindowPos(hwnd, HWND_TOP, 64, 64, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED);
            }
        }

        LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            switch (msg)
            {
            case WM_CLOSE:
                g_quit.store(true);
                return 0;
            case WM_DESTROY:
                g_quit.store(true);
                PostQuitMessage(0);
                return 0;
            case kMsgSetFullscreen:
                applyFullscreen(hwnd, wp != 0);
                return 0;
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
                if (wp < 256)
                {
                    std::lock_guard lock(g_inputMutex);
                    g_keys[wp] = true;
                }
                if (wp == VK_F1 && !(lp & (1 << 30)))
                    g_showFps = !g_showFps;
                if (wp == VK_RETURN && (lp & (1 << 29)))
                {
                    // Alt+Enter toggles borderless fullscreen.
                    applyFullscreen(hwnd, !g_config.fullscreen);
                    return 0;
                }
                break;
            case WM_KEYUP:
            case WM_SYSKEYUP:
                if (wp < 256)
                {
                    std::lock_guard lock(g_inputMutex);
                    g_keys[wp] = false;
                }
                break;
            case WM_KILLFOCUS:
            {
                std::lock_guard lock(g_inputMutex);
                std::memset(g_keys, 0, sizeof(g_keys));
                break;
            }
            default:
                break;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }

        bool compileShader(const char *src, const char *entry, const char *target, ComPtr<ID3DBlob> &out)
        {
            ComPtr<ID3DBlob> err;
            HRESULT hr = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, entry, target,
                                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &err);
            if (FAILED(hr))
            {
                std::fprintf(stderr, "[host] shader compile failed (%s): %s\n", entry,
                             err ? static_cast<const char *>(err->GetBufferPointer()) : "?");
                return false;
            }
            return true;
        }

        void createBackBufferView()
        {
            g_backRtv.Reset();
            ComPtr<ID3D11Texture2D> back;
            g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
            g_device->CreateRenderTargetView(back.Get(), nullptr, &g_backRtv);
        }

        void resizeIfNeeded()
        {
            RECT rc{};
            GetClientRect(g_hwnd, &rc);
            UINT w = std::max<LONG>(1, rc.right - rc.left);
            UINT h = std::max<LONG>(1, rc.bottom - rc.top);
            DXGI_SWAP_CHAIN_DESC1 desc{};
            g_swap->GetDesc1(&desc);
            if (desc.Width == w && desc.Height == h)
                return;
            g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
            g_backRtv.Reset();
            g_swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, swapChainFlags());
            createBackBufferView();
        }

        double nowSeconds()
        {
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            return static_cast<double>(t.QuadPart - g_qpcStart.QuadPart) / static_cast<double>(g_qpcFreq.QuadPart);
        }

        void frameLimit()
        {
            if (g_config.targetFps == 0)
                return;
            const double period = 1.0 / static_cast<double>(g_config.targetFps);
            double now = nowSeconds();
            if (g_nextFrameTime == 0.0 || now - g_nextFrameTime > 0.25)
                g_nextFrameTime = now;
            g_nextFrameTime += period;
            while (true)
            {
                now = nowSeconds();
                double remain = g_nextFrameTime - now;
                if (remain <= 0.0)
                    break;
                if (remain > 0.002)
                    Sleep(static_cast<DWORD>((remain - 0.0015) * 1000.0));
                else
                    std::this_thread::yield();
            }
            g_lastFrameMs = (now - g_lastPresentTime) * 1000.0;
            g_lastPresentTime = now;
        }

        double nowSeconds();

        void saveScreenshot()
        {
            ComPtr<ID3D11Texture2D> back;
            if (FAILED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back))))
                return;
            D3D11_TEXTURE2D_DESC td{};
            back->GetDesc(&td);
            td.Usage = D3D11_USAGE_STAGING;
            td.BindFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            td.MiscFlags = 0;
            ComPtr<ID3D11Texture2D> staging;
            if (FAILED(g_device->CreateTexture2D(&td, nullptr, &staging)))
                return;
            g_ctx->CopyResource(staging.Get(), back.Get());
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(g_ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)))
                return;
            // Copy the pixels out and write the file on a worker: a 6 MB write to a busy disk under the D3D
            // lock stalled the GS thread for seconds (test runs only).
            const uint32_t w = td.Width, h = td.Height;
            std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4u);
            for (uint32_t y = 0; y < h; ++y)
                std::memcpy(pixels.data() + static_cast<size_t>(y) * w * 4u,
                            static_cast<const uint8_t *>(m.pData) + static_cast<size_t>(y) * m.RowPitch, w * 4u);
            g_ctx->Unmap(staging.Get(), 0);
            char path[512];
            std::snprintf(path, sizeof(path), "%s/shot_%04d.bmp", g_shotDir.c_str(), g_shotIndex++);
            std::thread([pixels = std::move(pixels), w, h, file = std::string(path)]()
            {
                FILE *f = std::fopen(file.c_str(), "wb");
                if (!f)
                    return;
                const uint32_t row = w * 3u, pad = (4u - row % 4u) % 4u;
                const uint32_t dataSize = (row + pad) * h;
                BITMAPFILEHEADER fh{};
                fh.bfType = 0x4D42;
                fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
                fh.bfSize = fh.bfOffBits + dataSize;
                BITMAPINFOHEADER ih{};
                ih.biSize = sizeof(ih);
                ih.biWidth = static_cast<LONG>(w);
                ih.biHeight = static_cast<LONG>(h);
                ih.biPlanes = 1;
                ih.biBitCount = 24;
                std::fwrite(&fh, sizeof(fh), 1, f);
                std::fwrite(&ih, sizeof(ih), 1, f);
                std::vector<uint8_t> line(row + pad, 0);
                for (int y = static_cast<int>(h) - 1; y >= 0; --y)
                {
                    const uint8_t *src = pixels.data() + static_cast<size_t>(y) * w * 4u;
                    for (uint32_t x = 0; x < w; ++x)
                    {
                        line[x * 3 + 0] = src[x * 4 + 2];
                        line[x * 3 + 1] = src[x * 4 + 1];
                        line[x * 3 + 2] = src[x * 4 + 0];
                    }
                    std::fwrite(line.data(), 1, line.size(), f);
                }
                std::fclose(f);
            }).detach();
        }

        // 5x7 glyphs (one byte per row, low 5 bits, bit 4 = left column) for the FPS box.
        const uint8_t *glyph(char c)
        {
            static const uint8_t digits[10][7] = {
                {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},  {14, 17, 1, 2, 4, 8, 31},
                {31, 2, 4, 2, 1, 17, 14},     {2, 6, 10, 18, 31, 2, 2}, {31, 16, 30, 1, 1, 17, 14},
                {6, 8, 16, 30, 17, 17, 14},   {31, 1, 2, 4, 8, 8, 8},   {14, 17, 17, 14, 17, 17, 14},
                {14, 17, 17, 15, 1, 2, 12}};
            static const uint8_t F[7] = {31, 16, 16, 30, 16, 16, 16}, P[7] = {30, 17, 17, 30, 16, 16, 16},
                                 S[7] = {15, 16, 16, 14, 1, 1, 30}, G[7] = {14, 17, 16, 23, 17, 17, 15},
                                 A[7] = {14, 17, 17, 31, 17, 17, 17}, M[7] = {17, 27, 21, 21, 17, 17, 17},
                                 E[7] = {31, 16, 16, 30, 16, 16, 31}, blank[7] = {0, 0, 0, 0, 0, 0, 0};
            if (c >= '0' && c <= '9')
                return digits[c - '0'];
            switch (c)
            {
            case 'F': return F;
            case 'P': return P;
            case 'S': return S;
            case 'G': return G;
            case 'A': return A;
            case 'M': return M;
            case 'E': return E;
            default: return blank;
            }
        }

        // Counts presents and game frames (VIF1 frame kicks) and draws the FPS box over the back buffer.
        void updateAndDrawFps(float bh)
        {
            const double now = nowSeconds();
            ++g_fpsPresents;
            if (g_fpsWindowStart == 0.0)
            {
                g_fpsWindowStart = now;
                g_fpsKickStart = ps2Vif1KickCount();
            }
            else if (now - g_fpsWindowStart >= 0.5)
            {
                const double dt = now - g_fpsWindowStart;
                const uint64_t kicks = ps2Vif1KickCount();
                g_presentFps = g_fpsPresents / dt;
                g_gameFps = static_cast<double>(kicks - g_fpsKickStart) / dt;
                g_fpsPresents = 0;
                g_fpsWindowStart = now;
                g_fpsKickStart = kicks;
                g_titleFps.store(static_cast<int>(g_presentFps + 0.5));
                g_titleGameFps.store(static_cast<int>(g_gameFps + 0.5));
            }
            if (!g_showFps)
                return;

            char lines[2][16];
            std::snprintf(lines[0], sizeof(lines[0]), "FPS  %3d", static_cast<int>(g_presentFps + 0.5));
            std::snprintf(lines[1], sizeof(lines[1]), "GAME %3d", static_cast<int>(g_gameFps + 0.5));
            constexpr uint32_t kCols = 8, kW = kCols * 6 + 3, kH = 2 * 9 + 3;
            uint32_t px[kW * kH];
            for (uint32_t &p : px)
                p = 0xFF000000u; // opaque black box
            for (int l = 0; l < 2; ++l)
            {
                // RGBA in memory (little endian ABGR): green normally, orange when the game drops below 55.
                const uint32_t color = (l == 1 && g_gameFps < 55.0) ? 0xFF3090FFu : 0xFF60FF60u;
                for (uint32_t ci = 0; ci < kCols && lines[l][ci]; ++ci)
                {
                    const uint8_t *g = glyph(lines[l][ci]);
                    for (uint32_t y = 0; y < 7; ++y)
                        for (uint32_t x = 0; x < 5; ++x)
                            if (g[y] & (16u >> x))
                                px[(2 + l * 9 + y) * kW + 2 + ci * 6 + x] = color;
                }
            }
            if (!g_fpsTex)
            {
                D3D11_TEXTURE2D_DESC td{};
                td.Width = kW;
                td.Height = kH;
                td.MipLevels = 1;
                td.ArraySize = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                if (FAILED(g_device->CreateTexture2D(&td, nullptr, &g_fpsTex)))
                    return;
                g_device->CreateShaderResourceView(g_fpsTex.Get(), nullptr, &g_fpsSrv);
                D3D11_SAMPLER_DESC sd{};
                sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
                sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
                sd.MaxLOD = D3D11_FLOAT32_MAX;
                g_device->CreateSamplerState(&sd, &g_pointSampler);
            }
            g_ctx->UpdateSubresource(g_fpsTex.Get(), 0, nullptr, px, kW * 4u, 0);
            const float s = std::max(2.0f, std::floor(bh / 360.0f)); // integer scale: 3x at 1080p
            D3D11_VIEWPORT vp{8.0f, 8.0f, kW * s, kH * s, 0.0f, 1.0f};
            g_ctx->RSSetViewports(1, &vp);
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(g_ctx->Map(g_blitCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
            {
                const float rect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
                std::memcpy(m.pData, rect, sizeof(rect));
                g_ctx->Unmap(g_blitCb.Get(), 0);
            }
            ID3D11ShaderResourceView *srv = g_fpsSrv.Get();
            g_ctx->PSSetShaderResources(0, 1, &srv);
            ID3D11SamplerState *smp = g_pointSampler.Get();
            g_ctx->PSSetSamplers(0, 1, &smp);
            g_ctx->Draw(3, 0);
            ID3D11ShaderResourceView *nullSrv = nullptr;
            g_ctx->PSSetShaderResources(0, 1, &nullSrv);
        }

        // Widescreen=1: the GS backend shows frames without a 3D scene (movies, loading screens) 4:3 with bars.
        std::atomic<bool> g_presentWide{true};
        int g_internalScale = 0; // [Display] InternalScale (0 = from the output size)

        // The mod menu panel (mod_menu.cpp renders it): centred over the frame, premultiplied alpha.
        ComPtr<ID3D11Texture2D> g_modTex;
        ComPtr<ID3D11ShaderResourceView> g_modSrv;
        ComPtr<ID3D11PixelShader> g_alphaPs;
        ComPtr<ID3D11BlendState> g_premulBlend;
        uint32_t g_modW = 0, g_modH = 0;

        void drawModMenu(float bw, float bh)
        {
            const uint32_t *px = nullptr;
            uint32_t w = 0, h = 0;
            bool changed = false;
            if (!modmenu::panel(static_cast<uint32_t>(bh), px, w, h, changed))
                return;
            if (!g_alphaPs)
            {
                static const char *kAlphaPs = R"(
Texture2D tex : register(t0);
SamplerState samp : register(s0);
float4 ps_main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return tex.Sample(samp, uv); }
)";
                ComPtr<ID3DBlob> b;
                if (!compileShader(kAlphaPs, "ps_main", "ps_5_0", b))
                    return;
                g_device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_alphaPs);
                D3D11_BLEND_DESC bd{};
                bd.RenderTarget[0].BlendEnable = TRUE;
                bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
                bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
                bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
                bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
                bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
                bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
                bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
                g_device->CreateBlendState(&bd, &g_premulBlend);
            }
            if (!g_modTex || w != g_modW || h != g_modH)
            {
                g_modTex.Reset();
                g_modSrv.Reset();
                D3D11_TEXTURE2D_DESC td{};
                td.Width = w;
                td.Height = h;
                td.MipLevels = 1;
                td.ArraySize = 1;
                td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                if (FAILED(g_device->CreateTexture2D(&td, nullptr, &g_modTex)))
                    return;
                g_device->CreateShaderResourceView(g_modTex.Get(), nullptr, &g_modSrv);
                g_modW = w;
                g_modH = h;
                changed = true;
            }
            if (changed)
                g_ctx->UpdateSubresource(g_modTex.Get(), 0, nullptr, px, w * 4u, 0);
            D3D11_VIEWPORT vp{std::floor((bw - static_cast<float>(w)) * 0.5f), std::floor((bh - static_cast<float>(h)) * 0.5f),
                              static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
            g_ctx->RSSetViewports(1, &vp);
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(g_ctx->Map(g_blitCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
            {
                const float rect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
                std::memcpy(m.pData, rect, sizeof(rect));
                g_ctx->Unmap(g_blitCb.Get(), 0);
            }
            g_ctx->OMSetBlendState(g_premulBlend.Get(), nullptr, 0xFFFFFFFFu);
            g_ctx->PSSetShader(g_alphaPs.Get(), nullptr, 0);
            ID3D11ShaderResourceView *srv = g_modSrv.Get();
            g_ctx->PSSetShaderResources(0, 1, &srv);
            ID3D11SamplerState *smp = g_pointSampler ? g_pointSampler.Get() : g_linearSampler.Get();
            g_ctx->PSSetSamplers(0, 1, &smp);
            g_ctx->Draw(3, 0);
            ID3D11ShaderResourceView *nullSrv = nullptr;
            g_ctx->PSSetShaderResources(0, 1, &nullSrv);
            g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
            g_ctx->PSSetShader(g_blitPs.Get(), nullptr, 0);
        }

        void drawBlit(ID3D11ShaderResourceView *srv, uint32_t srcW, uint32_t srcH, float u1, float v1)
        {
            g_d3dEpoch.fetch_add(1, std::memory_order_relaxed); // GS backend state cache must rebind
            resizeIfNeeded();
            DXGI_SWAP_CHAIN_DESC1 desc{};
            g_swap->GetDesc1(&desc);
            const float bw = static_cast<float>(desc.Width);
            const float bh = static_cast<float>(desc.Height);

            float vw = bw, vh = bh;
            if (g_config.keepAspect)
            {
                const float aspect = (g_config.widescreen && g_presentWide.load(std::memory_order_relaxed)) ? (16.0f / 9.0f) : (4.0f / 3.0f);
                if (bw / bh > aspect)
                    vw = bh * aspect;
                else
                    vh = bw / aspect;
            }
            (void)srcW;
            (void)srcH;

            const float clear[4] = {0, 0, 0, 1};
            g_ctx->ClearRenderTargetView(g_backRtv.Get(), clear);
            D3D11_VIEWPORT vp{(bw - vw) * 0.5f, (bh - vh) * 0.5f, vw, vh, 0.0f, 1.0f};
            g_ctx->RSSetViewports(1, &vp);
            ID3D11RenderTargetView *rtv = g_backRtv.Get();
            g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
            g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
            g_ctx->OMSetDepthStencilState(nullptr, 0);
            g_ctx->RSSetState(nullptr);

            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(g_ctx->Map(g_blitCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
            {
                float rect[4] = {0.0f, 0.0f, u1, v1};
                std::memcpy(m.pData, rect, sizeof(rect));
                g_ctx->Unmap(g_blitCb.Get(), 0);
            }
            g_ctx->IASetInputLayout(nullptr);
            g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g_ctx->VSSetShader(g_blitVs.Get(), nullptr, 0);
            g_ctx->PSSetShader(g_blitPs.Get(), nullptr, 0);
            ID3D11Buffer *cb = g_blitCb.Get();
            g_ctx->VSSetConstantBuffers(0, 1, &cb);
            g_ctx->PSSetShaderResources(0, 1, &srv);
            ID3D11SamplerState *s = g_linearSampler.Get();
            g_ctx->PSSetSamplers(0, 1, &s);
            g_ctx->GSSetShader(nullptr, nullptr, 0);
            g_ctx->Draw(3, 0);
            ID3D11ShaderResourceView *nullSrv = nullptr;
            g_ctx->PSSetShaderResources(0, 1, &nullSrv);
            updateAndDrawFps(bh);
            drawModMenu(bw, bh);

            if (!g_shotDir.empty() && nowSeconds() >= g_nextShot)
            {
                g_nextShot = nowSeconds() + g_shotInterval;
                saveScreenshot();
            }
            if (g_config.vsync)
                g_swap->Present(1, 0);
            else
                g_swap->Present(0, g_tearingSupported ? DXGI_PRESENT_ALLOW_TEARING : 0); // borderless / windowed only
        }

        // Called outside the D3D lock: waits for the swap chain (vsync) and applies the FPS cap.
        void waitForNextFrame()
        {
            if (g_frameWaitable && g_config.vsync)
                WaitForSingleObjectEx(g_frameWaitable, 100, TRUE);
            frameLimit();
        }

    }

    const HostConfig &config() { return g_config; }

    std::filesystem::path saveDirectory()
    {
        if (const wchar_t *dir = _wgetenv(L"PS2X_SAVE_DIR"); dir && *dir)
            return dir;
        if (!g_config.saveFolder.empty())
            return g_config.saveFolder;
        PWSTR savedGames = nullptr;
        std::filesystem::path result;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames, KF_FLAG_CREATE, nullptr, &savedGames)))
            result = std::filesystem::path(savedGames) / L"WWE Crush Hour";
        CoTaskMemFree(savedGames);
        return result;
    }

    std::filesystem::path configPath() { return g_configPath; }

    namespace
    {
        void writeSetting(const wchar_t *section, const wchar_t *key, int value)
        {
            if (g_configPath.empty())
                return;
            WritePrivateProfileStringW(section, key, std::to_wstring(value).c_str(), g_configPath.c_str());
        }
    }

    int getSetting(Setting setting)
    {
        switch (setting)
        {
        case Setting::ShowFps: return g_showFps ? 1 : 0;
        case Setting::VSync: return g_config.vsync ? 1 : 0;
        case Setting::Fullscreen: return g_config.fullscreen ? 1 : 0;
        case Setting::TargetFps: return static_cast<int>(g_config.targetFps);
        case Setting::Widescreen: return g_config.widescreen ? 1 : 0;
        case Setting::MasterVolume: return static_cast<int>(g_config.masterVolume);
        case Setting::EffectsVolume: return static_cast<int>(g_config.effectsVolume);
        case Setting::CommentaryVolume: return static_cast<int>(g_config.commentaryVolume);
        case Setting::MovieVolume: return static_cast<int>(g_config.movieVolume);
        }
        return 0;
    }

    void setSetting(Setting setting, int value)
    {
        const uint32_t volume = static_cast<uint32_t>(std::clamp(value, 0, 100));
        switch (setting)
        {
        case Setting::ShowFps:
            g_showFps = value != 0;
            writeSetting(L"Display", L"ShowFPS", value != 0);
            break;
        case Setting::VSync:
            g_config.vsync = value != 0;
            writeSetting(L"Display", L"VSync", value != 0);
            break;
        case Setting::Fullscreen:
            // From the input thread: the window belongs to the message thread, so post the change there.
            if (g_hwnd && (value != 0) != g_config.fullscreen)
                PostMessageW(g_hwnd, kMsgSetFullscreen, value != 0, 0);
            writeSetting(L"Display", L"Fullscreen", value != 0);
            break;
        case Setting::TargetFps:
            g_config.targetFps = static_cast<uint32_t>(std::clamp(value, 0, 360));
            writeSetting(L"Display", L"TargetFPS", static_cast<int>(g_config.targetFps));
            break;
        case Setting::Widescreen:
            g_config.widescreen = value != 0;
            writeSetting(L"Display", L"Widescreen", value != 0);
            break;
        case Setting::MasterVolume:
        {
            g_config.masterVolume = volume;
            std::lock_guard lock(g_audioMutex);
            const char *mute = std::getenv("PS2X_MUTE");
            if (g_master && !(mute && *mute && *mute != '0'))
                g_master->SetVolume(static_cast<float>(volume) / 100.0f);
            writeSetting(L"Audio", L"MasterVolume", static_cast<int>(volume));
            break;
        }
        case Setting::EffectsVolume:
            g_config.effectsVolume = volume;
            writeSetting(L"Audio", L"EffectsVolume", static_cast<int>(volume));
            break;
        case Setting::CommentaryVolume:
            g_config.commentaryVolume = volume;
            writeSetting(L"Audio", L"CommentaryVolume", static_cast<int>(volume));
            break;
        case Setting::MovieVolume:
            g_config.movieVolume = volume;
            writeSetting(L"Audio", L"MovieVolume", static_cast<int>(volume));
            break;
        }
    }

    bool loadConfig(const char *iniName)
    {
        if (!iniName)
            return false;
        // The working directory (the old launcher cd'd into GameFiles), then next to the executable.
        std::vector<std::filesystem::path> candidates;
        std::error_code ec;
        const std::filesystem::path name(iniName);
        candidates.push_back(std::filesystem::absolute(name, ec));
        if (name.is_relative())
        {
            wchar_t exe[MAX_PATH];
            const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
            if (n > 0 && n < MAX_PATH)
            {
                const std::filesystem::path exeDir = std::filesystem::path(exe).parent_path();
                candidates.push_back(exeDir / "GameFiles" / name);
                candidates.push_back(exeDir / name);
            }
        }
        g_configPath.clear();
        for (const auto &candidate : candidates)
            if (std::filesystem::is_regular_file(candidate, ec))
            {
                g_configPath = candidate;
                break;
            }
        if (g_configPath.empty())
        {
            // None yet: the settings page writes one where the game files are (or the working directory).
            g_configPath = candidates.size() > 1 && std::filesystem::is_directory(candidates[1].parent_path(), ec)
                               ? candidates[1]
                               : candidates[0];
            return false;
        }
        const std::string pathA = g_configPath.string(); // GetPrivateProfile*A: needs an absolute path
        const char *iniPath = pathA.c_str();
        g_config.width = GetPrivateProfileIntA("Display", "Width", g_config.width, iniPath);
        g_config.height = GetPrivateProfileIntA("Display", "Height", g_config.height, iniPath);
        g_config.fullscreen = GetPrivateProfileIntA("Display", "Fullscreen", g_config.fullscreen, iniPath) != 0;
        g_config.vsync = GetPrivateProfileIntA("Display", "VSync", g_config.vsync, iniPath) != 0;
        g_config.targetFps = GetPrivateProfileIntA("Display", "TargetFPS", g_config.targetFps, iniPath);
        g_config.keepAspect = GetPrivateProfileIntA("Display", "KeepAspect", g_config.keepAspect, iniPath) != 0;
        g_config.widescreen = GetPrivateProfileIntA("Display", "Widescreen", g_config.widescreen, iniPath) != 0;
        g_showFps = GetPrivateProfileIntA("Display", "ShowFPS", g_showFps ? 1 : 0, iniPath) != 0;
        g_config.wallClockTimers = GetPrivateProfileIntA("Timing", "WallClockTimers", g_config.wallClockTimers, iniPath) != 0;
        g_config.deadzonePercent = std::min(90u, static_cast<uint32_t>(GetPrivateProfileIntA("Controls", "Deadzone", g_config.deadzonePercent, iniPath)));
        loadKeyBindings(iniPath);
        g_config.masterVolume = GetPrivateProfileIntA("Audio", "MasterVolume", g_config.masterVolume, iniPath);
        g_config.effectsVolume = GetPrivateProfileIntA("Audio", "EffectsVolume", g_config.effectsVolume, iniPath);
        g_config.commentaryVolume = GetPrivateProfileIntA("Audio", "CommentaryVolume", g_config.commentaryVolume, iniPath);
        g_config.movieVolume = GetPrivateProfileIntA("Audio", "MovieVolume", g_config.movieVolume, iniPath);
        {
            wchar_t widePath[MAX_PATH], folder[MAX_PATH];
            if (MultiByteToWideChar(CP_ACP, 0, iniPath, -1, widePath, MAX_PATH) != 0 &&
                GetPrivateProfileStringW(L"Saves", L"Folder", L"", folder, MAX_PATH, widePath) > 0)
                g_config.saveFolder = folder;
        }
        // InternalScale=N: the GS is rendered at N x the PS2 resolution (640x448 -> 1920x1344 at 3), 1..6;
        // 0 = from the output size (init). The GS backend reads PS2X_GS_SCALE when it starts (an explicit
        // PS2X_GS_SCALE wins).
        g_internalScale = static_cast<int>(GetPrivateProfileIntA("Display", "InternalScale", 0, iniPath));
        if (g_internalScale >= 1 && g_internalScale <= 6 && !std::getenv("PS2X_GS_SCALE"))
            _putenv_s("PS2X_GS_SCALE", std::to_string(g_internalScale).c_str());
        if (const char *ws = std::getenv("PS2X_WIDESCREEN")) // test override
            g_config.widescreen = *ws != '0';
        return true;
    }

    namespace
    {
        // Caller holds g_audioMutex (or runs before any voice exists).
        bool createAudioEngine()
        {
            if (FAILED(XAudio2Create(&g_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)))
            {
                g_xaudio.Reset();
                return false;
            }
            if (FAILED(g_xaudio->CreateMasteringVoice(&g_master, 2, 48000)))
            {
                g_master = nullptr;
                g_xaudio.Reset();
                return false;
            }
            g_xaudio->RegisterForCallbacks(&g_audioEngineCallback);
            float master = static_cast<float>(std::min(g_config.masterVolume, 100u)) / 100.0f;
            if (const char *mute = std::getenv("PS2X_MUTE"); mute && *mute && *mute != '0')
                master = 0.0f; // silent automated test runs
            g_master->SetVolume(master);
            return true;
        }

        void destroyAudioEngine()
        {
            std::lock_guard lock(g_audioMutex);
            for (const auto &onReset : g_audioResetListeners)
                onReset();
            if (g_master)
            {
                g_master->DestroyVoice();
                g_master = nullptr;
            }
            if (g_xaudio)
            {
                g_xaudio->UnregisterForCallbacks(&g_audioEngineCallback);
                g_xaudio.Reset();
            }
        }

        // Runs on the message thread: rebuild a lost engine at once, retry a missing one every few seconds.
        void serviceAudio()
        {
            const bool lost = g_audioLost.exchange(false);
            if (!lost && (g_master || nowSeconds() < g_nextAudioRetry))
                return;
            std::lock_guard lock(g_audioMutex);
            if (lost)
                std::fprintf(stdout, "[host] audio device lost, reopening\n");
            destroyAudioEngine();
            if (createAudioEngine())
                std::fprintf(stdout, "[host] XAudio2 ready\n");
            else
                g_nextAudioRetry = nowSeconds() + 3.0;
        }
    }

    bool init(const char *title)
    {
        if (g_ready)
            return true;
        SetUnhandledExceptionFilter(ps2xCrashFilter);

        QueryPerformanceFrequency(&g_qpcFreq);
        QueryPerformanceCounter(&g_qpcStart);
        timeBeginPeriod(1);
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = wndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        wc.lpszClassName = L"PS2XNativeWindow";
        RegisterClassExW(&wc);

        DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
        int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
        int w = static_cast<int>(g_config.width), h = static_cast<int>(g_config.height);
        if (g_config.fullscreen)
        {
            style = WS_POPUP | WS_VISIBLE;
            x = 0;
            y = 0;
            w = GetSystemMetrics(SM_CXSCREEN);
            h = GetSystemMetrics(SM_CYSCREEN);
        }
        if (g_internalScale == 0 && !std::getenv("PS2X_GS_SCALE"))
        {
            // InternalScale=0: enough GS pixels for the picture on screen. The 640x448 frame fills the screen's
            // height and 4:3 (or, with Widescreen, 16:9) of its width: 1080p -> 3, 1440p -> 4, 2160p (4K) -> 6.
            const double outW = static_cast<double>(w), outH = static_cast<double>(h);
            const double picW = std::min(outW, outH * (g_config.widescreen ? 16.0 / 9.0 : 4.0 / 3.0));
            const int scale = std::clamp(static_cast<int>(std::ceil(std::max(picW / 640.0, outH / 448.0) - 0.05)), 1, 6);
            _putenv_s("PS2X_GS_SCALE", std::to_string(scale).c_str());
            std::printf("[host] output %dx%d: internal scale %d%c", w, h, scale, 10);
        }
        if (!g_config.fullscreen)
        {
            RECT r{0, 0, w, h};
            AdjustWindowRect(&r, style, FALSE);
            w = r.right - r.left;
            h = r.bottom - r.top;
        }
        if (const char *bg = std::getenv("PS2X_BACKGROUND"); bg && *bg && *bg != '0')
        {
            g_background = true;
            style &= ~WS_VISIBLE;
            g_config.fullscreen = false;
        }
        if (const char *d = std::getenv("PS2X_SCREENSHOT_DIR"))
        {
            g_shotDir = d;
            CreateDirectoryA(d, nullptr);
            if (const char *iv = std::getenv("PS2X_SCREENSHOT_SEC"))
                g_shotInterval = std::max(0.5, std::atof(iv));
            g_nextShot = g_shotInterval;
        }
        for (auto &a : g_autoAnchorAt)
            a.store(-1.0);
        if (const char *an = std::getenv("PS2X_AUTOINPUT_ANCHOR"))
        {
            char *endp = nullptr;
            g_autoAnchorPc = static_cast<uint32_t>(std::strtoul(an, &endp, 16));
            if (endp && *endp == ':')
                g_autoAnchorCount = std::max(1, std::atoi(endp + 1));
        }
        if (const char *aw = std::getenv("PS2X_AUTOINPUT_WATCH"))
        {
            const char *q = aw;
            while (*q && g_autoWatchN < 8)
            {
                char *endp = nullptr;
                const unsigned long v = std::strtoul(q, &endp, 16);
                if (endp == q)
                    break;
                g_autoWatchPc[g_autoWatchN++] = static_cast<uint32_t>(v);
                q = (*endp == ',') ? endp + 1 : endp;
            }
        }
        if (const char *ai = std::getenv("PS2X_AUTOINPUT"))
        {
            // "seconds:BUTTON" (0.25 s press) or "from-to:BUTTON" (held) entries separated by commas.
            std::string spec = ai;
            size_t pos = 0;
            while (pos < spec.size())
            {
                size_t end = spec.find(',', pos);
                std::string item = spec.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
                size_t colon = item.find(':');
                if (colon != std::string::npos)
                {
                    std::string when = item.substr(0, colon);
                    // "@seconds" entries count from the first guest call of PS2X_AUTOINPUT_ANCHOR=<hex pc>
                    // (menu timing drifts with load; e.g. anchor OnInitLevelSelect_Tag for the arena chooser).
                    int anchorHit = 0;
                    if (!when.empty() && when[0] == '@')
                    {
                        when.erase(0, 1);
                        anchorHit = g_autoAnchorCount;
                        const size_t plus = when.find('+');
                        if (plus != std::string::npos)
                        {
                            anchorHit = std::max(1, std::atoi(when.c_str()));
                            when.erase(0, plus + 1);
                        }
                        anchorHit = std::min(anchorHit, kAutoAnchorHits);
                    }
                    const size_t dash = when.find('-', 1);
                    const double t = std::atof(when.c_str());
                    const double until = dash != std::string::npos ? std::atof(when.c_str() + dash + 1) : t + 0.25;
                    // "BUTTON#n": pad n (1-based; default 1), e.g. "CROSS#2" for a split-screen player 2.
                    std::string b = item.substr(colon + 1);
                    int padIndex = 0;
                    const size_t hash = b.find('#');
                    if (hash != std::string::npos)
                    {
                        padIndex = std::clamp(std::atoi(b.c_str() + hash + 1) - 1, 0, kMaxPads - 1);
                        b.erase(hash);
                    }
                    static const std::pair<const char *, uint16_t> names[] = {
                        {"START", kBtnStart}, {"SELECT", kBtnSelect}, {"CROSS", kBtnCross}, {"CIRCLE", kBtnCircle},
                        {"SQUARE", kBtnSquare}, {"TRIANGLE", kBtnTriangle}, {"UP", kBtnUp}, {"DOWN", kBtnDown},
                        {"LEFT", kBtnLeft}, {"RIGHT", kBtnRight}, {"L1", kBtnL1}, {"R1", kBtnR1}, {"L2", kBtnL2}, {"R2", kBtnR2}};
                    for (const auto &n : names)
                        if (b == n.first)
                            g_autoPress.push_back({t, until, n.second, anchorHit, padIndex});
                }
                if (end == std::string::npos)
                    break;
                pos = end + 1;
            }
        }
        g_baseTitle = widen(title);
        g_hwnd = CreateWindowExW(g_background ? (WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW) : 0, wc.lpszClassName, widen(title).c_str(), style, x, y, w, h,
                                 nullptr, nullptr, inst, nullptr);
        if (!g_hwnd)
            return false;

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
        if (GetEnvironmentVariableA("PS2X_D3D_DEBUG", nullptr, 0) > 0)
            flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2,
                                       D3D11_SDK_VERSION, &g_device, nullptr, &g_ctx);
        if (FAILED(hr))
        {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 2,
                                   D3D11_SDK_VERSION, &g_device, nullptr, &g_ctx);
            if (FAILED(hr))
                return false;
        }

        ComPtr<IDXGIDevice> dxgiDev;
        g_device.As(&dxgiDev);
        ComPtr<IDXGIAdapter> adapter;
        dxgiDev->GetAdapter(&adapter);
        ComPtr<IDXGIFactory2> factory;
        adapter->GetParent(IID_PPV_ARGS(&factory));
        {
            ComPtr<IDXGIFactory5> factory5;
            BOOL allowTearing = FALSE;
            if (SUCCEEDED(factory.As(&factory5)) &&
                SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing))))
                g_tearingSupported = allowTearing != FALSE;
        }
        {
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
                g_displayHz = static_cast<double>(mode.dmDisplayFrequency);
        }

        RECT rc{};
        GetClientRect(g_hwnd, &rc);
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = std::max<LONG>(1, rc.right - rc.left);
        sd.Height = std::max<LONG>(1, rc.bottom - rc.top);
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Flags = swapChainFlags();
        hr = factory->CreateSwapChainForHwnd(g_device.Get(), g_hwnd, &sd, nullptr, nullptr, &g_swap);
        if (FAILED(hr))
            return false;
        {
            ComPtr<IDXGISwapChain2> swap2;
            if (SUCCEEDED(g_swap.As(&swap2)))
            {
                swap2->SetMaximumFrameLatency(1);
                g_frameWaitable = swap2->GetFrameLatencyWaitableObject();
            }
        }
        factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);
        createBackBufferView();

        ComPtr<ID3DBlob> vsb, psb;
        if (!compileShader(kBlitShader, "vs_main", "vs_5_0", vsb) ||
            !compileShader(kBlitShader, "ps_main", "ps_5_0", psb))
            return false;
        g_device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_blitVs);
        g_device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_blitPs);

        D3D11_SAMPLER_DESC samp{};
        samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samp.MaxLOD = D3D11_FLOAT32_MAX;
        g_device->CreateSamplerState(&samp, &g_linearSampler);

        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = 16;
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        g_device->CreateBuffer(&cbd, nullptr, &g_blitCb);

        // XAudio2 (optional: a missing audio device must not stop the game; the sound engine then runs silent
        // and pumpMessages keeps looking for a device).
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        createAudioEngine();

        if (!g_keyBindingsLoaded)
            loadKeyBindings(nullptr); // no settings file: the default keyboard layout
        input::init(static_cast<float>(g_config.deadzonePercent) / 100.0f);

        if (!g_background)
        {
            ShowWindow(g_hwnd, SW_SHOW);
            UpdateWindow(g_hwnd);
        }
        g_ready = true;
        std::fprintf(stdout, "[host] D3D11 %ux%u ready, XAudio2 %s\n", sd.Width, sd.Height,
                     g_master ? "ready" : "unavailable");
        return true;
    }

    void shutdown()
    {
        if (!g_ready)
            return;
        input::shutdown();
        destroyAudioEngine();
        g_uploadSrv.Reset();
        g_uploadTex.Reset();
        g_backRtv.Reset();
        g_swap.Reset();
        g_ctx.Reset();
        g_device.Reset();
        if (g_frameWaitable)
            CloseHandle(g_frameWaitable);
        g_frameWaitable = nullptr;
        if (g_hwnd)
            DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
        timeEndPeriod(1);
        g_ready = false;
    }

    bool isReady() { return g_ready; }

    bool pumpMessages()
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                g_quit.store(true);
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        pollInput();
        serviceAudio();
        const int fps = g_titleFps.load(), gameFps = g_titleGameFps.load();
        if (g_hwnd && fps >= 0 && (fps != g_shownTitleFps || gameFps != g_shownTitleGameFps))
        {
            g_shownTitleFps = fps;
            g_shownTitleGameFps = gameFps;
            const std::wstring t = g_baseTitle + L"  |  " + std::to_wstring(fps) + L" fps  (game " + std::to_wstring(gameFps) + L")";
            SetWindowTextW(g_hwnd, t.c_str());
        }
        return !g_quit.load();
    }

    bool quitRequested() { return g_quit.load(); }

    void presentRgba(const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t stride)
    {
        if (!g_ready || !rgba || width == 0 || height == 0)
            return;
        waitForNextFrame();
        std::lock_guard lock(g_d3dMutex);
        if (!g_uploadTex || g_uploadW != width || g_uploadH != height)
        {
            g_uploadSrv.Reset();
            g_uploadTex.Reset();
            D3D11_TEXTURE2D_DESC td{};
            td.Width = width;
            td.Height = height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(g_device->CreateTexture2D(&td, nullptr, &g_uploadTex)))
                return;
            g_device->CreateShaderResourceView(g_uploadTex.Get(), nullptr, &g_uploadSrv);
            g_uploadW = width;
            g_uploadH = height;
        }
        g_ctx->UpdateSubresource(g_uploadTex.Get(), 0, nullptr, rgba, stride, 0);
        drawBlit(g_uploadSrv.Get(), width, height, 1.0f, 1.0f);
    }

    void setPresentWide(bool wide) { g_presentWide.store(wide, std::memory_order_relaxed); }

    void presentTexture(void *srv, uint32_t width, uint32_t height, float u1, float v1)
    {
        if (!g_ready || !srv)
            return;
        std::lock_guard lock(g_d3dMutex);
        drawBlit(static_cast<ID3D11ShaderResourceView *>(srv), width, height, u1, v1);
    }

    std::recursive_mutex &d3dLock() { return g_d3dMutex; }
    uint64_t d3dStateEpoch() { return g_d3dEpoch.load(std::memory_order_relaxed); }
    void beginFrame() { waitForNextFrame(); }
    ID3D11Device *device() { return g_device.Get(); }
    ID3D11DeviceContext *context() { return g_ctx.Get(); }
    void *windowHandle() { return g_hwnd; }

    void pollInput()
    {
        PadState pads[kMaxPads];
        // Hidden test runs (PS2X_BACKGROUND) and PS2X_NO_XINPUT=1 ignore real controllers: controller input is
        // global, so a pad the user plays something else with would otherwise drive the test too.
        static const bool s_noControllers = std::getenv("PS2X_BACKGROUND") != nullptr || std::getenv("PS2X_NO_XINPUT") != nullptr;
        if (!s_noControllers)
            input::poll(pads, nowSeconds());

        if (!g_autoPress.empty())
        {
            const double t = nowSeconds();
            // PS2X_AUTOINPUT_STOP_AT_ANCHOR=1: timed (unanchored) presses end at the first anchor call -- the START
            // taps that get a slow boot past the title must not reach the menu the anchored presses navigate.
            static const bool s_stopAtAnchor = std::getenv("PS2X_AUTOINPUT_STOP_AT_ANCHOR") != nullptr;
            const bool anchored = g_autoAnchorAt[0].load() >= 0.0;
            for (const auto &ap : g_autoPress)
            {
                if (!ap.anchorHit && s_stopAtAnchor && anchored)
                    continue;
                const double anchorAt = ap.anchorHit ? g_autoAnchorAt[ap.anchorHit - 1].load() : 0.0;
                if (ap.anchorHit && anchorAt < 0.0)
                    continue;
                const double rt = ap.anchorHit ? t - anchorAt : t;
                if (ap.pad > 0)
                    pads[ap.pad].connected = true; // a scripted pad 2..4 is plugged in from boot
                if (rt >= ap.t && rt < ap.until)
                    pads[ap.pad].buttons |= ap.button;
            }
        }
        std::lock_guard lock(g_inputMutex);
        // Keyboard drives player 1 (merged with controller 1), with the [Controls] bindings.
        PadState &k = pads[0];
        for (size_t bit = 0; bit < g_keyBindings.size(); ++bit)
            for (const int vk : g_keyBindings[bit])
                if (vk > 0 && vk < 256 && g_keys[vk])
                {
                    k.buttons |= static_cast<uint16_t>(1u << bit);
                    if (bit == 8)
                        k.l2 = 0xFF; // a key is a fully pressed trigger
                    else if (bit == 9)
                        k.r2 = 0xFF;
                }
        float ax = 0, ay = 0;
        if (g_keys['D']) ax += 1;
        if (g_keys['A']) ax -= 1;
        if (g_keys['S']) ay += 1;
        if (g_keys['W']) ay -= 1;
        if (ax != 0 || ay != 0)
        {
            k.lx = static_cast<uint8_t>(std::clamp((ax + 1.0f) * 127.5f, 0.0f, 255.0f));
            k.ly = static_cast<uint8_t>(std::clamp((ay + 1.0f) * 127.5f, 0.0f, 255.0f));
        }
        // Keyboard player is always considered present so the game has a pad on port 0.
        k.connected = true;
        // The mod menu (F2, the guide button, BACK + RB, or the main menu's m plank) takes player 1's input while
        // it is up.
        bool menuKey = g_keys[VK_F2];
        for (const PadState &p : pads)
            menuKey |= p.menu;
        modmenu::filterInput(pads, kMaxPads, menuKey);
        for (int i = 0; i < kMaxPads; ++i)
            g_pads[i] = pads[i];
    }

    PadState pad(int index)
    {
        if (index < 0 || index >= kMaxPads)
            return {};
        std::lock_guard lock(g_inputMutex);
        return g_pads[index];
    }

    int connectedPadCount()
    {
        std::lock_guard lock(g_inputMutex);
        int n = 0;
        for (const auto &p : g_pads)
            n += p.connected ? 1 : 0;
        return n;
    }

    bool keyDown(int vk)
    {
        if (vk < 0 || vk >= 256)
            return false;
        std::lock_guard lock(g_inputMutex);
        return g_keys[vk];
    }

    void setVibration(int index, uint8_t smallMotor, uint8_t largeMotor)
    {
        if (index < 0 || index >= kMaxPads)
            return;
        input::rumble(index, smallMotor, largeMotor);
    }

    IXAudio2 *xaudio() { return g_xaudio.Get(); }
    IXAudio2MasteringVoice *masteringVoice() { return g_master; }
    bool audioReady() { return g_master != nullptr; }
    std::recursive_mutex &audioMutex() { return g_audioMutex; }

    void addAudioResetListener(std::function<void()> onReset)
    {
        std::lock_guard lock(g_audioMutex);
        g_audioResetListeners.push_back(std::move(onReset));
    }

    double displayRefreshHz() { return g_displayHz; }

    float effectsGain() { return static_cast<float>(std::min(g_config.effectsVolume, 100u)) / 100.0f; }
    float commentaryGain() { return static_cast<float>(std::min(g_config.commentaryVolume, 100u)) / 100.0f; }
    float movieGain() { return static_cast<float>(std::min(g_config.movieVolume, 100u)) / 100.0f; }

    double timeSeconds() { return nowSeconds(); }
    double lastFrameMs() { return g_lastFrameMs; }

    // ---- Sampling profiler (PS2X_PROFILE=<output file>) ------------------------------------------
    namespace
    {
        std::atomic<bool> g_profRun{false};
        std::thread g_profThread;
        HANDLE g_profTarget = nullptr;
        std::string g_profPath;
        std::unordered_map<std::string, uint64_t> g_profStacks;
    }

    void profileCurrentThread()
    {
        const char *out = std::getenv("PS2X_PROFILE");
        if (!out || !*out || g_profRun.load())
            return;
        g_profPath = out;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_profTarget,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
        g_profRun.store(true);
        g_profThread = std::thread([]()
        {
            std::unordered_map<uint64_t, uint64_t> hist;
            const uint64_t base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
            uint64_t samples = 0;
            auto dump = [&]()
            {
                if (FILE *f = std::fopen(g_profPath.c_str(), "w"))
                {
                    std::fprintf(f, "base %llx samples %llu\n", (unsigned long long)base, (unsigned long long)samples);
                    for (const auto &[rip, n] : hist)
                        std::fprintf(f, "%llx %llu\n", (unsigned long long)rip, (unsigned long long)n);
                    for (const auto &[stack, n] : g_profStacks)
                        std::fprintf(f, "S %llu%s\n", (unsigned long long)n, stack.c_str());
                    std::fclose(f);
                }
            };
            auto lastDump = std::chrono::steady_clock::now();
            // PS2X_PROFILE_SEC=<s>: start sampling that long after launch (e.g. once the menu is up).
            static const double s_profStart = std::getenv("PS2X_PROFILE_SEC") ? std::atof(std::getenv("PS2X_PROFILE_SEC")) : 0.0;
            const auto profBoot = std::chrono::steady_clock::now();
            while (g_profRun.load() && std::chrono::duration<double>(std::chrono::steady_clock::now() - profBoot).count() < s_profStart)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            while (g_profRun.load())
            {
                // PS2X_PROFILE_STALL=1 (with PS2X_HITCH_LOG): sample only while no frame has been flipped for 50 ms.
                static const bool s_stallOnly = std::getenv("PS2X_PROFILE_STALL") != nullptr;
                if (s_stallOnly)
                {
                    const double lastFlip = g_hitchLastFlip.load(std::memory_order_relaxed);
                    if (lastFlip <= 0.0 || nowSeconds() - lastFlip < 0.05)
                    {
                        Sleep(1);
                        if (std::chrono::steady_clock::now() - lastDump > std::chrono::seconds(5))
                        {
                            dump();
                            lastDump = std::chrono::steady_clock::now();
                        }
                        continue;
                    }
                }
                // PS2X_PROFILE_ALL=1 samples every thread of the process (guest threads run on their own
                // host threads, so the game thread alone mostly shows hand-off waits).
                static const bool s_all = std::getenv("PS2X_PROFILE_ALL") != nullptr;
                static std::vector<HANDLE> s_threads;
                static auto s_lastEnum = std::chrono::steady_clock::time_point{};
                if (s_all && std::chrono::steady_clock::now() - s_lastEnum > std::chrono::milliseconds(500))
                {
                    s_lastEnum = std::chrono::steady_clock::now();
                    for (HANDLE h : s_threads)
                        CloseHandle(h);
                    s_threads.clear();
                    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                    if (snap != INVALID_HANDLE_VALUE)
                    {
                        THREADENTRY32 te{};
                        te.dwSize = sizeof(te);
                        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
                        {
                            if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId())
                                continue;
                            if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID))
                                s_threads.push_back(h);
                        }
                        CloseHandle(snap);
                    }
                }
                std::vector<HANDLE> targets = s_all ? s_threads : std::vector<HANDLE>{g_profTarget};
                for (HANDLE target : targets)
                {
                if (SuspendThread(target) != static_cast<DWORD>(-1))
                {
                    CONTEXT c{};
                    c.ContextFlags = CONTEXT_FULL;
                    if (GetThreadContext(target, &c))
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
                        // No allocation while the target is suspended (it may hold the heap lock).
                        uint64_t frames[10];
                        int nframes = 0;
                        CONTEXT w = (c.Rip >= modBase && c.Rip < modEnd) ? c : u;
                        for (; nframes < 10 && w.Rip >= modBase && w.Rip < modEnd; ++nframes)
                        {
                            frames[nframes] = w.Rip;
                            DWORD64 imageBase = 0;
                            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(w.Rip, &imageBase, nullptr);
                            if (!fn)
                            {
                                ++nframes;
                                break;
                            }
                            PVOID handlerData = nullptr;
                            DWORD64 establisher = 0;
                            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, w.Rip, fn, &w, &handlerData, &establisher, nullptr);
                        }
                        ResumeThread(target);
                        ++hist[attributed];
                        std::string key;
                        for (int i = 0; i < nframes; ++i)
                        {
                            char buf[24];
                            std::snprintf(buf, sizeof(buf), " %llx", static_cast<unsigned long long>(frames[i]));
                            key += buf;
                        }
                        ++g_profStacks[key];
                        ++samples;
                        continue;
                    }
                    ResumeThread(target);
                }
                }
                Sleep(1);
                if (std::chrono::steady_clock::now() - lastDump > std::chrono::seconds(5))
                {
                    dump();
                    lastDump = std::chrono::steady_clock::now();
                }
            }
            dump();
        });
        g_profThread.detach();
    }

}
