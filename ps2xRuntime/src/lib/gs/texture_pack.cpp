// Texture replacement packs (runtime/gs/texture_pack.h): images read and written through WIC.

#include "runtime/gs/texture_pack.h"
#include "host/win32_host.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace ps2gs::pack
{
    namespace
    {
        std::mutex g_mutex;
        bool g_scanned = false;
        std::filesystem::path g_dir;
        std::unordered_map<uint64_t, std::filesystem::path> g_files; // content id -> replacement file
        std::unordered_map<uint64_t, Image> g_loaded;
        std::unordered_set<uint64_t> g_failed, g_dumped;
        bool g_dumping = false;

        IWICImagingFactory *factory()
        {
            // Called from the GS worker: COM per thread (S_FALSE / RPC_E_CHANGED_MODE when already set up).
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            static ComPtr<IWICImagingFactory> s_factory;
            if (!s_factory)
                CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&s_factory));
            return s_factory.Get();
        }

        void scanLocked()
        {
            if (g_scanned)
                return;
            g_scanned = true;
            const std::filesystem::path ini = ps2host::configPath();
            g_dir = (ini.empty() ? std::filesystem::path(".") : ini.parent_path()) / "Textures";
            const char *dumpEnv = std::getenv("PS2X_TEXTURE_DUMP");
            g_dumping = dumpEnv && *dumpEnv ? *dumpEnv != '0' : ps2host::config().dumpTextures;
            std::error_code ec;
            if (!std::filesystem::is_directory(g_dir, ec))
                return;
            for (const auto &entry : std::filesystem::directory_iterator(g_dir, ec))
            {
                if (!entry.is_regular_file(ec))
                    continue;
                const std::string stem = entry.path().stem().string();
                if (stem.size() != 16u || stem.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
                    continue;
                g_files[std::strtoull(stem.c_str(), nullptr, 16)] = entry.path();
            }
            if (!g_files.empty())
                std::printf("[textures] %zu replacement textures in %s\n", g_files.size(), g_dir.string().c_str());
        }

        bool loadImage(const std::filesystem::path &path, Image &out)
        {
            IWICImagingFactory *wic = factory();
            ComPtr<IWICBitmapDecoder> decoder;
            ComPtr<IWICBitmapFrameDecode> frame;
            ComPtr<IWICBitmapSource> rgba;
            if (!wic ||
                FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) ||
                FAILED(decoder->GetFrame(0, &frame)) ||
                FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, frame.Get(), &rgba)))
                return false;
            UINT w = 0, h = 0;
            rgba->GetSize(&w, &h);
            if (w == 0 || h == 0 || w > 8192 || h > 8192)
                return false;
            out.w = w;
            out.h = h;
            out.px.resize(static_cast<size_t>(w) * h);
            return SUCCEEDED(rgba->CopyPixels(nullptr, w * 4u, static_cast<UINT>(out.px.size() * 4u),
                                              reinterpret_cast<BYTE *>(out.px.data())));
        }

        bool savePng(const std::filesystem::path &path, const std::vector<uint32_t> &px, uint32_t w, uint32_t h)
        {
            IWICImagingFactory *wic = factory();
            ComPtr<IWICStream> stream;
            ComPtr<IWICBitmapEncoder> encoder;
            ComPtr<IWICBitmapFrameEncode> frame;
            if (!wic || FAILED(wic->CreateStream(&stream)) ||
                FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
                FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
                FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
                FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
                FAILED(frame->SetSize(w, h)))
                return false;
            WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
            if (FAILED(frame->SetPixelFormat(&format)))
                return false;
            std::vector<uint32_t> converted;
            const std::vector<uint32_t> *pixels = &px;
            if (format != GUID_WICPixelFormat32bppRGBA) // the encoder settled on BGRA: swap red and blue
            {
                converted.resize(px.size());
                for (size_t i = 0; i < px.size(); ++i)
                {
                    const uint32_t c = px[i];
                    converted[i] = (c & 0xFF00FF00u) | ((c & 0xFFu) << 16) | ((c >> 16) & 0xFFu);
                }
                pixels = &converted;
            }
            return SUCCEEDED(frame->WritePixels(h, w * 4u, static_cast<UINT>(pixels->size() * 4u),
                                                reinterpret_cast<BYTE *>(const_cast<uint32_t *>(pixels->data())))) &&
                   SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
        }

        uint32_t withAlpha(uint32_t c, uint32_t a) { return (c & 0x00FFFFFFu) | (std::min(a, 255u) << 24); }
    }

    const Image *find(uint64_t id, bool ps2Alpha)
    {
        std::lock_guard lock(g_mutex);
        scanLocked();
        const auto file = g_files.find(id);
        if (file == g_files.end() || g_failed.count(id))
            return nullptr;
        auto it = g_loaded.find(id);
        if (it == g_loaded.end())
        {
            Image image;
            if (!loadImage(file->second, image))
            {
                std::printf("[textures] cannot read %s\n", file->second.string().c_str());
                g_failed.insert(id);
                return nullptr;
            }
            if (ps2Alpha) // 0..255 in the file -> the game's 0..0x80
                for (uint32_t &c : image.px)
                    c = withAlpha(c, ((c >> 24) * 128u + 127u) / 255u);
            it = g_loaded.emplace(id, std::move(image)).first;
        }
        return &it->second;
    }

    void dump(uint64_t id, const uint32_t *px, uint32_t w, uint32_t h, bool ps2Alpha)
    {
        std::lock_guard lock(g_mutex);
        scanLocked();
        if (!g_dumping || !px || !w || !h || !g_dumped.insert(id).second)
            return;
        std::error_code ec;
        const std::filesystem::path dir = g_dir / "dump";
        std::filesystem::create_directories(dir, ec);
        std::vector<uint32_t> out(px, px + static_cast<size_t>(w) * h);
        if (ps2Alpha)
            for (uint32_t &c : out)
                c = withAlpha(c, (c >> 24) * 255u / 128u);
        char name[32];
        std::snprintf(name, sizeof(name), "%016llx.png", static_cast<unsigned long long>(id));
        savePng(dir / name, out, w, h);
    }
}
