// Split-screen windows (WWE Crush Hour port): in a two-player match each player gets a window of their own, with a
// full-size picture and the one-player HUD ([Display] SplitWindows=1, the default; PS2X_SPLIT_WINDOWS=0/1 overrides).
//
// The game keeps drawing its usual top/bottom split frame; win32_host.cpp shows the top half (player 1) in the main
// window and the bottom half (player 2) in a second one, each stretched to the window. Three things change so that a
// stretched half looks like a one-player screen:
//  - 3D: each half's camera gets a view window twice as tall (ps2x_split_view_window, from RwCameraSetViewWindow's
//    hook in ps2_runtime.cpp), so the picture keeps its proportions at twice the height;
//  - HUD layout: Hud::Update hands the per-player elements the whole screen and the one-player screen config instead
//    of their half (ps2x_split_hud_owner; the sprite infos hold one layout per screen config);
//  - HUD sprites: PcplSpriteRenderer::AddSprite squeezes every sprite of a player's elements into that player's half
//    at half height (ps2x_split_sprite). Sprites both players see -- the scoreboard, messages, the pause menu and
//    other overlays -- are added twice, once into each half.
// Hooks: port/tools/patch_splitwin.py. A split frame is one whose HUD was laid out for two players within the last
// half second, or since then rendered with both players' cameras (pause); player 2's window opens with the match and
// closes after it.

#include "host/win32_host.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    // Whose sprites are being added: 0 / 1 a player's HUD element; 2 a HUD element both players see; -1 anything
    // else (overlays, the pause menu: also seen by both); 3 a sprite drawn into a texture (left as it is).
    std::atomic<int> g_owner{-1};
    std::atomic<double> g_splitSeen{-1.0e9};
    std::atomic<bool> g_matchLatched{false}; // a two-player HUD was seen and both cameras still render
    thread_local bool t_inCopy = false; // ps2x_split_sprite: drawing the first of two copies

    constexpr uint32_t kSafeAreaOffsetY = 0x4A9468u; // PcplSpriteBank::sSafeAreaOffsetY
    constexpr uint32_t kSafeAreaScaleY = 0x3FF9B0u;  // PcplSpriteBank::sSafeAreaScaleY

    double nowSec()
    {
        static LARGE_INTEGER s_freq{};
        if (!s_freq.QuadPart)
            QueryPerformanceFrequency(&s_freq);
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return static_cast<double>(t.QuadPart) / static_cast<double>(s_freq.QuadPart);
    }

    bool featureOn()
    {
        static const bool s_on = []()
        {
            if (const char *v = std::getenv("PS2X_SPLIT_WINDOWS"); v && *v)
                return *v != '0';
            return ps2host::config().splitWindows;
        }();
        return s_on;
    }

    bool logOn()
    {
        static const bool s_log = std::getenv("PS2X_SPLIT_LOG") != nullptr;
        return s_log;
    }

    uint32_t rd32(const uint8_t *rdram, uint32_t a)
    {
        uint32_t v = 0;
        std::memcpy(&v, rdram + (a & 0x01FFFFFCu), 4);
        return v;
    }

    float rdf(const uint8_t *rdram, uint32_t a)
    {
        float v = 0.0f;
        std::memcpy(&v, rdram + (a & 0x01FFFFFCu), 4);
        return v;
    }

    float pointY(uint64_t p)
    {
        const uint32_t bits = static_cast<uint32_t>(p >> 32);
        float y;
        std::memcpy(&y, &bits, 4);
        return y;
    }

    uint64_t withY(uint64_t p, float y)
    {
        uint32_t bits;
        std::memcpy(&bits, &y, 4);
        return (p & 0xFFFFFFFFull) | (static_cast<uint64_t>(bits) << 32);
    }

    // Sprite y (renderer space, before the sprite camera's matrix) into one half of the frame at half height. The
    // sprite camera maps y to the screen as (y - 0.5 + offsetY) * scaleY, so the screen spans y0 .. y0 + 1/scaleY.
    void squeeze(const uint8_t *rdram, uint64_t *pts, int half)
    {
        const float offY = rdf(rdram, kSafeAreaOffsetY);
        float scaleY = rdf(rdram, kSafeAreaScaleY);
        if (!(scaleY > 0.01f && scaleY < 100.0f))
            scaleY = 1.0f;
        const float span = 1.0f / scaleY;
        const float y0 = 0.5f - offY - 0.5f * span;
        const float top = y0 + 0.5f * span * static_cast<float>(half);
        for (int i = 0; i < 4; ++i)
            pts[i] = withY(pts[i], top + (pointY(pts[i]) - y0) * 0.5f);
    }
}

namespace ps2host
{
    bool splitScreenActive()
    {
        return featureOn() && nowSec() - g_splitSeen.load(std::memory_order_relaxed) < 0.5;
    }
}

// Hud::Update, before each element's update: owner 0 / 1 (a player's element, two-player layout), 2 (an element
// both players share) or -1 (after the last element); also 2 for the overlays, 3 for a sprite bank's texture camera
// and -1 when a sprite bank is flushed. Returns 1 when a player's element gets the whole screen and
// the one-player screen config instead of its half (the hook then sets its rectangle and config arguments).
extern "C" int ps2x_split_hud_owner(int owner)
{
    if (!featureOn())
        return 0;
    if (owner == 0 || owner == 1)
    {
        if (g_splitSeen.load(std::memory_order_relaxed) < nowSec() - 0.5 && logOn())
            std::printf("[split] two-player HUD: split windows on\n");
        g_splitSeen.store(nowSec(), std::memory_order_relaxed);
        g_matchLatched.store(true, std::memory_order_relaxed);
    }
    g_owner.store(owner, std::memory_order_relaxed);
    return owner == 0 || owner == 1 ? 1 : 0;
}

// GameRenderer::RenderWorldCameras entry: how many world cameras render this frame. The HUD is not updated while
// the match is paused (pause menu), so the split also stays on as long as both players' cameras keep rendering
// after a two-player HUD was seen.
extern "C" void ps2x_split_world_cameras(uint32_t count)
{
    if (!featureOn())
        return;
    if (count < 2u)
        g_matchLatched.store(false, std::memory_order_relaxed);
    else if (g_matchLatched.load(std::memory_order_relaxed))
        g_splitSeen.store(nowSec(), std::memory_order_relaxed);
}

// PcplSpriteRenderer::AddSprite (a sprite's four corners, before it is queued for the frame).
//  pass 0: returns 0 (untouched), 1 (moved into its owner's half) or 2 (both players see it: the hook adds it twice,
//          pass 1 moving the corners for the first copy and pass 2 for the second).
extern "C" int ps2x_split_sprite(uint8_t *rdram, uint32_t renderer, uint64_t *pts, int pass)
{
    if (!rdram)
        return 0;
    if (pass == 1)
    {
        t_inCopy = true;
        squeeze(rdram, pts, 0);
        return 1;
    }
    if (pass == 2)
    {
        t_inCopy = false;
        squeeze(rdram, pts, 1);
        return 1;
    }
    if (t_inCopy || !ps2host::splitScreenActive())
        return 0;
    const int owner = g_owner.load(std::memory_order_relaxed);
    if (owner == 3)
        return 0;
    if (logOn())
    {
        static int s_shown = 0;
        if (s_shown++ < 40)
            std::printf("[split] sprite owner %d y %.3f %.3f %.3f %.3f x %.3f offY %.3f scaleY %.3f\n", owner,
                        pointY(pts[0]), pointY(pts[1]), pointY(pts[2]), pointY(pts[3]),
                        [&] { float x; uint32_t b = static_cast<uint32_t>(pts[0]); std::memcpy(&x, &b, 4); return x; }(),
                        rdf(rdram, kSafeAreaOffsetY), rdf(rdram, kSafeAreaScaleY));
    }
    if (owner == 0 || owner == 1)
    {
        squeeze(rdram, pts, owner);
        return 1;
    }
    // Two copies: only while the renderer's vertex buffer has room for both (4 vertices of 36 bytes per sprite).
    const uint32_t capacity = rd32(rdram, renderer + 4u);
    const uint32_t used = (rd32(rdram, renderer + 0x10u) - rd32(rdram, renderer + 0x0Cu)) / 144u;
    if (used + 2u > capacity)
    {
        static bool s_warned = false;
        if (!s_warned && logOn())
            std::printf("[split] sprite buffer full (%u of %u): shared sprite left as is\n", used, capacity);
        s_warned = true;
        return 0;
    }
    return 2;
}

// DrawFace's RwIm2DRenderPrimitive (the scoreboard portraits, RwSky2DVertex triangles in screen pixels: y at +4):
//  pass 0: returns 1 when the split is on (the hook then draws the triangles twice), else 0;
//  pass 1: the first copy, into the top half; pass 2: the second one, into the bottom half.
extern "C" int ps2x_split_im2d(uint8_t *rdram, uint32_t verts, uint32_t count, uint32_t stride, int pass)
{
    static thread_local float t_y[16];
    if (!rdram || count > 16u)
        return 0;
    if (pass == 0)
        return ps2host::splitScreenActive() ? 1 : 0;
    uint16_t h16 = 0;
    std::memcpy(&h16, rdram + 0x4AAC5Eu, 2); // gGraphicsLibInfo: screen height
    const float height = h16 >= 224u ? static_cast<float>(h16) : 448.0f;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t a = verts + i * stride + 4u;
        if (pass == 1)
            t_y[i] = rdf(rdram, a);
        const float y = t_y[i] * 0.5f + (pass == 2 ? 0.5f * height : 0.0f);
        std::memcpy(rdram + (a & 0x01FFFFFCu), &y, 4);
    }
    return 1;
}

// RwCameraSetViewWindow from the game's camera API (ps2x_widescreen_view_window): a camera drawing into half of the
// screen (a split-screen player's camera: a camera raster as wide as the screen and at most ~half as tall) gets a
// view window twice as tall, so its picture keeps its proportions in the player's window.
extern "C" void ps2x_split_view_window(uint8_t *rdram, uint32_t camera, uint32_t viewWindow)
{
    if (!featureOn() || !rdram)
        return;
    const uint32_t raster = rd32(rdram, camera + 0x60u); // RwCamera::frameBuffer
    if (!raster)
        return;
    const uint32_t type = rdram[(raster + 0x20u) & 0x01FFFFFFu] & 7u; // RwRaster::cType
    const uint32_t width = rd32(rdram, raster + 0x0Cu), height = rd32(rdram, raster + 0x10u);
    const bool half = type == 2u && width >= 512u && height >= 160u && height <= 300u;
    if (logOn())
    {
        static int s_shown = 0;
        if (s_shown++ < 64)
            std::printf("[split] view window camera 0x%x raster 0x%x type %u %ux%u view %.3f x %.3f -> %s\n", camera,
                        raster, type, width, height, rdf(rdram, viewWindow), rdf(rdram, viewWindow + 4u),
                        half ? "x2 tall" : "kept");
    }
    if (!half)
        return;
    const float y = rdf(rdram, viewWindow + 4u) * 2.0f;
    std::memcpy(rdram + ((viewWindow + 4u) & 0x01FFFFFCu), &y, 4);
}
