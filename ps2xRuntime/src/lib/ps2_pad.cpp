#include "runtime/ps2_pad.h"
#include "host/win32_host.h"
#include <algorithm>
#include <cstring>

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
}

// Reads a DualShock 2 report for a PS2 (port, slot) from the native host input layer
// (XInput pads + keyboard). Port 0/1 = players 1/2; multitap slots map to players 2..4.
bool PSPadBackend::readState(int port, int slot, uint8_t *data, size_t size)
{
    if (!data || size < 32 || !ps2host::isReady())
        return false;

    const int index = (slot > 0) ? std::min(slot + (port == 0 ? 0 : 1), ps2host::kMaxPads - 1) : port;
    const ps2host::PadState pad = ps2host::pad(index);
    if (!pad.connected)
        return false;

    std::memset(data, 0, 32);
    data[0] = 0x00;
    data[1] = kPadAnalogMarker;
    const uint16_t btns = static_cast<uint16_t>(~pad.buttons);
    data[2] = static_cast<uint8_t>(btns & 0xFFu);
    data[3] = static_cast<uint8_t>(btns >> 8);
    data[4] = pad.rx;
    data[5] = pad.ry;
    data[6] = pad.lx;
    data[7] = pad.ly;
    return true;
}
