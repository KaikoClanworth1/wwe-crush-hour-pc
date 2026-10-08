p = r'D:\Xbox Games Ports\WWE Crush Hour\PS2Recomp\ps2xRuntime\src\lib\Kernel\Stubs\MPEG.cpp'
s = open(p, encoding='utf-8').read()

helper = r'''
#if defined(PS2X_NATIVE_HOST)
        // ---- Native PSS movie audio (XAudio2) -------------------------------------------------
        // PSS audio lives in private_stream_1 PES packets: a 4-byte sub-stream header, then (first
        // packet) 'SShd' {size, type 1=PCM16, rate, channels, interleave} + 'SSbd' {size}, then
        // little-endian PCM16 interleaved in `interleave`-byte blocks per channel. Played directly on
        // the host instead of through the game's IOP audio path (libsdr / sceSdRemote).
        struct PssAudioStream
        {
            bool headerParsed = false;
            bool pcm = false;
            uint32_t rate = 48000;
            uint32_t channels = 2;
            uint32_t interleave = 512;
            std::vector<uint8_t> pending;
            IXAudio2SourceVoice *voice = nullptr;
            std::deque<std::vector<int16_t>> queued;
            bool started = false;
        };
        std::mutex g_pssAudioMutex;
        std::unordered_map<uint32_t, PssAudioStream> g_pssAudio;

        void destroyPssAudioLocked(PssAudioStream &a)
        {
            if (a.voice)
            {
                a.voice->Stop();
                a.voice->FlushSourceBuffers();
                a.voice->DestroyVoice();
                a.voice = nullptr;
            }
            a.queued.clear();
        }

        void stopPssAudio(uint32_t mpegAddr)
        {
            std::lock_guard<std::mutex> lock(g_pssAudioMutex);
            auto it = g_pssAudio.find(mpegAddr);
            if (it != g_pssAudio.end())
            {
                destroyPssAudioLocked(it->second);
                g_pssAudio.erase(it);
            }
        }

        void stopAllPssAudio()
        {
            std::lock_guard<std::mutex> lock(g_pssAudioMutex);
            for (auto &[addr, a] : g_pssAudio)
                destroyPssAudioLocked(a);
            g_pssAudio.clear();
        }

        void feedPssAudio(uint32_t mpegAddr, const uint8_t *payload, size_t size)
        {
            if (!ps2host::audioReady() || size <= 4u)
                return;
            std::lock_guard<std::mutex> lock(g_pssAudioMutex);
            PssAudioStream &a = g_pssAudio[mpegAddr];
            a.pending.insert(a.pending.end(), payload + 4, payload + size);
            if (!a.headerParsed)
            {
                if (a.pending.size() < 32u)
                    return;
                if (std::memcmp(a.pending.data(), "SShd", 4) != 0)
                {
                    a.pending.clear();
                    return;
                }
                uint32_t type = 0, rate = 0, ch = 0, inter = 0;
                std::memcpy(&type, a.pending.data() + 8, 4);
                std::memcpy(&rate, a.pending.data() + 12, 4);
                std::memcpy(&ch, a.pending.data() + 16, 4);
                std::memcpy(&inter, a.pending.data() + 20, 4);
                a.pcm = (type == 1u);
                a.rate = rate ? rate : 48000u;
                a.channels = std::clamp<uint32_t>(ch, 1u, 2u);
                a.interleave = inter ? inter : 512u;
                a.pending.erase(a.pending.begin(), a.pending.begin() + 32); // SShd(24) + SSbd(8)
                a.headerParsed = true;
                if (!a.pcm)
                    return; // ADPCM movie audio: not used by this game
                WAVEFORMATEX wf{};
                wf.wFormatTag = WAVE_FORMAT_PCM;
                wf.nChannels = static_cast<WORD>(a.channels);
                wf.nSamplesPerSec = a.rate;
                wf.wBitsPerSample = 16;
                wf.nBlockAlign = static_cast<WORD>(2u * a.channels);
                wf.nAvgBytesPerSec = a.rate * wf.nBlockAlign;
                if (FAILED(ps2host::xaudio()->CreateSourceVoice(&a.voice, &wf)))
                    a.voice = nullptr;
            }
            if (!a.pcm || !a.voice)
                return;
            const size_t block = static_cast<size_t>(a.interleave) * a.channels;
            while (a.pending.size() >= block)
            {
                const size_t frames = a.interleave / 2u;
                std::vector<int16_t> out(frames * a.channels);
                for (uint32_t c = 0; c < a.channels; ++c)
                {
                    const uint8_t *src = a.pending.data() + static_cast<size_t>(c) * a.interleave;
                    for (size_t f = 0; f < frames; ++f)
                    {
                        int16_t v;
                        std::memcpy(&v, src + f * 2u, 2);
                        out[f * a.channels + c] = v;
                    }
                }
                a.pending.erase(a.pending.begin(), a.pending.begin() + static_cast<std::ptrdiff_t>(block));
                a.queued.push_back(std::move(out));
                XAUDIO2_BUFFER buf{};
                buf.AudioBytes = static_cast<UINT32>(a.queued.back().size() * 2u);
                buf.pAudioData = reinterpret_cast<const BYTE *>(a.queued.back().data());
                a.voice->SubmitSourceBuffer(&buf);
            }
            XAUDIO2_VOICE_STATE st{};
            a.voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            while (a.queued.size() > st.BuffersQueued)
                a.queued.pop_front();
            if (!a.started && st.BuffersQueued > 0)
            {
                a.voice->Start();
                a.started = true;
            }
        }
#endif
'''

anchor = '        void finishPlaybackStream(uint32_t mpegAddr, MpegPlaybackState &playback)'
assert anchor in s
# helper must be defined before processPssBuffer uses it: insert before the function containing the audio branch.
proc_anchor = s.rfind('\n        void ', 0, s.index('else if (isAudioStreamId(streamId))'))
s = s[:proc_anchor] + '\n' + helper + s[proc_anchor:]

old = '''                else if (isAudioStreamId(streamId))
                {
                    const MpegPesHeader pes = parsePesHeader(buffer.data(), packetEnd);
                    const size_t payloadStart = pes.payloadOffset;
                    if (payloadStart < packetEnd && payloadStart < playback.pssGuestAddrs.size())
                    {'''
new = '''                else if (isAudioStreamId(streamId))
                {
                    const MpegPesHeader pes = parsePesHeader(buffer.data(), packetEnd);
                    const size_t payloadStart = pes.payloadOffset;
#if defined(PS2X_NATIVE_HOST)
                    if (payloadStart < packetEnd)
                        feedPssAudio(mpegAddr, buffer.data() + payloadStart, packetEnd - payloadStart);
#endif
                    if (payloadStart < packetEnd && payloadStart < playback.pssGuestAddrs.size())
                    {'''
assert old in s
s = s.replace(old, new, 1)

# Stop audio when a movie is deleted / reset / a new stream starts.
for fn in ['    void sceMpegDelete(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)\n    {\n',
           '    void sceMpegReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)\n    {\n']:
    assert fn in s, fn
    s = s.replace(fn, fn + '#if defined(PS2X_NATIVE_HOST)\n        stopPssAudio(getRegU32(ctx, 4));\n#endif\n', 1)
fn = '    void resetMpegStubState()\n    {\n'
assert fn in s
s = s.replace(fn, fn + '#if defined(PS2X_NATIVE_HOST)\n        stopAllPssAudio();\n#endif\n', 1)

inc = '#if defined(PS2X_NATIVE_HOST)\n#include "host/win32_host.h"\n#include <xaudio2.h>\n#include <deque>\n#include <unordered_map>\n#endif\n'
i = s.index('#include')
j = s.index('\n', i) + 1
s = s[:j] + inc + s[j:]
open(p, 'w', encoding='utf-8').write(s)
print('ok')
