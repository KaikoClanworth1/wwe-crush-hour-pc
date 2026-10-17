#pragma once

// Game controller backend of the native Windows host (host/gamepad_input.cpp): SDL3 when built with it, else
// XInput. Fills ps2host::PadState for player slots 0..kMaxPads-1.

#include "host/win32_host.h"

namespace ps2host::input
{
    // deadzone: radial stick deadzone as a fraction of full deflection ([Controls] Deadzone, percent / 100).
    void init(float deadzone);
    void shutdown();
    // Adds the controllers' state to pads (cleared by the caller); `now` = host time in seconds.
    void poll(PadState pads[kMaxPads], double now);
    void rumble(int index, uint8_t smallMotor, uint8_t largeMotor);
}
