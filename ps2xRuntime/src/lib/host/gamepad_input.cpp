// Game controllers for the native Windows host. With SDL3 (PS2X_HAS_SDL3) every controller SDL knows works: Xbox,
// DualShock 4 / DualSense, Switch Pro, Steam Deck and generic HID pads, hot-plugged, mapped by position to the
// DualShock 2 (south = Cross, east = Circle, west = Square, north = Triangle). Without SDL3, XInput pads only.
// Both feed the same PadState: radial stick deadzone, analog triggers as L2 / R2 pressure, the guide button
// opens the mod menu.

#include "host/gamepad_input.h"
#include "host/input_mapping.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <Xinput.h>

#if PS2X_HAS_SDL3
#include <SDL3/SDL.h>
#endif

#include <cstdio>

namespace ps2host::input
{
    namespace
    {
        float g_deadzone = 0.15f;

        void fillButtons(PadState &p, bool up, bool down, bool left, bool right, bool south, bool east, bool west,
                         bool north, bool l1, bool r1, bool l3, bool r3, bool start, bool select)
        {
            const struct
            {
                bool on;
                uint16_t bit;
            } map[] = {{up, kBtnUp}, {down, kBtnDown}, {left, kBtnLeft}, {right, kBtnRight},
                       {south, kBtnCross}, {east, kBtnCircle}, {west, kBtnSquare}, {north, kBtnTriangle},
                       {l1, kBtnL1}, {r1, kBtnR1}, {l3, kBtnL3}, {r3, kBtnR3}, {start, kBtnStart}, {select, kBtnSelect}};
            for (const auto &m : map)
                if (m.on)
                    p.buttons |= m.bit;
        }

        void fillAnalog(PadState &p, int lx, int ly, int rx, int ry, int lt, int rt)
        {
            const StickBytes left = radialStick(lx, ly, g_deadzone);
            const StickBytes right = radialStick(rx, ry, g_deadzone);
            p.lx = left.x;
            p.ly = left.y;
            p.rx = right.x;
            p.ry = right.y;
            p.l2 = triggerPressure(lt);
            p.r2 = triggerPressure(rt);
            if (p.l2 > kTriggerPressedAt)
                p.buttons |= kBtnL2;
            if (p.r2 > kTriggerPressedAt)
                p.buttons |= kBtnR2;
        }

        // ---- XInput -----------------------------------------------------------------------------------------
        // XInputGetState on an empty slot is slow (it probes the device), so empty slots are re-checked only
        // every second.
        double g_xinputRetryAt[kMaxPads] = {};

        void pollXInput(PadState pads[kMaxPads], double now)
        {
            for (int i = 0; i < kMaxPads; ++i)
            {
                if (now < g_xinputRetryAt[i])
                    continue;
                XINPUT_STATE xs{};
                if (XInputGetState(static_cast<DWORD>(i), &xs) != ERROR_SUCCESS)
                {
                    g_xinputRetryAt[i] = now + 1.0;
                    continue;
                }
                PadState &p = pads[i];
                p.connected = true;
                const WORD b = xs.Gamepad.wButtons;
                fillButtons(p, b & XINPUT_GAMEPAD_DPAD_UP, b & XINPUT_GAMEPAD_DPAD_DOWN, b & XINPUT_GAMEPAD_DPAD_LEFT,
                            b & XINPUT_GAMEPAD_DPAD_RIGHT, b & XINPUT_GAMEPAD_A, b & XINPUT_GAMEPAD_B, b & XINPUT_GAMEPAD_X,
                            b & XINPUT_GAMEPAD_Y, b & XINPUT_GAMEPAD_LEFT_SHOULDER, b & XINPUT_GAMEPAD_RIGHT_SHOULDER,
                            b & XINPUT_GAMEPAD_LEFT_THUMB, b & XINPUT_GAMEPAD_RIGHT_THUMB, b & XINPUT_GAMEPAD_START,
                            b & XINPUT_GAMEPAD_BACK);
                // XInput's y axes point up; the PS2's point down.
                fillAnalog(p, xs.Gamepad.sThumbLX, -static_cast<int>(xs.Gamepad.sThumbLY), xs.Gamepad.sThumbRX,
                           -static_cast<int>(xs.Gamepad.sThumbRY), xs.Gamepad.bLeftTrigger * 32767 / 255,
                           xs.Gamepad.bRightTrigger * 32767 / 255);
            }
        }

#if PS2X_HAS_SDL3
        // ---- SDL3 -------------------------------------------------------------------------------------------
        bool g_sdl = false;
        SDL_Gamepad *g_slots[kMaxPads] = {}; // player slot -> controller, in the order they were connected

        void updateSlots()
        {
            for (SDL_Gamepad *&g : g_slots)
            {
                if (g && !SDL_GamepadConnected(g))
                {
                    SDL_CloseGamepad(g);
                    g = nullptr;
                }
            }
            int count = 0;
            SDL_JoystickID *ids = SDL_GetGamepads(&count);
            for (int i = 0; ids && i < count; ++i)
            {
                bool open = false;
                for (SDL_Gamepad *g : g_slots)
                    open |= g && SDL_GetGamepadID(g) == ids[i];
                if (open)
                    continue;
                for (SDL_Gamepad *&g : g_slots)
                {
                    if (!g)
                    {
                        g = SDL_OpenGamepad(ids[i]);
                        if (g)
                            std::printf("[input] controller connected: %s\n", SDL_GetGamepadName(g));
                        break;
                    }
                }
            }
            SDL_free(ids);
        }

        void pollSdl(PadState pads[kMaxPads])
        {
            SDL_UpdateGamepads();
            updateSlots();
            for (int i = 0; i < kMaxPads; ++i)
            {
                SDL_Gamepad *g = g_slots[i];
                if (!g)
                    continue;
                PadState &p = pads[i];
                p.connected = true;
                auto btn = [g](SDL_GamepadButton b) { return SDL_GetGamepadButton(g, b); };
                fillButtons(p, btn(SDL_GAMEPAD_BUTTON_DPAD_UP), btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN),
                            btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT), btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT),
                            btn(SDL_GAMEPAD_BUTTON_SOUTH), btn(SDL_GAMEPAD_BUTTON_EAST), btn(SDL_GAMEPAD_BUTTON_WEST),
                            btn(SDL_GAMEPAD_BUTTON_NORTH), btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER),
                            btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), btn(SDL_GAMEPAD_BUTTON_LEFT_STICK),
                            btn(SDL_GAMEPAD_BUTTON_RIGHT_STICK), btn(SDL_GAMEPAD_BUTTON_START), btn(SDL_GAMEPAD_BUTTON_BACK));
                p.menu = btn(SDL_GAMEPAD_BUTTON_GUIDE) || btn(SDL_GAMEPAD_BUTTON_TOUCHPAD);
                auto axis = [g](SDL_GamepadAxis a) { return static_cast<int>(SDL_GetGamepadAxis(g, a)); };
                // SDL's stick y axes already point down, like the PS2's.
                fillAnalog(p, axis(SDL_GAMEPAD_AXIS_LEFTX), axis(SDL_GAMEPAD_AXIS_LEFTY), axis(SDL_GAMEPAD_AXIS_RIGHTX),
                           axis(SDL_GAMEPAD_AXIS_RIGHTY), axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER),
                           axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
            }
            // Nothing reads SDL's event queue (the host has its own window and message loop): keep it empty.
            SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
        }
#endif
    }

    void init(float deadzone)
    {
        g_deadzone = deadzone;
#if PS2X_HAS_SDL3
        // The host owns the window and its message loop; SDL only reads controllers, also while the window is
        // in the background (hidden test runs never get here: they ignore controllers).
        SDL_SetHint(SDL_HINT_WINDOWS_ENABLE_MESSAGELOOP, "0");
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        g_sdl = SDL_Init(SDL_INIT_GAMEPAD);
        std::printf("[input] %s\n", g_sdl ? "SDL3 game controllers" : "SDL3 unavailable, XInput only");
#endif
    }

    void shutdown()
    {
#if PS2X_HAS_SDL3
        if (!g_sdl)
            return;
        for (SDL_Gamepad *&g : g_slots)
        {
            if (g)
                SDL_CloseGamepad(g);
            g = nullptr;
        }
        SDL_Quit();
        g_sdl = false;
#endif
    }

    void poll(PadState pads[kMaxPads], double now)
    {
#if PS2X_HAS_SDL3
        if (g_sdl)
        {
            pollSdl(pads);
            return;
        }
#endif
        pollXInput(pads, now);
    }

    void rumble(int index, uint8_t smallMotor, uint8_t largeMotor)
    {
        if (index < 0 || index >= kMaxPads)
            return;
#if PS2X_HAS_SDL3
        if (g_sdl)
        {
            // The DualShock 2's small motor is on/off, the large one 0..255. SDL wants a duration: the game sets
            // the motors every frame while they run, so a short one stops them soon after it stops asking.
            if (SDL_Gamepad *g = g_slots[index])
                SDL_RumbleGamepad(g, static_cast<Uint16>(largeMotor * 257u), smallMotor ? 0xFFFFu : 0u, 250u);
            return;
        }
#endif
        XINPUT_VIBRATION v{};
        v.wLeftMotorSpeed = static_cast<WORD>(largeMotor * 257);
        v.wRightMotorSpeed = static_cast<WORD>(smallMotor ? 0xFFFF : 0);
        XInputSetState(static_cast<DWORD>(index), &v);
    }
}
