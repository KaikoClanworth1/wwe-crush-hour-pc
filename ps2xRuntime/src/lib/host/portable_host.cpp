// Headless stand-in for the native Windows host (win32_host.cpp) on builds without PS2X_NATIVE_HOST
// (Linux CI, tests, the raylib runner). Shared runtime code calls the ps2host input/timing/audio entry points
// unconditionally; here they report "no host": no pads, no audio device, a monotonic clock.

#include "host/win32_host.h"

#include <chrono>
#include <cstdlib>
#include <cstddef>
#include <filesystem>

namespace ps2host
{
    namespace
    {
        const auto g_start = std::chrono::steady_clock::now();
        HostConfig g_config;
    }

    const HostConfig &config() { return g_config; }

    std::filesystem::path configPath() { return {}; }
    int getSetting(Setting) { return 0; }
    void setSetting(Setting, int) {}

    std::filesystem::path saveDirectory()
    {
        const char *dir = std::getenv("PS2X_SAVE_DIR");
        return (dir && *dir) ? std::filesystem::path(dir) : std::filesystem::path();
    }
    bool loadConfig(const char *) { return false; }

    bool isReady() { return false; }

    PadState pad(int) { return {}; }
    int connectedPadCount() { return 0; }
    bool keyDown(int) { return false; }
    void setVibration(int, uint8_t, uint8_t) {}

    IXAudio2 *xaudio() { return nullptr; }
    IXAudio2MasteringVoice *masteringVoice() { return nullptr; }
    bool audioReady() { return false; }

    double timeSeconds()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    }
    double lastFrameMs() { return 0.0; }

    void profileCurrentThread() {}
}

// The mod menu (host/mod_menu.cpp) owns the disc-file fixes; without it disc reads are passed through as-is.
void ps2x_disc_file_patch(const std::filesystem::path &, uint64_t, uint8_t *, size_t) {}
