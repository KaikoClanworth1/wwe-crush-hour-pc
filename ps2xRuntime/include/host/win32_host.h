#pragma once

// Native Windows host layer: Win32 window, Direct3D 11 presentation, XInput pads,
// keyboard, and XAudio2 audio. Replaces the raylib host on Windows builds.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IXAudio2;
struct IXAudio2MasteringVoice;

namespace ps2host
{
    constexpr int kMaxPads = 4;

    struct HostConfig
    {
        uint32_t width = 1920;
        uint32_t height = 1080;
        bool fullscreen = false;
        bool vsync = true;
        uint32_t targetFps = 60; // 30, 60, or higher (120 / 144 / ...); 0 = the display's refresh rate
        bool keepAspect = true; // 4:3 pillarbox for the PS2 framebuffer
        bool widescreen = false;
        // [Display] SplitWindows: a two-player match shows each player in a window of their own (split_windows.cpp).
        bool splitWindows = true;
        // [Display] TextureFiltering: 0 = as on the PS2 (no mipmaps), 1 = mipmaps (trilinear),
        // 2 / 4 / 8 / 16 = mipmaps + that much anisotropic filtering.
        uint32_t textureFiltering = 16;
        bool dumpTextures = false; // [Display] DumpTextures: write decoded textures to Textures/dump
        // [Audio] volumes, 0..100 (percent).
        uint32_t masterVolume = 100;
        uint32_t effectsVolume = 100;    // sound effects (SENG events)
        uint32_t commentaryVolume = 100; // SENG streams (commentary)
        uint32_t movieVolume = 100;      // PSS movie audio
        // [Timing] WallClockTimers: EE timers follow host time instead of synthetic guest cycles.
        bool wallClockTimers = false;
        // [Controls] Deadzone: radial stick deadzone in percent of full deflection.
        uint32_t deadzonePercent = 15;
        // [Saves] Folder: where the memory card folders (mc0, mc1) live; empty = the platform default.
        std::filesystem::path saveFolder;
    };

    // PS2 DualShock 2 buttons in libpad bit order (active-high here; callers invert).
    enum PadButton : uint16_t
    {
        kBtnSelect = 1u << 0,
        kBtnL3 = 1u << 1,
        kBtnR3 = 1u << 2,
        kBtnStart = 1u << 3,
        kBtnUp = 1u << 4,
        kBtnRight = 1u << 5,
        kBtnDown = 1u << 6,
        kBtnLeft = 1u << 7,
        kBtnL2 = 1u << 8,
        kBtnR2 = 1u << 9,
        kBtnL1 = 1u << 10,
        kBtnR1 = 1u << 11,
        kBtnTriangle = 1u << 12,
        kBtnCircle = 1u << 13,
        kBtnCross = 1u << 14,
        kBtnSquare = 1u << 15,
    };

    struct PadState
    {
        bool connected = false;
        uint16_t buttons = 0; // active-high PadButton mask
        uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
        uint8_t l2 = 0, r2 = 0; // analog trigger pressure
        bool menu = false;      // guide / touchpad button: opens the mod menu
    };

    const HostConfig &config();
    // Loads the settings file. A relative name is looked up in the working directory, then next to the executable
    // and in its GameFiles folder, so the game also starts without a launcher setting the working directory.
    bool loadConfig(const char *iniPath);
    // The settings file in use (or where it will be written when none was found); its folder also holds Mods.
    std::filesystem::path configPath();

    // Settings page (mod menu): read / change a setting live and write it back to the settings file.
    enum class Setting
    {
        ShowFps,
        VSync,
        Fullscreen,
        TargetFps,  // applies on restart: 30, 60, 120, 144, 165, 240, 0 = display refresh
        Widescreen, // applies on restart
        TextureFiltering, // applies on restart: 0 (PS2), 1 (mipmaps), 2 / 4 / 8 / 16 (anisotropic)
        MasterVolume,
        EffectsVolume,
        CommentaryVolume,
        MovieVolume,
    };
    int getSetting(Setting setting);
    void setSetting(Setting setting, int value);

    // Folder that holds the player's memory card folders (mc0, mc1): PS2X_SAVE_DIR, else [Saves] Folder, else the
    // platform's save location (Windows: Saved Games\WWE Crush Hour). Empty = no host default (saves stay next
    // to the game files).
    std::filesystem::path saveDirectory();

    bool init(const char *title);
    void shutdown();
    bool isReady();

    // Pump Win32 messages. Returns false once the user asked to quit.
    bool pumpMessages();
    bool quitRequested();

    // Present a guest framebuffer (RGBA8, tightly packed rows of `stride` bytes).
    // Scaled with bilinear filtering into the 1080p back buffer.
    void presentRgba(const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t stride);
    // Blocks (outside any lock) until the next frame should be presented: vsync + FPS cap.
    // presentRgba() calls this itself; presentTexture() callers must call it first.
    void beginFrame();
    // Present whatever is currently bound as the hardware-rendered guest frame.
    void presentTexture(void *shaderResourceView, uint32_t width, uint32_t height,
                        float u1 = 1.0f, float v1 = 1.0f);

    // Serializes all use of the immediate context (game thread draws vs host presents).
    std::recursive_mutex &d3dLock();
    ID3D11Device *device();
    ID3D11DeviceContext *context();
    void *windowHandle();
    // Two-player split screen in separate windows is on for the current frame (split_windows.cpp).
    bool splitScreenActive();

    // Input -------------------------------------------------------------
    void pollInput();
    PadState pad(int index);        // XInput pad (index 0..3) merged with keyboard on pad 0
    int connectedPadCount();
    bool keyDown(int virtualKey);
    void setVibration(int index, uint8_t smallMotor, uint8_t largeMotor);

    // Audio -------------------------------------------------------------
    // Hold audioMutex() around any use of xaudio() / masteringVoice() and of source voices created from them:
    // the host tears the engine down and rebuilds it under this lock when the audio device is lost (unplugged)
    // and when a device appears after the game started without one.
    IXAudio2 *xaudio();
    IXAudio2MasteringVoice *masteringVoice();
    bool audioReady();
    std::recursive_mutex &audioMutex();
    // Registers a callback run (under audioMutex) just before the engine is destroyed: destroy every source
    // voice created from it. Voices are recreated against the new engine on next use.
    void addAudioResetListener(std::function<void()> onReset);
    // Volume (0..1) of an [Audio] channel from config(); the master volume is applied on the mastering voice.
    float effectsGain();
    float commentaryGain();
    float movieGain();

    // Timing ------------------------------------------------------------
    double timeSeconds();
    // Refresh rate of the primary display (TargetFPS=0: the game runs at this rate).
    double displayRefreshHz();
    double lastFrameMs();

    // Debug: start sampling the calling thread's instruction pointer (PS2X_PROFILE=<file>).
    void profileCurrentThread();
}
