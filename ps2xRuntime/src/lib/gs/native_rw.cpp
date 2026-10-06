// Native RenderWare rendering: EE-side hooks and VIF marker handling (see runtime/native_rw.h).
#include "runtime/native_rw.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"

#include <algorithm>
#include <array>
#include <vector>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

bool ps2GsThreadEnabled();                                         // ps2_runtime.cpp
void ps2GsThreadSubmit(GS &gs, const uint8_t *data, uint32_t size); // ps2_runtime.cpp

namespace nrw
{
    namespace
    {
        struct Entry
        {
            uint32_t object, mesh, meshFlags, primType, transType, matCol, texture, vu1CodeIndex, surfProps, code;
            uint32_t kind, verts, normals, uvs, prelit, numVerts, geomFlags;
        };
        uint32_t g_sectorCode = 0; // VU1 code pointer of RenderWare's generic PS2All program (seen on sectors)
        constexpr uint32_t kTableSize = 1u << 16;
        std::array<Entry, kTableSize> g_table{};
        std::atomic<uint32_t> g_next{0};
        uint32_t g_curSector = 0;
        GS *g_gs = nullptr;
        const uint8_t *g_rdram = nullptr;

        uint32_t rd32(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (a & 0x1FFFFFFu), 4);
            return v;
        }
        uint64_t rd64(const uint8_t *rdram, uint32_t a)
        {
            uint64_t v;
            std::memcpy(&v, rdram + (a & 0x1FFFFFFu), 8);
            return v;
        }

        const char *vifName(uint32_t op)
        {
            switch (op)
            {
            case 0x00: return "NOP";
            case 0x01: return "STCYCL";
            case 0x02: return "OFFSET";
            case 0x03: return "BASE";
            case 0x04: return "ITOP";
            case 0x05: return "STMOD";
            case 0x06: return "MSKPATH3";
            case 0x07: return "MARK";
            case 0x10: return "FLUSHE";
            case 0x11: return "FLUSH";
            case 0x13: return "FLUSHA";
            case 0x14: return "MSCAL";
            case 0x15: return "MSCALF";
            case 0x17: return "MSCNT";
            case 0x20: return "STMASK";
            case 0x30: return "STROW";
            case 0x31: return "STCOL";
            case 0x4A: return "MPG";
            case 0x50: return "DIRECT";
            case 0x51: return "DIRECTHL";
            default: return (op & 0x60) == 0x60 ? "UNPACK" : "?";
            }
        }

        // Print the VIF codes in a run of words (data words skipped using each code's payload size).
        void dumpVifWords(const uint8_t *rdram, uint32_t addr, uint32_t words, uint32_t &cl, uint32_t &wl)
        {
            uint32_t i = 0;
            while (i < words)
            {
                const uint32_t code = rd32(rdram, addr + i * 4u);
                const uint32_t op = (code >> 24) & 0x7F, num = (code >> 16) & 0xFF, imm = code & 0xFFFF;
                ++i;
                uint32_t payload = 0;
                if (op == 0x01)
                {
                    cl = imm & 0xFF;
                    wl = (imm >> 8) & 0xFF;
                }
                else if (op == 0x20)
                    payload = 1;
                else if (op == 0x30 || op == 0x31)
                    payload = 4;
                else if (op == 0x4A)
                    payload = (num ? num : 256u) * 2u;
                else if (op == 0x50 || op == 0x51)
                    payload = (imm ? imm : 65536u) * 4u;
                else if ((op & 0x60) == 0x60)
                {
                    const uint32_t vn = (op >> 2) & 3, vl = op & 3;
                    const uint32_t n = num ? num : 256u;
                    const uint32_t bits = (32u >> vl) * (vn + 1u);
                    uint32_t writes = n;
                    if (wl && cl && cl < wl)
                        writes = n; // (fill writes consume no data; rare in RW)
                    payload = (writes * bits + 31u) / 32u;
                }
                std::printf("      %s", vifName(op));
                if ((op & 0x60) == 0x60)
                    std::printf("(vn=%u vl=%u num=%u addr=%u%s%s)", (op >> 2) & 3, op & 3, num, imm & 0x3FF, (imm & 0x8000) ? " tops" : "",
                                (op & 0x10) ? " mask" : "");
                else if (op == 0x14 || op == 0x15 || op == 0x06 || op == 0x07 || op == 0x02 || op == 0x03 || op == 0x04 || op == 0x05 || op == 0x01)
                    std::printf("(%04x)", imm);
                else if (op == 0x50 || op == 0x51)
                    std::printf("(%u qw)", imm);
                std::printf("\n");
                i += payload;
            }
        }

        // Walk a DMA chain entered by CALL at addr until the matching RET (TTE: tag VIF words included).
        void dumpCallChain(const uint8_t *rdram, uint32_t addr)
        {
            uint32_t cl = 1, wl = 1;
            uint32_t stack[4];
            int depth = 0;
            for (int n = 0; n < 64; ++n)
            {
                const uint64_t tag = rd64(rdram, addr);
                const uint32_t qwc = static_cast<uint32_t>(tag & 0xFFFF);
                const uint32_t id = static_cast<uint32_t>((tag >> 28) & 7);
                const uint32_t ta = static_cast<uint32_t>((tag >> 32) & 0x7FFFFFF0u);
                std::printf("    tag @%08x id=%u qwc=%u addr=%08x\n", addr, id, qwc, ta);
                dumpVifWords(rdram, addr + 8u, 2u, cl, wl);
                uint32_t data = addr + 16u, next = data + qwc * 16u;
                switch (id)
                {
                case 1: break;                                    // CNT
                case 2: next = ta; break;                          // NEXT
                case 3: case 4: data = ta; next = addr + 16u; break; // REF / REFS
                case 5: if (depth < 4) stack[depth++] = next; next = ta; break; // CALL
                case 6: // RET
                    if (qwc)
                        dumpVifWords(rdram, data, qwc * 4u, cl, wl);
                    if (depth == 0)
                        return;
                    next = stack[--depth];
                    addr = next;
                    continue;
                case 0: case 7: // REFE / END
                    if (id == 0)
                        data = ta;
                    if (qwc)
                        dumpVifWords(rdram, data, qwc * 4u, cl, wl);
                    return;
                default: return;
                }
                if (qwc)
                    dumpVifWords(rdram, data, qwc * 4u, cl, wl);
                addr = next;
            }
        }
    }

    // ---- Native object constants -------------------------------------------------------------------
    // What RenderWare's PS2All object setup uploads to VU1 per object, computed from the RW structures
    // (verified against the real uploads with PS2X_NRW_OBJVERIFY=1):
    //   lightOffset..: light block (RxPipelineNodePS2AllApplyLight) or the node's 2-qword null block
    //   1008..1011   : rows of LTM x camera->viewMatrix (sectors: viewMatrix) as {x, y, z, z}
    //   1012..1013   : zero
    //   1014..1018   : the per-camera block RenderWare keeps at 0x4BE1B0
    namespace
    {
        constexpr uint32_t kRwGlobals = 0x4B8C50u;   // +0 curCamera, +4 curWorld, +0xA lightFrame
        constexpr uint32_t kSkyCamBlock = 0x4BE1B0u; // 5 qwords -> VU1 1014..1018

        struct Mat
        {
            float r[4][4]; // right, up, at, pos (w lane of right = flags)
            uint32_t flags() const
            {
                uint32_t f;
                std::memcpy(&f, &r[0][3], 4);
                return f;
            }
        };

        Mat readMat(const uint8_t *ee, uint32_t a)
        {
            Mat m;
            std::memcpy(&m, ee + (a & 0x1FFFFF0u), 64);
            return m;
        }

        // RwMatrixMultiply (VU0): row_i = b.right * a_i.x + b.up * a_i.y + b.at * a_i.z (+ b.pos for pos).
        void matMul(Mat &d, const Mat &a, const Mat &b)
        {
            for (int i = 0; i < 4; ++i)
                for (int k = 0; k < 3; ++k)
                {
                    float v = b.r[0][k] * a.r[i][0];
                    v = v + b.r[1][k] * a.r[i][1];
                    v = v + b.r[2][k] * a.r[i][2];
                    if (i == 3)
                        v = v + b.r[3][k];
                    d.r[i][k] = v;
                }
            const uint32_t f = a.flags() & b.flags();
            std::memcpy(&d.r[0][3], &f, 4);
        }

        // MatrixInvertOrthoNormalized: transpose, pos' = -(p.x*c.x + p.y*c.y + p.z*c.z) per column.
        void invertOrtho(Mat &d, const Mat &m)
        {
            for (int i = 0; i < 3; ++i)
                for (int k = 0; k < 3; ++k)
                    d.r[i][k] = m.r[k][i];
            for (int k = 0; k < 3; ++k)
            {
                float v = m.r[3][0] * m.r[k][0];
                v = v + m.r[3][1] * m.r[k][1];
                v = m.r[3][2] * m.r[k][2] + v;
                d.r[3][k] = -v;
            }
            const uint32_t f = 3u;
            std::memcpy(&d.r[0][3], &f, 4);
        }

        // General inverse (adjugate / determinant), RenderWare row-vector layout.
        void invertGeneric(Mat &d, const Mat &m)
        {
            const float(*a)[4] = m.r;
            const float c00 = a[1][1] * a[2][2] - a[1][2] * a[2][1];
            const float c01 = a[1][2] * a[2][0] - a[1][0] * a[2][2];
            const float c02 = a[1][0] * a[2][1] - a[1][1] * a[2][0];
            const float det = a[0][0] * c00 + a[0][1] * c01 + a[0][2] * c02;
            const float id = det != 0.0f ? 1.0f / det : 0.0f;
            d.r[0][0] = c00 * id;
            d.r[1][0] = c01 * id;
            d.r[2][0] = c02 * id;
            d.r[0][1] = (a[2][1] * a[0][2] - a[2][2] * a[0][1]) * id;
            d.r[1][1] = (a[2][2] * a[0][0] - a[2][0] * a[0][2]) * id;
            d.r[2][1] = (a[2][0] * a[0][1] - a[2][1] * a[0][0]) * id;
            d.r[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * id;
            d.r[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * id;
            d.r[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * id;
            for (int k = 0; k < 3; ++k)
                d.r[3][k] = -(a[3][0] * d.r[0][k] + a[3][1] * d.r[1][k] + a[3][2] * d.r[2][k]);
            const uint32_t f = 0u;
            std::memcpy(&d.r[0][3], &f, 4);
        }

        void matInvert(Mat &d, const Mat &m)
        {
            if ((m.flags() & 3u) == 3u)
                invertOrtho(d, m);
            else
                invertGeneric(d, m);
        }

        struct LightBlock
        {
            float q[124][4];
            int count = 0;
            bool unsupported = false;
        };

        void putq(float *q, float x, float y, float z, float w)
        {
            q[0] = x;
            q[1] = y;
            q[2] = z;
            q[3] = w;
        }

        // RxPipelineNodePS2AllApplyLight: header (inverse matrix rows) on the first light, then per light
        // {rgb, type} + direction (directional) / position + radius (point). Spot lights are not handled.
        void applyLight(const uint8_t *ee, uint32_t light, const Mat *inv, float invScale, float recip, LightBlock &lb)
        {
            if (lb.count >= 0x7C)
                return;
            if (lb.count == 0)
            {
                if (inv)
                {
                    putq(lb.q[0], inv->r[0][0], inv->r[0][1], inv->r[0][2], invScale);
                    putq(lb.q[1], inv->r[1][0], inv->r[1][1], inv->r[1][2], invScale);
                    putq(lb.q[2], inv->r[2][0], inv->r[2][1], inv->r[2][2], invScale);
                    putq(lb.q[3], inv->r[3][0], inv->r[3][1], inv->r[3][2], recip);
                }
                else
                {
                    putq(lb.q[0], 1.0f, 0.0f, 0.0f, invScale);
                    putq(lb.q[1], 0.0f, 1.0f, 0.0f, invScale);
                    putq(lb.q[2], 0.0f, 0.0f, 1.0f, invScale);
                    putq(lb.q[3], 0.0f, 0.0f, 0.0f, recip);
                }
                lb.count = 4;
            }
            const uint32_t type = ee[(light + 1u) & 0x1FFFFFFu];
            float col[3];
            std::memcpy(col, ee + ((light + 24u) & 0x1FFFFFFu), 12);
            float *q = lb.q[lb.count];
            q[0] = col[0];
            q[1] = col[1];
            q[2] = col[2];
            std::memcpy(&q[3], &type, 4);
            const uint32_t frame = rd32(ee, light + 4u);
            Mat ltm{};
            if (frame)
                ltm = readMat(ee, frame + 80u);
            switch (type)
            {
            case 1: // directional: light frame "at"
                putq(lb.q[lb.count + 1], ltm.r[2][0], ltm.r[2][1], ltm.r[2][2], 0.0f);
                lb.count += 2;
                break;
            case 2: // ambient
                lb.count += 1;
                break;
            case 0x80: // point: position, radius
            {
                float radius;
                std::memcpy(&radius, ee + ((light + 20u) & 0x1FFFFFFu), 4);
                putq(lb.q[lb.count + 1], ltm.r[3][0], ltm.r[3][1], ltm.r[3][2], radius);
                lb.count += 2;
                break;
            }
            default:
                lb.unsupported = true;
                break;
            }
        }

        // RenderWare's light gathering for an atomic (RpAtomicPS2AllLightingSetupFunc) or a world sector
        // (RpWorldSectorPS2AllLightingSetupFunc). false: lighting is off for the object (null block).
        bool gatherLights(const uint8_t *ee, uint32_t object, bool sector, LightBlock &lb)
        {
            const uint32_t world = rd32(ee, kRwGlobals + 4u);
            if (!world)
                return false;
            Mat inv{};
            float invScale = 1.0f, recip = 1.0f;
            const uint32_t flagBit = sector ? 2u : 1u; // rpLIGHTLIGHTWORLD / rpLIGHTLIGHTATOMICS
            if (sector)
            {
                if ((rd32(ee, world + 8u) & 0x20u) == 0u) // rpWORLDLIGHT
                    return false;
            }
            else
            {
                const uint32_t geom = rd32(ee, object + 24u);
                if (!geom || (rd32(ee, geom + 8u) & 0x20u) == 0u) // rpGEOMETRYLIGHT
                    return false;
                const uint32_t frame = rd32(ee, object + 4u);
                if (!frame)
                    return false;
                matInvert(inv, readMat(ee, frame + 80u));
                if ((inv.flags() & 3u) != 3u)
                {
                    const float s2 = inv.r[2][0] * inv.r[2][0] + inv.r[2][1] * inv.r[2][1] + inv.r[2][2] * inv.r[2][2];
                    invScale = std::sqrt(s2);
                    if (invScale > 0.0f)
                        recip = 1.0f / invScale;
                }
            }
            const Mat *invp = sector ? nullptr : &inv;
            // Global lights: curWorld->directionalLightList (RpLight.inWorld at +52).
            const uint32_t head = world + 60u;
            uint32_t node = rd32(ee, head);
            for (int n = 0; node != head && node && n < 64; ++n, node = rd32(ee, node))
            {
                const uint32_t light = node - 52u;
                if (ee[(light + 2u) & 0x1FFFFFFu] & flagBit)
                    applyLight(ee, light, invp, invScale, recip, lb);
            }
            // Local lights of the sector(s): sector+0x48 list of RpLightTie {link, light}. An atomic walks its
            // RpTie list (atomic+100, sector at node+8); each light once per object.
            uint32_t seen[32];
            int nseen = 0;
            auto sectorLights = [&](uint32_t sec)
            {
                const uint32_t lh = sec + 0x48u;
                uint32_t ln = rd32(ee, lh);
                for (int n = 0; ln != lh && ln && n < 64; ++n, ln = rd32(ee, ln))
                {
                    const uint32_t light = rd32(ee, ln + 8u);
                    if (!light || !(ee[(light + 2u) & 0x1FFFFFFu] & flagBit))
                        continue;
                    bool dup = false;
                    for (int i = 0; i < nseen; ++i)
                        dup |= seen[i] == light;
                    if (dup)
                        continue;
                    if (nseen < 32)
                        seen[nseen++] = light;
                    applyLight(ee, light, invp, invScale, recip, lb);
                }
            };
            if (sector)
                sectorLights(object);
            else
            {
                const uint32_t th = object + 100u;
                uint32_t tn = rd32(ee, th);
                for (int n = 0; tn != th && tn && n < 64; ++n, tn = rd32(ee, tn))
                    if (const uint32_t sec = rd32(ee, tn + 8u))
                        sectorLights(sec);
            }
            return true;
        }

        struct ObjExpect
        {
            float vu[kVuQwCount][4];
            uint64_t mask = 0; // bit i: VU1 kVuFirstQw + i was computed
            uint32_t setupCB = 0;
            bool unsupported = false;
        };
        constexpr uint32_t kExpectRing = 4096u;
        ObjExpect g_objExpect[kExpectRing];

        // Fill ex with the object constants RenderWare would upload for this object.
        void computeObjectConstants(const uint8_t *ee, uint32_t object, bool sector, uint32_t objPvt, ObjExpect &ex)
        {
            ex.mask = 0;
            ex.unsupported = false;
            const uint32_t cam = rd32(ee, kRwGlobals);
            if (!cam)
            {
                ex.unsupported = true;
                return;
            }
            const Mat view = readMat(ee, cam + 32u);
            Mat t;
            if (sector)
                t = view;
            else
            {
                const uint32_t frame = rd32(ee, object + 4u);
                if (!frame)
                {
                    ex.unsupported = true;
                    return;
                }
                matMul(t, readMat(ee, frame + 80u), view);
            }
            for (int i = 0; i < 4; ++i)
            {
                putq(ex.vu[vuIndex(1008u + i)], t.r[i][0], t.r[i][1], t.r[i][2], t.r[i][2]);
                ex.mask |= 1ull << vuIndex(1008u + i);
            }
            putq(ex.vu[vuIndex(1012)], 0, 0, 0, 0);
            putq(ex.vu[vuIndex(1013)], 0, 0, 0, 0);
            ex.mask |= (1ull << vuIndex(1012)) | (1ull << vuIndex(1013));
            for (uint32_t i = 0; i < 5; ++i)
            {
                std::memcpy(ex.vu[vuIndex(1014u + i)], ee + ((kSkyCamBlock + i * 16u) & 0x1FFFFFFu), 16);
                ex.mask |= 1ull << vuIndex(1014u + i);
            }
            const uint32_t lightOffset = rd32(ee, objPvt + 28u);
            static LightBlock lb;
            lb.count = 0;
            lb.unsupported = false;
            const bool lit = gatherLights(ee, object, sector, lb);
            if (lb.unsupported)
                ex.unsupported = true;
            // The node's null block is a VIF UNPACK header + data: for an unlit object only the light list
            // terminator (VU1 980) is written. A lit object gets header + lights + that terminator.
            const uint32_t nullBlock = rd32(ee, objPvt + 36u);
            if (lightOffset >= kVuFirstQw && lightOffset < 1008u)
            {
                if (lit && lb.count > 0)
                {
                    for (int i = 0; i < lb.count && lightOffset + i < 1008u; ++i)
                    {
                        std::memcpy(ex.vu[vuIndex(lightOffset + i)], lb.q[i], 16);
                        ex.mask |= 1ull << vuIndex(lightOffset + i);
                    }
                    const uint32_t term = lightOffset + static_cast<uint32_t>(lb.count);
                    if (nullBlock && term < 1008u)
                    {
                        std::memcpy(ex.vu[vuIndex(term)], ee + ((nullBlock + 16u) & 0x1FFFFFFu), 16);
                        ex.mask |= 1ull << vuIndex(term);
                    }
                }
                else if (nullBlock)
                {
                    const uint32_t hdr = rd32(ee, nullBlock + 12u); // UNPACK: num << 16 | addr
                    const uint32_t addr = hdr & 0x3FFu, num = (hdr >> 16) & 0xFFu;
                    for (uint32_t i = 0; i < num && addr + i >= kVuFirstQw && addr + i < 1008u; ++i)
                    {
                        std::memcpy(ex.vu[vuIndex(addr + i)], ee + ((nullBlock + 16u + i * 16u) & 0x1FFFFFFu), 16);
                        ex.mask |= 1ull << vuIndex(addr + i);
                    }
                }
            }
        }

        // What RpMeshPS2AllBridgeCallBack uploads per mesh (VU1 1018..1023). Globals are gp-relative.
        constexpr uint32_t kGp = 0x497870u;
        constexpr uint32_t kSkyGifTag = 0x4BE100u; // GIF tag template (PRIM patched per mesh)
        void computeMeshConstants(const uint8_t *ee, uint32_t pipeData, ObjExpect &ex)
        {
            auto g32 = [&](uint32_t off) { return rd32(ee, kGp - off); };
            auto gf = [&](uint32_t off) { float f; uint32_t u = g32(off); std::memcpy(&f, &u, 4); return f; };
            const uint32_t transType = ee[(pipeData + 40u) & 0x1FFFFFFu];
            const uint32_t primType = ee[(pipeData + 41u) & 0x1FFFFFFu];
            uint8_t col[4];
            std::memcpy(col, ee + ((pipeData + 68u) & 0x1FFFFFFu), 4);
            // 1018: GIF tag with PRIM = skyPrim_State (ABE when the material is translucent or g-0x797C) | primType.
            uint64_t lo, hi;
            std::memcpy(&lo, ee + (kSkyGifTag & 0x1FFFFFFu), 8);
            std::memcpy(&hi, ee + ((kSkyGifTag + 8u) & 0x1FFFFFFu), 8);
            uint64_t primState;
            std::memcpy(&primState, ee + ((kGp - 0x7CC0u) & 0x1FFFFFFu), 8);
            const bool abe = col[3] != 0xFFu || g32(0x797C) != 0u;
            primState = abe ? (primState | 0x40u) : (primState & ~0x40ull);
            const uint64_t prim = (static_cast<uint32_t>(primState) | primType) & 0x7FFu;
            lo = (lo & ~(0x7FFull << 47)) | (prim << 47);
            std::memcpy(&ex.vu[vuIndex(1018)][0], &lo, 8);
            std::memcpy(&ex.vu[vuIndex(1018)][2], &hi, 8);
            ex.mask |= 1ull << vuIndex(1018);
            // 1019: material colour; rgb scale depends on texturing (PRIM TME), alpha always gp-0x7F7C.
            const float s = (primState & 0x10u) ? gf(0x7F7C) : gf(0x7F78);
            putq(ex.vu[vuIndex(1019)], static_cast<float>(col[0]) * s, static_cast<float>(col[1]) * s, static_cast<float>(col[2]) * s,
                 static_cast<float>(col[3]) * gf(0x7F7C));
            ex.mask |= 1ull << vuIndex(1019);
            // 1020: surface properties * 255, w = spExtra.
            const uint32_t sp = rd32(ee, pipeData + 60u);
            float amb = 0, spec = 0, diff = 0, spExtra;
            if (sp)
            {
                std::memcpy(&amb, ee + (sp & 0x1FFFFFFu), 4);
                std::memcpy(&spec, ee + ((sp + 4u) & 0x1FFFFFFu), 4);
                std::memcpy(&diff, ee + ((sp + 8u) & 0x1FFFFFFu), 4);
            }
            std::memcpy(&spExtra, ee + ((pipeData + 28u) & 0x1FFFFFFu), 4);
            const float k2 = gf(0x7F70);
            putq(ex.vu[vuIndex(1020)], amb * k2, spec * k2, diff * k2, spExtra);
            ex.mask |= 1ull << vuIndex(1020);
            // 1021..1023: clip planes (small / large guard band) and flags.
            uint32_t flags = transType & 7u;
            bool large;
            if (g32(0x792C) != 0u && (flags & 4u) == 0u)
                large = true;
            else if (g32(0x7C98) == 0u)
                large = false;
            else
                large = (flags & 4u) != 0u;
            if (large)
                flags |= 8u;
            const uint32_t clipSrc = large ? 0x4BE170u : 0x4BE150u;
            std::memcpy(ex.vu[vuIndex(1021)], ee + (clipSrc & 0x1FFFFFFu), 16);
            std::memcpy(ex.vu[vuIndex(1022)], ee + ((clipSrc + 16u) & 0x1FFFFFFu), 16);
            const uint32_t w2 = g32(0x7988) == 3u ? 0x20u : 0u;
            const uint32_t w[4] = {flags, 0u, w2, g32(0x7930)};
            std::memcpy(ex.vu[vuIndex(1023)], w, 16);
            ex.mask |= (1ull << vuIndex(1021)) | (1ull << vuIndex(1022)) | (1ull << vuIndex(1023));
        }

        bool objVerifyMode()
        {
            static const bool s_on = std::getenv("PS2X_NRW_OBJVERIFY") != nullptr;
            return s_on;
        }

        // Compare the native constants for marker idx with the VU1 data memory RenderWare produced.
        void verifyObjectConstants(uint32_t idx, const MeshJob &job)
        {
            const ObjExpect &ex = g_objExpect[idx & (kExpectRing - 1u)];
            static uint64_t s_checked = 0, s_exact = 0, s_unsup = 0, s_badQw[kVuQwCount] = {};
            static int s_reports = 0;
            ++s_checked;
            if (ex.unsupported)
            {
                ++s_unsup;
                return;
            }
            bool exact = true;
            for (uint32_t i = 0; i < kVuQwCount; ++i)
            {
                if (!(ex.mask & (1ull << i)))
                    continue;
                if (std::memcmp(ex.vu[i], job.vu[i], 16) != 0)
                {
                    // Tolerate float rounding differences (a few ulps) in the computed lanes.
                    bool close = true;
                    for (int k = 0; k < 4; ++k)
                    {
                        const float a = ex.vu[i][k], b = job.vu[i][k];
                        uint32_t ua, ub;
                        std::memcpy(&ua, &a, 4);
                        std::memcpy(&ub, &b, 4);
                        if (ua == ub)
                            continue;
                        const float diff = std::fabs(a - b), mag = std::max(std::fabs(a), std::fabs(b));
                        if (!(diff <= mag * 1e-5f || diff < 1e-6f))
                            close = false;
                    }
                    if (!close)
                    {
                        exact = false;
                        ++s_badQw[i];
                        if (s_reports < 60)
                        {
                            ++s_reports;
                            uint32_t ua[4], ub[4];
                            std::memcpy(ua, ex.vu[i], 16);
                            std::memcpy(ub, job.vu[i], 16);
                            std::printf("[nrw-obj] kind=%u obj=%08x setupCB=%08x vu%u NAT=(%g %g %g %g | %08x %08x %08x %08x) "
                                        "RW=(%g %g %g %g | %08x %08x %08x %08x)\n",
                                        job.kind, job.object, ex.setupCB, kVuFirstQw + i, ex.vu[i][0], ex.vu[i][1], ex.vu[i][2], ex.vu[i][3], ua[0],
                                        ua[1], ua[2], ua[3], job.vu[i][0], job.vu[i][1], job.vu[i][2], job.vu[i][3], ub[0], ub[1], ub[2], ub[3]);
                        }
                    }
                }
            }
            if (exact)
                ++s_exact;
            if ((s_checked % 20000u) == 0u)
            {
                std::printf("[nrw-obj-stats] checked=%llu match=%llu unsupported=%llu badQw:", (unsigned long long)s_checked,
                            (unsigned long long)s_exact, (unsigned long long)s_unsup);
                for (uint32_t i = 0; i < kVuQwCount; ++i)
                    if (s_badQw[i])
                        std::printf(" %u:%llu", kVuFirstQw + i, (unsigned long long)s_badQw[i]);
                std::printf("\n");
                std::fflush(stdout);
            }
        }
    }

    bool enabled()
    {
        static const bool s_on = []()
        {
            const char *v = std::getenv("PS2X_NATIVE_WORLD"); // default on; PS2X_NATIVE_WORLD=0 uses VU1
            return !(v && *v == '0');
        }();
        return s_on;
    }

    namespace
    {
        bool verifyMode()
        {
            static const bool s_v = []()
            {
                const char *v = std::getenv("PS2X_NATIVE_WORLD");
                return v && std::strcmp(v, "verify") == 0;
            }();
            return s_v;
        }

        // Verify mode: the native result for the first vertices of the last marked mesh.
        struct ExpectVtx
        {
            int32_t x, y;
            uint32_t z;
            int32_t r, g, b, a;
            float s, t, q;
            int32_t f;
        };
        constexpr int kExpect = 512; // clip variants: nearest-match over the mesh; first 6 printed
        ExpectVtx g_expect[kExpect];
        int g_expectCount = 0;
        bool g_expectArmed = false;
        uint32_t g_expectFlags = 0;
        uint32_t g_expectLights = 0;
        uint32_t g_expectKind = 0; // bit per light kind present: 1 ambient, 2 directional, 4 point
        int g_verifyReports = 0;

        // CPU twin of vs_native (gs_d3d11_backend.cpp).
        void computeExpected(const MeshJob &job)
        {
            const uint8_t *ee = g_rdram;
            auto vu = [&](uint32_t a) { return job.vu[vuIndex(a)]; };
            float cols[4][4];
            for (int i = 0; i < 4; ++i)
                std::memcpy(cols[i], vu(1008u + static_cast<uint32_t>(i)), 16);
            for (int i = 0; i < 3; ++i)
                cols[i][2] = 0.0f;
            cols[3][2] = 1.0f;
            const float *scale = vu(1015);
            for (int i = 0; i < 4; ++i)
            {
                cols[i][0] = (cols[i][0] - 0.5f * cols[i][3]) * scale[0];
                cols[i][1] = (cols[i][1] - 0.5f * cols[i][3]) * scale[1];
            }
            cols[3][2] *= scale[2];
            const float off[3] = {vu(1016)[0] + vu(1017)[0], vu(1016)[1] + vu(1017)[1], vu(1016)[2]};
            const float *mat = vu(1019);
            const uint32_t sec = job.object;
            const uint32_t verts = job.verts, uvs = job.uvs, prelit = job.prelit;
            (void)sec;
            const uint32_t indices = rd32(ee, job.mesh);
            const uint32_t n = std::min<uint32_t>(rd32(ee, job.mesh + 4), kExpect);
            g_expectCount = static_cast<int>(n);
            std::memcpy(&g_expectFlags, vu(1023), 4);
            g_expectKind = job.kind;
            {
                float lc[6 * 4], ld[6 * 4];
                const int nl = parseLights(job, lc, ld, 6);
                g_expectLights = 0;
                for (int li = 0; li < nl; ++li)
                    g_expectLights |= 1u << (static_cast<int>(lc[li * 4 + 3]) - 1);
            }
            for (uint32_t k = 0; k < n; ++k)
            {
                uint16_t idx;
                std::memcpy(&idx, ee + ((indices + k * 2u) & 0x1FFFFFFu), 2);
                float pos[3], uv[2] = {0, 0};
                const uint8_t base = job.kind == kAtomicMesh ? 0u : 255u; // see prepareNative
                uint8_t col[4] = {base, base, base, 255};
                std::memcpy(pos, ee + ((verts + idx * 12u) & 0x1FFFFFFu), 12);
                if (uvs)
                    std::memcpy(uv, ee + ((uvs + idx * 8u) & 0x1FFFFFFu), 8);
                if (prelit)
                    std::memcpy(col, ee + ((prelit + idx * 4u) & 0x1FFFFFFu), 4);
                float colf[4] = {static_cast<float>(col[0]), static_cast<float>(col[1]), static_cast<float>(col[2]), static_cast<float>(col[3])};
                {
                    float lc[6 * 4], ld[6 * 4];
                    const int nl = parseLights(job, lc, ld, 6);
                    int8_t nrm[4] = {0, 0, 0, 0};
                    const uint32_t normals = job.normals;
                    if (normals)
                        readNormal(ee, normals, idx, job.kind == kAtomicMesh, nrm);
                    for (int li = 0; li < nl; ++li)
                    {
                        float f = 1.0f;
                        if (lc[li * 4 + 3] == 2.0f)
                            f = std::max(0.0f, (nrm[0] * ld[li * 4] + nrm[1] * ld[li * 4 + 1] + nrm[2] * ld[li * 4 + 2]) / 128.0f);
                        else if (lc[li * 4 + 3] == 3.0f)
                        {
                            const float dx = ld[li * 4] - pos[0], dy = ld[li * 4 + 1] - pos[1], dz = ld[li * 4 + 2] - pos[2];
                            const float d2 = dx * dx + dy * dy + dz * dz;
                            const float inv = d2 > 0.0f ? 1.0f / std::sqrt(d2) : 0.0f;
                            f = std::max(0.0f, (nrm[0] * dx + nrm[1] * dy + nrm[2] * dz) / 128.0f) * std::max(0.0f, inv - ld[li * 4 + 3]);
                        }
                        for (int k = 0; k < 3; ++k)
                            colf[k] += lc[li * 4 + k] * f;
                    }
                }
                float clip[4];
                for (int l = 0; l < 4; ++l)
                    clip[l] = pos[0] * cols[0][l] + pos[1] * cols[1][l] + pos[2] * cols[2][l] + cols[3][l];
                const float q = 1.0f / clip[3];
                ExpectVtx &e = g_expect[k];
                e.x = static_cast<int32_t>((clip[0] * q + off[0]) * 16.0f);
                e.y = static_cast<int32_t>((clip[1] * q + off[1]) * 16.0f);
                e.z = static_cast<uint32_t>(std::max(0.0f, (clip[2] * q + off[2]) * 16.0f)) >> 4;
                e.r = static_cast<int32_t>(std::min(colf[0], 255.0f) * mat[0]);
                e.g = static_cast<int32_t>(std::min(colf[1], 255.0f) * mat[1]);
                e.b = static_cast<int32_t>(std::min(colf[2], 255.0f) * mat[2]);
                e.a = static_cast<int32_t>(std::min(colf[3], 255.0f) * mat[3]);
                {
                    const float fz = vu(1014)[2], fw = vu(1014)[3], lo = fz * fw;
                    const float v = std::min(std::max(clip[3] * fz, std::min(lo, lo + 255.0f)), std::max(lo, lo + 255.0f));
                    e.f = (g_expectFlags & 1u) ? static_cast<int32_t>(v - lo) : -1;
                }
                e.s = uv[0] * q;
                e.t = uv[1] * q;
                e.q = q;
            }
            // Only the no-clip output routines emit the strip 1:1 (the clip ones re-emit triangles).
            g_expectArmed = n > 0 && (g_expectFlags & 2u) == 0u;
            static const bool s_all = std::getenv("PS2X_NRW_VERIFY_CLIP") != nullptr;
            if (s_all)
                g_expectArmed = n > 0;
        }
    }

    void observeKick(const uint8_t *data, uint32_t size)
    {
        if (!g_expectArmed || !data)
            return;
        // Walk GIF tags (PACKED) until a vertex-carrying one; collect STQ / RGBAQ / XYZ(F)2 per vertex.
        uint32_t off = 0;
        while (off + 16u <= size)
        {
            uint64_t tag, regs;
            std::memcpy(&tag, data + off, 8);
            std::memcpy(&regs, data + off + 8, 8);
            const uint32_t nloop = static_cast<uint32_t>(tag & 0x7FFF);
            const uint32_t flg = static_cast<uint32_t>((tag >> 58) & 3);
            uint32_t nreg = static_cast<uint32_t>((tag >> 60) & 0xF);
            if (nreg == 0)
                nreg = 16;
            off += 16u;
            if (flg != 0)
            {
                off += (flg == 1 ? (nloop * nreg + 1u) / 2u : nloop) * 16u;
                continue;
            }
            bool hasVertex = false;
            for (uint32_t r = 0; r < nreg; ++r)
            {
                const uint32_t d = static_cast<uint32_t>((regs >> (4 * r)) & 0xF);
                hasVertex |= d == 4 || d == 5;
            }
            if (!hasVertex)
            {
                off += nloop * nreg * 16u;
                continue;
            }
            static uint32_t s_perFlags[256] = {};
            const uint32_t key = (g_expectFlags & 15u) | (g_expectLights << 4) | ((g_expectKind & 1u) << 7);
            if (g_verifyReports < 60 && s_perFlags[key] < 3u)
            {
                ++g_verifyReports;
                ++s_perFlags[key];
                const uint64_t prim = (tag >> 47) & 0x7FF;
                std::printf("[nrw-verify] kind=%u lights=%x flags=%04x prim=%03llx nloop=%u nreg=%u regs=%016llx\n", g_expectKind, g_expectLights, g_expectFlags & 0xFFFF,
                            static_cast<unsigned long long>(prim), nloop, nreg, static_cast<unsigned long long>(regs));
                for (int k = 0; k < std::min(g_expectCount, 6) && static_cast<uint32_t>(k) < nloop; ++k)
                {
                    int32_t x = 0, y = 0, rr = 0, gg = 0, bb = 0, aa = 0, fog = -1;
                    uint32_t z = 0;
                    float ss = 0, tt = 0, qq = 0;
                    for (uint32_t r = 0; r < nreg; ++r)
                    {
                        const uint8_t *q = data + off + (k * nreg + r) * 16u;
                        const uint32_t d = static_cast<uint32_t>((regs >> (4 * r)) & 0xF);
                        uint32_t w[4];
                        std::memcpy(w, q, 16);
                        if (d == 2)
                        {
                            std::memcpy(&ss, &w[0], 4);
                            std::memcpy(&tt, &w[1], 4);
                            std::memcpy(&qq, &w[2], 4);
                        }
                        else if (d == 1)
                        {
                            rr = w[0] & 0xFF;
                            gg = w[1] & 0xFF;
                            bb = w[2] & 0xFF;
                            aa = w[3] & 0xFF;
                        }
                        else if (d == 4)
                        {
                            x = w[0] & 0xFFFF;
                            y = w[1] & 0xFFFF;
                            z = (w[2] >> 4) & 0xFFFFFF;
                            fog = (w[3] >> 4) & 0xFF;
                        }
                        else if (d == 5)
                        {
                            x = w[0] & 0xFFFF;
                            y = w[1] & 0xFFFF;
                            z = w[2];
                        }
                    }
                    const ExpectVtx &e = g_expect[k];
                    std::printf("  v%d VU  xyz=(%d %d %u) f=%d rgba=(%d %d %d %d) stq=(%.4f %.4f %.6f)\n", k, x, y, z, fog, rr, gg, bb, aa, ss,
                                tt, qq);
                    std::printf("  v%d NAT xyz=(%d %d %u) f=%d rgba=(%d %d %d %d) stq=(%.4f %.4f %.6f)\n", k, e.x & 0xFFFF, e.y & 0xFFFF, e.z, e.f,
                                e.r, e.g, e.b, e.a, e.s, e.t, e.q);
                }
                if (g_expectFlags & 2u)
                {
                    // Clip variants re-emit (and clip) triangles: match each VU vertex to the nearest native one.
                    uint32_t exact = 0, near1 = 0, far = 0;
                    for (uint32_t k = 0; k < nloop; ++k)
                    {
                        int32_t x = -1, y = -1;
                        uint32_t z = 0;
                        for (uint32_t r = 0; r < nreg; ++r)
                        {
                            const uint32_t d = static_cast<uint32_t>((regs >> (4 * r)) & 0xF);
                            uint32_t w[4];
                            std::memcpy(w, data + off + (k * nreg + r) * 16u, 16);
                            if (d == 4 || d == 5)
                            {
                                x = w[0] & 0xFFFF;
                                y = w[1] & 0xFFFF;
                                z = d == 4 ? (w[2] >> 4) & 0xFFFFFF : w[2];
                            }
                        }
                        int best = 1 << 30, bi = -1;
                        for (int e = 0; e < g_expectCount; ++e)
                        {
                            const int dd = std::abs((g_expect[e].x & 0xFFFF) - x) + std::abs((g_expect[e].y & 0xFFFF) - y);
                            if (dd < best)
                                best = dd, bi = e;
                        }
                        if (best == 0 && bi >= 0 && g_expect[bi].z == z)
                            ++exact;
                        else if (best <= 2)
                            ++near1;
                        else
                        {
                            if (far < 3 && bi >= 0)
                                std::printf("    far: VU (%d %d %u) nearest NAT (%d %d %u)\n", x, y, z, g_expect[bi].x & 0xFFFF,
                                            g_expect[bi].y & 0xFFFF, g_expect[bi].z);
                            ++far;
                        }
                    }
                    std::printf("  clip-match: verts=%u exact=%u near=%u far=%u (native set %d)\n", nloop, exact, near1, far, g_expectCount);
                }
                std::fflush(stdout);
            }
            g_expectArmed = false;
            return;
        }
    }

    int parseLights(const MeshJob &job, float *colOut, float *dirOut, int maxLights)
    {
        auto vu = [&](uint32_t a) { return job.vu[vuIndex(a)]; };
        const float *surf = vu(1020); // x ambient, y specular, z diffuse
        const float *m0 = vu(976), *m1 = vu(977), *m2 = vu(978);
        const float dscale = -vu(979)[3];
        int n = 0;
        uint32_t a = 980;
        while (a < 1008 && n < maxLights)
        {
            uint32_t w;
            std::memcpy(&w, &vu(a)[3], 4);
            const uint32_t type = w & 0xFFFFu;
            float *c = colOut + n * 4, *d = dirOut + n * 4;
            if (type == 2) // ambient
            {
                for (int k = 0; k < 3; ++k)
                    c[k] = vu(a)[k] * surf[0];
                c[3] = 1.0f;
                d[0] = d[1] = d[2] = d[3] = 0.0f;
                a += 1;
            }
            else if (type == 1) // directional
            {
                const float *dir = vu(a + 1);
                for (int k = 0; k < 3; ++k)
                {
                    c[k] = vu(a)[k] * surf[2];
                    d[k] = (m0[k] * dir[0] + m1[k] * dir[1] + m2[k] * dir[2]) * dscale;
                }
                c[3] = 2.0f;
                d[3] = 0.0f;
                a += 2;
            }
            else if (type == 128) // point: VU handler 0x658
            {
                // col += L * max(0, n.d) * max(0, 1/|d| - 1/R), d = lightPos - pos (object space, unnormalised).
                const float *pos = vu(a + 1), *t = vu(979);
                for (int k = 0; k < 3; ++k)
                {
                    c[k] = vu(a)[k] * surf[2];
                    d[k] = m0[k] * pos[0] + m1[k] * pos[1] + m2[k] * pos[2] + t[k];
                }
                const float r = pos[3] * m0[3];
                c[3] = 3.0f;
                d[3] = r != 0.0f ? 1.0f / r : 0.0f;
                a += 2;
            }
            else
            {
                static int s_logged = 0;
                if (type >= 128u && type <= 130u && s_logged < 8)
                {
                    ++s_logged;
                    std::printf("[nrw] unhandled light type %u (point/spot)\n", type);
                    std::fflush(stdout);
                }
                break;
            }
            ++n;
        }
        return n;
    }

    void setGs(GS *gs) { g_gs = gs; }
    void setRdram(const uint8_t *rdram) { g_rdram = rdram; }
    const uint8_t *rdram() { return g_rdram; }

    void sectorBegin(uint32_t sector) { g_curSector = sector; }
    void sectorEnd() { g_curSector = 0; }

    bool bridge(uint8_t *rdram, uint32_t pipeData, uint64_t callLo, uint64_t callHi, uint64_t &lo, uint64_t &hi,
                bool &insertBefore)
    {
        (void)callHi;
        insertBefore = verifyMode();
        static const bool s_dump = std::getenv("PS2X_NRW_DUMP") != nullptr;
        const uint32_t object = rd32(rdram, pipeData + 0x08);
        // RpWorldSector.type is -1 (RpPlaneSector types are 0..2). Sectors drawn outside the default sector
        // render callback (other worlds, the game's own callbacks) never pass sectorBegin(); their type marks them.
        static const bool s_anySector = []()
        {
            const char *v = std::getenv("PS2X_NATIVE_ATOMICS"); // default on; =0 keeps them on VU1
            return !(v && *v == '0');
        }();
        const bool isSector = object != 0u &&
                              (object == g_curSector || (s_anySector && rd32(rdram, object) == 0xFFFFFFFFu));
        if (s_dump && isSector)
        {
            static int s_dumps = 0;
            if (s_dumps < 3)
            {
                ++s_dumps;
                const uint32_t callAddr = static_cast<uint32_t>((callLo >> 32) & 0x7FFFFFF0u);
                std::printf("[nrw] sector=%08x mesh=%08x prim=%u trans=%u code=%u CALL -> %08x\n", object, rd32(rdram, pipeData + 0x2C),
                            rdram[(pipeData + 0x29) & 0x1FFFFFF], rdram[(pipeData + 0x28) & 0x1FFFFFF], rdram[(pipeData + 0x2B) & 0x1FFFFFF],
                            callAddr);
                dumpCallChain(rdram, callAddr);
                std::fflush(stdout);
            }
        }
        // PS2X_NRW_STATS: where bridge calls go (sector / atomic / why not native).
        static const bool s_stats = std::getenv("PS2X_NRW_STATS") != nullptr;
        static uint64_t s_n[8] = {};
        auto stat = [&](int k)
        {
            if (!s_stats)
                return;
            ++s_n[k];
            if ((++s_n[7] % 20000u) == 0u)
                std::printf("[nrw-stats] sector=%llu atomic=%llu notAtomic=%llu otherCode=%llu morph=%llu noVerts=%llu off=%llu\n",
                            (unsigned long long)s_n[0], (unsigned long long)s_n[1], (unsigned long long)s_n[2], (unsigned long long)s_n[3],
                            (unsigned long long)s_n[4], (unsigned long long)s_n[5], (unsigned long long)s_n[6]);
        };
        if (!enabled() || object == 0u)
        {
            stat(6);
            return false;
        }
        const uint32_t vu1CodeIndex = rdram[(pipeData + 0x2B) & 0x1FFFFFF];
        const uint32_t code = rd32(rdram, rd32(rdram, rd32(rdram, pipeData + 0x04) + 0x18) + vu1CodeIndex * 4u);
        // Vertex source. Sector: RpWorldSector arrays. Atomic (RwObject type 1): RpAtomic.geometry (+24) ->
        // RpGeometry numVertices (+20), numMorphTargets (+24), preLitLum (+48), texCoords[0] (+52),
        // morphTarget (+92) -> verts (+20), normals (+24). Atomics only on the same VU1 program as the sectors,
        // with one morph target; skinned / custom pipelines (their own bridge callbacks) never get here.
        uint32_t kind, verts, normals, uvs, prelit, numVerts, geomFlags = 0;
        if (isSector)
        {
            if (object != g_curSector && code != g_sectorCode)
                return false; // only on the generic program, like atomics
            if (object == g_curSector)
                g_sectorCode = code;
            kind = kWorldSectorMesh;
            stat(0);
            verts = rd32(rdram, object + 0x08);
            normals = rd32(rdram, object + 0x0C);
            uvs = rd32(rdram, object + 0x10);
            prelit = rd32(rdram, object + 0x30);
            uint16_t nv;
            std::memcpy(&nv, rdram + ((object + 0x8A) & 0x1FFFFFFu), 2);
            numVerts = nv;
        }
        else
        {
            static const bool s_atomics = []()
            {
                const char *v = std::getenv("PS2X_NATIVE_ATOMICS"); // default on; =0 keeps them on VU1
                return !(v && *v == '0');
            }();
            if (!s_atomics)
            {
                stat(6);
                return false;
            }
            if (rdram[object & 0x1FFFFFFu] != 1u)
            {
                if (s_stats)
                {
                    static int s_samples = 0;
                    if (s_samples < 12)
                    {
                        ++s_samples;
                        std::printf("[nrw-other] obj=%08x w0=%08x w1=%08x w2=%08x w3=%08x curSector=%08x code=%08x sectorCode=%08x callers ra-chain n/a\n",
                                    object, rd32(rdram, object), rd32(rdram, object + 4), rd32(rdram, object + 8), rd32(rdram, object + 12),
                                    g_curSector, code, g_sectorCode);
                    }
                }
                stat(2);
                return false;
            }
            if (code == 0u || code != g_sectorCode)
            {
                stat(3);
                return false;
            }
            const uint32_t geom = rd32(rdram, object + 24u);
            if (!geom || rd32(rdram, geom + 24u) != 1u || rd32(rdram, pipeData + 0x20) > 1u)
            {
                stat(4);
                return false;
            }
            const uint32_t mt = rd32(rdram, geom + 92u);
            kind = kAtomicMesh;
            geomFlags = rd32(rdram, geom + 8u);
            verts = rd32(rdram, mt + 20u);
            normals = rd32(rdram, mt + 24u);
            prelit = rd32(rdram, geom + 48u);
            uvs = rd32(rdram, geom + 52u);
            numVerts = rd32(rdram, geom + 20u);
            if (!verts || numVerts == 0u || numVerts > 65535u)
            {
                stat(5);
                return false;
            }
            stat(1);
        }
        const uint32_t idx = g_next.fetch_add(1, std::memory_order_relaxed) & (kTableSize - 1u);
        Entry &e = g_table[idx];
        e.kind = kind;
        e.verts = verts;
        e.normals = normals;
        e.uvs = uvs;
        e.prelit = prelit;
        e.numVerts = numVerts;
        e.geomFlags = geomFlags;
        e.object = object;
        e.mesh = rd32(rdram, pipeData + 0x2C);
        const uint32_t meshHeader = rd32(rdram, pipeData + 0x0C);
        e.meshFlags = meshHeader ? rd32(rdram, meshHeader) : 0u;
        e.transType = rdram[(pipeData + 0x28) & 0x1FFFFFF];
        e.primType = rdram[(pipeData + 0x29) & 0x1FFFFFF];
        e.vu1CodeIndex = vu1CodeIndex;
        e.surfProps = rd32(rdram, pipeData + 0x3C);
        e.texture = rd32(rdram, pipeData + 0x40);
        e.matCol = rd32(rdram, pipeData + 0x44);
        e.code = code;
        if (objVerifyMode())
        {
            // Native object constants from the RW structures, compared at the marker with what RW uploaded.
            ObjExpect &ex = g_objExpect[idx & (kExpectRing - 1u)];
            const uint32_t objPvt = rd32(rdram, pipeData + 0x00);
            ex.setupCB = rd32(rdram, objPvt + 16u);
            computeObjectConstants(rdram, object, kind == kWorldSectorMesh, objPvt, ex);
            if (!ex.unsupported)
                computeMeshConstants(rdram, pipeData, ex);
        }
        // DMA CNT, qwc 0; with TTE its upper half goes to VIF1: MARK 0xC0DE, then NOP carrying the index.
        lo = 0x10000000ull;
        hi = (0x07000000ull | kMarkerMark) | (static_cast<uint64_t>(idx & 0xFFFFu) << 32);
        return true;
    }

    void marker(PS2Memory &memory, uint32_t index)
    {
        if (!g_gs)
            return;
        const Entry &e = g_table[index & (kTableSize - 1u)];
        MeshJob job{};
        job.kind = e.kind;
        job.verts = e.verts;
        job.normals = e.normals;
        job.uvs = e.uvs;
        job.prelit = e.prelit;
        job.numVerts = e.numVerts;
        job.geomFlags = e.geomFlags;
        job.object = e.object;
        job.mesh = e.mesh;
        job.meshHeaderFlags = e.meshFlags;
        job.primType = e.primType;
        job.transType = e.transType;
        job.matCol = e.matCol;
        job.texture = e.texture;
        job.vu1CodeIndex = e.vu1CodeIndex;
        job.surfProps = e.surfProps;
        if (const uint8_t *vu = memory.getVU1Data())
            std::memcpy(job.vu, vu + kVuFirstQw * 16u, sizeof(job.vu));
        if (objVerifyMode())
            verifyObjectConstants(index, job);
        if (verifyMode())
        {
            computeExpected(job);
            return; // the original VU1 path still draws this mesh
        }
        static const bool s_dump = std::getenv("PS2X_NRW_DUMP") != nullptr;
        if (s_dump)
        {
            // One line per new (VU1 code, transType, primType, object flags) combination.
            uint32_t flags;
            std::memcpy(&flags, job.vu[vuIndex(1023)], 4);
            static std::vector<uint64_t> s_seen;
            const uint64_t key = (static_cast<uint64_t>(e.code) << 32) ^ (e.transType << 24) ^ (e.primType << 16) ^ (flags & 0xFFFFu);
            if (std::find(s_seen.begin(), s_seen.end(), key) == s_seen.end() && s_seen.size() < 64)
            {
                s_seen.push_back(key);
                uint32_t light0[4];
                std::memcpy(light0, job.vu[vuIndex(976)], 16);
                std::printf("[nrw-variant] code=%08x trans=%u prim=%u flags=%04x sector=%08x mat1019=(%.3f %.3f %.3f %.3f) light976=%08x %08x %08x %08x\n",
                            e.code, e.transType, e.primType, flags & 0xFFFFu, e.object, job.vu[vuIndex(1019)][0], job.vu[vuIndex(1019)][1],
                            job.vu[vuIndex(1019)][2], job.vu[vuIndex(1019)][3], light0[0], light0[1], light0[2], light0[3]);
                std::fflush(stdout);
            }
        }
        const auto *bytes = reinterpret_cast<const uint8_t *>(&job);
        if (ps2GsThreadEnabled())
            ps2GsThreadSubmit(*g_gs, bytes, sizeof(job));
        else
            dispatchPacket(*g_gs, bytes, sizeof(job));
    }

    void dispatchPacket(GS &gs, const uint8_t *data, uint32_t size)
    {
        if (size == sizeof(MeshJob))
        {
            uint64_t magic;
            std::memcpy(&magic, data, 8);
            if (magic == kJobMagic)
            {
                MeshJob job;
                std::memcpy(&job, data, sizeof(job));
                // GIF tag the VU1 program would have kicked (VU1 1018): PRE (bit 46) + PRIM (bits 47..57).
                uint64_t tag;
                std::memcpy(&tag, job.vu[vuIndex(1018)], 8);
                const uint64_t prim = (tag >> 47) & 0x7FFu;
                gs.nativeDraw(prim, &job, sizeof(job));
                return;
            }
        }
        gs.processGIFPacket(data, size);
    }
}
