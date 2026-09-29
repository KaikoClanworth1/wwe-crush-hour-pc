#pragma once

// Native (host) replacement for WWE Crush Hour's SENG.IRX sound engine (SIF RPC SID 0x1234).
// The EE-side SoundDevice sends a u16 command stream once per frame; instead of running the IRX
// on an IOP interpreter we decode the command stream here and play the game's VBK sample banks
// and COMM_JR commentary streams through XAudio2. Protocol: docs/seng_protocol.md.

#include <cstdint>
#include <filesystem>
#include <memory>

class PS2Runtime;

namespace ps2native
{
    constexpr uint32_t kSengSid = 0x1234u;

    class NativeSeng
    {
    public:
        NativeSeng();
        ~NativeSeng();

        static bool enabled();

        // Processes one command batch (EE send buffer) and writes the status reply.
        void handleBatch(uint8_t *rdram, uint32_t sendAddr, uint32_t sendSize, uint32_t recvAddr, uint32_t recvSize);
        void reset();

        struct Impl;

    private:
        std::unique_ptr<Impl> m_impl;
    };

    NativeSeng &nativeSeng();
}
