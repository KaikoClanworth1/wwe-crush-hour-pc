// In-game mod menu (WWE Crush Hour port).
//
// Opening it: on the main menu an "m" plank sits right of "options" (painted into the sign's texture by the GS
// backend, port/tools/make_mod_button.py); RIGHT on "options" gives it the focus, A opens the menu. F2 (or BACK + RB
// on a pad) opens it anywhere, also in a match. While the focus or the menu is up, the game sees no input from
// player 1.
//
// Mods: built-in ones (the game's own cheats) and PCSX2-style .pnach cheat files in the Mods folder next to the game
// files (each [Section] is a mod; patch=0 lines are written when the mod is turned on, patch=1 lines every frame).
// Mods are applied live by ps2x_mods_frame (the game's vsync wait, EE thread); turning one off writes back the
// values its patches replaced. Which mods are on is kept in Mods/enabled.txt. The recompiled game runs from host
// code, so patches that change MIPS instructions have no effect -- only data patches (most cheats) do.
//
// Main menu state comes from Shell::Update (ps2x_modmenu_shell, hooked by port/tools/patch_modmenu.py): the active
// menu (Shell + 0x109C, 0 = main menu) and its highlighted item (+0x109D, 4 = options). The game highlights an item by
// swapping its material texture (Shell + 0xF14 + item * 8: normal, + 0xF18: highlighted); while the m plank has the
// focus the native renderer draws the "options" item with its normal texture (nrw::setTextureRemap).
// PS2X_MODMENU=0 turns the whole feature off.

#include "host/win32_host.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace nrw
{
    void setTextureRemap(uint32_t from, uint32_t to); // native_rw.cpp
}

namespace
{
    // ------------------------------------------------------------------------------------------------ mods
    struct Op
    {
        enum Kind : uint8_t { kWrite, kSkipNext, kSkipN };
        Kind kind = kWrite;
        bool continuous = false; // patch=1: every frame; patch=0: when turned on
        uint8_t size = 4;        // 1, 2, 4, 8 bytes
        bool bigEndian = false;
        uint8_t cmp = 0;         // conditionals: 0 ==, 1 !=, 2 <, 3 >
        uint16_t skip = 0;       // kSkipN: lines to skip when the condition fails
        uint32_t addr = 0;
        uint64_t value = 0;
    };

    struct Mod
    {
        std::string id;          // "builtin:<name>" or "<file>|<section>"
        std::string name, description, source;
        int builtin = 0;         // 1 unlimited turbo, 2 power hungry, 3 unlock everything
        std::vector<Op> ops;
        bool enabled = false;    // wanted (menu)
        bool applied = false;    // live state (EE thread)
        bool missing = false;    // its file is gone
        std::vector<std::pair<uint32_t, std::vector<uint8_t>>> originals; // values before the mod's writes
        uint32_t unlockOptions = 0;
        uint8_t unlockOriginal = 0;
    };

    std::mutex g_mx; // mods, enabled flags (host thread) vs application (EE thread)
    std::vector<Mod> g_mods;
    bool g_loaded = false;

    constexpr uint32_t kRam = 0x2000000u;
    constexpr uint32_t kCheatFlags = 0x4A2298u;   // Player::muCheatFlags: bit 0 unlimited turbo, bit 1 power hungry
    constexpr uint32_t kOptionsPtr = 0x4A57F8u;   // WWFOptions::Instance()
    constexpr uint32_t kUnlockAll = 0x558u;       // WWFOptions + 0x558: the "unlock everything" cheat flag

    bool featureOn()
    {
        static const bool s_on = []()
        {
            const char *v = std::getenv("PS2X_MODMENU");
            return !(v && *v == '0');
        }();
        return s_on;
    }

    std::string trim(std::string s)
    {
        const auto b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos)
            return {};
        const auto e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    std::string lower(std::string s)
    {
        for (char &c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    std::vector<std::string> split(const std::string &s, char sep)
    {
        std::vector<std::string> out;
        size_t p = 0;
        while (true)
        {
            const size_t q = s.find(sep, p);
            out.push_back(trim(s.substr(p, q == std::string::npos ? std::string::npos : q - p)));
            if (q == std::string::npos)
                break;
            p = q + 1;
        }
        return out;
    }

    // One pnach "patch=" line into ops. Supported types: byte, short, word, double (and be* variants), extended
    // (CodeBreaker style: 0/1/2 = 8/16/32-bit write, 4 = serial 32-bit writes, D = 16-bit test of the next line,
    // E = 16-bit test of the next n lines).
    struct PendingSerial
    {
        bool active = false;
        uint32_t addr = 0, count = 0, step = 0;
        bool continuous = false;
    };

    bool parsePatch(const std::string &rhs, std::vector<Op> &ops, PendingSerial &serial)
    {
        const auto f = split(rhs, ',');
        if (f.size() < 5)
            return false;
        const int place = std::atoi(f[0].c_str());
        if (lower(f[1]) != "ee")
            return false; // IOP patches: not used by this port
        const uint32_t addr = static_cast<uint32_t>(std::strtoul(f[2].c_str(), nullptr, 16));
        const std::string type = lower(f[3]);
        const uint64_t value = std::strtoull(f[4].c_str(), nullptr, 16);
        Op op;
        op.continuous = place != 0;
        if (type == "extended")
        {
            if (serial.active)
            {
                // Second line of a 4-type serial write: value, increment.
                const uint32_t inc = addr;
                for (uint32_t i = 0; i < serial.count; ++i)
                {
                    Op w;
                    w.continuous = serial.continuous;
                    w.size = 4;
                    w.addr = serial.addr + i * serial.step * 4u;
                    w.value = static_cast<uint32_t>(value + static_cast<uint64_t>(inc) * i);
                    ops.push_back(w);
                }
                serial.active = false;
                return true;
            }
            const uint32_t cmd = addr >> 28, a = addr & 0x0FFFFFFFu;
            switch (cmd)
            {
            case 0x0: op.size = 1; op.addr = a; op.value = value & 0xFFu; break;
            case 0x1: op.size = 2; op.addr = a; op.value = value & 0xFFFFu; break;
            case 0x2: op.size = 4; op.addr = a; op.value = value & 0xFFFFFFFFu; break;
            case 0x4:
                serial = {true, a, static_cast<uint32_t>(value >> 16) & 0xFFFFu, static_cast<uint32_t>(value) & 0xFFFFu, op.continuous};
                return true;
            case 0xD:
                op.kind = Op::kSkipNext;
                op.size = 2;
                op.addr = a;
                op.cmp = static_cast<uint8_t>((value >> 20) & 3u);
                op.value = value & 0xFFFFu;
                break;
            case 0xE:
                op.kind = Op::kSkipN;
                op.size = 2;
                op.skip = static_cast<uint16_t>((addr >> 16) & 0xFFFu);
                op.value = addr & 0xFFFFu;
                op.cmp = static_cast<uint8_t>((value >> 28) & 3u);
                op.addr = static_cast<uint32_t>(value) & 0x0FFFFFFFu;
                break;
            default:
                return false;
            }
            ops.push_back(op);
            return true;
        }
        op.addr = addr;
        op.value = value;
        if (type == "byte") op.size = 1;
        else if (type == "short") op.size = 2;
        else if (type == "word") op.size = 4;
        else if (type == "double") op.size = 8;
        else if (type == "beshort") { op.size = 2; op.bigEndian = true; }
        else if (type == "beword") { op.size = 4; op.bigEndian = true; }
        else if (type == "bedouble") { op.size = 8; op.bigEndian = true; }
        else
            return false;
        ops.push_back(op);
        return true;
    }

    void loadPnach(const std::filesystem::path &file, std::vector<Mod> &out)
    {
        std::ifstream in(file);
        if (!in)
            return;
        const std::string fname = file.filename().string();
        Mod cur;
        cur.name = file.stem().string();
        cur.source = fname;
        cur.id = fname + "|";
        PendingSerial serial;
        std::string fileComment;
        auto flush = [&]()
        {
            if (!cur.ops.empty())
            {
                if (cur.description.empty())
                    cur.description = fileComment;
                out.push_back(cur);
            }
        };
        std::string line;
        while (std::getline(in, line))
        {
            line = trim(line);
            if (line.empty() || line.rfind("//", 0) == 0 || line[0] == ';' || line[0] == '#')
                continue;
            if (line.front() == '[' && line.back() == ']')
            {
                flush();
                std::string name = trim(line.substr(1, line.size() - 2));
                for (char &c : name)
                    if (c == '\\')
                        c = '/';
                cur = Mod{};
                cur.name = name;
                cur.source = fname;
                cur.id = fname + "|" + name;
                serial = {};
                continue;
            }
            const auto eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string key = lower(trim(line.substr(0, eq))), val = trim(line.substr(eq + 1));
            if (key == "patch")
                parsePatch(val, cur.ops, serial);
            else if (key == "description")
                cur.description = val;
            else if (key == "comment")
            {
                if (cur.ops.empty() && cur.id == fname + "|")
                    fileComment = val;
                else
                    cur.description = val;
            }
            else if (key == "author")
                cur.description += (cur.description.empty() ? "by " : "  (by ") + val + (cur.description.empty() ? "" : ")");
        }
        flush();
    }

    std::filesystem::path modsDir() { return std::filesystem::path("Mods"); }

    std::set<std::string> readEnabled()
    {
        std::set<std::string> ids;
        std::ifstream in(modsDir() / "enabled.txt");
        std::string line;
        while (std::getline(in, line))
        {
            line = trim(line);
            if (!line.empty() && line[0] != '#')
                ids.insert(line);
        }
        return ids;
    }

    void writeEnabledLocked()
    {
        std::error_code ec;
        std::filesystem::create_directories(modsDir(), ec);
        std::ofstream out(modsDir() / "enabled.txt", std::ios::trunc);
        out << "# Mods turned on in the in-game mod menu (one id per line).\n";
        for (const Mod &m : g_mods)
            if (m.enabled)
                out << m.id << "\n";
    }

    // (Re)scan built-ins and Mods/*.pnach. Mods still applied keep their entry (so they can be turned off).
    void scanLocked()
    {
        std::vector<Mod> found;
        {
            Mod m;
            m.id = "builtin:unlimited_turbo";
            m.name = "Unlimited Turbo";
            m.description = "The game's own cheat: your turbo stays full (6 charges).";
            m.source = "built in";
            m.builtin = 1;
            found.push_back(m);
            m.id = "builtin:power_hungry";
            m.name = "Power Hungry";
            m.description = "The game's own cheat: your special power charges five times faster.";
            m.builtin = 2;
            found.push_back(m);
            m.id = "builtin:unlock_everything";
            m.name = "Unlock Everything";
            m.description = "The game's own unlock cheat: every superstar, arena and match type (takes effect the next "
                            "time those menus are shown).";
            m.builtin = 3;
            found.push_back(m);
        }
        std::error_code ec;
        std::vector<std::filesystem::path> files;
        for (const auto &e : std::filesystem::directory_iterator(modsDir(), ec))
            if (e.is_regular_file() && lower(e.path().extension().string()) == ".pnach")
                files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (const auto &f : files)
            loadPnach(f, found);

        const std::set<std::string> enabledIds = g_loaded ? std::set<std::string>{} : readEnabled();
        std::vector<Mod> merged;
        for (Mod &m : found)
        {
            auto it = std::find_if(g_mods.begin(), g_mods.end(), [&](const Mod &o) { return o.id == m.id; });
            if (it != g_mods.end())
            {
                if (it->applied)
                {
                    it->missing = false;
                    merged.push_back(*it); // keep what is live (its originals)
                    continue;
                }
                m.enabled = it->enabled;
            }
            else if (!g_loaded)
                m.enabled = enabledIds.count(m.id) != 0;
            merged.push_back(m);
        }
        for (Mod &old : g_mods)
            if (old.applied && std::none_of(merged.begin(), merged.end(), [&](const Mod &m) { return m.id == old.id; }))
            {
                old.missing = true;
                merged.push_back(old);
            }
        g_mods = std::move(merged);
        g_loaded = true;
    }

    // ------------------------------------------------------------------------------------------------ apply
    uint64_t readMem(const uint8_t *ram, uint32_t addr, uint32_t size, bool be)
    {
        uint64_t v = 0;
        for (uint32_t i = 0; i < size; ++i)
            v |= static_cast<uint64_t>(ram[(addr + (be ? size - 1u - i : i)) & (kRam - 1u)]) << (8u * i);
        return v;
    }

    void writeMem(uint8_t *ram, uint32_t addr, uint32_t size, uint64_t v, bool be)
    {
        for (uint32_t i = 0; i < size; ++i)
            ram[(addr + (be ? size - 1u - i : i)) & (kRam - 1u)] = static_cast<uint8_t>(v >> (8u * i));
    }

    bool testCond(const uint8_t *ram, const Op &op)
    {
        const uint64_t v = readMem(ram, op.addr & (kRam - 1u), 2u, false);
        switch (op.cmp)
        {
        case 0: return v == op.value;
        case 1: return v != op.value;
        case 2: return v < op.value;
        default: return v > op.value;
        }
    }

    // Runs a mod's ops; once = also the patch=0 writes (the frame the mod is turned on).
    void runOps(uint8_t *ram, Mod &m, bool once)
    {
        for (size_t i = 0; i < m.ops.size(); ++i)
        {
            const Op &op = m.ops[i];
            if (op.kind == Op::kSkipNext)
            {
                if (!testCond(ram, op))
                    ++i;
                continue;
            }
            if (op.kind == Op::kSkipN)
            {
                if (!testCond(ram, op))
                    i += op.skip;
                continue;
            }
            if (!op.continuous && !once)
                continue;
            const uint32_t a = op.addr & (kRam - 1u);
            if (std::none_of(m.originals.begin(), m.originals.end(), [&](const auto &o) { return o.first == a && o.second.size() == op.size; }))
            {
                std::vector<uint8_t> before(op.size);
                for (uint32_t k = 0; k < op.size; ++k)
                    before[k] = ram[(a + k) & (kRam - 1u)];
                m.originals.emplace_back(a, std::move(before));
            }
            writeMem(ram, a, op.size, op.value, op.bigEndian);
        }
    }

    void restore(uint8_t *ram, Mod &m)
    {
        for (auto it = m.originals.rbegin(); it != m.originals.rend(); ++it)
            for (size_t k = 0; k < it->second.size(); ++k)
                ram[(it->first + k) & (kRam - 1u)] = it->second[k];
        m.originals.clear();
    }

    uint32_t rd32(const uint8_t *ram, uint32_t a)
    {
        uint32_t v;
        std::memcpy(&v, ram + (a & (kRam - 4u)), 4);
        return v;
    }

    void wr32(uint8_t *ram, uint32_t a, uint32_t v) { std::memcpy(ram + (a & (kRam - 4u)), &v, 4); }

    void applyBuiltin(uint8_t *ram, Mod &m, bool on)
    {
        switch (m.builtin)
        {
        case 1:
        case 2:
        {
            const uint32_t bit = m.builtin == 1 ? 1u : 2u; // the game clears these when a match starts: set every frame
            const uint32_t f = rd32(ram, kCheatFlags);
            wr32(ram, kCheatFlags, on ? (f | bit) : (f & ~bit));
            break;
        }
        case 3:
        {
            const uint32_t opts = rd32(ram, kOptionsPtr);
            if (on)
            {
                if (opts && opts < kRam && opts != m.unlockOptions)
                {
                    m.unlockOptions = opts;
                    m.unlockOriginal = ram[(opts + kUnlockAll) & (kRam - 1u)];
                }
                if (m.unlockOptions)
                    ram[(m.unlockOptions + kUnlockAll) & (kRam - 1u)] = 1;
            }
            else if (m.unlockOptions)
            {
                if (m.unlockOptions == opts)
                    ram[(m.unlockOptions + kUnlockAll) & (kRam - 1u)] = m.unlockOriginal;
                m.unlockOptions = 0;
            }
            break;
        }
        default:
            break;
        }
    }

    // ------------------------------------------------------------------------------------------------ menu state
    std::atomic<uint32_t> g_shell{0};
    std::atomic<int> g_menu{-1}, g_sel{-1};
    std::atomic<double> g_shellSeen{0.0}; // host time of the last Shell::Update
    std::atomic<bool> g_focus{false};     // the m plank has the focus
    std::atomic<bool> g_open{false};      // the mod menu is up
    std::atomic<bool> g_openFromPlank{false};
    std::atomic<uint32_t> g_dirty{1};     // panel needs a redraw
    int g_cursor = 0, g_scroll = 0;       // host thread
    uint16_t g_prevButtons = 0, g_suppress = 0;
    double g_repeatAt = 0.0;
    uint16_t g_repeatButton = 0;
    bool g_prevHotkey = false;

    double nowSec()
    {
        static const auto t0 = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }

    bool mainMenuShown()
    {
        return g_menu.load() == 0 && nowSec() - g_shellSeen.load() < 0.5;
    }

    // ------------------------------------------------------------------------------------------------ panel (GDI)
    struct PanelCache
    {
        uint32_t w = 0, h = 0, gen = ~0u, outH = 0;
        std::vector<uint32_t> px;
    } g_panel;

    struct Row
    {
        std::string name, desc, source;
        bool on = false, missing = false;
    };

    std::wstring widen(const std::string &s)
    {
        if (s.empty())
            return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(static_cast<size_t>(std::max(0, n - 1)), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        return w;
    }

    void renderPanel(uint32_t outH, const std::vector<Row> &rows, int cursor, int scroll, std::vector<uint32_t> &px,
                     uint32_t &W, uint32_t &H)
    {
        const float s = std::clamp(static_cast<float>(outH) / 1080.0f, 0.6f, 3.0f);
        auto S = [&](float v) { return static_cast<int>(v * s + 0.5f); };
        constexpr int kMaxVisible = 8;
        const int visible = std::clamp(static_cast<int>(rows.size()), 4, kMaxVisible);
        W = static_cast<uint32_t>(S(900));
        H = static_cast<uint32_t>(S(108.0f + visible * 48.0f + 18.0f + 92.0f + 46.0f));
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = static_cast<LONG>(W);
        bi.bmiHeader.biHeight = -static_cast<LONG>(H);
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void *bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp || !bits)
        {
            DeleteDC(dc);
            W = H = 0;
            return;
        }
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        SetBkMode(dc, TRANSPARENT);
        auto fill = [&](int x0, int y0, int x1, int y1, COLORREF c)
        {
            RECT r{x0, y0, x1, y1};
            HBRUSH b = CreateSolidBrush(c);
            FillRect(dc, &r, b);
            DeleteObject(b);
        };
        auto rrect = [&](int x0, int y0, int x1, int y1, int rad, COLORREF fillC, COLORREF penC, int penW)
        {
            HBRUSH b = CreateSolidBrush(fillC);
            HPEN p = CreatePen(PS_SOLID, std::max(1, penW), penC);
            HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
            RoundRect(dc, x0, y0, x1, y1, rad, rad);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(b);
            DeleteObject(p);
        };
        auto font = [&](const wchar_t *face, int px, int weight, bool italic = false)
        {
            return CreateFontW(-px, 0, 0, 0, weight, italic, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                               CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
        };
        auto text = [&](HFONT f, const std::wstring &t, RECT r, COLORREF c, UINT fmt)
        {
            HGDIOBJ of = SelectObject(dc, f);
            SetTextColor(dc, c);
            DrawTextW(dc, t.c_str(), static_cast<int>(t.size()), &r, fmt);
            SelectObject(dc, of);
        };

        // Frame: dark steel panel, the game's highlight blue as the border.
        fill(0, 0, static_cast<int>(W), static_cast<int>(H), RGB(18, 20, 28));
        rrect(S(3), S(3), static_cast<int>(W) - S(3), static_cast<int>(H) - S(3), S(18), RGB(18, 20, 28), RGB(70, 92, 210), S(4));
        HFONT fTitle = font(L"Impact", S(58), FW_NORMAL);
        HFONT fRow = font(L"Arial", S(27), FW_BOLD);
        HFONT fBadge = font(L"Arial", S(20), FW_BOLD);
        HFONT fDesc = font(L"Arial", S(21), FW_NORMAL);
        HFONT fSmall = font(L"Arial", S(18), FW_NORMAL, true);
        HFONT fBtn = font(L"Arial", S(20), FW_BOLD);

        text(fTitle, L"MODS", RECT{S(36), S(18), S(500), S(90)}, RGB(154, 162, 255), DT_LEFT | DT_TOP | DT_SINGLELINE);
        int onCount = 0;
        for (const Row &r : rows)
            onCount += r.on ? 1 : 0;
        wchar_t cnt[64];
        std::swprintf(cnt, 64, L"%d of %d on", onCount, static_cast<int>(rows.size()));
        text(fSmall, cnt, RECT{S(400), S(46), static_cast<int>(W) - S(36), S(80)}, RGB(150, 152, 165), DT_RIGHT | DT_TOP | DT_SINGLELINE);
        fill(S(30), S(94), static_cast<int>(W) - S(30), S(96), RGB(70, 92, 210));

        const int kVisible = visible;
        const int rowH = S(48), listY = S(108);
        for (int i = 0; i < kVisible && scroll + i < static_cast<int>(rows.size()); ++i)
        {
            const Row &r = rows[static_cast<size_t>(scroll + i)];
            const int y = listY + i * rowH;
            const bool sel = scroll + i == cursor;
            if (sel)
                rrect(S(24), y + S(3), static_cast<int>(W) - S(24), y + rowH - S(3), S(12), RGB(46, 60, 150), RGB(110, 130, 240), S(2));
            const std::wstring name = widen(r.name) + (r.missing ? L"  (file removed)" : L"");
            text(fRow, name, RECT{S(44), y, static_cast<int>(W) - S(150), y + rowH},
                 sel ? RGB(236, 238, 255) : RGB(205, 207, 215), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            const int bx1 = static_cast<int>(W) - S(44), bx0 = bx1 - S(78);
            rrect(bx0, y + S(10), bx1, y + rowH - S(10), S(10), r.on ? RGB(40, 165, 85) : RGB(52, 54, 64),
                  r.on ? RGB(90, 215, 130) : RGB(80, 82, 94), S(2));
            text(fBadge, r.on ? L"ON" : L"OFF", RECT{bx0, y + S(10), bx1, y + rowH - S(10)},
                 r.on ? RGB(255, 255, 255) : RGB(150, 152, 165), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        if (scroll > 0)
            text(fBadge, L"▲", RECT{0, listY - S(26), static_cast<int>(W), listY}, RGB(154, 162, 255), DT_CENTER | DT_TOP | DT_SINGLELINE);
        if (scroll + kVisible < static_cast<int>(rows.size()))
            text(fBadge, L"▼", RECT{0, listY + kVisible * rowH - S(4), static_cast<int>(W), listY + kVisible * rowH + S(20)},
                 RGB(154, 162, 255), DT_CENTER | DT_TOP | DT_SINGLELINE);

        const int descY = listY + kVisible * rowH + S(18);
        fill(S(30), descY - S(4), static_cast<int>(W) - S(30), descY - S(2), RGB(48, 52, 70));
        if (cursor >= 0 && cursor < static_cast<int>(rows.size()))
        {
            const Row &r = rows[static_cast<size_t>(cursor)];
            text(fDesc, widen(r.desc.empty() ? std::string("No description.") : r.desc),
                 RECT{S(40), descY + S(4), static_cast<int>(W) - S(40), descY + S(58)}, RGB(185, 187, 198), DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
            text(fSmall, widen(r.source), RECT{S(40), descY + S(58), static_cast<int>(W) - S(40), descY + S(82)},
                 RGB(120, 122, 136), DT_LEFT | DT_TOP | DT_SINGLELINE);
        }
        else
            text(fDesc, L"No mods found.", RECT{S(40), descY + S(4), static_cast<int>(W) - S(40), descY + S(40)}, RGB(185, 187, 198), DT_LEFT | DT_TOP);

        // Footer: button glyphs as the Xbox colours.
        const int fy = static_cast<int>(H) - S(46);
        auto btn = [&](int x, COLORREF c, const wchar_t *letter, const wchar_t *label)
        {
            HBRUSH b = CreateSolidBrush(c);
            HPEN p = CreatePen(PS_SOLID, 1, c);
            HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
            Ellipse(dc, x, fy, x + S(30), fy + S(30));
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(b);
            DeleteObject(p);
            text(fBtn, letter, RECT{x, fy, x + S(30), fy + S(30)}, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            text(fBtn, label, RECT{x + S(38), fy, x + S(220), fy + S(30)}, RGB(205, 207, 215), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            return x + S(38) + S(static_cast<float>(wcslen(label)) * 12.0f) + S(30);
        };
        int bx = S(40);
        bx = btn(bx, RGB(60, 170, 60), L"A", L"Toggle");
        bx = btn(bx, RGB(200, 40, 40), L"B", L"Back");
        text(fSmall, L"Add mods: .pnach files in the Mods folder", RECT{bx, fy, static_cast<int>(W) - S(36), fy + S(30)},
             RGB(120, 122, 136), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        for (HFONT f : {fTitle, fRow, fBadge, fDesc, fSmall, fBtn})
            DeleteObject(f);
        GdiFlush();
        // GDI leaves alpha at 0: the panel is opaque but for a light translucency, premultiplied for the blend.
        px.resize(static_cast<size_t>(W) * H);
        const uint32_t *src = static_cast<const uint32_t *>(bits);
        constexpr uint32_t kA = 250u;
        for (size_t i = 0; i < px.size(); ++i)
        {
            const uint32_t c = src[i];
            const uint32_t b = (c & 0xFFu) * kA / 255u, g = ((c >> 8) & 0xFFu) * kA / 255u, r = ((c >> 16) & 0xFFu) * kA / 255u;
            px[i] = b | (g << 8) | (r << 16) | (kA << 24); // BGRA
        }
        // Rounded corners: clear outside the border's arc.
        const int rad = S(9);
        for (int y = 0; y < rad; ++y)
            for (int x = 0; x < rad; ++x)
            {
                const int dx = rad - x, dy = rad - y;
                if (dx * dx + dy * dy > rad * rad)
                {
                    const int xs[2] = {x, static_cast<int>(W) - 1 - x}, ys[2] = {y, static_cast<int>(H) - 1 - y};
                    for (int cy : ys)
                        for (int cx : xs)
                            px[static_cast<size_t>(cy) * W + static_cast<size_t>(cx)] = 0u;
                }
            }
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(dc);
    }
}

// ---------------------------------------------------------------------------------------------------- EE side
// Shell::Update (port/tools/patch_modmenu.py): the main menu's state, and the "options" highlight while the m plank
// has the focus.
extern "C" void ps2x_modmenu_shell(uint8_t *rdram, uint32_t shell)
{
    if (!featureOn() || !rdram || !shell || shell >= kRam)
        return;
    const int menu = static_cast<int8_t>(rdram[(shell + 0x109Cu) & (kRam - 1u)]);
    const int sel = static_cast<int8_t>(rdram[(shell + 0x109Du) & (kRam - 1u)]);
    g_shell.store(shell);
    g_menu.store(menu);
    g_sel.store(sel);
    g_shellSeen.store(nowSec());
    const bool plank = menu == 0 && (g_focus.load() || (g_open.load() && g_openFromPlank.load()));
    if (plank)
        nrw::setTextureRemap(rd32(rdram, shell + 0xF18u + 4u * 8u), rd32(rdram, shell + 0xF14u + 4u * 8u));
    else
        nrw::setTextureRemap(0u, 0u);
}

// The game's vsync wait (once per game frame, menus and matches): applies the mods.
extern "C" void ps2x_mods_frame(uint8_t *rdram)
{
    if (!featureOn() || !rdram)
        return;
    std::lock_guard<std::mutex> lock(g_mx);
    if (!g_loaded)
        scanLocked();
    for (Mod &m : g_mods)
    {
        if (m.enabled != m.applied)
        {
            if (m.enabled)
            {
                if (m.builtin)
                    applyBuiltin(rdram, m, true);
                else
                    runOps(rdram, m, true);
            }
            else
            {
                if (m.builtin)
                    applyBuiltin(rdram, m, false);
                else
                    restore(rdram, m);
            }
            m.applied = m.enabled;
            continue;
        }
        if (m.applied)
        {
            if (m.builtin)
                applyBuiltin(rdram, m, true);
            else
                runOps(rdram, m, false);
        }
    }
}

// GS backend: which variant of the m plank the main menu sign shows (-1 none, 0 normal, 1 highlighted).
extern "C" int ps2x_modmenu_button_state()
{
    if (!featureOn())
        return -1;
    return (g_focus.load() || (g_open.load() && g_openFromPlank.load())) ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------------- host side
namespace modmenu
{
    bool isOpen() { return g_open.load(); }

    // pollInput: navigation of the m plank and the menu; masks player 1's input while either has it.
    void filterInput(ps2host::PadState *pads, int count, bool hotkeyDown)
    {
        using namespace ps2host;
        if (!featureOn() || count <= 0)
            return;
        PadState &p = pads[0];
        // The left stick navigates too.
        uint16_t b = p.buttons;
        if (p.ly < 0x40) b |= kBtnUp;
        if (p.ly > 0xC0) b |= kBtnDown;
        if (p.lx < 0x40) b |= kBtnLeft;
        if (p.lx > 0xC0) b |= kBtnRight;
        const uint16_t pressed = static_cast<uint16_t>(b & ~g_prevButtons);
        g_prevButtons = b;
        // Buttons held through a change of who has the input stay away from the game until released.
        g_suppress &= b;
        const bool hotkey = (hotkeyDown && !g_prevHotkey) ||
                            ((pressed & (kBtnSelect | kBtnR1)) && (b & (kBtnSelect | kBtnR1)) == (kBtnSelect | kBtnR1));
        g_prevHotkey = hotkeyDown;
        auto takeInput = [&]()
        {
            g_suppress |= b;
            p.buttons = 0;
            p.lx = p.ly = p.rx = p.ry = 0x80;
            p.l2 = p.r2 = 0;
        };

        if (g_open.load())
        {
            std::lock_guard<std::mutex> lock(g_mx);
            const int n = static_cast<int>(g_mods.size());
            // Held UP / DOWN repeat after 0.4 s, every 0.09 s.
            uint16_t nav = pressed & (kBtnUp | kBtnDown);
            const uint16_t held = b & (kBtnUp | kBtnDown);
            if (nav)
            {
                g_repeatButton = nav;
                g_repeatAt = nowSec() + 0.4;
            }
            else if (held && held == g_repeatButton && nowSec() >= g_repeatAt)
            {
                nav = held;
                g_repeatAt = nowSec() + 0.09;
            }
            if (n > 0)
            {
                if (nav & kBtnUp)
                    g_cursor = (g_cursor + n - 1) % n;
                if (nav & kBtnDown)
                    g_cursor = (g_cursor + 1) % n;
                if (pressed & (kBtnCross | kBtnLeft | kBtnRight))
                {
                    Mod &m = g_mods[static_cast<size_t>(g_cursor)];
                    m.enabled = !m.enabled;
                    writeEnabledLocked();
                }
                g_scroll = std::clamp(g_scroll, std::max(0, g_cursor - 7), g_cursor);
                g_scroll = std::clamp(g_scroll, 0, std::max(0, n - 8));
            }
            if (pressed)
                g_dirty.fetch_add(1);
            if (hotkey || (pressed & (kBtnCircle | kBtnTriangle | kBtnStart)))
            {
                g_open.store(false);
                g_focus.store(g_openFromPlank.load() && mainMenuShown());
                g_dirty.fetch_add(1);
            }
            takeInput();
            return;
        }
        if (hotkey)
        {
            std::lock_guard<std::mutex> lock(g_mx);
            scanLocked();
            g_openFromPlank.store(g_focus.load());
            g_open.store(true);
            g_dirty.fetch_add(1);
            takeInput();
            return;
        }
        if (g_focus.load())
        {
            if (!mainMenuShown())
            {
                g_focus.store(false);
            }
            else if (pressed & kBtnCross)
            {
                std::lock_guard<std::mutex> lock(g_mx);
                scanLocked();
                g_cursor = std::clamp(g_cursor, 0, std::max(0, static_cast<int>(g_mods.size()) - 1));
                g_openFromPlank.store(true);
                g_open.store(true);
                g_dirty.fetch_add(1);
                takeInput();
                return;
            }
            else if (pressed & (kBtnLeft | kBtnCircle | kBtnTriangle))
            {
                g_focus.store(false);
                takeInput();
                return;
            }
            else if (pressed & (kBtnUp | kBtnDown))
            {
                // Back to the game's menu: the press moves its highlight on from "options".
                g_focus.store(false);
                p.buttons = static_cast<uint16_t>(p.buttons & (kBtnUp | kBtnDown));
                g_suppress |= static_cast<uint16_t>(b & ~(kBtnUp | kBtnDown));
                return;
            }
            if (g_focus.load())
            {
                takeInput();
                return;
            }
        }
        else if ((pressed & kBtnRight) && mainMenuShown() && g_sel.load() == 4)
        {
            g_focus.store(true);
            takeInput();
            return;
        }
        // The game has the input; buttons still held from the menu stay masked until released.
        p.buttons = static_cast<uint16_t>(p.buttons & ~g_suppress);
    }

    // The menu panel as premultiplied BGRA (redrawn when something changed). False: nothing to show.
    bool panel(uint32_t outH, const uint32_t *&px, uint32_t &w, uint32_t &h, bool &changed)
    {
        changed = false;
        if (!featureOn() || !g_open.load())
            return false;
        const uint32_t gen = g_dirty.load();
        if (gen != g_panel.gen || outH != g_panel.outH)
        {
            std::vector<Row> rows;
            int cursor, scroll;
            {
                std::lock_guard<std::mutex> lock(g_mx);
                for (const Mod &m : g_mods)
                    rows.push_back({m.name, m.description, m.source, m.enabled, m.missing});
                g_cursor = std::clamp(g_cursor, 0, std::max(0, static_cast<int>(rows.size()) - 1));
                cursor = g_cursor;
                scroll = g_scroll;
            }
            renderPanel(outH, rows, cursor, scroll, g_panel.px, g_panel.w, g_panel.h);
            g_panel.gen = gen;
            g_panel.outH = outH;
            changed = true;
        }
        if (!g_panel.w || !g_panel.h)
            return false;
        px = g_panel.px.data();
        w = g_panel.w;
        h = g_panel.h;
        return true;
    }
}
