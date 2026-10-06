#include "runtime/host_file_cache.h"
#include "ps2_runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace ps2cache
{
    namespace
    {
        constexpr uint64_t kMaxFileBytes = 64ull << 20;  // larger files (the credits movie) stay on disk
        constexpr uint64_t kOnDemandBudget = 256ull << 20; // files loaded on first read (movies) kept at once
        constexpr uint64_t kChunkBytes = 1ull << 20;
        // Folders read into memory at boot: everything the game loads while a match is running.
        const char *const kPreloadDirs[] = {"BUNDLES", "AUDIO", "IOPDATA", "MODULES"};

        struct Entry
        {
            std::filesystem::path path;
            uint64_t size = 0;
            bool pinned = false; // preloaded folder: never evicted
            std::unique_ptr<uint8_t[]> data;
            std::atomic<uint64_t> loaded{0}; // bytes [0, loaded) are valid
            std::atomic<uint64_t> lastUse{0};
        };

        std::mutex g_mutex;
        std::condition_variable g_cv;
        // nullptr: a file that is not cached (too large, empty or unreadable).
        std::unordered_map<std::wstring, std::shared_ptr<Entry>> g_entries;
        std::deque<std::shared_ptr<Entry>> g_queue;
        uint64_t g_onDemandBytes = 0;
        std::atomic<uint64_t> g_useClock{0};
        std::once_flag g_startOnce;
        bool g_enabled = true;

        std::wstring keyOf(const std::filesystem::path &path)
        {
            std::error_code error;
            std::filesystem::path full = std::filesystem::absolute(path, error);
            std::wstring key = (error ? path : full).lexically_normal().generic_wstring();
            for (wchar_t &c : key)
                c = static_cast<wchar_t>(std::towlower(c));
            return key;
        }

        std::shared_ptr<Entry> makeEntry(const std::filesystem::path &path, uint64_t size, bool pinned)
        {
            auto entry = std::make_shared<Entry>();
            entry->path = path;
            entry->size = size;
            entry->pinned = pinned;
            entry->data.reset(new uint8_t[size]);
            return entry;
        }

        // Drops least recently used on-demand files nobody is reading until `incoming` more bytes fit.
        void evictLocked(uint64_t incoming)
        {
            while (g_onDemandBytes + incoming > kOnDemandBudget)
            {
                auto victim = g_entries.end();
                for (auto it = g_entries.begin(); it != g_entries.end(); ++it)
                {
                    const std::shared_ptr<Entry> &e = it->second;
                    if (!e || e->pinned || e.use_count() != 1) // queued or being read
                        continue;
                    if (victim == g_entries.end() || e->lastUse.load() < victim->second->lastUse.load())
                        victim = it;
                }
                if (victim == g_entries.end())
                    return;
                g_onDemandBytes -= victim->second->size;
                g_entries.erase(victim);
            }
        }

        void loaderMain()
        {
            const auto t0 = std::chrono::steady_clock::now();
            bool reportedPreload = false;
            uint64_t preloadBytes = 0, preloadFiles = 0;
            for (;;)
            {
                std::shared_ptr<Entry> entry;
                {
                    std::unique_lock lock(g_mutex);
                    if (g_queue.empty() && !reportedPreload)
                    {
                        reportedPreload = true;
                        std::printf("[file-cache] preloaded %llu files, %.1f MB in %.1f s\n",
                                    static_cast<unsigned long long>(preloadFiles), preloadBytes / 1048576.0,
                                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
                        std::fflush(stdout);
                    }
                    g_cv.wait(lock, [] { return !g_queue.empty(); });
                    entry = std::move(g_queue.front());
                    g_queue.pop_front();
                }
#if defined(_WIN32)
                std::FILE *file = ::_wfopen(entry->path.c_str(), L"rb");
#else
                std::FILE *file = std::fopen(entry->path.string().c_str(), "rb");
#endif
                if (!file)
                    continue; // stays unloaded: reads keep going to the disk
                uint64_t done = 0;
                while (done < entry->size)
                {
                    const size_t want = static_cast<size_t>(std::min(kChunkBytes, entry->size - done));
                    const size_t got = std::fread(entry->data.get() + done, 1, want, file);
                    if (got == 0)
                        break;
                    done += got;
                    entry->loaded.store(done, std::memory_order_release);
                }
                std::fclose(file);
                if (entry->pinned)
                {
                    preloadBytes += done;
                    ++preloadFiles;
                }
            }
        }

        void start()
        {
            const char *env = std::getenv("PS2X_FILE_CACHE");
            g_enabled = !(env && env[0] == '0');
            if (!g_enabled)
                return;
            const PS2Runtime::IoPaths &paths = PS2Runtime::getIoPaths();
            const std::filesystem::path root = !paths.cdRoot.empty() ? paths.cdRoot : paths.hostRoot;
            {
                std::lock_guard lock(g_mutex);
                for (const char *dir : kPreloadDirs)
                {
                    std::error_code error;
                    const std::filesystem::path folder = root / dir;
                    if (!std::filesystem::is_directory(folder, error))
                        continue;
                    for (std::filesystem::recursive_directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
                    {
                        if (!it->is_regular_file(error))
                            continue;
                        const uint64_t size = it->file_size(error);
                        if (error || size == 0 || size > kMaxFileBytes)
                            continue;
                        auto entry = makeEntry(it->path(), size, true);
                        g_entries[keyOf(it->path())] = entry;
                        g_queue.push_back(std::move(entry));
                    }
                }
            }
            std::thread(loaderMain).detach();
            g_cv.notify_one();
        }
    }

    bool read(const std::filesystem::path &path, uint64_t offset, void *dst, size_t size)
    {
        std::call_once(g_startOnce, start);
        if (!g_enabled || size == 0)
            return false;
        const std::wstring key = keyOf(path);
        std::shared_ptr<Entry> entry;
        {
            std::lock_guard lock(g_mutex);
            auto it = g_entries.find(key);
            if (it == g_entries.end())
            {
                // First read of a file outside the preloaded folders: load it in the background, ahead of
                // the preload queue (it is being streamed right now).
                std::error_code error;
                const uint64_t fileSize = std::filesystem::file_size(path, error);
                if (error || fileSize == 0 || fileSize > kMaxFileBytes)
                {
                    g_entries.emplace(key, nullptr);
                    return false;
                }
                evictLocked(fileSize);
                auto fresh = makeEntry(path, fileSize, false);
                fresh->lastUse.store(++g_useClock);
                g_onDemandBytes += fileSize;
                g_entries.emplace(key, fresh);
                g_queue.push_front(std::move(fresh));
                g_cv.notify_one();
                return false;
            }
            entry = it->second;
        }
        if (!entry || offset >= entry->size)
            return false;
        entry->lastUse.store(++g_useClock, std::memory_order_relaxed);
        const uint64_t end = std::min<uint64_t>(offset + size, entry->size);
        if (entry->loaded.load(std::memory_order_acquire) < end)
            return false;
        std::memcpy(dst, entry->data.get() + offset, static_cast<size_t>(end - offset));
        return true;
    }
}
