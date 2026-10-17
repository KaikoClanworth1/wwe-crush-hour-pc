#pragma once

// Controller and keyboard mapping shared by the native host's input backends (host/gamepad_input.cpp): stick
// deadzones, trigger pressure, and the [Controls] key bindings in ps2x_settings.ini. Header-only and free of
// platform headers so the unit tests can exercise it.

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ps2host::input
{
    // A stick (signed 16-bit axes, +y down as the PS2 reports it) to DualShock bytes (0..255, centre 0x80) with
    // a radial deadzone: inside `deadzone` (fraction of full deflection) the stick is centred, outside it the
    // remaining range is rescaled so the edge of the deadzone maps to zero and full deflection stays full.
    // Unlike a per-axis (square) deadzone this keeps diagonals and small angles intact.
    struct StickBytes
    {
        uint8_t x = 0x80, y = 0x80;
    };

    inline StickBytes radialStick(int x, int y, float deadzone)
    {
        const float fx = std::clamp(static_cast<float>(x) / 32767.0f, -1.0f, 1.0f);
        const float fy = std::clamp(static_cast<float>(y) / 32767.0f, -1.0f, 1.0f);
        const float magnitude = std::sqrt(fx * fx + fy * fy);
        const float dz = std::clamp(deadzone, 0.0f, 0.9f);
        if (magnitude <= dz || magnitude == 0.0f)
            return {};
        const float scaled = std::min(1.0f, (magnitude - dz) / (1.0f - dz)) / magnitude;
        auto toByte = [](float v)
        {
            return static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround((v + 1.0f) * 127.5f)), 0, 255)); // -1..1 -> 0..255
        };
        return {toByte(fx * scaled), toByte(fy * scaled)};
    }

    // An analog trigger (0..32767) to DualShock 2 pressure (0..255).
    inline uint8_t triggerPressure(int value)
    {
        return static_cast<uint8_t>(std::clamp(value * 255 / 32767, 0, 255));
    }

    // Pressure at which a trigger also sets its digital L2 / R2 bit.
    inline constexpr uint8_t kTriggerPressedAt = 40;

    // PS2 pad buttons by name, in libpad bit order (matches ps2host::PadButton).
    struct ButtonName
    {
        const char *name;
        uint16_t bit;
    };
    inline constexpr std::array<ButtonName, 16> kButtonNames{{
        {"Select", 1u << 0}, {"L3", 1u << 1}, {"R3", 1u << 2}, {"Start", 1u << 3},
        {"Up", 1u << 4}, {"Right", 1u << 5}, {"Down", 1u << 6}, {"Left", 1u << 7},
        {"L2", 1u << 8}, {"R2", 1u << 9}, {"L1", 1u << 10}, {"R1", 1u << 11},
        {"Triangle", 1u << 12}, {"Circle", 1u << 13}, {"Cross", 1u << 14}, {"Square", 1u << 15},
    }};

    // Keyboard keys by name, as Windows virtual-key codes (letters and digits are their ASCII codes).
    struct KeyName
    {
        const char *name;
        int vk;
    };
    inline constexpr std::array<KeyName, 28> kKeyNames{{
        {"Up", 0x26}, {"Down", 0x28}, {"Left", 0x25}, {"Right", 0x27}, {"Space", 0x20}, {"Enter", 0x0D},
        {"Return", 0x0D}, {"Backspace", 0x08}, {"Tab", 0x09}, {"Escape", 0x1B}, {"Shift", 0x10},
        {"LShift", 0xA0}, {"RShift", 0xA1}, {"Ctrl", 0x11}, {"LCtrl", 0xA2}, {"RCtrl", 0xA3}, {"Alt", 0x12},
        {"Insert", 0x2D}, {"Delete", 0x2E}, {"Home", 0x24}, {"End", 0x23}, {"PageUp", 0x21},
        {"PageDown", 0x22}, {"Numpad0", 0x60}, {"Numpad1", 0x61}, {"Numpad2", 0x62}, {"Numpad3", 0x63},
        {"Numpad5", 0x65},
    }};

    inline bool equalsNoCase(std::string_view a, std::string_view b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    }

    // "X", "Space", "F5", "Numpad2" ... to a virtual-key code; 0 if unknown.
    inline int keyCode(std::string_view name)
    {
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front())))
            name.remove_prefix(1);
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
            name.remove_suffix(1);
        if (name.size() == 1)
        {
            const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
                return c;
        }
        if (name.size() >= 2 && name.size() <= 3 && (name[0] == 'F' || name[0] == 'f'))
        {
            int n = 0;
            for (size_t i = 1; i < name.size(); ++i)
            {
                if (!std::isdigit(static_cast<unsigned char>(name[i])))
                    return 0;
                n = n * 10 + (name[i] - '0');
            }
            return (n >= 1 && n <= 24) ? 0x70 + n - 1 : 0;
        }
        for (const KeyName &k : kKeyNames)
            if (equalsNoCase(name, k.name))
                return k.vk;
        return 0;
    }

    // Keys bound to one PS2 button: "X, Space" -> {'X', VK_SPACE}. Unknown names are skipped.
    inline std::vector<int> parseKeyList(std::string_view list)
    {
        std::vector<int> keys;
        while (!list.empty())
        {
            const size_t comma = list.find(',');
            const int vk = keyCode(list.substr(0, comma));
            if (vk != 0)
                keys.push_back(vk);
            if (comma == std::string_view::npos)
                break;
            list.remove_prefix(comma + 1);
        }
        return keys;
    }

    // Default keyboard layout for player 1 (also what the README documents).
    inline constexpr std::array<std::pair<const char *, const char *>, 16> kDefaultKeys{{
        {"Up", "Up"}, {"Down", "Down"}, {"Left", "Left"}, {"Right", "Right"},
        {"Cross", "X, Space"}, {"Circle", "C, Backspace"}, {"Square", "Z"}, {"Triangle", "V"},
        {"L1", "Q"}, {"R1", "E"}, {"L2", "1"}, {"R2", "3"},
        {"Start", "Enter"}, {"Select", "RShift, Tab"}, {"L3", ""}, {"R3", ""},
    }};
}
