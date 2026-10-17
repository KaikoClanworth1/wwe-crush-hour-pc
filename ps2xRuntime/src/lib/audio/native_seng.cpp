#include "runtime/host_file_cache.h"
#include "runtime/native_seng.h"
#include "ps2_runtime.h"
#include "host/win32_host.h"
#include <atomic>

// Time the arena sound bank (bank 1) finished loading: a load-independent anchor for the match-start
// debug windows (PS2X_GS_TRACE_BANK1, PS2X_RAM_DUMP_BANK1). -1 until it happens.
std::atomic<double> g_ps2xBank1LoadedAt{-1.0};

#include <xaudio2.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps2native
{
    namespace
    {
        // ---------------------------------------------------------------- constants (spec §3, §4)
        constexpr uint16_t kEventUnused = 0, kEventInactive = 1, kEventActive = 2;
        constexpr uint16_t kGroupUnused = 0, kGroupActive = 1, kGroupInactive = 2;
        constexpr uint16_t kStreamUnused = 0, kStreamOff = 1, kStreamPreload = 2, kStreamPlaying = 3, kStreamPaused = 5;
        constexpr uint16_t kPanUnset = 1000;
        constexpr uint32_t kEeCdFileTable = 0x4A9718u; // {char* path; sceCdlFILE file;}[count] (0x28 bytes each)
        constexpr uint32_t kEeCdFileCount = 0x4A9720u;

        constexpr uint8_t kPanVols[256] = {
            32, 32, 32, 32, 32, 32, 32, 32, 33, 33, 33, 33, 33, 33, 34, 34, 34, 34, 35, 35, 35, 36, 36, 36, 37, 37, 38, 38, 39, 39, 40, 40,
            41, 41, 42, 42, 43, 43, 44, 45, 45, 46, 47, 47, 48, 49, 49, 50, 51, 52, 52, 53, 54, 55, 56, 57, 58, 58, 59, 60, 61, 62, 63, 64,
            65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 79, 80, 81, 82, 83, 84, 85, 87, 88, 89, 90, 91, 93, 94, 95, 96, 98, 99, 100,
            101, 103, 104, 105, 106, 108, 109, 110, 112, 113, 114, 116, 117, 118, 120, 121, 122, 124, 125, 126, 128, 129, 130, 132, 133, 135, 136, 137, 139, 140, 141, 143,
            144, 146, 147, 148, 150, 151, 152, 154, 155, 157, 158, 159, 161, 162, 163, 165, 166, 167, 169, 170, 171, 173, 174, 175, 177, 178, 179, 181, 182, 183, 184, 186,
            187, 188, 189, 191, 192, 193, 194, 196, 197, 198, 199, 200, 202, 203, 204, 205, 206, 207, 208, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222,
            223, 224, 225, 226, 227, 228, 229, 229, 230, 231, 232, 233, 234, 235, 235, 236, 237, 238, 238, 239, 240, 240, 241, 242, 242, 243, 244, 244, 245, 245, 246, 246,
            247, 247, 248, 248, 249, 249, 250, 250, 251, 251, 251, 252, 252, 252, 253, 253, 253, 253, 254, 254, 254, 254, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255};

        // ---------------------------------------------------------------- VAG ADPCM
        // Decodes PS ADPCM until an end flag (1 or 7) or the end of data. Returns true if a loop
        // marker (flag 3 = loop end + repeat) was seen.
        bool decodeVag(const uint8_t *data, size_t size, std::vector<int16_t> &out)
        {
            static const int kCoef[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
            int h1 = 0, h2 = 0;
            bool looped = false;
            for (size_t pos = 0; pos + 16 <= size; pos += 16)
            {
                const uint8_t hdr = data[pos];
                const uint8_t flags = data[pos + 1];
                if (flags == 7)
                    break;
                int pred = (hdr >> 4) & 0xF;
                const int shift = hdr & 0xF;
                if (pred > 4)
                    pred = 0;
                for (int i = 0; i < 14; ++i)
                {
                    const uint8_t b = data[pos + 2 + i];
                    for (int n = 0; n < 2; ++n)
                    {
                        int s = n ? (b >> 4) : (b & 0xF);
                        if (s > 7)
                            s -= 16;
                        s = (s << 12) >> shift;
                        int sample = s + ((h1 * kCoef[pred][0] + h2 * kCoef[pred][1]) >> 6);
                        sample = std::clamp(sample, -32768, 32767);
                        out.push_back(static_cast<int16_t>(sample));
                        h2 = h1;
                        h1 = sample;
                    }
                }
                if (flags == 3)
                {
                    looped = true;
                    break;
                }
                if (flags == 1)
                    break;
            }
            return looped;
        }

        bool readFile(const std::filesystem::path &path, std::vector<uint8_t> &out)
        {
            // Streams start inside a sound RPC the game waits on: serve them from memory when preloaded.
            std::error_code error;
            const uint64_t size = std::filesystem::file_size(path, error);
            if (!error && size != 0)
            {
                out.resize(static_cast<size_t>(size));
                if (ps2cache::read(path, 0, out.data(), out.size()))
                    return true;
            }
            std::ifstream f(path, std::ios::binary);
            if (!f)
                return false;
            f.seekg(0, std::ios::end);
            out.resize(static_cast<size_t>(f.tellg()));
            f.seekg(0);
            f.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size()));
            return true;
        }

        std::string upper(std::string s)
        {
            for (char &c : s)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return s;
        }

        struct Sample
        {
            std::vector<int16_t> pcm;
            uint32_t rate = 22050;
            bool loop = false;
        };

        struct Bank
        {
            std::vector<Sample> samples;
        };

        double nowSeconds() { return ps2host::timeSeconds(); }

        struct Voice
        {
            IXAudio2SourceVoice *src = nullptr;
            uint32_t rate = 22050;
            bool started = false;
            bool running = false;
            bool loop = false;
            // No audio device: a silent stand-in that ends after the sample's duration, so the game still sees
            // one-shot sounds and commentary lines finish (it waits on those states).
            bool silent = false;
            double length = 0.0;    // seconds at the original rate
            double remaining = 0.0; // seconds left while stopped (pitch 0)
            double endTime = 0.0;
            const int16_t *pcm = nullptr; // the buffer the voice plays (pool purges by range)
            size_t count = 0;
        };

        bool hasVoice(const Voice &v) { return v.src != nullptr || v.silent; }

        struct Event
        {
            uint16_t group = 0xFFFF;
            uint16_t flags = 0;
            uint16_t status = kEventUnused;
            bool dirty = false;
            uint16_t bank = 0, sample = 0;
            int32_t volume = 0;
            uint16_t pitch = 0x1000;
            uint16_t pan = kPanUnset;
            bool retrigger = false;
            Voice voice;
        };

        struct Group
        {
            uint16_t flags = 0;
            uint16_t status = kGroupUnused;
            bool dirty = false;
            int32_t volRatio = 0x100;
            uint16_t pitchRatio = 0x100;
        };

        struct Stream
        {
            uint16_t state = kStreamUnused;
            uint16_t flags = 0;
            uint32_t rate = 22050;
            float gain = 1.0f;
            std::vector<int16_t> pcm;
            std::future<std::vector<int16_t>> decoding; // STREAM_PLAY reads + decodes off the EE thread
            Voice voice;
            bool paused = false;
        };

        // Destroys the XAudio2 voice now (DestroyVoice waits until the audio thread no longer uses it), so the
        // PCM it played can be freed right after.
        void destroyVoiceNow(Voice &v)
        {
            if (v.src)
            {
                v.src->Stop();
                v.src->FlushSourceBuffers();
                v.src->DestroyVoice();
            }
            v = {};
        }

        // Mono PCM16 source voices, reused per sample rate instead of created and destroyed for every sound.
        // A released voice is stopped and flushed, then waits in `draining` until XAudio2 has dropped its buffers.
        class VoicePool
        {
        public:
            IXAudio2SourceVoice *acquire(IXAudio2 *xa, uint32_t rate)
            {
                auto it = m_free.find(rate);
                if (it != m_free.end() && !it->second.empty())
                {
                    IXAudio2SourceVoice *v = it->second.back();
                    it->second.pop_back();
                    return v;
                }
                WAVEFORMATEX wf{};
                wf.wFormatTag = WAVE_FORMAT_PCM;
                wf.nChannels = 1;
                wf.nSamplesPerSec = rate;
                wf.wBitsPerSample = 16;
                wf.nBlockAlign = 2;
                wf.nAvgBytesPerSec = rate * 2;
                IXAudio2SourceVoice *v = nullptr;
                if (FAILED(xa->CreateSourceVoice(&v, &wf, 0, 8.0f)))
                    return nullptr;
                return v;
            }

            void release(Voice &v)
            {
                if (v.src)
                {
                    v.src->Stop();
                    v.src->FlushSourceBuffers();
                    m_draining.push_back({v.src, v.rate, v.pcm, v.pcm + v.count});
                }
                v = {};
            }

            // Once per batch: drained voices become reusable.
            void service()
            {
                for (size_t i = 0; i < m_draining.size();)
                {
                    XAUDIO2_VOICE_STATE st{};
                    m_draining[i].src->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
                    if (st.BuffersQueued != 0)
                    {
                        ++i;
                        continue;
                    }
                    auto &list = m_free[m_draining[i].rate];
                    if (list.size() < kMaxFreePerRate)
                        list.push_back(m_draining[i].src);
                    else
                        m_draining[i].src->DestroyVoice();
                    m_draining[i] = m_draining.back();
                    m_draining.pop_back();
                }
            }

            // Before PCM in [begin, end) is freed: no draining voice may still read it.
            void purge(const int16_t *begin, const int16_t *end)
            {
                for (size_t i = 0; i < m_draining.size();)
                {
                    if (m_draining[i].begin < end && begin < m_draining[i].end)
                    {
                        m_draining[i].src->DestroyVoice();
                        m_draining[i] = m_draining.back();
                        m_draining.pop_back();
                    }
                    else
                        ++i;
                }
            }

            void clear()
            {
                for (auto &d : m_draining)
                    d.src->DestroyVoice();
                m_draining.clear();
                for (auto &[rate, list] : m_free)
                    for (IXAudio2SourceVoice *v : list)
                        v->DestroyVoice();
                m_free.clear();
            }

        private:
            static constexpr size_t kMaxFreePerRate = 24;
            struct Draining
            {
                IXAudio2SourceVoice *src;
                uint32_t rate;
                const int16_t *begin, *end;
            };
            std::unordered_map<uint32_t, std::vector<IXAudio2SourceVoice *>> m_free;
            std::vector<Draining> m_draining;
        };
    }

    struct NativeSeng::Impl
    {
        std::mutex mutex;
        uint16_t maxEvents = 200, maxStreams = 2, maxBanks = 2;
        std::vector<Event> events;
        std::vector<Group> groups;
        std::vector<Stream> streams;
        std::vector<Bank> banks;
        bool stereo = true;
        std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> cdTableByName; // name -> (lsn,size)
        uint64_t batches = 0;
        VoicePool pool;

        void destroyVoice(Voice &v) { pool.release(v); }

        // The host is about to destroy the XAudio2 engine (device lost): every voice goes. Looping sounds come
        // back on the next batch (their events are still active); one-shots and streams are reported finished.
        void onAudioReset()
        {
            for (Event &e : events)
            {
                if (!hasVoice(e.voice))
                    continue;
                const bool loop = e.voice.loop;
                destroyVoiceNow(e.voice);
                if (!loop && e.status == kEventActive)
                    setEventStatus(e, kEventInactive);
            }
            for (Stream &st : streams)
            {
                if (hasVoice(st.voice))
                {
                    destroyVoiceNow(st.voice);
                    st.state = kStreamOff;
                }
            }
            pool.clear();
        }

        void ensureSizes()
        {
            events.resize(std::max<size_t>(events.size(), maxEvents));
            groups.resize(std::max<size_t>(groups.size(), maxEvents));
            streams.resize(std::max<size_t>(streams.size(), maxStreams));
            banks.resize(std::max<size_t>(banks.size(), maxBanks));
        }

        static std::filesystem::path gameRoot()
        {
            return PS2Runtime::getIoPaths().cdRoot;
        }

        // ------------------------------------------------------------ banks
        void loadBank(uint16_t id, const std::string &dir, const std::string &name)
        {
            if (id >= banks.size())
                banks.resize(id + 1u);
            for (Event &e : events)
                if (e.bank == id)
                    destroyVoice(e.voice);
            Bank &bank = banks[id];
            for (const Sample &smp : bank.samples)
                pool.purge(smp.pcm.data(), smp.pcm.data() + smp.pcm.size());
            bank.samples.clear();

            std::string d = upper(dir);
            for (char &c : d)
                if (c == '/')
                    c = '\\';
            const std::string prefix = "CDDVD_IMAGE\\";
            if (d.rfind(prefix, 0) == 0)
                d = d.substr(prefix.size());
            while (!d.empty() && d.front() == '\\')
                d.erase(d.begin());
            std::filesystem::path base = gameRoot();
            if (!d.empty())
                base /= std::filesystem::path(d);
            std::vector<uint8_t> vbi, vbk;
            const std::string n = upper(name);
            if (!readFile(base / (n + ".VBI"), vbi) || !readFile(base / (n + ".VBK"), vbk))
            {
                // Fall back to a search by basename (what the IOP effectively does).
                bool found = false;
                for (const auto &entry : std::filesystem::recursive_directory_iterator(gameRoot() / "AUDIO"))
                {
                    if (upper(entry.path().filename().string()) == n + ".VBI")
                    {
                        found = readFile(entry.path(), vbi) &&
                                readFile(entry.path().parent_path() / (n + ".VBK"), vbk);
                        break;
                    }
                }
                if (!found)
                {
                    std::fprintf(stderr, "[seng] bank %u '%s\\%s' not found\n", id, dir.c_str(), name.c_str());
                    return;
                }
            }
            const size_t count = vbi.size() / 16u;
            bank.samples.resize(count);
            for (size_t i = 0; i < count; ++i)
            {
                int32_t off;
                float rate;
                uint32_t flags;
                std::memcpy(&off, vbi.data() + i * 16u, 4);
                std::memcpy(&rate, vbi.data() + i * 16u + 4u, 4);
                std::memcpy(&flags, vbi.data() + i * 16u + 12u, 4);
                size_t end = vbk.size();
                if (i + 1 < count)
                {
                    int32_t next;
                    std::memcpy(&next, vbi.data() + (i + 1) * 16u, 4);
                    end = std::min<size_t>(end, static_cast<size_t>(std::max(next, off)));
                }
                Sample &s = bank.samples[i];
                s.rate = rate > 1000.0f ? static_cast<uint32_t>(rate) : 22050u;
                if (off >= 0 && static_cast<size_t>(off) < vbk.size())
                    s.loop = decodeVag(vbk.data() + off, end - static_cast<size_t>(off), s.pcm) || (flags & 1u);
            }
            std::printf("[seng] loaded bank %u %s (%zu samples)\n", id, n.c_str(), count);
            if (id == 1u) // arena bank: PS2X_GS_TRACE_BANK1 / PS2X_RAM_DUMP_BANK1 count from here (match-start anchor)
                g_ps2xBank1LoadedAt.store(ps2host::timeSeconds());
        }

        // ------------------------------------------------------------ voices
        bool createVoice(Voice &v, const int16_t *pcm, size_t count, uint32_t rate, bool loop)
        {
            if (count == 0)
                return false;
            v.rate = rate;
            v.loop = loop;
            v.pcm = pcm;
            v.count = count;
            v.length = static_cast<double>(count) / static_cast<double>(rate);
            IXAudio2 *xa = ps2host::xaudio();
            if (!xa)
            {
                v.silent = true;
                return true;
            }
            v.src = pool.acquire(xa, rate);
            if (!v.src)
            {
                v.silent = true;
                return true;
            }
            XAUDIO2_BUFFER buf{};
            buf.AudioBytes = static_cast<UINT32>(count * 2u);
            buf.pAudioData = reinterpret_cast<const BYTE *>(pcm);
            buf.Flags = XAUDIO2_END_OF_STREAM;
            if (loop)
                buf.LoopCount = XAUDIO2_LOOP_INFINITE;
            v.src->SubmitSourceBuffer(&buf);
            return true;
        }

        static bool voiceFinished(const Voice &v)
        {
            if (v.silent)
                return v.started && !v.loop && v.running && nowSeconds() >= v.endTime;
            if (!v.src || !v.started || v.loop)
                return false;
            XAUDIO2_VOICE_STATE st{};
            v.src->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            return st.BuffersQueued == 0;
        }

        void applyMix(Voice &v, float left, float right, uint32_t pitch)
        {
            if (v.silent)
            {
                if (pitch == 0u)
                {
                    if (v.running)
                        v.remaining = std::max(0.0, v.endTime - nowSeconds());
                    v.running = false;
                    return;
                }
                if (!v.running)
                {
                    const double ratio = std::clamp(static_cast<double>(pitch) * 48000.0 / 4096.0 / v.rate, 1.0 / 1024.0, 8.0);
                    v.endTime = nowSeconds() + (v.started ? v.remaining : v.length / ratio);
                    v.running = true;
                    v.started = true;
                }
                return;
            }
            if (!v.src)
                return;
            const float matrix[2] = {std::clamp(left, 0.0f, 1.0f), std::clamp(right, 0.0f, 1.0f)};
            v.src->SetOutputMatrix(ps2host::masteringVoice(), 1, 2, matrix);
            if (pitch == 0u)
            {
                if (v.running)
                {
                    v.src->Stop();
                    v.running = false;
                }
                return;
            }
            const float hz = static_cast<float>(pitch) * 48000.0f / 4096.0f;
            v.src->SetFrequencyRatio(std::clamp(hz / static_cast<float>(v.rate), 1.0f / 1024.0f, 8.0f));
            if (!v.running)
            {
                v.src->Start();
                v.running = true;
                v.started = true;
            }
        }

        void setEventStatus(Event &e, uint16_t status)
        {
            e.status = status;
            e.dirty = true;
        }

        void setGroupStatus(Group &g, uint16_t status)
        {
            g.status = status;
            g.dirty = true;
        }

        // ------------------------------------------------------------ streams
        bool resolveStreamFile(uint8_t *rdram, uint32_t lsn, uint32_t size, std::filesystem::path &out)
        {
            // Walk the EE-side pcpl_cddvd table: {char *path; u32 lsn; u32 size; ...} * count.
            uint32_t tablePtr = 0, count = 0;
            std::memcpy(&tablePtr, rdram + kEeCdFileTable, 4);
            std::memcpy(&count, rdram + kEeCdFileCount, 4);
            tablePtr &= 0x01FFFFFFu;
            for (uint32_t i = 0; i < count && i < 4096u && tablePtr + (i + 1) * 0x28u < PS2_RAM_SIZE; ++i)
            {
                const uint8_t *entry = rdram + tablePtr + i * 0x28u;
                uint32_t pathPtr, eLsn, eSize;
                std::memcpy(&pathPtr, entry, 4);
                std::memcpy(&eLsn, entry + 4, 4);
                std::memcpy(&eSize, entry + 8, 4);
                if (eLsn != lsn)
                    continue;
                pathPtr &= 0x01FFFFFFu;
                std::string p;
                for (uint32_t k = 0; k < 256 && pathPtr + k < PS2_RAM_SIZE; ++k)
                {
                    const char c = static_cast<char>(rdram[pathPtr + k]);
                    if (!c)
                        break;
                    p.push_back(c);
                }
                const size_t semi = p.find(';');
                if (semi != std::string::npos)
                    p.resize(semi);
                while (!p.empty() && (p.front() == '\\' || p.front() == '/'))
                    p.erase(p.begin());
                for (char &c : p)
                    if (c == '\\')
                        c = '/';
                out = gameRoot() / std::filesystem::path(p);
                (void)size;
                (void)eSize;
                return true;
            }
            return false;
        }

        void streamPlay(uint8_t *rdram, uint16_t id, int32_t rate, uint32_t lsn, uint32_t size)
        {
            if (id >= streams.size())
                return;
            Stream &s = streams[id];
            if (s.decoding.valid())
                s.decoding.wait(); // a line still decoding is replaced: let it finish, then drop it
            destroyVoiceNow(s.voice); // its PCM is replaced below
            s.pcm.clear();
            s.rate = rate > 0 ? static_cast<uint32_t>(rate) : 22050u;
            s.paused = (s.flags & 0x08u) != 0u;
            std::filesystem::path file;
            if (!resolveStreamFile(rdram, lsn, size, file))
            {
                s.state = kStreamOff;
                return;
            }
            // The whole line is read and decoded on a worker (a commentary file is up to a few hundred KB of
            // ADPCM); the stream reports PRELOAD until endBatch finds it ready, as the IOP's preload did.
            s.state = kStreamPreload;
            s.decoding = std::async(std::launch::async, [file, size]()
                                    {
                                        std::vector<int16_t> pcm;
                                        std::vector<uint8_t> data;
                                        if (readFile(file, data))
                                        {
                                            if (size && size < data.size())
                                                data.resize(size);
                                            decodeVag(data.data(), data.size(), pcm);
                                        }
                                        return pcm;
                                    });
        }

        // ------------------------------------------------------------ END_BATCH
        void endBatch(uint8_t *recv, uint32_t recvSize)
        {
            pool.service();
            // 1) natural ends of one-shot voices
            for (Event &e : events)
            {
                if (hasVoice(e.voice) && !(e.flags & 1u) && voiceFinished(e.voice))
                {
                    destroyVoice(e.voice);
                    if (e.status == kEventActive)
                        setEventStatus(e, kEventInactive);
                }
            }
            // 2) start / stop voices
            for (size_t id = 0; id < events.size(); ++id)
            {
                Event &e = events[id];
                const bool groupActive = e.group < groups.size() && groups[e.group].status == kGroupActive;
                if (e.status == kEventActive && groupActive)
                {
                    if (e.retrigger && hasVoice(e.voice))
                        destroyVoice(e.voice);
                    e.retrigger = false;
                    if (!hasVoice(e.voice) && e.bank < banks.size() && e.sample < banks[e.bank].samples.size())
                    {
                        const Sample &smp = banks[e.bank].samples[e.sample];
                        createVoice(e.voice, smp.pcm.data(), smp.pcm.size(), smp.rate, (e.flags & 1u) && smp.loop);
                    }
                }
                else if (hasVoice(e.voice))
                {
                    destroyVoice(e.voice);
                }
                // 3) mix
                if (hasVoice(e.voice))
                {
                    const Group &g = groups[e.group];
                    float l, r;
                    if (e.pan == kPanUnset)
                    {
                        l = r = static_cast<float>((g.volRatio * e.volume) >> 8) / 16383.0f;
                    }
                    else
                    {
                        const int pan = std::clamp<int>(e.pan, 0, 255);
                        l = static_cast<float>((kPanVols[std::min(255, 256 - pan)] * e.volume) >> 8) / 16383.0f;
                        r = static_cast<float>((kPanVols[pan] * e.volume) >> 8) / 16383.0f;
                    }
                    if (e.flags & 4u)
                        l = 0.0f;
                    if (e.flags & 8u)
                        r = 0.0f;
                    if (!stereo)
                        l = r = (l + r) * 0.5f;
                    const float gain = ps2host::effectsGain();
                    l *= gain;
                    r *= gain;
                    applyMix(e.voice, l, r, (static_cast<uint32_t>(g.pitchRatio) * e.pitch) >> 8);
                }
            }
            // 4) streams
            for (Stream &s : streams)
            {
                if (s.decoding.valid() && s.decoding.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                {
                    s.pcm = s.decoding.get();
                    if (s.state != kStreamPreload) // stopped while it decoded
                        s.pcm.clear();
                    else if (s.pcm.empty() || !createVoice(s.voice, s.pcm.data(), s.pcm.size(), s.rate, false))
                        s.state = kStreamOff;
                }
                if (!hasVoice(s.voice))
                    continue;
                if (s.voice.started && voiceFinished(s.voice))
                {
                    destroyVoiceNow(s.voice);
                    s.state = kStreamOff;
                    continue;
                }
                const uint32_t pitch = s.paused ? 0u : (s.rate * 4096u) / 48000u;
                const float gain = s.gain * ps2host::commentaryGain();
                applyMix(s.voice, gain, gain, pitch);
                if (s.voice.running && s.state == kStreamPreload)
                    s.state = kStreamPlaying;
            }

            // 5) status reply
            if (!recv)
                return;
            std::vector<uint16_t> out;
            out.reserve(recvSize / 2u);
            for (uint16_t i = 0; i < maxStreams; ++i)
                out.push_back(i < streams.size() ? streams[i].state : kStreamUnused);
            for (size_t i = 0; i < groups.size(); ++i)
                if (groups[i].dirty)
                {
                    out.push_back(static_cast<uint16_t>(i));
                    out.push_back(groups[i].status);
                    groups[i].dirty = false;
                }
            out.push_back(0xFFFFu);
            for (size_t i = 0; i < events.size(); ++i)
                if (events[i].dirty)
                {
                    out.push_back(static_cast<uint16_t>(i));
                    out.push_back(events[i].status);
                    events[i].dirty = false;
                }
            out.push_back(0xFFFFu);
            const size_t bytes = std::min<size_t>(out.size() * 2u, std::min<uint32_t>(recvSize, 0x800u));
            std::memcpy(recv, out.data(), bytes);
        }

        // ------------------------------------------------------------ command stream
        void process(uint8_t *rdram, const uint8_t *cmd, uint32_t size, uint8_t *recv, uint32_t recvSize)
        {
            auto u16 = [&](uint32_t off) -> uint16_t
            {
                uint16_t v = 0;
                if (off + 2u <= size)
                    std::memcpy(&v, cmd + off, 2);
                return v;
            };
            auto u32 = [&](uint32_t off) -> uint32_t
            {
                uint32_t v = 0;
                if (off + 4u <= size)
                    std::memcpy(&v, cmd + off, 4);
                return v;
            };
            ensureSizes();
            uint32_t p = 0;
            while (p + 2u <= size)
            {
                const uint16_t op = u16(p);
                auto ev = [&](uint16_t id) -> Event * { return id < events.size() ? &events[id] : nullptr; };
                auto gr = [&](uint16_t id) -> Group * { return id < groups.size() ? &groups[id] : nullptr; };
                switch (op)
                {
                case 0x00: // BANK_LOAD
                {
                    const uint16_t bankId = u16(p + 2);
                    uint32_t q = p + 6;
                    std::string dir, name;
                    while (q < size && cmd[q])
                        dir.push_back(static_cast<char>(cmd[q++]));
                    ++q;
                    while (q < size && cmd[q])
                        name.push_back(static_cast<char>(cmd[q++]));
                    ++q;
                    loadBank(bankId, dir, name);
                    p = (q + 3u) & ~3u;
                    continue;
                }
                case 0x01: p += 4; continue; // BANK_UNLOAD (no-op on the IOP too)
                case 0x02: // EVENT_CREATE
                    if (Event *e = ev(u16(p + 2)))
                    {
                        destroyVoice(e->voice);
                        *e = Event{};
                        e->group = u16(p + 4);
                        setEventStatus(*e, kEventInactive);
                    }
                    p += 8;
                    continue;
                case 0x03: // EVENT_DESTROY
                    if (Event *e = ev(u16(p + 2)))
                    {
                        destroyVoice(e->voice);
                        setEventStatus(*e, kEventUnused);
                    }
                    p += 4;
                    continue;
                case 0x04: // EVENT_START
                    if (Event *e = ev(u16(p + 2)))
                    {
                        e->retrigger = true;
                        setEventStatus(*e, kEventActive);
                        if (Group *g = gr(e->group))
                            setGroupStatus(*g, kGroupActive);
                    }
                    p += 4;
                    continue;
                case 0x05: // EVENT_STOP
                    if (Event *e = ev(u16(p + 2)))
                        setEventStatus(*e, kEventInactive);
                    p += 4;
                    continue;
                case 0x06: if (Event *e = ev(u16(p + 2))) e->volume = u16(p + 4); p += 8; continue;
                case 0x07: if (Event *e = ev(u16(p + 2))) e->pan = u16(p + 4); p += 8; continue;
                case 0x08: if (Event *e = ev(u16(p + 2))) e->pitch = u16(p + 4); p += 8; continue;
                case 0x09: // EVENT_SET_SAMPLE
                    if (Event *e = ev(u16(p + 2)))
                    {
                        e->bank = u16(p + 4);
                        e->sample = u16(p + 6);
                        e->flags = u16(p + 8);
                    }
                    p += 12;
                    continue;
                case 0x0A: // GROUP_CREATE
                    if (Group *g = gr(u16(p + 2)))
                    {
                        *g = Group{};
                        g->flags = u16(p + 4);
                        setGroupStatus(*g, kGroupInactive);
                    }
                    p += 8;
                    continue;
                case 0x0B: // GROUP_DESTROY
                {
                    const uint16_t gid = u16(p + 2);
                    if (Group *g = gr(gid))
                    {
                        for (Event &e : events)
                            if (e.group == gid && e.status != kEventUnused)
                            {
                                destroyVoice(e.voice);
                                setEventStatus(e, kEventUnused);
                            }
                        setGroupStatus(*g, kGroupUnused);
                    }
                    p += 4;
                    continue;
                }
                case 0x0C: // GROUP_START
                case 0x0D: // GROUP_STOP
                {
                    const uint16_t gid = u16(p + 2);
                    if (Group *g = gr(gid))
                    {
                        const bool start = op == 0x0C;
                        for (Event &e : events)
                            if (e.group == gid && e.status != kEventUnused)
                                setEventStatus(e, start ? kEventActive : kEventInactive);
                        setGroupStatus(*g, start ? kGroupActive : kGroupInactive);
                    }
                    p += 4;
                    continue;
                }
                case 0x0E: if (Group *g = gr(u16(p + 2))) g->pitchRatio = 0; p += 4; continue;      // GROUP_PAUSE
                case 0x0F: if (Group *g = gr(u16(p + 2))) g->pitchRatio = 0x100; p += 4; continue;  // GROUP_RESUME
                case 0x10: if (Group *g = gr(u16(p + 2))) g->flags = u16(p + 4); p += 8; continue;
                case 0x11: p += 16; continue;
                case 0x12: if (Group *g = gr(u16(p + 2))) g->volRatio = u16(p + 4); p += 8; continue;
                case 0x13: p += 8; continue;
                case 0x14: p += 24; continue; // LISTENER_UPDATE (3D path unused by this game)
                case 0x15: case 0x16: p += 4; continue;
                case 0x17: // STREAM_CREATE
                {
                    const uint16_t sid = u16(p + 2);
                    if (sid >= streams.size())
                        streams.resize(sid + 1u);
                    destroyVoiceNow(streams[sid].voice); // the Stream (and its PCM) is replaced
                    streams[sid] = Stream{};
                    streams[sid].flags = u16(p + 8);
                    streams[sid].state = kStreamOff;
                    p += 12;
                    continue;
                }
                case 0x18: streamPlay(rdram, u16(p + 2), static_cast<int32_t>(u32(p + 4)), u32(p + 8), u32(p + 12)); p += 16; continue;
                case 0x19: // STREAM_STOP
                    if (u16(p + 2) < streams.size())
                    {
                        Stream &s = streams[u16(p + 2)];
                        destroyVoiceNow(s.voice); // streamPlay frees its PCM later
                        s.state = kStreamOff;
                    }
                    p += 8;
                    continue;
                case 0x1A: if (u16(p + 2) < streams.size()) streams[u16(p + 2)].paused = true; p += 4; continue;
                case 0x1B: if (u16(p + 2) < streams.size()) streams[u16(p + 2)].paused = false; p += 4; continue;
                case 0x1C: p += 8 + 8u * u16(p + 2); continue;
                case 0x1D: // STREAM_SET_VOLUME
                    if (u16(p + 2) < streams.size())
                        streams[u16(p + 2)].gain = (static_cast<float>(u16(p + 4)) + static_cast<float>(u16(p + 6))) * 0.5f / 16383.0f;
                    p += 8;
                    continue;
                case 0x1E: // INIT
                    maxEvents = std::max<uint16_t>(1, u16(p + 2));
                    maxBanks = std::max<uint16_t>(1, u16(p + 8));
                    maxStreams = std::max<uint16_t>(1, u16(p + 10));
                    ensureSizes();
                    p += 20;
                    continue;
                case 0x1F: // INIT_IOP_FS
                {
                    const uint16_t fstype = u16(p + 2);
                    if (fstype == 1)
                    {
                        const uint32_t n = u32(p + 4);
                        p += 8 + n * 36u;
                    }
                    else
                        p += 4;
                    continue;
                }
                case 0x20: // RESET
                    for (Event &e : events)
                        destroyVoice(e.voice);
                    for (Stream &s : streams)
                        destroyVoiceNow(s.voice);
                    events.assign(maxEvents, Event{});
                    groups.assign(maxEvents, Group{});
                    for (Stream &s : streams)
                        s.state = kStreamOff;
                    p += 4;
                    continue;
                case 0x21:
                {
                    p += 4;
                    while (p + 4u <= size && static_cast<int32_t>(u32(p)) != -1)
                        p += 4;
                    p += 4;
                    continue;
                }
                case 0x22: case 0x23: case 0x24: p += 4; continue;
                case 0x25: stereo = u16(p + 2) != 0; p += 4; continue;
                case 0x26: p += 24; continue;
                case 0x27: // END_BATCH
                    endBatch(recv, recvSize);
                    ++batches;
                    return;
                default:
                    std::fprintf(stderr, "[seng] unknown opcode 0x%x at %u, dropping batch\n", op, p);
                    endBatch(recv, recvSize);
                    return;
                }
            }
            endBatch(recv, recvSize);
        }
    };

    NativeSeng::NativeSeng() : m_impl(std::make_unique<Impl>())
    {
        ps2host::addAudioResetListener([impl = m_impl.get()]()
                                       {
                                           std::lock_guard lock(impl->mutex);
                                           impl->onAudioReset();
                                       });
    }
    NativeSeng::~NativeSeng() = default;

    bool NativeSeng::enabled()
    {
        // Also without an audio device: the engine then runs silent (timed stand-in voices), which keeps the
        // game's sound state machine going and the IOP interpreter out of the picture.
        static const bool s_enabled = []() {
            const char *v = std::getenv("PS2X_IOP_SENG");
            return !(v && *v == '1'); // PS2X_IOP_SENG=1 falls back to running SENG.IRX on the IOP interpreter
        }();
        return s_enabled;
    }

    void NativeSeng::handleBatch(uint8_t *rdram, uint32_t sendAddr, uint32_t sendSize, uint32_t recvAddr, uint32_t recvSize)
    {
        // Host audio lock first (engine rebuilds take it, then this engine's mutex via the reset listener).
        std::scoped_lock lock(ps2host::audioMutex(), m_impl->mutex);
        const uint32_t s = sendAddr & 0x01FFFFFFu;
        const uint32_t r = recvAddr & 0x01FFFFFFu;
        if (s + sendSize > PS2_RAM_SIZE)
            return;
        uint8_t *recv = (recvAddr && r + recvSize <= PS2_RAM_SIZE) ? rdram + r : nullptr;
        m_impl->process(rdram, rdram + s, sendSize, recv, recvSize);
    }

    void NativeSeng::reset()
    {
        std::scoped_lock lock(ps2host::audioMutex(), m_impl->mutex);
        for (Event &e : m_impl->events)
            destroyVoiceNow(e.voice);
        for (Stream &st : m_impl->streams)
            destroyVoiceNow(st.voice);
        m_impl->pool.clear();
        m_impl->events.clear();
        m_impl->groups.clear();
        m_impl->streams.clear();
        m_impl->banks.clear();
    }

    NativeSeng &nativeSeng()
    {
        static NativeSeng s_instance;
        return s_instance;
    }
}
