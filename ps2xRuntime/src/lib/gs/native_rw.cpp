// Native RenderWare rendering: EE-side hooks and VIF marker handling (see runtime/native_rw.h).
#include "runtime/native_rw.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"

#include <algorithm>
#include <array>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

bool ps2GsThreadEnabled();                                         // ps2_runtime.cpp
void ps2GsThreadSubmit(GS &gs, const uint8_t *data, uint32_t size); // ps2_runtime.cpp
extern int g_p3Trace;                                               // ps2_memory.cpp
bool ps2vu1nRunPrivate(const uint8_t *image, uint64_t key, uint8_t *data, uint32_t pc, // vu1_native_glue.cpp
                       void (*onKick)(void *user, const uint8_t *packet, uint32_t bytes), void *user);
bool ps2vu1nRunPrivateTop(const uint8_t *image, uint64_t key, uint8_t *data, uint32_t pc, uint32_t top, uint32_t itop,
                          void (*onKick)(void *user, const uint8_t *packet, uint32_t bytes), void *user);

namespace nrw
{
    namespace
    {
        struct Entry
        {
            uint32_t object, mesh, meshFlags, primType, transType, matCol, texture, vu1CodeIndex, surfProps, code;
            uint32_t kind, verts, normals, uvs, prelit, numVerts, geomFlags;
            bool native; // native node path: constants and GS state below, not VU1 memory / GS registers
            uint64_t gsTest, gsTex1, gsClamp;
            uint32_t geomGen; // bumped whenever the game edited the geometry (vertex buffer cache key)
            uint32_t envTexture, envFlags; // G3x Gem env pass (MeshJob::envTexture..)
            uint32_t ringIndices, numIndices;  // kIm3DMesh
            bool gsFromRegs;                   // native constants, GS state (texture, blending) from the GS registers
            float envCoef, envR[3], envU[3];
            uint64_t envAlpha, envTex1;
        };
        uint64_t g_nodeMarkersWritten = 0; // PS2X_NRW_ENVLOG diagnostics
        // Edit generation per RpGeometry: RpGeometryLock marks lockedSinceLastInst, RenderWare re-instances.
        std::unordered_map<uint32_t, uint32_t> g_geomGen;
        // VU1 constants of native node entries, by marker index (a frame's markers are consumed long before
        // the ring wraps).
        constexpr uint32_t kNativeVuRing = 1u << 14;
        float g_nativeVu[kNativeVuRing][kVuQwCount][4];
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
        constexpr uint32_t kExpectRing = 16384u;
        ObjExpect g_objExpect[kExpectRing];

        // RpPDS_G3x_ADL_LightingUpload: the ambient + directional light the PDS G3x programs light with (set by the
        // game's renderer, VU1-resident 995..997). 0 until the game uploads them.
        uint32_t g_g3xAmbient = 0, g_g3xDirectional = 0;

        // The light block RenderWare's generic program would need for a PDS G3x ADL object: header (inverse LTM),
        // ambient, directional. Same math as the G3x program (object-space direction normalised by the LTM scale,
        // light * surface coefficient * material colour, clamped at 1).
        bool g3xLightBlock(const uint8_t *ee, uint32_t object, LightBlock &lb)
        {
            if (!g_g3xAmbient || !g_g3xDirectional)
                return false;
            const uint32_t frame = rd32(ee, object + 4u);
            if (!frame)
                return false;
            Mat inv{};
            float invScale = 1.0f, recip = 1.0f;
            matInvert(inv, readMat(ee, frame + 80u));
            if ((inv.flags() & 3u) != 3u)
            {
                const float s2 = inv.r[2][0] * inv.r[2][0] + inv.r[2][1] * inv.r[2][1] + inv.r[2][2] * inv.r[2][2];
                invScale = std::sqrt(s2);
                if (invScale > 0.0f)
                    recip = 1.0f / invScale;
            }
            applyLight(ee, g_g3xAmbient, &inv, invScale, recip, lb);
            applyLight(ee, g_g3xDirectional, &inv, invScale, recip, lb);
            return !lb.unsupported;
        }

        // Fill ex with the object constants RenderWare would upload for this object (g3x: RenderWare-generic
        // equivalents of a PDS G3x ADL object's constants).
        void computeObjectConstants(const uint8_t *ee, uint32_t object, bool sector, uint32_t objPvt, ObjExpect &ex,
                                    bool g3x = false, bool unlit = false)
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
            static LightBlock lb;
            lb.count = 0;
            lb.unsupported = false;
            if (g3x)
            {
                if (!g3xLightBlock(ee, object, lb))
                {
                    ex.unsupported = true;
                    return;
                }
                for (int i = 0; i < lb.count && 976 + i < 1007; ++i)
                {
                    std::memcpy(ex.vu[vuIndex(976u + i)], lb.q[i], 16);
                    ex.mask |= 1ull << vuIndex(976u + i);
                }
                putq(ex.vu[vuIndex(976u + lb.count)], 0, 0, 0, 0); // terminator
                ex.mask |= 1ull << vuIndex(976u + lb.count);
                return;
            }
            const uint32_t lightOffset = rd32(ee, objPvt + 28u);
            const bool lit = !unlit && gatherLights(ee, object, sector, lb);
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
        // primState: skyPrim_State with TME set for a textured mesh; texAlpha: the texture raster has alpha
        // (RenderWare's g-0x797C). ABE is derived here like the bridge does.
        void meshConstants(const uint8_t *ee, uint32_t transType, uint32_t primType, const uint8_t col[4], uint64_t primState,
                           bool texAlpha, uint32_t sp, float spExtra, ObjExpect &ex, bool g3x = false)
        {
            auto g32 = [&](uint32_t off) { return rd32(ee, kGp - off); };
            auto gf = [&](uint32_t off) { float f; uint32_t u = g32(off); std::memcpy(&f, &u, 4); return f; };
            // 1018: GIF tag with PRIM = skyPrim_State (ABE when the material is translucent or the texture has
            // alpha) | primType.
            uint64_t lo, hi;
            std::memcpy(&lo, ee + (kSkyGifTag & 0x1FFFFFFu), 8);
            std::memcpy(&hi, ee + ((kSkyGifTag + 8u) & 0x1FFFFFFu), 8);
            const bool abe = col[3] != 0xFFu || texAlpha;
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
            float amb = 0, spec = 0, diff = 0;
            if (sp)
            {
                std::memcpy(&amb, ee + (sp & 0x1FFFFFFu), 4);
                std::memcpy(&spec, ee + ((sp + 4u) & 0x1FFFFFFu), 4);
                std::memcpy(&diff, ee + ((sp + 8u) & 0x1FFFFFFu), 4);
            }
            const float k2 = gf(0x7F70);
            putq(ex.vu[vuIndex(1020)], amb * k2, spec * k2, diff * k2, spExtra);
            ex.mask |= 1ull << vuIndex(1020);
            // 1021..1023: clip planes (small / large guard band) and flags.
            uint32_t flags = transType & 7u;
            bool large;
            if (g3x) // RpPDS_G3x_Generic_MeshPS2AllBridgeCallBack: always the small planes, 1023.z = 0
                large = false;
            else if (g32(0x792C) != 0u && (flags & 4u) == 0u)
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
            const uint32_t w2 = (!g3x && g32(0x7988) == 3u) ? 0x20u : 0u;
            const uint32_t w[4] = {flags, 0u, w2, g32(0x7930)};
            std::memcpy(ex.vu[vuIndex(1023)], w, 16);
            ex.mask |= (1ull << vuIndex(1021)) | (1ull << vuIndex(1022)) | (1ull << vuIndex(1023));
        }

        // The same from RenderWare's own RxPS2AllPipeData at its bridge (verify mode; globals already updated).
        void computeMeshConstants(const uint8_t *ee, uint32_t pipeData, ObjExpect &ex)
        {
            uint8_t col[4];
            std::memcpy(col, ee + ((pipeData + 68u) & 0x1FFFFFFu), 4);
            uint64_t primState;
            std::memcpy(&primState, ee + ((kGp - 0x7CC0u) & 0x1FFFFFFu), 8);
            float spExtra;
            std::memcpy(&spExtra, ee + ((pipeData + 28u) & 0x1FFFFFFu), 4);
            meshConstants(ee, ee[(pipeData + 40u) & 0x1FFFFFFu], ee[(pipeData + 41u) & 0x1FFFFFFu], col, primState,
                          rd32(ee, kGp - 0x797Cu) != 0u, rd32(ee, pipeData + 60u), spExtra, ex);
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

    namespace
    {
        // PS2X_NRW_IM2D=log: the next VU1 kick after an Im2D call is printed next to the call's first vertex.
        int g_im2dLogArmed = 0;
    }

    void observeKick(const uint8_t *data, uint32_t size)
    {
        if (g_im2dLogArmed > 0 && data && size >= 64u)
        {
            --g_im2dLogArmed;
            uint64_t tag, regs;
            std::memcpy(&tag, data, 8);
            std::memcpy(&regs, data + 8, 8);
            uint32_t q[12];
            std::memcpy(q, data + 16, 48);
            float stq[3];
            std::memcpy(stq, q, 12);
            std::printf("[nrw-im2d-kick] tag=%016llx regs=%016llx nloop=%u prim=%03x | STQ=(%g %g %g) RGBA=(%u %u %u %u) XYZF=(%u %u %u %u)\n",
                        (unsigned long long)tag, (unsigned long long)regs, static_cast<uint32_t>(tag & 0x7FFF),
                        static_cast<uint32_t>((tag >> 47) & 0x7FF), stq[0], stq[1], stq[2], q[4], q[5], q[6], q[7], q[8], q[9], q[10], q[11]);
            std::fflush(stdout);
        }
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

    // ---- Native PS2All node body --------------------------------------------------------------------
    namespace
    {
        // PS2X_NRW_NODE: unset/1 native node path, 0 off (RenderWare's pipeline + bridge markers), verify = dry
        // run (RenderWare still draws; the native predictions are checked at its bridge and markers).
        int nodeMode()
        {
            static const int s_mode = []()
            {
                const char *v = std::getenv("PS2X_NRW_NODE");
                if (v && *v == '0')
                    return 0;
                if (v && std::strcmp(v, "verify") == 0)
                    return 2;
                return 1;
            }();
            return s_mode;
        }

        struct NodePred
        {
            uint32_t transType, primType, matCol, texture;
            uint64_t test, tex1, clamp;
            uint32_t cam;
            bool g3x;
            ObjExpect ex;
        };
        std::unordered_map<uint64_t, NodePred> g_preds;

        enum NodeStat
        {
            kNsNative, kNsOtherSetup, kNsFinalize, kNsObject, kNsDirty, kNsPipe, kNsPrim, kNsNoRoom, kNsLight, kNsG3xNoLights,
            kNsLocked, kNsCount
        };
        uint64_t g_nstat[kNsCount] = {};
        // Why meshes stay on RenderWare's pipeline (PS2X_NRW_STATS): 0 unknown pipeline, 1 MatFX line/effect code index,
        // 2 MatFX effect, 3 G3x non-Gem pipeline, 4 Gem env rejected, 5 texture not a plain texture raster.
        uint64_t g_pipeWhy[6] = {};

        void nstat(int k)
        {
            static const bool s_on = std::getenv("PS2X_NRW_STATS") != nullptr;
            ++g_nstat[k];
            uint64_t total = 0;
            for (uint64_t v : g_nstat)
                total += v;
            if (s_on && (total % 20000u) == 0u)
            {
                std::printf("[nrw-node] native=%llu otherSetup=%llu finalize=%llu object=%llu dirty=%llu pipe=%llu prim=%llu noRoom=%llu light=%llu "
                            "g3xNoLights=%llu locked=%llu\n",
                            (unsigned long long)g_nstat[0], (unsigned long long)g_nstat[1], (unsigned long long)g_nstat[2],
                            (unsigned long long)g_nstat[3], (unsigned long long)g_nstat[4], (unsigned long long)g_nstat[5],
                            (unsigned long long)g_nstat[6], (unsigned long long)g_nstat[7], (unsigned long long)g_nstat[8],
                            (unsigned long long)g_nstat[9], (unsigned long long)g_nstat[10]);
                std::printf("[nrw-node-pipe-why] unknown=%llu matfxTrans=%llu matfxEffect=%llu g3xOther=%llu gemEnv=%llu texture=%llu\n",
                            (unsigned long long)g_pipeWhy[0], (unsigned long long)g_pipeWhy[1], (unsigned long long)g_pipeWhy[2],
                            (unsigned long long)g_pipeWhy[3], (unsigned long long)g_pipeWhy[4], (unsigned long long)g_pipeWhy[5]);
                std::fflush(stdout);
            }
        }

        // _rxPS2AllTexFilterASMFunc: TEX1 MMAG/MMIN bits per RwTextureFilterMode.
        uint64_t texFilterBits(uint32_t mode)
        {
            switch (mode)
            {
            case 6: return 0x160;
            case 5: return 0xC0;
            case 4: return 0x120;
            case 3: return 0x80;
            case 2: return 0x60;
            default: return 0;
            }
        }

        // RpAtomicPS2AllFrustumTestFunc / RpWorldSectorPS2AllFrustumTestFunc against the camera's large frustum
        // planes: 2 = inside, 1 = boundary (clip variant). Only picks the clip variant; culling happens earlier.
        // RpAtomicGetWorldBoundingSphere: the cached world sphere (atomic+44), recomputed like RenderWare when the
        // atomic's world bound (private flag 1) or its frame hierarchy (RwFrameDirty) is dirty: centre through
        // the LTM, radius scaled by the longest LTM axis unless the LTM is orthonormal. Written back as RW does.
        // false: needs a morph-target resync (interpolator flag 2), left to RenderWare.
        bool worldSphere(uint8_t *ee, uint32_t object, float sph[4])
        {
            if (rd32(ee, object + 76u) & 2u)
                return false;
            const uint32_t frame = rd32(ee, object + 4u), root = frame ? rd32(ee, frame + 160u) : 0u;
            if (!root)
                return false;
            if ((ee[(object + 3u) & 0x1FFFFFFu] & 1u) || (ee[(root + 3u) & 0x1FFFFFFu] & 3u))
            {
                const Mat ltm = readMat(ee, frame + 80u);
                float c[3], r;
                std::memcpy(c, ee + ((object + 28u) & 0x1FFFFFFu), 12);
                std::memcpy(&r, ee + ((object + 40u) & 0x1FFFFFFu), 4);
                for (int k = 0; k < 3; ++k)
                    sph[k] = ((ltm.r[0][k] * c[0] + ltm.r[1][k] * c[1]) + ltm.r[2][k] * c[2]) + ltm.r[3][k];
                if ((ltm.flags() & 3u) != 3u)
                {
                    float l[3];
                    for (int i = 0; i < 3; ++i)
                    {
                        float v = ltm.r[i][1] * ltm.r[i][1];
                        v = ltm.r[i][0] * ltm.r[i][0] + v;
                        l[i] = ltm.r[i][2] * ltm.r[i][2] + v;
                    }
                    const float m12 = l[1] < l[2] ? l[2] : l[1];
                    const float m = l[0] < m12 ? m12 : l[0];
                    sph[3] = r * std::sqrt(m);
                }
                else
                    sph[3] = r;
                std::memcpy(ee + ((object + 44u) & 0x1FFFFFFu), sph, 16);
                ee[(object + 3u) & 0x1FFFFFFu] &= 0xFEu;
                return true;
            }
            std::memcpy(sph, ee + ((object + 44u) & 0x1FFFFFFu), 16);
            return true;
        }

        uint32_t frustumTest(uint8_t *ee, uint32_t object, bool sector, uint32_t cam)
        {
            const uint32_t planes = cam + rd32(ee, kGp - 0x7990u) + 0x60u;
            float sph[4] = {};
            if (!sector && !worldSphere(ee, object, sph))
                return 1;
            for (uint32_t i = 0; i < 6; ++i)
            {
                const uint32_t pl = planes + i * 20u;
                float n[3], d;
                std::memcpy(n, ee + (pl & 0x1FFFFFFu), 12);
                std::memcpy(&d, ee + ((pl + 12u) & 0x1FFFFFFu), 4);
                if (sector)
                {
                    float p[3];
                    for (uint32_t k = 0; k < 3; ++k)
                    {
                        const uint32_t c = ee[(pl + 16u + k) & 0x1FFFFFFu];
                        std::memcpy(&p[k], ee + ((object + 0x68u + (1u - c) * 12u + k * 4u) & 0x1FFFFFFu), 4);
                    }
                    float v = p[0] * n[0];
                    v = v + p[1] * n[1];
                    v = p[2] * n[2] + v;
                    v = v - d;
                    if (!(v <= 0.0f))
                        return 1;
                }
                else
                {
                    float v = sph[1] * n[1];
                    v = sph[0] * n[0] + v;
                    v = sph[2] * n[2] + v;
                    v = v - d;
                    if (!(v <= -sph[3]))
                        return 1;
                }
            }
            return 2;
        }

        void wr32(uint8_t *ee, uint32_t a, uint32_t v) { std::memcpy(ee + (a & 0x1FFFFFCu), &v, 4); }

        // RenderWare DMA manager globals (gp-relative in its code).
        constexpr uint32_t kDmaPktType = 0x48FDECu, kDmaBufTop = 0x48FE08u, kDmaBufData = 0x48FE0Cu;
        constexpr uint32_t kDmaPktPtr = 0x48FE2Cu, kDmaSprInMem = 0x48FE40u, kDmaSprBase = 0x48FE44u;
        constexpr uint32_t kDmaDirectHdr = 0x48FDF4u; // header tag of an open GIF DIRECT section (back-patched)

        // Room for n marker qwords in RenderWare's open VIF packet (what _rwDMAOpenVIFPkt's fast path checks before
        // letting a caller write into the same packet). A packet may have a GIF DIRECT section open (type flag
        // 0x80000000): its header tag at 0x48FDF4 is back-patched with the section length when the next non-DIRECT
        // writer opens the packet, so anything appended before that becomes GIF data; writeMarkers closes it first
        // like _rwDMAOpenVIFPkt does (an empty section gives its reserved header qword back).
        struct MarkerSlot
        {
            uint32_t pktType = 0, ptr = 0, sprBase = 0, inMem = 0, directHdr = 0, directQwc = 0, inMemAfter = 0;
            bool directOpen = false;
        };
        // Native jobs that sample their texture from VRAM (RwIm3D / ISO / Im2D: GS state from the registers) also get
        // the PATH3 release their RenderWare packet would have carried: masked GIF texture uploads are released one per
        // MSKPATH3 unmask, and every RenderWare batch releases the upload queued for its own draw. Without it the
        // native draw sampled VRAM before its texture arrived once no RenderWare batch happened to sit in between
        // (native ISO + native MatFX Gem: the Match Type panel's sprites). PS2X_NRW_P3RELEASE=0 turns it off.
        bool path3Release()
        {
            static const bool s_on = []()
            {
                const char *v = std::getenv("PS2X_NRW_P3RELEASE");
                return !(v && *v == '0');
            }();
            return s_on;
        }

        bool reserveMarkers(const uint8_t *ee, uint32_t n, MarkerSlot &m)
        {
            m.pktType = rd32(ee, kDmaPktType);
            m.ptr = rd32(ee, kDmaPktPtr);
            m.sprBase = rd32(ee, kDmaSprBase);
            m.inMem = rd32(ee, kDmaSprInMem);
            m.directOpen = (m.pktType & 0x80000000u) != 0u;
            m.directHdr = m.directOpen ? rd32(ee, kDmaDirectHdr) : 0u;
            m.directQwc = m.directOpen ? ((((m.inMem - m.directHdr) + (m.ptr - m.sprBase)) >> 4) - 1u) : 0u;
            m.inMemAfter = (m.directOpen && m.directQwc == 0u) ? m.inMem - 16u : m.inMem;
            const uint32_t need = n * 16u;
            return (m.pktType & 0x7FFFFFFFu) == 0x1101u && !(m.directOpen && !m.directHdr) && (m.ptr - m.sprBase) + need <= 0x2000u &&
                   m.inMemAfter + (m.ptr - m.sprBase) + need < rd32(ee, kDmaBufData);
        }

        // Append one marker per index and advance RenderWare's packet pointer. RenderWare's VIF "packet" is a DMA
        // chain in which every writer starts with its own tag (openVU1SetupPktNew writes CNT qwc first; bridges write
        // CALL tags), so each marker is a tag too: CNT qwc 0 whose TTE upper half carries MARK 0xC0DE + NOP index --
        // the same qword the bridge hook stores in place of a mesh's CALL.
        void writeMarkers(uint8_t *ee, PS2Memory &memory, const MarkerSlot &m, const uint32_t *idxs, uint32_t n,
                          bool releasePath3 = false)
        {
            if (m.directOpen)
            {
                if (m.directQwc != 0u)
                {
                    // DMA CNT qwc, TTE upper half: VIF NOP + DIRECT qwc.
                    const uint32_t hdrTag[4] = {0x10000000u | m.directQwc, 0u, 0u, 0x50000000u | m.directQwc};
                    std::memcpy(ee + (m.directHdr & 0x1FFFFF0u), hdrTag, 16);
                }
                else
                    wr32(ee, kDmaSprInMem, m.inMemAfter);
                wr32(ee, kDmaDirectHdr, 0u);
                wr32(ee, kDmaPktType, m.pktType & 0x7FFFFFFFu);
            }
            uint8_t *dst = (m.ptr & 0xF0000000u) == 0x70000000u ? memory.getScratchpad() + (m.ptr & 0x3FFFu) : ee + (m.ptr & 0x1FFFFFFu);
            uint32_t q = 0;
            if (releasePath3)
            {
                // DMA CNT qwc 0, TTE: MSKPATH3 0 (unmask: release one masked upload), MSKPATH3 1 (mask again).
                const uint32_t w[4] = {0x10000000u, 0u, 0x06000000u, 0x06008000u};
                std::memcpy(dst, w, 16);
                q = 1;
            }
            for (uint32_t i = 0; i < n; ++i, ++q)
            {
                const uint32_t w[4] = {0x10000000u, 0u, 0x07000000u | kMarkerMark, idxs[i] & 0xFFFFu};
                std::memcpy(dst + q * 16u, w, 16);
            }
            wr32(ee, kDmaPktPtr, m.ptr + q * 16u);
            wr32(ee, kDmaBufTop, m.inMemAfter + (m.ptr - m.sprBase) + q * 16u);
            g_nodeMarkersWritten += n;
        }

        // Ring of transient vertex data snapshotted on the EE side (RwIm3D vertices live in the game's buffers only
        // until the call returns); consumed by the GS side in stream order, long before the ring wraps.
        constexpr uint32_t kRingSize = 32u << 20;
        constexpr uint32_t kEeRingSize = kRingSize - (8u << 20); // the top 8 MB: marker-side allocations (PGen)
        std::vector<uint8_t> g_ring(kRingSize);
        uint32_t g_ringPos = 0;
        uint32_t ringAlloc(uint32_t bytes)
        {
            bytes = (bytes + 15u) & ~15u;
            if (g_ringPos + bytes > kEeRingSize)
                g_ringPos = 0;
            const uint32_t at = g_ringPos;
            g_ringPos += bytes;
            return at;
        }

        // RpSkin on the CPU (RenderWare's PS2 skin VU1 program blends up to four bones per vertex). Matrices as
        // _rpSkinMatrixUpdating: skinToBone[i] x bone[i] x inverse(atomic LTM), bone = the HAnim matrix array, or
        // the node frames' LTMs (rpHANIMHIERARCHYNOMATRICES); local-space matrices skip the atomic inverse. Then
        // p = sum w_k (v x M[i_k]), n = sum w_k (n x M[i_k]) into the ring as RxObjSpace3DVertex (pos, RGBA,
        // normal, u, v). Returns the ring offset, or ~0u when the object has to stay on RenderWare.
        constexpr uint32_t kSkinAtomicOffset = 0x466164u, kSkinGeomOffset = 0x466168u; // _rpSkinGlobals
        uint32_t skinVertices(const uint8_t *ee, uint32_t atomic, uint32_t geom, uint32_t numVerts, uint32_t verts,
                              uint32_t normals, uint32_t uvs, uint32_t prelit)
        {
            const uint32_t hier = rd32(ee, atomic + rd32(ee, kSkinAtomicOffset));
            const uint32_t skin = rd32(ee, geom + rd32(ee, kSkinGeomOffset));
            if (!hier || !skin)
                return ~0u;
            const uint32_t hflags = rd32(ee, hier), numNodes = rd32(ee, hier + 4u);
            const uint32_t toBone = rd32(ee, skin + 12u), vIdx = rd32(ee, skin + 20u), vW = rd32(ee, skin + 24u);
            if (numNodes == 0u || numNodes > 256u || !toBone || !vIdx || !vW)
                return ~0u;
            const bool noMatrices = (hflags & 2u) != 0u, localSpace = (hflags & 0x4000u) != 0u;
            const uint32_t matrixArray = rd32(ee, hier + 8u), nodeInfo = rd32(ee, hier + 16u);
            if ((noMatrices && !nodeInfo) || (!noMatrices && !matrixArray))
                return ~0u;
            static Mat mats[256];
            Mat invAtomic{};
            if (noMatrices || !localSpace)
                matInvert(invAtomic, readMat(ee, rd32(ee, atomic + 4u) + 80u));
            for (uint32_t i = 0; i < numNodes; ++i)
            {
                Mat bone;
                if (noMatrices)
                {
                    const uint32_t frame = rd32(ee, nodeInfo + i * 16u + 12u);
                    if (!frame)
                        return ~0u;
                    bone = readMat(ee, frame + 80u);
                }
                else
                    bone = readMat(ee, matrixArray + i * 64u);
                Mat t;
                matMul(t, readMat(ee, toBone + i * 64u), bone);
                if (noMatrices || !localSpace)
                    matMul(mats[i], t, invAtomic);
                else
                    mats[i] = t;
            }
            const uint32_t at = ringAlloc(numVerts * kIm3DVertexStride);
            uint8_t *out = g_ring.data() + at;
            for (uint32_t v = 0; v < numVerts; ++v, out += kIm3DVertexStride)
            {
                float p[3], n[3] = {0.0f, 0.0f, 0.0f}, w[4];
                uint8_t ix[4];
                std::memcpy(p, ee + ((verts + v * 12u) & 0x1FFFFFFu), 12);
                if (normals)
                    std::memcpy(n, ee + ((normals + v * 12u) & 0x1FFFFFFu), 12);
                std::memcpy(w, ee + ((vW + v * 16u) & 0x1FFFFFFu), 16);
                std::memcpy(ix, ee + ((vIdx + v * 4u) & 0x1FFFFFFu), 4);
                float po[3] = {0.0f, 0.0f, 0.0f}, no[3] = {0.0f, 0.0f, 0.0f};
                for (int k = 0; k < 4; ++k)
                {
                    if (!(w[k] > 0.0f) || ix[k] >= numNodes)
                        continue;
                    const Mat &m = mats[ix[k]];
                    for (int c = 0; c < 3; ++c)
                    {
                        po[c] += w[k] * (p[0] * m.r[0][c] + p[1] * m.r[1][c] + p[2] * m.r[2][c] + m.r[3][c]);
                        no[c] += w[k] * (n[0] * m.r[0][c] + n[1] * m.r[1][c] + n[2] * m.r[2][c]);
                    }
                }
                std::memcpy(out, po, 12);
                if (prelit)
                    std::memcpy(out + 12, ee + ((prelit + v * 4u) & 0x1FFFFFFu), 4);
                else
                {
                    const uint8_t black[4] = {0, 0, 0, 255}; // atomics without pre-lit colours start black
                    std::memcpy(out + 12, black, 4);
                }
                std::memcpy(out + 16, no, 12);
                if (uvs)
                    std::memcpy(out + 28, ee + ((uvs + v * 8u) & 0x1FFFFFFu), 8);
                else
                    std::memset(out + 28, 0, 8);
            }
            return at;
        }

    }

    namespace
    {
        const NodePred *g_bridgePred = nullptr;

        // Dry run of the native node path (PS2X_NRW_NODE=verify): compare its prediction for this mesh with
        // RenderWare's pipe data and the GS state globals its bridge (generic or PDS G3x) just wrote.
        void nodeVerifyBridge(const uint8_t *rdram, uint32_t pipeData, uint32_t object)
        {
            g_bridgePred = nullptr;
            const auto it = g_preds.find((static_cast<uint64_t>(object) << 32) | rd32(rdram, pipeData + 0x2C));
            static uint64_t s_n = 0, s_bad = 0, s_missing = 0, s_g3x = 0;
            static int s_reports = 0;
            ++s_n;
            if (it == g_preds.end())
                ++s_missing;
            else
            {
                const NodePred &p = it->second;
                g_bridgePred = &p;
                s_g3x += p.g3x ? 1u : 0u;
                const uint32_t tt = rdram[(pipeData + 40u) & 0x1FFFFFFu], pt = rdram[(pipeData + 41u) & 0x1FFFFFFu];
                const uint32_t mc = rd32(rdram, pipeData + 68u), tex = rd32(rdram, pipeData + 64u);
                const uint64_t test = rd64(rdram, kGp - 0x79F8u), tex1 = rd64(rdram, kGp - 0x79D8u), clamp = rd64(rdram, kGp - 0x79E0u);
                const bool bad = tt != p.transType || pt != p.primType || mc != p.matCol || tex != p.texture ||
                                 (tex && (test != p.test || tex1 != p.tex1 || clamp != p.clamp));
                if (bad)
                {
                    ++s_bad;
                    if (s_reports < 40)
                    {
                        ++s_reports;
                        std::printf("[nrw-node-verify] obj=%08x g3x=%d trans %u/%u prim %u/%u matCol %08x/%08x tex %08x/%08x "
                                    "test %llx/%llx tex1 %llx/%llx clamp %llx/%llx (native/RW)\n",
                                    object, p.g3x ? 1 : 0, p.transType, tt, p.primType, pt, p.matCol, mc, p.texture, tex,
                                    (unsigned long long)p.test, (unsigned long long)test, (unsigned long long)p.tex1,
                                    (unsigned long long)tex1, (unsigned long long)p.clamp, (unsigned long long)clamp);
                    }
                }
            }
            if ((s_n % 20000u) == 0u)
            {
                std::printf("[nrw-node-verify] meshes=%llu stateMismatch=%llu noPrediction=%llu g3x=%llu\n", (unsigned long long)s_n,
                            (unsigned long long)s_bad, (unsigned long long)s_missing, (unsigned long long)s_g3x);
                std::fflush(stdout);
            }
        }
    }

    bool bridge(uint8_t *rdram, uint32_t pipeData, uint64_t callLo, uint64_t callHi, uint64_t &lo, uint64_t &hi,
                bool &insertBefore)
    {
        (void)callHi;
        insertBefore = verifyMode();
        static const bool s_dump = std::getenv("PS2X_NRW_DUMP") != nullptr;
        const uint32_t object = rd32(rdram, pipeData + 0x08);
        if (nodeMode() == 2)
            nodeVerifyBridge(rdram, pipeData, object);
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
        e.native = false;
        e.envTexture = 0;
        e.envFlags = 0;
        e.gsFromRegs = false;
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
        if (nodeMode() == 2)
        {
            // Dry run: the node path's VU1 constants are checked at the marker (state compared at bridge entry).
            ObjExpect &ex = g_objExpect[idx & (kExpectRing - 1u)];
            if (g_bridgePred)
                ex = g_bridgePred->ex;
            else
                ex.unsupported = true;
        }
        else if (objVerifyMode())
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

    void g3xLights(uint32_t ambient, uint32_t directional)
    {
        g_g3xAmbient = ambient;
        g_g3xDirectional = directional;
    }

    // RwIm3DPS2AllObjectSetupCallBack objects (RwIm3DTransform + RwIm3DRender[Indexed]Primitive). The object is the
    // rwIm3DPoolStash {flags, ltm, numVerts, objVerts, .., primType @36, indices @40, numIndices @44}; the mesh is
    // RenderWare's static Im3D mesh (material 0x466390: white, alpha 254, surface properties 1). The generic VU1
    // program draws it unlit with the vertices' RGBA; texture and blending come from the render states (GS
    // registers), so the job takes those from the GS. Vertices and indices are snapshotted into the ring.
    // Parallel-projection cameras (transType bit 8: the HUD, 2D cameras) are drawn natively with RenderWare's ISO
    // VU1 math (no divide, see bindNative); fogged ISO objects stay on RenderWare. PS2X_NRW_ISO=0: all ISO on RW.
    // RwIm2D (SkyIm2DRenderPrimitive / SkyIm2DRenderIndexedPrimitive, tools/patch_im2d.py). PS2X_NRW_IM2D=log
    // prints each call's first vertex and the vertex RenderWare's Im2D VU1 program kicked for it.
    bool im2d(uint8_t *ee, PS2Memory &memory, uint32_t primType, uint32_t verts, uint32_t numVerts, uint32_t indices,
              uint32_t numIndices)
    {
        static const char *s_mode = []()
        {
            const char *v = std::getenv("PS2X_NRW_IM2D"); // default native; =0: RenderWare's Im2D VU1 program; =log
            return v ? v : "1";
        }();
        if (!ee || !verts)
            return false;
        if (std::strcmp(s_mode, "log") == 0)
        {
            static int s_left = 40;
            if (s_left > 0)
            {
                --s_left;
                float f[16];
                std::memcpy(f, ee + (verts & 0x1FFFFF0u), 64);
                const uint64_t prim = rd64(ee, kGp - 0x7CC0u);
                float c1017[4], c1019a[4], c1019b[4];
                std::memcpy(c1017, ee + ((kSkyCamBlock + 48u) & 0x1FFFFF0u), 16);
                std::memcpy(c1019a, ee + 0x4BE230u, 16);
                std::memcpy(c1019b, ee + 0x4BE240u, 16);
                std::printf("[nrw-im2d] prim=%u n=%u skyPrim=%llx v0: xyz=(%g %g %g %g) uv rz=(%g %g %g %g) rgba=(%g %g %g %g) | "
                            "cam1017=(%g %g %g %g) 4be230=(%g %g %g %g) 4be240=(%g %g %g %g)\n",
                            primType, numVerts, (unsigned long long)prim, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8],
                            f[9], f[10], f[11], c1017[0], c1017[1], c1017[2], c1017[3], c1019a[0], c1019a[1], c1019a[2],
                            c1019a[3], c1019b[0], c1019b[1], c1019b[2], c1019b[3]);
                g_im2dLogArmed = 1;
            }
            return false;
        }
        if (s_mode[0] == '0')
            return false;
        // Native: RenderWare's Im2D VU1 program (open2DVU1Pkt constants, 0x3d0dc0 / 0x3d0f20) on the CPU.
        // GS XY = (x, y) + the 2D offset (0x4BE260), Z = z, STQ = (u, v) * 1/z, RGBA = colour * scale (0x4BE230
        // textured / 0x4BE220), F = 255; each triangle was a 3-vertex fan with PRIM = sky prim state.
        if (primType < 3u || primType > 5u || numVerts == 0u || numVerts > 65535u || (indices && numIndices > (1u << 20)))
            return false;
        static std::vector<uint16_t> tri;
        tri.clear();
        const uint32_t count = indices ? numIndices : numVerts;
        auto at = [&](uint32_t i) -> uint16_t
        {
            if (!indices)
                return static_cast<uint16_t>(i);
            uint16_t v;
            std::memcpy(&v, ee + ((indices + i * 2u) & 0x1FFFFFFu), 2);
            return v;
        };
        if (primType == 3u)
            for (uint32_t i = 0; i + 2u < count; i += 3u)
                tri.insert(tri.end(), {at(i), at(i + 1u), at(i + 2u)});
        else if (primType == 4u)
            for (uint32_t i = 0; i + 2u < count; ++i)
                tri.insert(tri.end(), {at(i), at(i + 1u), at(i + 2u)});
        else
            for (uint32_t i = 1; i + 1u < count; ++i)
                tri.insert(tri.end(), {at(0), at(i), at(i + 1u)});
        if (tri.empty())
            return false;
        for (uint16_t v : tri)
            if (v >= numVerts)
                return false;
        MarkerSlot slot;
        if (!reserveMarkers(ee, 2u, slot)) // marker + PATH3 release
        {
            static uint64_t s_noRoom = 0;
            if (std::getenv("PS2X_NRW_STATS") && (++s_noRoom % 2000u) == 1u)
                std::printf("[nrw-im2d] no open VIF packet: %llu calls on RenderWare\n", (unsigned long long)s_noRoom);
            return false;
        }
        const uint64_t skyPrim = rd64(ee, kGp - 0x7CC0u);
        float off[2], scale[4];
        std::memcpy(off, ee + 0x4BE260u, 8);
        std::memcpy(scale, ee + ((skyPrim & 0x10u) ? 0x4BE230u : 0x4BE220u), 16);
        const uint32_t vAt = ringAlloc(numVerts * static_cast<uint32_t>(sizeof(Im2DVertex)));
        Im2DVertex *out = reinterpret_cast<Im2DVertex *>(g_ring.data() + vAt);
        for (uint32_t v = 0; v < numVerts; ++v)
        {
            float f[12];
            std::memcpy(f, ee + ((verts + v * 64u) & 0x1FFFFFFu), 48);
            Im2DVertex &o = out[v];
            o.x = std::trunc((f[0] + off[0]) * 16.0f) / 16.0f; // FTOI4
            o.y = std::trunc((f[1] + off[1]) * 16.0f) / 16.0f;
            const int32_t z16 = static_cast<int32_t>(std::max(0.0f, std::min(f[2] * 16.0f, 2147483520.0f)));
            o.z = static_cast<float>((z16 >> 4) & 0xFFFFFF);
            o.s = f[4] * f[6];
            o.t = f[5] * f[6];
            o.q = f[6];
            for (int k = 0; k < 4; ++k)
            {
                const float c = f[8 + k] * scale[k];
                o.rgba[k] = static_cast<uint8_t>(c <= 0.0f ? 0 : (c >= 255.0f ? 255 : static_cast<int>(c))); // FTOI0
            }
            o.pad = 0;
        }
        const uint32_t iAt = ringAlloc(static_cast<uint32_t>(tri.size()) * 2u);
        std::memcpy(g_ring.data() + iAt, tri.data(), tri.size() * 2u);
        const uint32_t idx = g_next.fetch_add(1, std::memory_order_relaxed) & (kTableSize - 1u);
        Entry &e = g_table[idx];
        e.native = true;
        e.gsFromRegs = true;
        e.kind = kIm2DMesh;
        e.object = verts;
        e.mesh = 0;
        e.meshFlags = 0;
        e.primType = 3u;
        e.transType = 0;
        e.matCol = 0xFFFFFFFFu;
        e.texture = 0;
        e.vu1CodeIndex = 0;
        e.surfProps = 0;
        e.code = 0;
        e.verts = vAt;
        e.normals = e.uvs = e.prelit = 0;
        e.numVerts = numVerts;
        e.geomFlags = 0;
        e.gsTest = e.gsTex1 = e.gsClamp = 0;
        e.geomGen = 0;
        e.envTexture = 0;
        e.envFlags = 0;
        e.ringIndices = iAt;
        e.numIndices = static_cast<uint32_t>(tri.size());
        float(*vu)[4] = g_nativeVu[idx & (kNativeVuRing - 1u)];
        std::memset(vu, 0, sizeof(float) * 4u * kVuQwCount);
        const uint64_t tag = 0x8000ull | (1ull << 46) | ((3ull | (skyPrim & 0x6F8ull)) << 47); // EOP, PRE, TRIANGLE (FST 0)
        std::memcpy(vu[vuIndex(1018)], &tag, 8);
        writeMarkers(ee, memory, slot, &idx, 1u, path3Release());
        return true;
    }

    // RpPDS_G3_Skin objects drawn natively (PS2X_NRW_SKIN=0: RenderWare's skin VU1 program).
    bool skinNative()
    {
        static const bool s_on = []()
        {
            const char *v = std::getenv("PS2X_NRW_SKIN");
            return !(v && *v == '0');
        }();
        return s_on;
    }

    bool isoNative(uint32_t transBase)
    {
        static const bool s_on = []()
        {
            const char *v = std::getenv("PS2X_NRW_ISO");
            return !(v && *v == '0');
        }();
        return !(transBase & 8u) || (s_on && !(transBase & 1u));
    }

    bool im3dNode(uint8_t *ee, PS2Memory &memory, uint32_t objPvt, uint32_t stash, int mode)
    {
        static const bool s_on = []()
        {
            const char *v = std::getenv("PS2X_NRW_IM3D"); // =0: Im3D stays on RenderWare
            return !(v && *v == '0');
        }();
        if (!s_on || mode != 1 || rd32(ee, objPvt + 20u))
            return false;
        const uint32_t cam = rd32(ee, kRwGlobals);
        // RwIm3DTransform records the vertex array in the rwIm3DPool that holds the stash {numElements u16 @-12,
        // elements @-8, stride @-4}; the stash's own numVerts/objVerts are only filled by some transform paths.
        const uint32_t flags = rd32(ee, stash), ltm = rd32(ee, stash + 4u);
        uint32_t numVerts = rd32(ee, stash + 8u), objVerts = rd32(ee, stash + 12u);
        if (!numVerts || !objVerts)
        {
            uint16_t n16;
            std::memcpy(&n16, ee + ((stash - 12u) & 0x1FFFFFFu), 2);
            numVerts = n16;
            objVerts = rd32(ee, stash - 8u);
            if (rd32(ee, stash - 4u) != kIm3DVertexStride)
                objVerts = 0u;
        }
        const uint32_t primType = rd32(ee, stash + 36u);
        const uint32_t indices = rd32(ee, stash + 40u);
        const uint32_t numIndices = indices ? rd32(ee, stash + 44u) : numVerts;
        if (!cam || !objVerts || numVerts == 0u || numVerts > 65535u || numIndices < 3u || numIndices > (1u << 20) ||
            (primType != 3u && primType != 4u)) // tri lists / strips (lines, fans, points: RW)
        {
            nstat(kNsPrim);
            static const bool s_diag = std::getenv("PS2X_NRW_STATS") != nullptr;
            static uint64_t s_hist[8] = {}, s_n = 0;
            if (s_diag)
            {
                ++s_hist[primType & 7u];
                if ((++s_n % 20000u) == 0u)
                    std::printf("[nrw-im3d-prim] by type 0..7: %llu %llu %llu %llu %llu %llu %llu %llu (verts %u idx %u)\n",
                                (unsigned long long)s_hist[0], (unsigned long long)s_hist[1], (unsigned long long)s_hist[2],
                                (unsigned long long)s_hist[3], (unsigned long long)s_hist[4], (unsigned long long)s_hist[5],
                                (unsigned long long)s_hist[6], (unsigned long long)s_hist[7], numVerts, numIndices);
            }
            return false;
        }
        // The group pipeline must be the Im3D one (RwIm3DPS2AllBridgeCallBack, generic code array).
        const uint32_t gp = rd32(ee, objPvt + 24u), nodes = gp ? rd32(ee, gp + 8u) : 0u, mp = nodes ? rd32(ee, nodes + 20u) : 0u;
        if (!mp || rd32(ee, mp + 12u) != 0x3004D0u || rd32(ee, mp + 24u) != 0x407440u || rd32(ee, mp + 16u) != 0u)
        {
            nstat(kNsPipe);
            return false;
        }
        MarkerSlot slot;
        if (!reserveMarkers(ee, 2u, slot)) // marker + PATH3 release
        {
            nstat(kNsNoRoom);
            return false;
        }
        // Constants: RwIm3DPS2AllTransformSetupFunc (LTM x view, or the view matrix), no lights, the frustum test
        // result from the stash flags (4: inside), prim/trans from the primitive type.
        static ObjExpect ex;
        ex.mask = 0;
        ex.unsupported = false;
        const Mat view = readMat(ee, cam + 32u);
        Mat t = view;
        if (ltm)
            matMul(t, readMat(ee, ltm), view);
        for (int i = 0; i < 4; ++i)
            putq(ex.vu[vuIndex(1008u + i)], t.r[i][0], t.r[i][1], t.r[i][2], t.r[i][2]);
        putq(ex.vu[vuIndex(1012)], 0, 0, 0, 0);
        putq(ex.vu[vuIndex(1013)], 0, 0, 0, 0);
        for (uint32_t i = 0; i < 5; ++i)
            std::memcpy(ex.vu[vuIndex(1014u + i)], ee + ((kSkyCamBlock + i * 16u) & 0x1FFFFFFu), 16);
        for (uint32_t a = kVuFirstQw; a < 1008u; ++a)
            putq(ex.vu[vuIndex(a)], 0, 0, 0, 0); // no lights (terminator at 980)
        const uint32_t transBase = ee[(kGp - 0x793Cu) & 0x1FFFFFFu];
        if (!isoNative(transBase))
        {
            nstat(kNsPrim);
            return false;
        }
        const bool strip = primType == 4u;
        uint32_t transType = strip ? transBase : ((transBase | 4u) & 0xFFu);
        if (!(flags & 4u))
            transType |= 2u;
        constexpr uint32_t kIm3DMaterial = 0x466390u;
        uint8_t col[4];
        std::memcpy(col, ee + ((kIm3DMaterial + 4u) & 0x1FFFFFFu), 4);
        const uint64_t primGlobal = rd64(ee, kGp - 0x7CC0u);
        meshConstants(ee, transType, strip ? 4u : 3u, col, primGlobal, rd32(ee, kGp - 0x797Cu) != 0u, kIm3DMaterial + 12u, 0.0f, ex);
        {
            static const bool s_log = std::getenv("PS2X_NRW_IM3DLOG") != nullptr;
            static uint64_t s_n = 0;
            if (s_log && ((++s_n % 997u) == 0u))
            {
                float v0[9];
                std::memcpy(v0, ee + (objVerts & 0x1FFFFFFu), 36);
                uint32_t c0;
                std::memcpy(&c0, ee + ((objVerts + 12u) & 0x1FFFFFFu), 4);
                std::printf("[nrw-im3d] stash=%08x flags=%x ltm=%08x verts=%u@%08x (stash %u) prim=%u idx=%08x n=%u trans=%u tex=%d "
                            "v0=(%g %g %g) rgba=%08x uv=(%g %g) cam=%08x prim1018=%llx\n",
                            stash, flags, ltm, numVerts, objVerts, rd32(ee, stash + 8u), primType, indices, numIndices, transType,
                            (int)((primGlobal >> 4) & 1u), v0[0], v0[1], v0[2], c0, v0[7], v0[8], cam,
                            (unsigned long long)((*reinterpret_cast<const uint64_t *>(&ex.vu[vuIndex(1018)][0]) >> 47) & 0x7FFu));
                std::fflush(stdout);
            }
        }
        // Snapshot vertices and indices.
        const uint32_t vbytes = numVerts * kIm3DVertexStride, ibytes = numIndices * 2u;
        const uint32_t vAt = ringAlloc(vbytes), iAt = ringAlloc(ibytes);
        std::memcpy(g_ring.data() + vAt, ee + (objVerts & 0x1FFFFFFu), std::min<uint32_t>(vbytes, 0x2000000u - (objVerts & 0x1FFFFFFu)));
        uint16_t *ix = reinterpret_cast<uint16_t *>(g_ring.data() + iAt);
        if (indices)
            std::memcpy(ix, ee + (indices & 0x1FFFFFFu), std::min<uint32_t>(ibytes, 0x2000000u - (indices & 0x1FFFFFFu)));
        else
            for (uint32_t i = 0; i < numIndices; ++i)
                ix[i] = static_cast<uint16_t>(i);
        const uint32_t idx = g_next.fetch_add(1, std::memory_order_relaxed) & (kTableSize - 1u);
        Entry &e = g_table[idx];
        e.native = true;
        e.gsFromRegs = true;
        e.kind = kIm3DMesh;
        e.object = stash;
        e.mesh = 0;
        e.meshFlags = strip ? 1u : 0u;
        e.primType = strip ? 4u : 3u;
        e.transType = transType;
        std::memcpy(&e.matCol, col, 4);
        e.texture = 0;
        e.vu1CodeIndex = transType & 0x1Fu;
        e.surfProps = kIm3DMaterial + 12u;
        e.code = 0;
        e.verts = vAt;
        e.normals = 0;
        e.uvs = 0;
        e.prelit = 0;
        e.numVerts = numVerts;
        e.geomFlags = 0;
        e.gsTest = e.gsTex1 = e.gsClamp = 0;
        e.geomGen = 0;
        e.envTexture = 0;
        e.envFlags = 0;
        e.ringIndices = iAt;
        e.numIndices = numIndices;
        std::memcpy(g_nativeVu[idx & (kNativeVuRing - 1u)], ex.vu, sizeof(ex.vu));
        writeMarkers(ee, memory, slot, &idx, 1u, path3Release());
        nstat(kNsNative);
        return true;
    }

    const uint8_t *ring() { return g_ring.data(); }

    // ---- Game particles (PGen) ------------------------------------------------------------------------------------
    // The game's particle generators ("_$VUCCode_particleGenerator*": Sprite, Streak, Instanced and their 2Cam split-
    // screen versions) are VU C programs that age, place, colour and expand every particle from the effect's parameter
    // block and its emitter history, then kick 20 triangles per batch. Natively, the inputs RenderWare's PS2All
    // pipeline would upload are gathered from the game's own structures, the game's particle program runs as compiled
    // x64 code against a private data memory (ps2vu1nRunPrivate: no VIF, no VU1, nothing reaches the GS), and its
    // triangles are drawn as one D3D11 job with the texture decoded from the raster. Variants without compiled code
    // (2Cam) stay on RenderWare. PS2X_NRW_PGEN=0: all particles on RenderWare; =verify: RenderWare draws, and every
    // run is repeated natively and compared (inputs and kicked triangles, [nrw-pgen] lines).
    //
    // Timing: the bridge copies the parameter block (and RenderWare the object constants) into its packet, but the
    // emitter history is only referenced, so the VU saw it as the DMA read it -- after the game wrote this frame's
    // newest entry. The node path therefore snapshots params and constants at render time and runs the program when
    // the VIF reaches the marker (marker()), reading the history then.
    int pgenMode()
    {
        static const int s_mode = []()
        {
            const char *v = std::getenv("PS2X_NRW_PGEN");
            if (v && std::strcmp(v, "verify") == 0)
                return 2;
            return (v && *v == '0') ? 0 : 1;
        }();
        return s_mode;
    }

    namespace
    {
        constexpr uint32_t kPGenCodeArray = 0x402980u;    // PGen's VU1 code array (Sprite, Streak, Instanced, 2Cam x3)
        constexpr uint32_t kPGenPluginOffset = 0x4B5C68u; // RpAtomic plugin offset of the PGen data
        constexpr uint32_t kPGenBridge = 0x296FB0u;       // PGenBridgeCallback (the material pipeline's bridge)
        constexpr uint32_t kPGenEntry = 0x80000000u;      // Entry.code flag: a PGen job (index in the low bits)

        // A VU1 program as its code-array DMA chain uploads it (one RET tag, VIF codes inline): the micro image (MPG)
        // and the initial data memory (STCYCL 4/4 + UNPACK V4-32: constants, GIF tags, tables).
        struct VuChainProgram
        {
            bool tried = false, ok = false;
            uint64_t key = 0;
            std::vector<uint8_t> image, data;
            std::vector<uint16_t> uploaded; // data qwords the chain writes
        };

        bool decodeVuChain(const uint8_t *ee, uint32_t chain, VuChainProgram &p)
        {
            if (!chain)
                return false;
            const uint32_t qwc = static_cast<uint32_t>(rd64(ee, chain) & 0xFFFFu);
            p.image.assign(0x4000, 0);
            p.data.assign(0x4000, 0);
            p.uploaded.clear();
            uint32_t pos = chain + 8u;
            const uint32_t end = chain + 16u + qwc * 16u;
            bool mpg = false;
            while (pos + 4u <= end)
            {
                const uint32_t code = rd32(ee, pos), cmd = (code >> 24) & 0x7Fu;
                uint32_t num = (code >> 16) & 0xFFu;
                if (cmd == 0x4Au) // MPG
                {
                    num = num ? num : 256u;
                    const uint32_t at = (code & 0xFFFFu) * 8u;
                    if (at + num * 8u > 0x4000u)
                        return false;
                    std::memcpy(p.image.data() + at, ee + ((pos + 4u) & 0x1FFFFFFu), num * 8u);
                    pos += 4u + num * 8u;
                    mpg = true;
                }
                else if (cmd == 0x6Cu && !(code & 0x8000u)) // UNPACK V4-32 to an absolute address
                {
                    num = num ? num : 256u;
                    const uint32_t at = code & 0x3FFu;
                    if (at + num > 1024u)
                        return false;
                    std::memcpy(p.data.data() + at * 16u, ee + ((pos + 4u) & 0x1FFFFFFu), num * 16u);
                    for (uint32_t k = 0; k < num; ++k)
                        p.uploaded.push_back(static_cast<uint16_t>(at + k));
                    pos += 4u + num * 16u;
                }
                else if (cmd == 0x00u || (cmd == 0x01u && (code & 0xFFFFu) == 0x0404u)) // NOP, STCYCL 4/4
                    pos += 4u;
                else
                    return false;
            }
            if (!mpg)
                return false;
            uint64_t h = 1469598103934665603ull;
            for (uint8_t b : p.image)
                h = (h ^ b) * 1099511628211ull;
            p.key = h;
            return true;
        }

        // RwResourcesUseResEntry (inlined in PS2AllNodeBody): move a resource entry to the head of the "used this
        // frame" list, so the arena does not reclaim it while space is short. Native paths whose data lives in
        // RenderWare's instance memory (PGen history buffers, the crowd's tiles) do it every frame, as RenderWare's
        // render of the object did.
        void useResEntry(uint8_t *ee, uint32_t entry)
        {
            if (!entry)
                return;
            const uint32_t next = rd32(ee, entry), prev = rd32(ee, entry + 4u);
            const uint32_t list = rd32(ee, kRwGlobals + rd32(ee, kGp - 0x7B40u) + 0x24u);
            if (!next || !prev || !list || list == entry)
                return;
            wr32(ee, prev, next);
            wr32(ee, next + 4u, prev);
            const uint32_t first = rd32(ee, list);
            wr32(ee, entry, first);
            wr32(ee, entry + 4u, list);
            wr32(ee, first + 4u, entry);
            wr32(ee, list, entry);
        }

        // The resource entry of an object's first mesh: the geometry's mesh cache with one morph target, else the
        // atomic's (as the PS2All object setups look it up).
        uint32_t meshResEntry(const uint8_t *ee, uint32_t atomic, uint32_t geom)
        {
            const uint32_t cache = rd32(ee, geom + 24u) == 1u ? rd32(ee, geom + rd32(ee, kGp - 0x7B78u))
                                                              : rd32(ee, atomic + rd32(ee, kGp - 0x7B7Cu));
            return (cache && rd32(ee, cache) >= 1u) ? rd32(ee, cache + 4u) : 0u;
        }

        using PGenProgram = VuChainProgram;

        const PGenProgram *pgenProgram(const uint8_t *ee, uint32_t variant)
        {
            static PGenProgram s_prog[6];
            PGenProgram &p = s_prog[variant % 6u];
            if (p.tried)
                return p.ok ? &p : nullptr;
            p.tried = true;
            if (!decodeVuChain(ee, rd32(ee, kPGenCodeArray + (variant % 6u) * 4u), p))
                return nullptr;
            uint32_t w0;
            std::memcpy(&w0, p.image.data(), 4);
            if (w0 != 0x100E03D4u) // the PGen start stub: iaddiu vi14, vi00, 0x3d4
                return nullptr;
            p.key ^= 0x5047454E00000000ull;
            p.ok = true;
            return &p;
        }

        // Render-time snapshot of one effect: what RenderWare copies into its packet, and where the history lives.
        struct PGenJob
        {
            const PGenProgram *prog = nullptr;
            uint32_t variant = 0, numVerts = 0, atomic = 0;
            uint32_t cl[3] = {};            // emitter history clusters (V4-32): position, direction, velocity
            uint8_t params[24 * 16];        // PGenUploadParameterBlock -> VU1 984..1007
            uint8_t cam[11 * 16];           // object constants -> VU1 1008..1018
            uint32_t camMask = 0;           // bit i: 1008 + i computed
        };
        constexpr uint32_t kPGenJobs = 4096u; // ring; consumed at the marker within a frame or two
        PGenJob g_pgenJobs[kPGenJobs];
        uint32_t g_pgenJobNext = 0;

        // Marker-side vertex ring (the top of g_ring, VIF thread only; the EE side allocates below it).
        uint32_t g_markerRingPos = 0;
        uint32_t markerRingAlloc(uint32_t bytes)
        {
            bytes = (bytes + 15u) & ~15u;
            if (g_markerRingPos < kEeRingSize || g_markerRingPos + bytes > kRingSize)
                g_markerRingPos = kEeRingSize;
            const uint32_t at = g_markerRingPos;
            g_markerRingPos += bytes;
            return at;
        }

        struct PGenOut
        {
            std::vector<Im2DVertex> verts;
            std::vector<uint8_t> raw; // kicked packets (verify)
            uint64_t tag = 0;
        };

        // Kicked packets -> vertices, decoded like the GS's PACKED mode: ST sets S/T/Q (Q 0 -> 1), RGBAQ the colour,
        // XYZF2 / XYZ2 kick a vertex (X/Y 12.4 fixed; Z bits 4..27 / the whole 32-bit word; fog unused: FGE is off).
        void pgenKick(void *user, const uint8_t *pkt, uint32_t bytes)
        {
            PGenOut &o = *static_cast<PGenOut *>(user);
            o.raw.insert(o.raw.end(), pkt, pkt + bytes);
            float s = 0.0f, t = 0.0f, q = 1.0f;
            uint8_t rgba[4] = {0, 0, 0, 0};
            uint32_t p = 0;
            while (p + 16u <= bytes)
            {
                uint64_t lo, hi;
                std::memcpy(&lo, pkt + p, 8);
                std::memcpy(&hi, pkt + p + 8, 8);
                p += 16u;
                const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu), flg = static_cast<uint32_t>((lo >> 58) & 3u);
                uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xFu);
                nreg = nreg ? nreg : 16u;
                if ((lo >> 46) & 1u)
                    o.tag = lo;
                if (flg != 0u)
                {
                    p += flg == 2u ? nloop * 16u : ((nloop * nreg + 1u) / 2u) * 16u;
                    continue;
                }
                for (uint32_t l = 0; l < nloop; ++l)
                    for (uint32_t r = 0; r < nreg; ++r)
                    {
                        if (p + 16u > bytes)
                            return;
                        uint32_t w[4];
                        std::memcpy(w, pkt + p, 16);
                        p += 16u;
                        const uint32_t reg = static_cast<uint32_t>((hi >> (4u * r)) & 0xFu);
                        if (reg == 2u)
                        {
                            std::memcpy(&s, &w[0], 4);
                            std::memcpy(&t, &w[1], 4);
                            std::memcpy(&q, &w[2], 4);
                            if (q == 0.0f)
                                q = 1.0f;
                        }
                        else if (reg == 1u)
                        {
                            for (int k = 0; k < 4; ++k)
                                rgba[k] = static_cast<uint8_t>(w[k] & 0xFFu);
                        }
                        else if (reg == 4u || reg == 5u) // XYZF2 (the particle programs' choice) / XYZ2
                        {
                            Im2DVertex v;
                            v.x = static_cast<float>(w[0] & 0xFFFFu) / 16.0f;
                            v.y = static_cast<float>(w[1] & 0xFFFFu) / 16.0f;
                            v.z = static_cast<float>(reg == 4u ? (w[2] >> 4) & 0xFFFFFFu : w[2]);
                            v.s = s;
                            v.t = t;
                            v.q = q;
                            std::memcpy(v.rgba, rgba, 4);
                            v.pad = 0;
                            o.verts.push_back(v);
                        }
                    }
                // EOP ends a kick; `pkt` may hold several (verify decodes RenderWare's kicks in one call).
            }
        }

        // The program's data memory for a job, with the history as it is in EE memory now, then the program run.
        alignas(16) uint8_t g_pgenMem[0x4000]; // VIF thread
        PGenOut g_pgenOut;
        bool pgenRun(const PGenJob &j)
        {
            const uint8_t *ee = g_rdram;
            if (!ee || !j.prog)
                return false;
            std::memcpy(g_pgenMem, j.prog->data.data(), sizeof(g_pgenMem));
            for (uint32_t i = 0; i < j.numVerts; ++i)
                for (uint32_t c = 0; c < 3u; ++c)
                    std::memcpy(g_pgenMem + (3u * i + c) * 16u, ee + ((j.cl[c] + i * 16u) & 0x1FFFFFFu), 16);
            std::memcpy(g_pgenMem + 984u * 16u, j.params, sizeof(j.params));
            for (uint32_t i = 0; i < 11u; ++i)
                if (j.camMask & (1u << i))
                    std::memcpy(g_pgenMem + (1008u + i) * 16u, j.cam + i * 16u, 16);
            g_pgenOut.verts.clear();
            g_pgenOut.raw.clear();
            g_pgenOut.tag = 0;
            return ::ps2vu1nRunPrivate(j.prog->image.data(), j.prog->key, g_pgenMem, 0u, pgenKick, &g_pgenOut);
        }

        // Verify mode: render-time snapshots waiting for RenderWare's run of the same effect (VIF thread).
        std::mutex g_pgenMx;
        std::vector<PGenJob> g_pgenExpect;
    }

    bool pgenVerifyWanted() { return pgenMode() == 2; }

    // VU1 glue (vu1_native_glue.cpp): RenderWare just ran a PGen program on `vuBefore` and kicked `kicks`.
    void pgenVerifyRun(const uint8_t *vuBefore, const uint8_t *kicks, uint32_t kickBytes)
    {
        static uint64_t s_runs = 0, s_unpaired = 0, s_inBad = 0, s_outBad = 0, s_exact = 0, s_variant[6] = {}, s_tris = 0;
        static int s_shown = 0;
        PGenJob j;
        {
            // This effect's snapshot: same parameter block and object matrix (984..1011). RenderWare also draws
            // effects the native path declined (no snapshot), so arrival order alone does not pair them.
            std::lock_guard<std::mutex> lock(g_pgenMx);
            auto it = std::find_if(g_pgenExpect.begin(), g_pgenExpect.end(), [&](const PGenJob &e)
                                   { return std::memcmp(e.params, vuBefore + 984u * 16u, sizeof(e.params)) == 0 &&
                                            std::memcmp(e.cam, vuBefore + 1008u * 16u, 4u * 16u) == 0; });
            if (it == g_pgenExpect.end())
            {
                ++s_unpaired;
                return;
            }
            j = *it;
            g_pgenExpect.erase(it);
        }
        if (!pgenRun(j))
            return;
        ++s_runs;
        ++s_variant[j.variant % 6u];
        // Inputs: the history clusters, the chain's constants (minus what the program computes before reading:
        // 729..733 / Streak 633..637, and its output buffers 271..632), params and camera.
        uint32_t badQw = 0, firstBad = 0xFFFFu;
        auto check = [&](uint32_t a)
        {
            if (std::memcmp(g_pgenMem + a * 16u, vuBefore + a * 16u, 16) != 0)
            {
                if (!badQw)
                    firstBad = a;
                ++badQw;
            }
        };
        for (uint32_t a = 0; a < 3u * j.numVerts; ++a)
            check(a);
        for (uint16_t a : j.prog->uploaded)
            if (!(a >= 271u && a < 638u) && !(a >= 729u && a < 734u))
                check(a);
        for (uint32_t a = 984u; a <= 1017u; ++a)
            check(a);
        // Output: what gets drawn -- the vertices both runs decode to (the GS ignores some bits RenderWare's run leaves
        // with stale memory, e.g. XYZF2 word 3 bits 0..3), and the GIF tag.
        const std::vector<uint8_t> &raw = g_pgenOut.raw;
        static PGenOut s_rw;
        s_rw.verts.clear();
        s_rw.raw.clear();
        s_rw.tag = 0;
        if (kickBytes)
            pgenKick(&s_rw, kicks, kickBytes);
        const std::vector<Im2DVertex> &nv = g_pgenOut.verts;
        const bool outOk = nv.size() == s_rw.verts.size() && g_pgenOut.tag == s_rw.tag &&
                           (nv.empty() || std::memcmp(nv.data(), s_rw.verts.data(), nv.size() * sizeof(Im2DVertex)) == 0);
        s_tris += nv.size() / 3u;
        s_inBad += badQw ? 1u : 0u;
        s_outBad += outOk ? 0u : 1u;
        s_exact += (!badQw && outOk) ? 1u : 0u;
        uint32_t firstOut = 0;
        while (firstOut < kickBytes && firstOut < raw.size() && raw[firstOut] == kicks[firstOut])
            ++firstOut;
        if ((badQw || !outOk) && s_shown < 40)
        {
            ++s_shown;
            std::printf("[nrw-pgen] MISMATCH variant=%u atomic=%08x verts=%u inputs: %u qwords differ (first %u: native %08x %08x %08x %08x "
                        "rw %08x %08x %08x %08x) kicks: native %zu bytes, rw %u bytes%s\n",
                        j.variant, j.atomic, j.numVerts, badQw, firstBad, rd32(g_pgenMem, (firstBad & 0x3FFu) * 16u),
                        rd32(g_pgenMem, (firstBad & 0x3FFu) * 16u + 4u), rd32(g_pgenMem, (firstBad & 0x3FFu) * 16u + 8u),
                        rd32(g_pgenMem, (firstBad & 0x3FFu) * 16u + 12u), rd32(vuBefore, (firstBad & 0x3FFu) * 16u),
                        rd32(vuBefore, (firstBad & 0x3FFu) * 16u + 4u), rd32(vuBefore, (firstBad & 0x3FFu) * 16u + 8u),
                        rd32(vuBefore, (firstBad & 0x3FFu) * 16u + 12u), raw.size(), kickBytes, outOk ? "" : " (triangles differ)");
            if (!outOk && firstOut + 16u <= kickBytes && firstOut + 16u <= raw.size())
            {
                const uint32_t q = firstOut & ~15u;
                std::printf("[nrw-pgen]   first triangle difference at byte %u (qword %u): native %08x %08x %08x %08x rw %08x %08x %08x %08x\n",
                            firstOut, q / 16u, rd32(raw.data(), q), rd32(raw.data(), q + 4u), rd32(raw.data(), q + 8u),
                            rd32(raw.data(), q + 12u), rd32(kicks, q), rd32(kicks, q + 4u), rd32(kicks, q + 8u), rd32(kicks, q + 12u));
            }
            std::fflush(stdout);
        }
        if ((s_runs % 500u) == 0u)
        {
            std::printf("[nrw-pgen] verified %llu runs (%llu triangles; sprite %llu streak %llu instanced %llu, 2cam %llu): %llu exact, "
                        "inputs differ %llu, triangles differ %llu, unpaired %llu\n",
                        (unsigned long long)s_runs, (unsigned long long)s_tris, (unsigned long long)s_variant[0],
                        (unsigned long long)s_variant[1], (unsigned long long)s_variant[2],
                        (unsigned long long)(s_variant[3] + s_variant[4] + s_variant[5]), (unsigned long long)s_exact,
                        (unsigned long long)s_inBad, (unsigned long long)s_outBad, (unsigned long long)s_unpaired);
            std::fflush(stdout);
        }
    }

    // marker(): run a PGen job's program now and fill the job's vertices / indices / GIF tag. False: nothing to draw.
    bool pgenMarker(const Entry &e, MeshJob &job)
    {
        const PGenJob &j = g_pgenJobs[e.code & (kPGenJobs - 1u)];
        if (!pgenRun(j))
            return false;
        // Triangles (the padding the program kicks to fill its last batch is degenerate: dropped).
        static std::vector<uint16_t> tri;
        tri.clear();
        const std::vector<Im2DVertex> &v = g_pgenOut.verts;
        const uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(v.size()) / 3u * 3u, 65535u / 3u * 3u);
        for (uint32_t i = 0; i < n; i += 3u)
        {
            const Im2DVertex &a = v[i], &b = v[i + 1u], &c = v[i + 2u];
            if ((a.x == b.x && a.y == b.y) || (a.x == c.x && a.y == c.y) || (b.x == c.x && b.y == c.y))
                continue;
            tri.insert(tri.end(), {static_cast<uint16_t>(i), static_cast<uint16_t>(i + 1u), static_cast<uint16_t>(i + 2u)});
        }
        if (tri.empty())
            return false;
        const uint32_t vAt = markerRingAlloc(n * static_cast<uint32_t>(sizeof(Im2DVertex)));
        std::memcpy(g_ring.data() + vAt, v.data(), n * sizeof(Im2DVertex));
        const uint32_t iAt = markerRingAlloc(static_cast<uint32_t>(tri.size()) * 2u);
        std::memcpy(g_ring.data() + iAt, tri.data(), tri.size() * 2u);
        job.verts = vAt;
        job.numVerts = n;
        job.ringIndices = iAt;
        job.numIndices = static_cast<uint32_t>(tri.size());
        std::memcpy(job.vu[vuIndex(1018)], &g_pgenOut.tag, 8); // the program's GIF tag: PRE + PRIM (textured, blended)
        return true;
    }

    bool pgenNode(uint8_t *ee, PS2Memory &memory, uint32_t objPvt, uint32_t atomic, int mode)
    {
        const int pm = pgenMode();
        if (pm == 0 || mode != 1)
            return false;
        if (rd32(ee, objPvt + 20u))
            return false; // finalize callback: RenderWare
        const uint32_t geom = rd32(ee, atomic + 24u), frame = rd32(ee, atomic + 4u);
        if (ee[atomic & 0x1FFFFFFu] != 1u || !geom || !frame)
            return false;
        const uint32_t pg = atomic + rd32(ee, kPGenPluginOffset);
        const uint32_t params = rd32(ee, pg + 0x10u), numVerts = rd32(ee, geom + 20u);
        // RenderWare instances the effect once (PGenInstanceCallback): the game then writes its emitter history
        // (position, direction, velocity: V4-32) straight into those cluster buffers. One batch of up to 90.
        if (ee[pg & 0x1FFFFFFu] != 1u || !params || numVerts == 0u || numVerts > 90u)
            return false;
        const uint32_t cl[3] = {rd32(ee, pg + 4u), rd32(ee, pg + 8u), rd32(ee, pg + 12u)};
        if (!cl[0] || !cl[1] || !cl[2])
            return false;
        const uint32_t header = rd32(ee, geom + 84u);
        uint16_t numMeshes = 0;
        if (header)
            std::memcpy(&numMeshes, ee + ((header + 4u) & 0x1FFFFFFu), 2);
        if (numMeshes != 1u)
            return false;
        const uint32_t mesh = header + 16u + rd32(ee, header + 12u), material = rd32(ee, mesh + 8u);
        const uint32_t groupPipe = rd32(ee, objPvt + 24u), pipe = groupPipe ? groupPipe : (material ? rd32(ee, material + 8u) : 0u);
        const uint32_t nodes = pipe ? rd32(ee, pipe + 8u) : 0u, matPvt = nodes ? rd32(ee, nodes + 20u) : 0u;
        if (!material || !matPvt || rd32(ee, matPvt + 12u) != kPGenBridge || rd32(ee, matPvt + 16u) != 0u)
            return false;
        const uint32_t texture = rd32(ee, material), raster = texture ? rd32(ee, texture) : 0u;
        if (!raster || (ee[(raster + 32u) & 0x1FFFFFFu] & 7u) != 4u || !rd32(ee, raster + 36u))
            return false; // untextured or camera-texture particles: RenderWare
        // PGenBridgeCallback's program choice: 2Cam if param 0xC0, then Streak (0xE8) or Instanced (0xCC > 0).
        float fC0, fE8, fCC;
        std::memcpy(&fC0, ee + ((params + 0xC0u) & 0x1FFFFFFu), 4);
        std::memcpy(&fE8, ee + ((params + 0xE8u) & 0x1FFFFFFu), 4);
        std::memcpy(&fCC, ee + ((params + 0xCCu) & 0x1FFFFFFu), 4);
        const uint32_t variant = (fC0 != 0.0f ? 3u : 0u) + (fE8 != 0.0f ? 1u : (fCC > 0.0f ? 2u : 0u));
        const PGenProgram *prog = pgenProgram(ee, variant);
        if (!prog)
            return false;
        // A variant whose program has no compiled code (2Cam) stays on RenderWare: probe once per variant.
        static int s_compiled[6] = {};
        if (s_compiled[variant] == 0)
        {
            alignas(16) static uint8_t probe[0x4000];
            std::memcpy(probe, prog->data.data(), sizeof(probe));
            s_compiled[variant] = ::ps2vu1nRunPrivate(prog->image.data(), prog->key, probe, 0u, nullptr, nullptr) ? 1 : -1;
        }
        if (s_compiled[variant] < 0)
            return false;
        // Object constants: PGenObjectSetupCallback's LTM x view and the camera block (VU1 1008..1018).
        static ObjExpect obj;
        computeObjectConstants(ee, atomic, false, objPvt, obj, false, true);
        if (obj.unsupported)
            return false;
        MarkerSlot slot;
        if (pm == 1 && !reserveMarkers(ee, 1u, slot))
        {
            nstat(kNsNoRoom);
            return false;
        }
        PGenJob snap;
        PGenJob &j = pm == 2 ? snap : g_pgenJobs[g_pgenJobNext & (kPGenJobs - 1u)];
        j.prog = prog;
        j.variant = variant;
        j.numVerts = numVerts;
        j.atomic = atomic;
        std::memcpy(j.cl, cl, sizeof(j.cl));
        std::memcpy(j.params, ee + (params & 0x1FFFFFFu), sizeof(j.params)); // PGenUploadParameterBlock
        j.camMask = 0;
        for (uint32_t i = 0; i < 11u; ++i)
            if (obj.mask & (1ull << vuIndex(1008u + i)))
            {
                std::memcpy(j.cam + i * 16u, obj.vu[vuIndex(1008u + i)], 16);
                j.camMask |= 1u << i;
            }
        if (pm == 2)
        {
            std::lock_guard<std::mutex> lock(g_pgenMx);
            if (g_pgenExpect.size() >= 256u)
                g_pgenExpect.erase(g_pgenExpect.begin()); // never matched (RenderWare dropped the effect)
            g_pgenExpect.push_back(snap);
            return false; // RenderWare draws; its run is repeated natively and compared
        }
        const uint32_t jobIndex = g_pgenJobNext++ & (kPGenJobs - 1u);
        useResEntry(ee, meshResEntry(ee, atomic, geom)); // the history buffers live in the instance data
        // PGenBridgeCallback's GS state: alpha test for alpha rasters, TEX1 / CLAMP from the texture (as the generic
        // bridge: mip K/L/MXL from the raster extension, the filter mode, wrap/clamp per axis).
        const uint32_t fmt = ee[(raster + 35u) & 0x1FFFFFFu] & 0xFu;
        const uint64_t testGlobal = rd64(ee, kGp - 0x79F8u), tex1Global = rd64(ee, kGp - 0x79D8u);
        const uint32_t fa = rd32(ee, texture + 80u);
        const uint32_t rext = raster + rd32(ee, kGp - 0x798Cu);
        const uint64_t kl = ee[(rext + 20u) & 0x1FFFFFFu] | (static_cast<uint64_t>(ee[(rext + 21u) & 0x1FFFFFFu]) << 8);
        uint64_t tex1 = (tex1Global & 0xFFFFF000FFE7FDE3ull) | ((kl & 0xFFFu) << 32) | ee[(rext + 22u) & 0x1FFFFFFu] | (((kl >> 12) & 3u) << 19);
        tex1 = (tex1 & ~0x1E0ull) | texFilterBits(fa & 0xFFu);
        const uint32_t idx = g_next.fetch_add(1, std::memory_order_relaxed) & (kTableSize - 1u);
        Entry &e = g_table[idx];
        e.native = true;
        e.gsFromRegs = false;
        e.kind = kIm2DMesh;
        e.object = atomic;
        e.mesh = mesh;
        e.meshFlags = 0;
        e.primType = 3u;
        e.transType = 0;
        e.matCol = 0xFFFFFFFFu;
        e.texture = texture;
        e.vu1CodeIndex = variant;
        e.surfProps = 0;
        e.code = kPGenEntry | jobIndex; // vertices come from the program run at the marker
        e.verts = 0;
        e.normals = e.uvs = e.prelit = 0;
        e.numVerts = 0;
        e.geomFlags = 0;
        e.gsTest = (testGlobal & ~1ull) | ((fmt == 1u || fmt == 5u) ? 1u : 0u);
        e.gsTex1 = tex1;
        e.gsClamp = (((fa >> 8) & 0xFu) == 3u ? 1u : 0u) | (((fa >> 12) & 0xFu) == 3u ? 4u : 0u);
        e.geomGen = 0;
        e.envTexture = 0;
        e.envFlags = 0;
        e.ringIndices = 0;
        e.numIndices = 0;
        std::memset(g_nativeVu[idx & (kNativeVuRing - 1u)], 0, sizeof(float) * 4u * kVuQwCount);
        writeMarkers(ee, memory, slot, &idx, 1u);
        nstat(kNsNative);
        return true;
    }

    // ---- The crowd (RpCrowd) ----------------------------------------------------------------------------------------
    // RenderWare's crowd toolkit: the stands are a static geometry of tiles (four corners each); its hand-written VU1
    // program scatters billboard spectators over every tile (count from the tile's area and the density, positions
    // and animation frames from the VU random generator seeded per tile), projects them and kicks triangle strips.
    // Natively: _rpCrowdPS2AllObjectSetupCallBack's constants (VU1 957.., the frame UV table) and the bridge's (1019
    // material colour, 1021..1023 small clip planes) are computed from the RpCrowd structures, the object constants
    // as for atomics, and when the VIF reaches the marker the program runs as compiled code on a private memory, batch
    // by batch as RenderWare's instance data lays them out (the tiles exist only there: the game frees the geometry's
    // arrays once RenderWare has instanced them). GS state of the setup's A+D (context 2): ALPHA_2 Cs/Cd/As,
    // CLAMP_2 repeat, TEST_2 alpha > 0x3F + depth GEQUAL; the texture is decoded from the raster.
    // PS2X_NRW_CROWD=0: the crowd stays on RenderWare; =verify: RenderWare draws and every batch is repeated natively
    // and compared ([nrw-crowd] lines).
    int crowdMode()
    {
        static const int s_mode = []()
        {
            const char *v = std::getenv("PS2X_NRW_CROWD");
            if (v && std::strcmp(v, "verify") == 0)
                return 2;
            return (v && *v == '0') ? 0 : 1;
        }();
        return s_mode;
    }

    namespace
    {
        constexpr uint32_t kCrowdSetup = 0x2B9130u;  // _rpCrowdPS2AllObjectSetupCallBack
        constexpr uint32_t kCrowdBridge = 0x2B9AB0u; // _rpCrowdPS2AllBridgeCallBack
        constexpr uint32_t kCrowdEntry = 0x40000000u; // Entry.code flag: a crowd job (index in the low bits)
        constexpr uint32_t kCrowdMaxFrames = 60u;    // frame UV table entries (957 + 3 + frames < 1008)

        const VuChainProgram *crowdProgram(const uint8_t *ee)
        {
            static VuChainProgram s_prog;
            if (!s_prog.tried)
            {
                s_prog.tried = true;
                // RxPipelineNodePS2AllMatSetVU1CodeArray(node, gp - 0x7ECC, 1): one program.
                s_prog.ok = decodeVuChain(ee, rd32(ee, kGp - 0x7ECCu), s_prog);
                s_prog.key ^= 0x43524F5744000000ull;
            }
            return s_prog.ok ? &s_prog : nullptr;
        }

        struct CrowdJob
        {
            const VuChainProgram *prog = nullptr;
            uint32_t atomic = 0, res = 0, chain = 0, numConst = 0; // resource entry, its instance DMA chain
            uint8_t consts[(3u + kCrowdMaxFrames) * 16u]; // VU1 957..
            uint8_t cam[16 * 16];                          // VU1 1008..1023
            uint32_t camMask = 0;                          // bit i: 1008 + i set
        };
        constexpr uint32_t kCrowdJobs = 256u;
        CrowdJob g_crowdJobs[kCrowdJobs];
        uint32_t g_crowdJobNext = 0;

        // Kicked packets -> triangles, as the GS assembles them: PRIM from each tag with PRE (list / strip / fan; a
        // PRIM write restarts the vertex queue), XYZF2 / XYZ2 with ADC (word 3 bit 15) queue a vertex without a
        // drawing kick.
        struct GifTris
        {
            std::vector<Im2DVertex> verts;
            std::vector<uint16_t> tris;
            uint64_t tag = 0;
        };
        void gifTrisKick(void *user, const uint8_t *pkt, uint32_t bytes)
        {
            GifTris &o = *static_cast<GifTris *>(user);
            float s = 0.0f, t = 0.0f, q = 1.0f;
            uint8_t rgba[4] = {0, 0, 0, 0};
            uint32_t prim = static_cast<uint32_t>((o.tag >> 47) & 7u);
            uint32_t queue[3] = {}, queued = 0;
            uint32_t p = 0;
            while (p + 16u <= bytes)
            {
                uint64_t lo, hi;
                std::memcpy(&lo, pkt + p, 8);
                std::memcpy(&hi, pkt + p + 8, 8);
                p += 16u;
                const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu), flg = static_cast<uint32_t>((lo >> 58) & 3u);
                uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xFu);
                nreg = nreg ? nreg : 16u;
                if ((lo >> 46) & 1u)
                {
                    o.tag = lo;
                    prim = static_cast<uint32_t>((lo >> 47) & 7u);
                    queued = 0;
                }
                if (flg != 0u)
                {
                    p += flg == 2u ? nloop * 16u : ((nloop * nreg + 1u) / 2u) * 16u;
                    continue;
                }
                for (uint32_t l = 0; l < nloop; ++l)
                    for (uint32_t r = 0; r < nreg; ++r)
                    {
                        if (p + 16u > bytes)
                            return;
                        uint32_t w[4];
                        std::memcpy(w, pkt + p, 16);
                        p += 16u;
                        const uint32_t reg = static_cast<uint32_t>((hi >> (4u * r)) & 0xFu);
                        if (reg == 2u)
                        {
                            std::memcpy(&s, &w[0], 4);
                            std::memcpy(&t, &w[1], 4);
                            std::memcpy(&q, &w[2], 4);
                            if (q == 0.0f)
                                q = 1.0f;
                        }
                        else if (reg == 1u)
                        {
                            for (int k = 0; k < 4; ++k)
                                rgba[k] = static_cast<uint8_t>(w[k] & 0xFFu);
                        }
                        else if (reg == 4u || reg == 5u)
                        {
                            if (o.verts.size() >= 65535u)
                                return;
                            Im2DVertex v;
                            v.x = static_cast<float>(w[0] & 0xFFFFu) / 16.0f;
                            v.y = static_cast<float>(w[1] & 0xFFFFu) / 16.0f;
                            v.z = static_cast<float>(reg == 4u ? (w[2] >> 4) & 0xFFFFFFu : w[2]);
                            v.s = s;
                            v.t = t;
                            v.q = q;
                            std::memcpy(v.rgba, rgba, 4);
                            v.pad = 0;
                            const uint32_t vi = static_cast<uint32_t>(o.verts.size());
                            o.verts.push_back(v);
                            const bool kick = !((w[3] >> 15) & 1u); // ADC clear
                            if (prim == 3u) // triangle list
                            {
                                queue[queued++] = vi;
                                if (queued == 3u)
                                {
                                    if (kick)
                                        o.tris.insert(o.tris.end(), {static_cast<uint16_t>(queue[0]), static_cast<uint16_t>(queue[1]),
                                                                     static_cast<uint16_t>(queue[2])});
                                    queued = 0;
                                }
                            }
                            else if (prim == 4u) // strip
                            {
                                if (queued >= 2u && kick)
                                    o.tris.insert(o.tris.end(), {static_cast<uint16_t>(queue[0]), static_cast<uint16_t>(queue[1]),
                                                                 static_cast<uint16_t>(vi)});
                                if (queued < 2u)
                                    queue[queued++] = vi;
                                else
                                {
                                    queue[0] = queue[1];
                                    queue[1] = vi;
                                }
                            }
                            else if (prim == 5u) // fan
                            {
                                if (queued >= 2u && kick)
                                    o.tris.insert(o.tris.end(), {static_cast<uint16_t>(queue[0]), static_cast<uint16_t>(queue[1]),
                                                                 static_cast<uint16_t>(vi)});
                                if (queued < 2u)
                                    queue[queued++] = vi;
                                else
                                    queue[1] = vi;
                            }
                        }
                    }
            }
        }

        // A crowd job's batches from RenderWare's instance data (PS2All instancing, built once): DMA tags (CNT / NEXT /
        // REF / RET / END) carrying per batch STCYCL 1/3 + UNPACK V3-32 positions (TOP+0), V2-32 UVs (TOP+1), V4-8
        // unsigned colours (TOP+2), ITOP = vertices and the MSCAL. Every batch is unpacked at TOP 0 of the private
        // memory and run there. onlyBatch >= 0: just that batch (verify); *batchVerts gets its ITOP.
        alignas(16) uint8_t g_crowdMem[0x4000]; // VIF thread
        bool crowdRunChain(const CrowdJob &j, GifTris &out, uint8_t *mem, int onlyBatch = -1, uint32_t *batchVerts = nullptr)
        {
            const uint8_t *ee = g_rdram;
            if (!ee || !j.prog || !j.chain)
                return false;
            // The VIF stream: each tag's upper half, then its data (after the tag, or at ADDR for REF / REFS / REFE).
            static thread_local std::vector<uint8_t> st;
            st.clear();
            uint32_t t = j.chain;
            bool ended = false;
            for (uint32_t guard = 0; guard < 512u && !ended; ++guard)
            {
                const uint64_t tag = rd64(ee, t);
                const uint32_t id = static_cast<uint32_t>((tag >> 28) & 7u), qwc = static_cast<uint32_t>(tag & 0xFFFFu);
                const uint32_t addr = static_cast<uint32_t>((tag >> 32) & 0x7FFFFFF0u);
                const uint32_t data = (id == 0u || id == 3u || id == 4u) ? addr : t + 16u;
                if (id == 5u || st.size() + 8u + qwc * 16u > (1u << 20))
                    return false; // CALL: not in instance data
                const uint8_t *hiHalf = ee + ((t + 8u) & 0x1FFFFFFu);
                st.insert(st.end(), hiHalf, hiHalf + 8);
                const uint8_t *d = ee + (data & 0x1FFFFFFu);
                st.insert(st.end(), d, d + qwc * 16u);
                if (id == 1u)
                    t = t + 16u + qwc * 16u;
                else if (id == 2u)
                    t = addr;
                else if (id == 3u || id == 4u)
                    t = t + 16u;
                else
                    ended = true; // REFE, RET, END
            }
            if (!ended)
                return false;
            std::memcpy(mem, j.prog->data.data(), 0x4000u);
            std::memcpy(mem + 957u * 16u, j.consts, j.numConst * 16u);
            for (uint32_t i = 0; i < 16u; ++i)
                if (j.camMask & (1u << i))
                    std::memcpy(mem + (1008u + i) * 16u, j.cam + i * 16u, 16);
            uint32_t wl = 1u, cl = 1u, itop = 0u, pos = 0u;
            int batch = 0;
            const uint32_t size = static_cast<uint32_t>(st.size());
            auto word = [&](uint32_t at) { uint32_t w; std::memcpy(&w, st.data() + at, 4); return w; };
            while (pos + 4u <= size)
            {
                const uint32_t code = word(pos), cmd = (code >> 24) & 0x7Fu, imm = code & 0xFFFFu;
                uint32_t num = (code >> 16) & 0xFFu;
                if (cmd >= 0x60u) // UNPACK
                {
                    num = num ? num : 256u;
                    const uint32_t vn = (cmd >> 2) & 3u, vl = cmd & 3u;
                    if (vl == 3u || (cmd & 0x10u) || cl < wl || !wl)
                        return false; // V4-5, masked or fill-mode unpacks: not in this format
                    const uint32_t compBytes = 4u >> vl, elemBytes = (vn + 1u) * compBytes;
                    const uint32_t bytes = (num * elemBytes + 3u) & ~3u;
                    if (pos + 4u + bytes > size)
                        return false;
                    const bool usn = (imm & 0x4000u) != 0u;
                    const uint32_t base = imm & 0x3FFu; // TOP-relative (FLG) with TOP 0, or absolute
                    for (uint32_t k = 0; k < num; ++k)
                    {
                        const uint32_t qw = (base + (k / wl) * cl + (k % wl)) & 0x3FFu;
                        const uint8_t *e = st.data() + pos + 4u + k * elemBytes;
                        for (uint32_t c = 0; c <= vn; ++c)
                        {
                            uint32_t v;
                            if (compBytes == 4u)
                                std::memcpy(&v, e + c * 4u, 4);
                            else if (compBytes == 2u)
                            {
                                uint16_t h;
                                std::memcpy(&h, e + c * 2u, 2);
                                v = usn ? h : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(h)));
                            }
                            else
                                v = usn ? e[c] : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(e[c])));
                            std::memcpy(mem + qw * 16u + c * 4u, &v, 4);
                        }
                    }
                    pos += 4u + bytes;
                }
                else if (cmd == 0x14u || cmd == 0x15u || cmd == 0x17u) // MSCAL / MSCALF / MSCNT: one batch
                {
                    pos += 4u;
                    if (onlyBatch < 0 || batch == onlyBatch)
                    {
                        if (batchVerts)
                            *batchVerts = itop;
                        if (!::ps2vu1nRunPrivateTop(j.prog->image.data(), j.prog->key, mem, 0u, 0u, itop, gifTrisKick, &out))
                            return false;
                        if (batch == onlyBatch)
                            return true;
                    }
                    ++batch;
                }
                else if (cmd == 0x01u) // STCYCL
                {
                    cl = imm & 0xFFu;
                    wl = (imm >> 8) & 0xFFu;
                    pos += 4u;
                }
                else if (cmd == 0x04u) // ITOP
                {
                    itop = imm & 0x3FFu;
                    pos += 4u;
                }
                else if (cmd == 0x05u) // STMOD: only the plain mode
                {
                    if (imm & 3u)
                        return false;
                    pos += 4u;
                }
                else if (cmd == 0x00u || cmd == 0x02u || cmd == 0x03u || cmd == 0x06u || cmd == 0x07u || cmd == 0x10u ||
                         cmd == 0x11u || cmd == 0x13u) // NOP, OFFSET, BASE, MSKPATH3, MARK, FLUSHE, FLUSH, FLUSHA
                    pos += 4u;
                else if (cmd == 0x20u) // STMASK + mask word
                    pos += 8u;
                else if (cmd == 0x30u || cmd == 0x31u) // STROW / STCOL
                    pos += 20u;
                else
                    return false;
            }
            return onlyBatch < 0;
        }

        std::mutex g_crowdMx;
        std::vector<CrowdJob> g_crowdExpect;
    }

    bool crowdVerifyWanted() { return crowdMode() == 2; }

    // VU1 glue: RenderWare ran the crowd program (entry pc 0 = an object's first batch, 0x520 = the next batch).
    bool crowdIsImage(const uint8_t *code)
    {
        const VuChainProgram *prog = g_rdram ? crowdProgram(g_rdram) : nullptr;
        return prog && std::memcmp(code, prog->image.data(), 0x530u) == 0;
    }

    void crowdVerifyRun(uint32_t pc, uint32_t top, uint32_t itop, const uint8_t *vuBefore, const uint8_t *kicks, uint32_t kickBytes)
    {
        static uint64_t s_runs = 0, s_unpaired = 0, s_inBad = 0, s_outBad = 0, s_exact = 0, s_tris = 0;
        static int s_shown = 0;
        static CrowdJob s_cur;
        static bool s_have = false;
        static uint64_t s_calls = 0;
        if ((++s_calls % 100u) == 1u)
        {
            std::printf("[nrw-crowd] verify calls %llu (paired batches %llu, unpaired %llu)\n", (unsigned long long)s_calls,
                        (unsigned long long)s_runs, (unsigned long long)s_unpaired);
            std::fflush(stdout);
        }
        static int s_batch = 0;
        if (pc == 0u)
        {
            std::lock_guard<std::mutex> lock(g_crowdMx);
            auto it = std::find_if(g_crowdExpect.begin(), g_crowdExpect.end(), [&](const CrowdJob &e)
                                   { return std::memcmp(e.consts, vuBefore + 957u * 16u, e.numConst * 16u) == 0 &&
                                            std::memcmp(e.cam, vuBefore + 1008u * 16u, 4u * 16u) == 0; });
            s_have = it != g_crowdExpect.end();
            if (s_have)
            {
                s_cur = *it;
                g_crowdExpect.erase(it);
            }
            s_batch = 0;
        }
        else
            ++s_batch;
        if (!s_have)
        {
            ++s_unpaired;
            return;
        }
        static GifTris nat, rw;
        nat.verts.clear();
        nat.tris.clear();
        nat.tag = 0;
        uint32_t count = 0;
        if (!crowdRunChain(s_cur, nat, g_crowdMem, s_batch, &count) || count != (itop & 0x3FFu))
        {
            ++s_unpaired;
            return;
        }
        ++s_runs;
        // Inputs: the tiles' used lanes (position xyz, UV xy, colour), the setup's constants, camera and clip block.
        uint32_t badQw = 0, firstBad = 0xFFFFu;
        // RenderWare's batch sits at its TOP (double buffered), the native one at 0.
        const uint32_t rwTop = (top & 0x3FFu) * 16u;
        for (uint32_t i = 0; i < count && rwTop + i * 48u + 48u <= 0x4000u; ++i)
        {
            const uint8_t *n = g_crowdMem + i * 48u, *r = vuBefore + rwTop + i * 48u;
            if (std::memcmp(n, r, 12) != 0 || std::memcmp(n + 16u, r + 16u, 8) != 0 || std::memcmp(n + 32u, r + 32u, 16) != 0)
            {
                if (!badQw)
                    firstBad = 0x400u | i;
                ++badQw;
            }
        }
        auto check = [&](uint32_t a, uint32_t bytes)
        {
            if (std::memcmp(g_crowdMem + a * 16u, vuBefore + a * 16u, bytes) != 0)
            {
                if (!badQw)
                    firstBad = a;
                ++badQw;
            }
        };
        for (uint32_t a = 957u; a < 957u + s_cur.numConst; ++a)
            check(a, 16);
        for (uint32_t a = 1008u; a <= 1011u; ++a)
            check(a, 16);
        for (uint32_t a = 1015u; a <= 1017u; ++a)
            check(a, 12);
        check(1019u, 16);
        check(1021u, 16);
        check(1022u, 16);
        check(1023u, 16);
        rw.verts.clear();
        rw.tris.clear();
        rw.tag = 0;
        if (kickBytes)
            gifTrisKick(&rw, kicks, kickBytes);
        s_tris += nat.tris.size() / 3u;
        const bool outOk = nat.verts.size() == rw.verts.size() && nat.tris == rw.tris &&
                           (nat.verts.empty() || std::memcmp(nat.verts.data(), rw.verts.data(), nat.verts.size() * sizeof(Im2DVertex)) == 0);
        s_inBad += badQw ? 1u : 0u;
        s_outBad += outOk ? 0u : 1u;
        s_exact += (!badQw && outOk) ? 1u : 0u;
        if ((badQw || !outOk) && s_shown < 40)
        {
            ++s_shown;
            uint32_t firstV = 0;
            while (firstV < nat.verts.size() && firstV < rw.verts.size() &&
                   std::memcmp(&nat.verts[firstV], &rw.verts[firstV], sizeof(Im2DVertex)) == 0)
                ++firstV;
            std::printf("[nrw-crowd] MISMATCH pc=%x batch %u count=%u inputs: %u differ (first %u: native %08x %08x %08x %08x rw %08x %08x %08x %08x) "
                        "verts native %zu rw %zu, tris native %zu rw %zu, first differing vertex %u\n",
                        pc, static_cast<uint32_t>(s_batch), count, badQw, firstBad, rd32(g_crowdMem, (firstBad & 0x3FFu) * 16u), rd32(g_crowdMem, (firstBad & 0x3FFu) * 16u + 4u),
                        rd32(g_crowdMem, (firstBad & 0x3FFu) * 16u + 8u), rd32(g_crowdMem, (firstBad & 0x3FFu) * 16u + 12u),
                        rd32(vuBefore, (firstBad & 0x3FFu) * 16u), rd32(vuBefore, (firstBad & 0x3FFu) * 16u + 4u),
                        rd32(vuBefore, (firstBad & 0x3FFu) * 16u + 8u), rd32(vuBefore, (firstBad & 0x3FFu) * 16u + 12u),
                        nat.verts.size(), rw.verts.size(), nat.tris.size() / 3u, rw.tris.size() / 3u, firstV);
            if (firstV < nat.verts.size() && firstV < rw.verts.size())
            {
                const Im2DVertex &a = nat.verts[firstV], &b = rw.verts[firstV];
                std::printf("[nrw-crowd]   native xyz %.4f %.4f %.0f st %.5f %.5f %.5f rgba %u %u %u %u | rw xyz %.4f %.4f %.0f st %.5f %.5f %.5f rgba %u %u %u %u\n",
                            a.x, a.y, a.z, a.s, a.t, a.q, a.rgba[0], a.rgba[1], a.rgba[2], a.rgba[3], b.x, b.y, b.z, b.s, b.t, b.q,
                            b.rgba[0], b.rgba[1], b.rgba[2], b.rgba[3]);
            }
            std::fflush(stdout);
        }
        if ((s_runs % 25u) == 0u)
        {
            std::printf("[nrw-crowd] verified %llu batches (%llu triangles): %llu exact, inputs differ %llu, triangles differ %llu, unpaired %llu\n",
                        (unsigned long long)s_runs, (unsigned long long)s_tris, (unsigned long long)s_exact, (unsigned long long)s_inBad,
                        (unsigned long long)s_outBad, (unsigned long long)s_unpaired);
            std::fflush(stdout);
        }
    }

    // marker(): run a crowd job batch by batch and fill the job's vertices / indices / GIF tag. False: nothing to draw.
    bool crowdMarker(const Entry &e, MeshJob &job)
    {
        const CrowdJob &j = g_crowdJobs[e.code & (kCrowdJobs - 1u)];
        static GifTris out;
        out.verts.clear();
        out.tris.clear();
        out.tag = 0;
        if (!crowdRunChain(j, out, g_crowdMem))
            return false;
        if (out.tris.empty())
            return false;
        const uint32_t nv = static_cast<uint32_t>(out.verts.size());
        const uint32_t vAt = markerRingAlloc(nv * static_cast<uint32_t>(sizeof(Im2DVertex)));
        std::memcpy(g_ring.data() + vAt, out.verts.data(), nv * sizeof(Im2DVertex));
        const uint32_t iAt = markerRingAlloc(static_cast<uint32_t>(out.tris.size()) * 2u);
        std::memcpy(g_ring.data() + iAt, out.tris.data(), out.tris.size() * 2u);
        job.verts = vAt;
        job.numVerts = nv;
        job.ringIndices = iAt;
        job.numIndices = static_cast<uint32_t>(out.tris.size());
        job.envAlpha = e.envAlpha; // the setup's ALPHA_2
        // GIF tag of the kicks (PRE + PRIM: strips, textured, blended, context 2) -> the job's PRIM.
        std::memcpy(job.vu[vuIndex(1018)], &out.tag, 8);
        return true;
    }

    bool crowdNode(uint8_t *ee, PS2Memory &memory, uint32_t objPvt, uint32_t atomic, int mode)
    {
        const int cm = crowdMode();
        if (cm == 0 || mode != 1)
            return false;
        // Why crowd objects stay on RenderWare (verify mode / PS2X_NRW_STATS): one counter per check below.
        static uint64_t s_why[12] = {}, s_calls = 0;
        static const bool s_stats = cm == 2 || std::getenv("PS2X_NRW_STATS") != nullptr;
        auto reject = [&](int k)
        {
            ++s_why[k];
            if (s_stats && (++s_calls % 2000u) == 1u)
            {
                std::printf("[nrw-crowd] node rejects: finalize %llu object %llu verts %llu meshes %llu pipe %llu texture %llu frames %llu program %llu compiled %llu camera %llu consts %llu room %llu\n",
                            (unsigned long long)s_why[0], (unsigned long long)s_why[1], (unsigned long long)s_why[2], (unsigned long long)s_why[3],
                            (unsigned long long)s_why[4], (unsigned long long)s_why[5], (unsigned long long)s_why[6], (unsigned long long)s_why[7],
                            (unsigned long long)s_why[8], (unsigned long long)s_why[9], (unsigned long long)s_why[10], (unsigned long long)s_why[11]);
                std::fflush(stdout);
            }
            return false;
        };
        if (rd32(ee, objPvt + 20u))
            return reject(0);
        const uint32_t crowd = rd32(ee, kGp - 0x7C58u); // the crowd being rendered (RpCrowdRender)
        const uint32_t geom = rd32(ee, atomic + 24u), frame = rd32(ee, atomic + 4u);
        if (!crowd || ee[atomic & 0x1FFFFFFu] != 1u || !geom || !frame || rd32(ee, geom + 24u) != 1u)
            return reject(1);
        // RenderWare's instance data of the crowd mesh (made on its first render through RenderWare).
        const uint32_t res = meshResEntry(ee, atomic, geom), chain = res ? rd32(ee, res + 0x20u) : 0u;
        if (!chain)
            return reject(2);
        // The crowd material pipeline (its bridge, no post-mesh callback) on the one mesh.
        const uint32_t header = rd32(ee, geom + 84u);
        uint16_t numMeshes = 0;
        if (header)
            std::memcpy(&numMeshes, ee + ((header + 4u) & 0x1FFFFFFu), 2);
        if (numMeshes != 1u)
            return reject(3);
        const uint32_t mesh = header + 16u + rd32(ee, header + 12u), material = rd32(ee, mesh + 8u);
        const uint32_t groupPipe = rd32(ee, objPvt + 24u), pipe = groupPipe ? groupPipe : (material ? rd32(ee, material + 8u) : 0u);
        const uint32_t nodes = pipe ? rd32(ee, pipe + 8u) : 0u, matPvt = nodes ? rd32(ee, nodes + 20u) : 0u;
        if (!material || !matPvt || rd32(ee, matPvt + 12u) != kCrowdBridge || rd32(ee, matPvt + 16u) != 0u)
            return reject(4);
        // The crowd's current texture and animation (_rpCrowdPS2AllObjectSetupCallBack).
        const uint32_t textures = rd32(ee, crowd + 0x28u), anims = rd32(ee, crowd + 0x30u);
        const uint32_t texture = textures ? rd32(ee, textures + rd32(ee, crowd + 0x34u) * 4u) : 0u;
        const uint32_t raster = texture ? rd32(ee, texture) : 0u;
        const uint32_t anim = anims ? rd32(ee, anims + rd32(ee, crowd + 0x38u) * 4u) : 0u;
        if (!raster || !anim || (ee[(raster + 32u) & 0x1FFFFFFu] & 7u) != 4u || !rd32(ee, raster + 36u))
            return reject(5);
        const uint32_t numFrames = rd32(ee, anim + 4u);
        const uint32_t cols = rd32(ee, crowd + 0x24u), rows = rd32(ee, crowd + 0x20u);
        if (numFrames == 0u || numFrames > kCrowdMaxFrames || !cols || !rows)
            return reject(6);
        const VuChainProgram *prog = crowdProgram(ee);
        if (!prog)
            return reject(7);
        const uint32_t cam = rd32(ee, kRwGlobals);
        if (!cam)
            return reject(9);
        static ObjExpect obj;
        computeObjectConstants(ee, atomic, false, objPvt, obj, false, true);
        if (obj.unsupported)
            return reject(10);
        MarkerSlot slot;
        if (cm == 1 && !reserveMarkers(ee, 1u, slot))
        {
            nstat(kNsNoRoom);
            return reject(11);
        }
        CrowdJob snap;
        CrowdJob &j = cm == 2 ? snap : g_crowdJobs[g_crowdJobNext & (kCrowdJobs - 1u)];
        j.prog = prog;
        j.atomic = atomic;
        j.res = res;
        j.chain = chain;
        j.numConst = 3u + numFrames;
        // 957: the output GIF tag's upper words (PRE, PRIM 0x25C: strips, IIP, TME, ABE, context 2; regs ST RGBAQ
        // XYZF2 ST XYZF2); 958: frames, 0.5 * height / camera +0x6C, current frame - frames, 1 / cols;
        // 959: crowd +8, rows, crowd +4, 1 / rows; 960..: each frame's UV rectangle (ints) / cols.
        auto f32 = [&](uint32_t a) { float f; std::memcpy(&f, ee + (a & 0x1FFFFFFu), 4); return f; };
        uint32_t curFrame = 0;
        const float t = f32(crowd + 0x3Cu), dur = f32(anim);
        if (!(t < 0.0f) && t < dur)
            curFrame = static_cast<uint32_t>(static_cast<float>(numFrames) * t / dur);
        const float invCols = 1.0f / static_cast<float>(cols), invRows = 1.0f / static_cast<float>(rows);
        const float height = 0.5f * (f32(crowd + 0x18u) / f32(cam + 0x6Cu));
        const uint32_t q957[4] = {0u, 0x512E4000u, 0x00042412u, 0u};
        uint32_t q958[4] = {numFrames, 0u, curFrame - numFrames, 0u}, q959[4] = {rd32(ee, crowd + 8u), rows, rd32(ee, crowd + 4u), 0u};
        std::memcpy(&q958[1], &height, 4);
        std::memcpy(&q958[3], &invCols, 4);
        std::memcpy(&q959[3], &invRows, 4);
        std::memcpy(j.consts, q957, 16);
        std::memcpy(j.consts + 16u, q958, 16);
        std::memcpy(j.consts + 32u, q959, 16);
        for (uint32_t i = 0; i < numFrames; ++i)
            for (uint32_t k = 0; k < 4u; ++k)
            {
                const float v = static_cast<float>(static_cast<int32_t>(rd32(ee, anim + 8u + i * 16u + k * 4u))) * invCols;
                std::memcpy(j.consts + (3u + i) * 16u + k * 4u, &v, 4);
            }
        // 1008..1018: object constants; 1019: material colour (textured scale); 1021..1023: the small clip planes
        // (the bridge zeroes the large-band globals before choosing) and flags.
        j.camMask = 0;
        for (uint32_t i = 0; i <= 10u; ++i)
            if (obj.mask & (1ull << vuIndex(1008u + i)))
            {
                std::memcpy(j.cam + i * 16u, obj.vu[vuIndex(1008u + i)], 16);
                j.camMask |= 1u << i;
            }
        uint8_t col[4];
        std::memcpy(col, ee + ((material + 4u) & 0x1FFFFFFu), 4);
        const float sc = f32(kGp - 0x7F7Cu);
        const float c1019[4] = {col[0] * sc, col[1] * sc, col[2] * sc, col[3] * sc};
        std::memcpy(j.cam + 11u * 16u, c1019, 16);
        std::memcpy(j.cam + 13u * 16u, ee + (0x4BE150u & 0x1FFFFFFu), 16);
        std::memcpy(j.cam + 14u * 16u, ee + (0x4BE160u & 0x1FFFFFFu), 16);
        const uint32_t transType = ee[(kGp - 0x793Cu) & 0x1FFFFFFu];
        const uint32_t w1023[4] = {transType & 7u, 0u, rd32(ee, kGp - 0x7988u) == 3u ? 0x20u : 0u, rd32(ee, kGp - 0x7930u)};
        std::memcpy(j.cam + 15u * 16u, w1023, 16);
        j.camMask |= (1u << 11) | (1u << 13) | (1u << 14) | (1u << 15);
        // Does the crowd program run as compiled code? Once, on this object's first batch (EE side, own memory).
        static int s_compiled = 0;
        if (s_compiled == 0)
        {
            alignas(16) static uint8_t probeMem[0x4000];
            static GifTris probeOut;
            s_compiled = crowdRunChain(j, probeOut, probeMem, 0) ? 1 : -1;
            std::printf("[nrw-crowd] program %s as compiled code (%zu triangles on the first batch)\n",
                        s_compiled > 0 ? "runs" : "does NOT run", probeOut.tris.size() / 3u);
            std::fflush(stdout);
        }
        if (s_compiled < 0)
            return reject(8);
        if (cm == 2)
        {
            std::lock_guard<std::mutex> lock(g_crowdMx);
            if (g_crowdExpect.size() >= 64u)
                g_crowdExpect.erase(g_crowdExpect.begin());
            g_crowdExpect.push_back(snap);
            return false; // RenderWare draws; each of its batches is repeated natively and compared
        }
        const uint32_t jobIndex = g_crowdJobNext++ & (kCrowdJobs - 1u);
        useResEntry(ee, res); // the tiles live in the instance data
        const uint32_t fa = rd32(ee, texture + 80u);
        const uint64_t tex1Global = rd64(ee, kGp - 0x79D8u);
        const uint32_t rext = raster + rd32(ee, kGp - 0x798Cu);
        const uint64_t kl = ee[(rext + 20u) & 0x1FFFFFFu] | (static_cast<uint64_t>(ee[(rext + 21u) & 0x1FFFFFFu]) << 8);
        uint64_t tex1 = (tex1Global & 0xFFFFF000FFE7FDE3ull) | ((kl & 0xFFFu) << 32) | ee[(rext + 22u) & 0x1FFFFFFu] | (((kl >> 12) & 3u) << 19);
        tex1 = (tex1 & ~0x1E0ull) | texFilterBits(fa & 0xFFu);
        const uint32_t idx = g_next.fetch_add(1, std::memory_order_relaxed) & (kTableSize - 1u);
        Entry &e = g_table[idx];
        e.native = true;
        e.gsFromRegs = false;
        e.kind = kIm2DMesh;
        e.object = atomic;
        e.mesh = mesh;
        e.meshFlags = 0;
        e.primType = 4u;
        e.transType = transType;
        e.matCol = 0xFFFFFFFFu;
        e.texture = texture;
        e.vu1CodeIndex = 0;
        e.surfProps = 0;
        e.code = kCrowdEntry | jobIndex; // vertices come from the program run at the marker
        e.verts = 0;
        e.normals = e.uvs = e.prelit = 0;
        e.numVerts = 0;
        e.geomFlags = 0;
        e.gsTest = 0x503FDu;  // TEST_2: alpha > 0x3F, depth GEQUAL
        e.gsTex1 = tex1;
        e.gsClamp = 0u;       // CLAMP_2: repeat
        e.geomGen = 0;
        e.envTexture = 0;
        e.envFlags = 0;
        e.envAlpha = 0x8000000044ull; // ALPHA_2: (Cs - Cd) * As + Cd (submitIm2D applies it to raster-state jobs)
        e.ringIndices = 0;
        e.numIndices = 0;
        std::memset(g_nativeVu[idx & (kNativeVuRing - 1u)], 0, sizeof(float) * 4u * kVuQwCount);
        writeMarkers(ee, memory, slot, &idx, 1u);
        nstat(kNsNative);
        return true;
    }

    bool nodeBody(uint8_t *ee, PS2Memory &memory, uint32_t self, uint32_t params)
    {
        const int mode = nodeMode();
        if (mode == 0 || !enabled() || !g_gs)
            return false;
        if (mode == 2)
            g_preds.clear(); // predictions hold for this node call only (RW's bridges follow before the next)
        const uint32_t objPvt = rd32(ee, self + 0x14u), object = rd32(ee, params);
        if (!objPvt || !object)
            return false;
        const uint32_t setupCB = rd32(ee, objPvt + 16u);
        if (setupCB == 0x300250u)
            return im3dNode(ee, memory, objPvt, object, mode);
        if (setupCB == 0x2976F0u && pgenNode(ee, memory, objPvt, object, mode))
            return true;
        if (setupCB == kCrowdSetup && crowdNode(ee, memory, objPvt, object, mode))
            return true;
        bool sector, gameCB = false, g3x = false, pgen = false, skin = false;
        if (setupCB == 0x2976F0u)
        {
            // The game's PGenObjectSetupCallback (particle geometry): LTM x view, no lighting setup (the node's null
            // block), not modulated, always the clipping variant.
            sector = false;
            pgen = true;
        }
        else if (setupCB == 0x2DCA30u)
        {
            // RpPDS_G3x_Generic_AtomicPS2AllSetupCallBack (most of the game's models): the same frustum test, prim/
            // trans and modulate rules as RenderWare's atomic setup; lit by the G3x ADL lights instead of the world's.
            sector = false;
            g3x = true;
        }
        else if (setupCB == 0x2FDBB0u)
            sector = false; // RpAtomicPS2AllObjectSetupCallBack
        else if (setupCB == 0x302720u)
            sector = true; // RpWorldSectorPS2AllObjectSetupCallBack
        else if (setupCB == 0x2E0540u && skinNative() && mode == 1)
        {
            // RpPDS_G3_Skin_PS2AllObjectSetupCallBack (drivers, wrestlers): RenderWare's atomic setup with RpSkin bone
            // matrices; drawn from vertices skinned on the CPU (skinVertices), always the clipping variant.
            sector = false;
            skin = true;
        }
        else if (setupCB == 0x2B17A0u)
        {
            // The game's object_setup_callback: RenderWare's atomic setup (same LTM x view transform and
            // lighting) without the frustum test (always the clipping VU1 variant) and always modulated.
            sector = false;
            gameCB = true;
        }
        else
        {
            nstat(kNsOtherSetup);
            static const bool s_hist = std::getenv("PS2X_NRW_STATS") != nullptr;
            if (s_hist && setupCB == 0x2E0540u)
            {
                // RpPDS_G3_Skin: does the geometry keep its source arrays (verts, skin weights/indices)?
                static int s_skinShown = 0;
                const uint32_t geom = rd32(ee, object + 24u);
                if (s_skinShown < 12 && geom)
                {
                    ++s_skinShown;
                    const uint32_t mt = rd32(ee, geom + 92u), skin = rd32(ee, geom + rd32(ee, 0x466168u));
                    std::printf("[nrw-skin] atomic=%08x geom=%08x flags=%08x numVerts=%u numMT=%u verts=%08x normals=%08x uvs=%08x "
                                "skin=%08x bones=%u ids=%u inv=%08x maxW=%u vIdx=%08x vW=%08x frame=%08x\n",
                                object, geom, rd32(ee, geom + 8u), rd32(ee, geom + 20u), rd32(ee, geom + 24u),
                                mt ? rd32(ee, mt + 20u) : 0u, mt ? rd32(ee, mt + 24u) : 0u, rd32(ee, geom + 52u), skin,
                                skin ? rd32(ee, skin) : 0u, skin ? rd32(ee, skin + 4u) : 0u, skin ? rd32(ee, skin + 12u) : 0u,
                                skin ? rd32(ee, skin + 16u) : 0u, skin ? rd32(ee, skin + 20u) : 0u, skin ? rd32(ee, skin + 24u) : 0u,
                                rd32(ee, object + 4u));
                }
            }
            if (s_hist)
            {
                // Which object setup callbacks keep objects on RenderWare's pipeline (finalize CB alongside).
                static std::unordered_map<uint64_t, uint64_t> s_cbs;
                static uint64_t s_n = 0;
                static int s_pipeDumps = 0;
                if (s_pipeDumps < 6 && !s_cbs.count((static_cast<uint64_t>(setupCB) << 32) | rd32(ee, objPvt + 20u)))
                {
                    // First object of each unsupported setup: its group material pipeline (bridge, code array).
                    ++s_pipeDumps;
                    const uint32_t gp = rd32(ee, objPvt + 24u), nodes = gp ? rd32(ee, gp + 8u) : 0u;
                    const uint32_t mp = nodes ? rd32(ee, nodes + 20u) : 0u;
                    std::printf("[nrw-node-setup-pipe] setup=%08x obj=%08x group=%08x bridge=%08x post=%08x code=%08x codes=%08x,%08x,%08x,%08x\n",
                                setupCB, object, gp, mp ? rd32(ee, mp + 12u) : 0u, mp ? rd32(ee, mp + 16u) : 0u,
                                mp ? rd32(ee, mp + 24u) : 0u, mp ? rd32(ee, rd32(ee, mp + 24u)) : 0u,
                                mp ? rd32(ee, rd32(ee, mp + 24u) + 4u) : 0u, mp ? rd32(ee, rd32(ee, mp + 24u) + 8u) : 0u,
                                mp ? rd32(ee, rd32(ee, mp + 24u) + 12u) : 0u);
                }
                ++s_cbs[(static_cast<uint64_t>(setupCB) << 32) | rd32(ee, objPvt + 20u)];
                if ((++s_n % 50000u) == 0u)
                {
                    std::printf("[nrw-node-setup]");
                    for (const auto &kv : s_cbs)
                        std::printf(" %08x/%08x:%llu", static_cast<uint32_t>(kv.first >> 32), static_cast<uint32_t>(kv.first),
                                    (unsigned long long)kv.second);
                    std::printf("\n");
                    std::fflush(stdout);
                }
            }
            return false;
        }
        if (rd32(ee, objPvt + 20u))
        {
            nstat(kNsFinalize);
            return false;
        }
        const uint32_t cam = rd32(ee, kRwGlobals), world = rd32(ee, kRwGlobals + 4u);
        if (!cam || !world)
            return false;
        uint32_t header, verts, normals, uvs, prelit, numVerts, geomFlags = 0, geomGen = 0;
        if (sector)
        {
            if (rd32(ee, object) != 0xFFFFFFFFu)
            {
                nstat(kNsObject);
                return false;
            }
            header = rd32(ee, object + 128u);
            verts = rd32(ee, object + 8u);
            normals = rd32(ee, object + 12u);
            uvs = rd32(ee, object + 16u);
            prelit = rd32(ee, object + 48u);
            uint16_t nv;
            std::memcpy(&nv, ee + ((object + 138u) & 0x1FFFFFFu), 2);
            numVerts = nv;
        }
        else
        {
            const uint32_t geom = rd32(ee, object + 24u), frame = rd32(ee, object + 4u);
            if (ee[object & 0x1FFFFFFu] != 1u || !geom || !frame || rd32(ee, geom + 24u) != 1u || (rd32(ee, object + 76u) & 2u))
            {
                nstat(kNsObject);
                return false;
            }
            const uint32_t root = rd32(ee, frame + 160u);
            if (!root || (ee[(root + 3u) & 0x1FFFFFFu] & 1u)) // RwFrameGetLTM would resync the hierarchy
            {
                nstat(kNsDirty);
                return false;
            }
            // Geometry edited since RenderWare last instanced it (RpGeometryLock): let RenderWare re-instance
            // (it clears the flags) and draw natively from the next frame with a fresh vertex buffer.
            uint16_t locked;
            std::memcpy(&locked, ee + ((geom + 12u) & 0x1FFFFFFu), 2);
            if (locked)
            {
                ++g_geomGen[geom];
                nstat(kNsLocked);
                return false;
            }
            const auto gi = g_geomGen.find(geom);
            geomGen = gi == g_geomGen.end() ? 0u : gi->second;
            header = rd32(ee, geom + 84u);
            const uint32_t mt = rd32(ee, geom + 92u);
            geomFlags = rd32(ee, geom + 8u);
            verts = mt ? rd32(ee, mt + 20u) : 0u;
            normals = mt ? rd32(ee, mt + 24u) : 0u;
            prelit = g3x ? 0u : rd32(ee, geom + 48u); // the G3x program has no vertex colour stream
            uvs = rd32(ee, geom + 52u);
            numVerts = rd32(ee, geom + 20u);
        }
        if (!header || !verts || numVerts == 0u || numVerts > 65535u)
        {
            nstat(kNsObject);
            return false;
        }
        const uint32_t hdrFlags = rd32(ee, header);
        uint16_t numMeshes16;
        std::memcpy(&numMeshes16, ee + ((header + 4u) & 0x1FFFFFFu), 2);
        const uint32_t numMeshes = numMeshes16;
        if ((hdrFlags & 0xFEu) != 0u || numMeshes > 64u) // tri strips / lists only (fans, lines, points: RW)
        {
            nstat(kNsPrim);
            static const bool s_diag = std::getenv("PS2X_NRW_STATS") != nullptr;
            static int s_n = 0;
            if (s_diag && s_n < 12)
            {
                ++s_n;
                const uint32_t m0 = header + 16u + rd32(ee, header + 12u), mat0 = rd32(ee, m0 + 8u);
                const uint32_t gp = rd32(ee, objPvt + 24u), mpipe = gp ? gp : (mat0 ? rd32(ee, mat0 + 8u) : 0u);
                const uint32_t nodes = mpipe ? rd32(ee, mpipe + 8u) : 0u, mp = nodes ? rd32(ee, nodes + 20u) : 0u;
                std::printf("[nrw-node-prim] setup=%08x obj=%08x meshHeaderFlags=%08x meshes=%u pipe=%08x bridge=%08x code=%08x codes=%08x,%08x numIdx=%u tex=%08x\n",
                            setupCB, object, hdrFlags, numMeshes, mpipe, mp ? rd32(ee, mp + 12u) : 0u, mp ? rd32(ee, mp + 24u) : 0u,
                            mp ? rd32(ee, rd32(ee, mp + 24u)) : 0u, mp ? rd32(ee, rd32(ee, mp + 24u) + 4u) : 0u, rd32(ee, m0 + 4u),
                            mat0 ? rd32(ee, mat0) : 0u);
            }
            return false;
        }
        const uint32_t meshes = header + 16u + rd32(ee, header + 12u);
        // Every mesh on RenderWare's default PS2All material pipeline (generic bridge, VU1 code, no post-mesh), or
        // for G3x objects on a PDS G3x Generic material pipeline (its bridge, the G3x ADL VU1 code, no post-mesh).
        const uint32_t defPipe = rd32(ee, kRwGlobals + rd32(ee, kGp - 0x7900u) + 0x44u);
        const uint32_t groupPipe = rd32(ee, objPvt + 24u);
        // PDS G3 MatFX material pipeline (RpPDS_G3_Matfx_PS2AllMatBridgeCallBack): a material without an effect goes
        // to the generic bridge with the generic VU1 programs (MatFX code array entries 0..3 = generic 0..3, and a
        // triangle mesh's code index is below 4), so it is drawn exactly like the default pipeline.
        // The game's bridge_callback (0x2b0e00) is a recompiled copy of the generic bridge (generic code array; its
        // post-mesh callback only gathers metrics, an empty function in this build).
        enum { kPipeNo, kPipeGeneric, kPipeMatFX, kPipeG3x, kPipeGame, kPipeG3xGem, kPipeSkin };
        auto pipeKind = [&](uint32_t pipe) -> int
        {
            static uint32_t s_pipe[7] = {};
            for (int k = 1; k < 7; ++k)
                if (pipe && s_pipe[k] == pipe)
                    return k;
            const uint32_t nodes = pipe ? rd32(ee, pipe + 8u) : 0u;
            const uint32_t matPvt = nodes ? rd32(ee, nodes + 20u) : 0u;
            if (!matPvt)
                return kPipeNo;
            const uint32_t bcb = rd32(ee, matPvt + 12u), code = rd32(ee, matPvt + 24u), post = rd32(ee, matPvt + 16u);
            if (post != 0u && !(bcb == 0x2B0E00u && post == 0x2FE740u))
                return kPipeNo;
            int k = kPipeNo;
            if (bcb == 0x2B0E00u && code == 0x407440u)
                k = kPipeGame;
            else if (bcb == 0x2FDF80u && pipe == defPipe)
                k = kPipeGeneric;
            else if (bcb == 0x2E03B0u && code == 0x407680u)
                k = kPipeMatFX;
            else if (bcb == 0x2DC320u && code == 0x48FA30u)
                k = kPipeG3x;
            else if (bcb == 0x2FDF80u && code == 0x407850u)
                k = kPipeSkin; // the generic bridge with RpPDS_G3_Skin's VU1 code array
            else if (bcb == 0x2E35E0u && code == 0x48FA40u)
                k = kPipeG3xGem;
            if (k != kPipeNo)
                s_pipe[k] = pipe;
            return k;
        };
        const uint32_t matfxOffset = rd32(ee, kGp - 0x7C48u);
        static const bool s_gem = []()
        {
            const char *v = std::getenv("PS2X_NRW_GEM"); // =0: env-mapped objects stay on RenderWare
            return !(v && *v == '0');
        }();
        // Env map (RpPDS_G3x_Gem_MeshPS2AllBridgeCallBack / RpPDS_G3_Gem_PS2AllMatBridgeCallBack): matfx data
        // {frame, texture, coef, fbAlpha}; the env raster must be a plain texture raster or a camera texture, and
        // the env frame's LTM in sync.
        auto gemEnvOk = [&](uint32_t material)
        {
            const uint32_t fx = rd32(ee, material + matfxOffset);
            const uint32_t envTex = fx ? rd32(ee, fx + 4u) : 0u, envRaster = envTex ? rd32(ee, envTex) : 0u;
            static const bool s_diag = std::getenv("PS2X_NRW_STATS") != nullptr;
            static uint64_t s_rej[4] = {};
            auto reject = [&](int why)
            {
                ++g_pipeWhy[4];
                if (s_diag && (++s_rej[why] % 20000u) == 1u)
                    std::printf("[nrw-gem-reject] raster=%llu rasterType=%llu frame=%llu (type %u pixels %08x)\n",
                                (unsigned long long)s_rej[0], (unsigned long long)s_rej[1], (unsigned long long)s_rej[2],
                                envRaster ? ee[(envRaster + 32u) & 0x1FFFFFFu] : 0u, envRaster ? rd32(ee, envRaster + 36u) : 0u);
                return false;
            };
            if (!envRaster)
                return reject(0);
            // A texture raster with its upload chain, or a camera texture (rendered by the GS, sampled from
            // its render target at the raster's VRAM address).
            const uint32_t envType = ee[(envRaster + 32u) & 0x1FFFFFFu] & 7u;
            if (envType != 5u && (envType != 4u || !rd32(ee, envRaster + 36u)))
                return reject(1);
            uint32_t envFrame = rd32(ee, fx);
            if (!envFrame)
                envFrame = rd32(ee, cam + 4u);
            const uint32_t envRoot = envFrame ? rd32(ee, envFrame + 160u) : 0u;
            if (!envRoot || (ee[(envRoot + 3u) & 0x1FFFFFFu] & 1u))
                return reject(2);
            return true;
        };
        auto pipeOk = [&](uint32_t pipe, uint32_t material)
        {
            const int k = pipeKind(pipe);
            if (g3x)
            {
                if (k == kPipeG3x)
                    return true;
                if (k != kPipeG3xGem)
                    ++g_pipeWhy[3];
                return k == kPipeG3xGem && s_gem && gemEnvOk(material);
            }
            if (skin)
                return k == kPipeSkin;
            if (k == kPipeGeneric || k == kPipeGame)
                return true;
            if (k != kPipeMatFX || (ee[(kGp - 0x793Cu) & 0x1FFFFFFu] & 0x10u)) // trans 0x10: code index 4+ (MatFX effect codes)
            {
                ++g_pipeWhy[k != kPipeMatFX ? 0 : 1];
                return false;
            }
            const uint32_t fx = rd32(ee, material + matfxOffset);
            const uint32_t effect = fx ? rd32(ee, fx + 0x80u) : 0u;
            static const bool s_fxDiag = std::getenv("PS2X_NRW_STATS") != nullptr;
            if (s_fxDiag && effect != 0u)
            {
                // MatFX effects kept on RenderWare (1 Emb bump, 2 Gem env, 3 Bem bump+env, 4 Dup dual pass).
                static uint64_t s_fx[8] = {}, s_n = 0;
                ++s_fx[effect & 7u];
                if ((++s_n % 5000u) == 1u)
                    std::printf("[nrw-matfx] effects 1..5: %llu %llu %llu %llu %llu (sector %d)\n", (unsigned long long)s_fx[1],
                                (unsigned long long)s_fx[2], (unsigned long long)s_fx[3], (unsigned long long)s_fx[4],
                                (unsigned long long)s_fx[5], sector ? 1 : 0);
            }
            // MatFX env maps on generic objects natively (PS2X_NRW_G3GEM=0: RenderWare). Together with native ISO
            // draws they once corrupted the Match Type panel: the masked PATH3 upload queue fell behind for good
            // (fixed in PS2Memory::submitGifPacket, a new GIF kick drains the previous one).
            static const bool s_g3Gem = []()
            {
                const char *v = std::getenv("PS2X_NRW_G3GEM");
                return !(v && *v == '0');
            }();
            if (effect == 2u && s_gem && s_g3Gem) // Gem: the generic programs plus an additive env pass
            {
                // Plain env textures only: camera-texture env maps on these objects were not live render targets
                // (each draw read VRAM back, the GPU stalled for seconds).
                const uint32_t envTex = fx ? rd32(ee, fx + 4u) : 0u, envRaster = envTex ? rd32(ee, envTex) : 0u;
                if (envRaster && (ee[(envRaster + 32u) & 0x1FFFFFFu] & 7u) == 4u)
                    return gemEnvOk(material);
                ++g_pipeWhy[4];
                return false;
            }
            if (effect != 0u)
                ++g_pipeWhy[2];
            return effect == 0u;
        };
        for (uint32_t i = 0; i < numMeshes; ++i)
        {
            const uint32_t material = rd32(ee, meshes + i * 12u + 8u);
            const uint32_t matPipe = material ? rd32(ee, material + 8u) : 0u;
            const uint32_t pipe = groupPipe ? groupPipe : (matPipe ? matPipe : defPipe);
            if (!material || !pipeOk(pipe, material))
            {
                nstat(kNsPipe);
                static const bool s_diag = std::getenv("PS2X_NRW_STATS") != nullptr;
                if (s_diag)
                {
                    // Which material pipelines keep objects on RenderWare's path (bridge / code array / post-mesh).
                    static std::unordered_map<uint64_t, uint32_t> s_seen;
                    static uint64_t s_total = 0;
                    const uint32_t nodes = pipe ? rd32(ee, pipe + 8u) : 0u;
                    const uint32_t matPvt = nodes ? rd32(ee, nodes + 20u) : 0u;
                    const uint32_t bcb = matPvt ? rd32(ee, matPvt + 12u) : 0u, code = matPvt ? rd32(ee, matPvt + 24u) : 0u;
                    const uint64_t key = (static_cast<uint64_t>(setupCB) << 32) | bcb;
                    if (s_seen.size() < 64)
                        ++s_seen[key];
                    if ((++s_total % 100000u) == 0u)
                    {
                        std::printf("[nrw-node-pipes]");
                        for (const auto &kv : s_seen)
                            std::printf(" %08x/%08x:%u", static_cast<uint32_t>(kv.first >> 32), static_cast<uint32_t>(kv.first), kv.second);
                        std::printf("\n");
                    }
                    if (s_seen[key] == 1u)
                        std::printf("[nrw-node-pipe] setup=%08x pipe=%08x(def %08x group %08x) bridge=%08x code=%08x postMesh=%08x\n",
                                    setupCB, pipe, defPipe, groupPipe, bcb, code, matPvt ? rd32(ee, matPvt + 16u) : 0u);
                }
                return false;
            }
            // Textures must be plain texture rasters with an upload chain. Camera textures (render targets) stay on
            // RenderWare: its texture cache places them in VRAM (RpSkyTexCacheAccessRaster), which the node path
            // skips -- drawing them natively blacked out the menus and the arena intro, which composite through them.
            const uint32_t texture = rd32(ee, material), raster = texture ? rd32(ee, texture) : 0u;
            const uint32_t rasterType = raster ? (ee[(raster + 32u) & 0x1FFFFFFu] & 7u) : 0u;
            if (texture && (!raster || rasterType != 4u || !rd32(ee, raster + 36u)))
            {
                ++g_pipeWhy[5];
                static const bool s_texDiag = std::getenv("PS2X_NRW_STATS") != nullptr;
                static int s_texShown = 0;
                if (s_texDiag && raster && s_texShown < 16)
                {
                    ++s_texShown;
                    uint32_t w = rd32(ee, raster + 12u), h = rd32(ee, raster + 16u);
                    std::printf("[nrw-tex-reject] setup=%08x obj=%08x tex=%08x raster=%08x type=%u flags=%02x priv=%02x fmt=%02x %ux%u chain=%08x pixels=%08x parent=%08x\n",
                                setupCB, object, texture, raster, ee[(raster + 32u) & 0x1FFFFFFu] & 7u, ee[(raster + 33u) & 0x1FFFFFFu],
                                ee[(raster + 34u) & 0x1FFFFFFu], ee[(raster + 35u) & 0x1FFFFFFu], w, h, rd32(ee, raster + 36u),
                                rd32(ee, raster + 4u), rd32(ee, raster));
                }
                nstat(kNsPipe);
                return false;
            }
        }
        MarkerSlot slot;
        if (mode == 1 && !reserveMarkers(ee, numMeshes, slot))
        {
            nstat(kNsNoRoom);
            return false;
        }
        static ObjExpect obj;
        computeObjectConstants(ee, object, sector, objPvt, obj, g3x, pgen);
        if (obj.unsupported)
        {
            nstat(g3x && (!g_g3xAmbient || !g_g3xDirectional) ? kNsG3xNoLights : kNsLight);
            return false;
        }
        // RpAtomic/RpWorldSector PS2All prim/trans setup: strip -> PRIM 4, list -> PRIM 3 + LIST; clip unless inside.
        const uint32_t transBase = ee[(kGp - 0x793Cu) & 0x1FFFFFFu];
        if (!isoNative(transBase))
        {
            nstat(kNsPrim);
            return false;
        }
        const uint32_t primType = (hdrFlags & 1u) ? 4u : 3u;
        uint32_t transType = (hdrFlags & 1u) ? transBase : ((transBase | 4u) & 0xFFu);
        if (gameCB || pgen || skin || frustumTest(ee, object, sector, cam) != 2u)
            transType |= 2u;
        const bool modulate = !pgen && (gameCB || (sector ? (rd32(ee, world + 8u) & 0x40u) != 0u : (geomFlags & 0x40u) != 0u));
        const uint64_t primGlobal = rd64(ee, kGp - 0x7CC0u);
        const uint64_t testGlobal = rd64(ee, kGp - 0x79F8u), tex1Global = rd64(ee, kGp - 0x79D8u), clampGlobal = rd64(ee, kGp - 0x79E0u);
        uint32_t skinRing = 0;
        if (skin)
        {
            skinRing = skinVertices(ee, object, rd32(ee, object + 24u), numVerts, verts, normals, uvs, prelit);
            if (skinRing == ~0u)
            {
                nstat(kNsObject);
                return false;
            }
        }
        uint32_t idxs[64];
        for (uint32_t i = 0; i < numMeshes; ++i)
        {
            const uint32_t m = meshes + i * 12u, material = rd32(ee, m + 8u);
            const uint32_t texture = rd32(ee, material), raster = texture ? rd32(ee, texture) : 0u;
            uint8_t col[4] = {0xFF, 0xFF, 0xFF, 0xFF};
            if (modulate)
                std::memcpy(col, ee + ((material + 4u) & 0x1FFFFFFu), 4);
            const uint32_t fmt = raster ? (ee[(raster + 35u) & 0x1FFFFFFu] & 0xFu) : 0u;
            const bool texAlpha = raster && (fmt == 1u || fmt == 5u);
            const uint64_t primState = raster ? ((primGlobal & ~0x50ull) | 0x10u) : (primGlobal & ~0x50ull);
            // Dry run: no marker index, the prediction is checked at RenderWare's bridge instead.
            const uint32_t idx = mode == 2 ? 0u : g_next.fetch_add(1, std::memory_order_relaxed) & (kTableSize - 1u);
            idxs[i] = idx;
            static ObjExpect s_dry;
            ObjExpect &ex = mode == 2 ? s_dry : g_objExpect[idx & (kExpectRing - 1u)];
            ex = obj;
            meshConstants(ee, transType, primType, col, primState, texAlpha, material + 12u, 0.0f, ex, g3x);
            // GS state of the bridge: alpha test on for alpha textures, TEX1 filter, CLAMP from the address modes.
            const uint64_t test = (testGlobal & ~1ull) | (texAlpha ? 1u : 0u);
            uint64_t tex1 = tex1Global, clamp = clampGlobal;
            if (texture)
            {
                const uint32_t fa = rd32(ee, texture + 80u);
                // RpSkyTexCacheAccessRaster: K/L (mipmapKL) and MXL (maxMipLevel, pre-shifted) from the raster
                // extension, then the bridge's filter (MMAG/MMIN).
                const uint32_t rext = raster + rd32(ee, kGp - 0x798Cu);
                const uint64_t kl = ee[(rext + 20u) & 0x1FFFFFFu] | (static_cast<uint64_t>(ee[(rext + 21u) & 0x1FFFFFFu]) << 8);
                tex1 = (tex1Global & 0xFFFFF000FFE7FDE3ull) | ((kl & 0xFFFu) << 32) | ee[(rext + 22u) & 0x1FFFFFFu] |
                       (((kl >> 12) & 3u) << 19);
                tex1 = (tex1 & ~0x1E0ull) | texFilterBits(fa & 0xFFu);
                clamp = (((fa >> 8) & 0xFu) == 3u ? 1u : 0u) | (((fa >> 12) & 0xFu) == 3u ? 4u : 0u);
            }
            uint32_t matCol;
            std::memcpy(&matCol, col, 4);
            if (mode == 2)
            {
                const uint64_t key = (static_cast<uint64_t>(object) << 32) | m;
                g_preds[key] = NodePred{transType, primType, matCol, texture, test, tex1, clamp, cam, g3x, ex};
                continue;
            }
            Entry &e = g_table[idx];
            e.native = true;
            e.kind = sector ? kWorldSectorMesh : kAtomicMesh;
            e.object = object;
            e.mesh = m;
            e.meshFlags = hdrFlags;
            e.primType = primType;
            e.transType = transType;
            e.matCol = matCol;
            e.texture = texture;
            e.vu1CodeIndex = transType & 0x1Fu;
            e.surfProps = material + 12u;
            e.code = 0;
            e.verts = verts;
            e.normals = normals;
            e.uvs = uvs;
            e.prelit = prelit;
            e.numVerts = numVerts;
            e.geomFlags = geomFlags;
            e.gsTest = test;
            e.gsTex1 = tex1;
            e.gsClamp = clamp;
            e.geomGen = geomGen;
            e.envTexture = 0;
            e.envFlags = (raster && (ee[(raster + 32u) & 0x1FFFFFFu] & 7u) == 5u) ? kBaseRenderTarget : 0u;
            e.gsFromRegs = false;
            if (skin)
            {
                // This frame's skinned vertices (ring) with the mesh's indices: drawn like an Im3D job, with this
                // mesh's native GS state and the atomic's lit constants.
                const uint32_t mi = rd32(ee, m), mn = std::min<uint32_t>(rd32(ee, m + 4u), 1u << 20);
                e.kind = kIm3DMesh;
                e.verts = skinRing;
                e.normals = e.uvs = e.prelit = 0;
                e.numIndices = mn;
                e.ringIndices = ringAlloc(mn * 2u);
                std::memcpy(g_ring.data() + e.ringIndices, ee + (mi & 0x1FFFFFFu),
                            std::min<uint32_t>(mn * 2u, 0x2000000u - (mi & 0x1FFFFFFu)));
            }
            const uint32_t meshMatPipe = rd32(ee, material + 8u);
            const int meshPipe = pipeKind(groupPipe ? groupPipe : (meshMatPipe ? meshMatPipe : defPipe));
            const uint32_t meshFx = rd32(ee, material + matfxOffset);
            // RpPDS_G3_Gem_PS2AllMatBridgeCallBack (MatFX effect 2 on RenderWare's generic atomics / sectors): its VU1
            // env pass computes UV = 0.5 - 0.5 * (N x M).xy with M = _rpMatFXSyncEffectMatrix (object LTM x inverse
            // env frame LTM, orthonormalised; the inverse env LTM alone for world sectors) -- the G3x formula with
            // the env vectors negated (the UVs wrap). Same GS state as the G3x env pass.
            const bool g3Gem = !g3x && meshPipe == kPipeMatFX && meshFx && rd32(ee, meshFx + 0x80u) == 2u;
            if ((g3x && meshPipe == kPipeG3xGem) || g3Gem)
            {
                {
                    // RpPDS_G3x_Gem_MeshPS2AllBridgeCallBack + its VU1 program: env frame right/up (the camera's frame
                    // without one) into object space -- dot with the LTM rows, normalised by 1 / |LTM right| -- and
                    // the env pass state (ALPHA_2 FIX or destination alpha by fbAlpha; TEX1_2 with the env raster's
                    // mip settings over the base filter).
                    const uint32_t fx = rd32(ee, material + matfxOffset);
                    uint32_t envFrame = rd32(ee, fx);
                    if (!envFrame)
                        envFrame = rd32(ee, cam + 4u);
                    const Mat env = readMat(ee, envFrame + 80u);
                    if (sector)
                    {
                        for (int i = 0; i < 3; ++i)
                        {
                            e.envR[i] = env.r[0][i];
                            e.envU[i] = env.r[1][i];
                        }
                    }
                    else
                    {
                        const Mat ltm = readMat(ee, rd32(ee, object + 4u) + 80u);
                        const float l2 = ltm.r[0][0] * ltm.r[0][0] + ltm.r[0][1] * ltm.r[0][1] + ltm.r[0][2] * ltm.r[0][2];
                        const float q = l2 > 0.0f ? 1.0f / std::sqrt(l2) : 1.0f;
                        for (int i = 0; i < 3; ++i)
                        {
                            e.envR[i] = (ltm.r[i][0] * env.r[0][0] + ltm.r[i][1] * env.r[0][1] + ltm.r[i][2] * env.r[0][2]) * q;
                            e.envU[i] = (ltm.r[i][0] * env.r[1][0] + ltm.r[i][1] * env.r[1][1] + ltm.r[i][2] * env.r[1][2]) * q;
                        }
                    }
                    if (g3Gem)
                        for (int i = 0; i < 3; ++i)
                        {
                            e.envR[i] = -e.envR[i];
                            e.envU[i] = -e.envU[i];
                        }
                    float coef;
                    std::memcpy(&coef, ee + ((fx + 8u) & 0x1FFFFFFu), 4);
                    e.envTexture = coef > 0.0f ? rd32(ee, fx + 4u) : 0u; // coefficient 0: the pass adds nothing
                    if (e.envTexture && (ee[(rd32(ee, e.envTexture) + 32u) & 0x1FFFFFFu] & 7u) == 5u)
                        e.envFlags |= kEnvRenderTarget;
                    std::memcpy(&e.envCoef, ee + ((fx + 8u) & 0x1FFFFFFu), 4);
                    const uint32_t fbAlpha = rd32(ee, fx + 12u);
                    e.envAlpha = 0x8000000048ull | (0x20u >> (fbAlpha & 31u));
                    const uint32_t envExt = (e.envTexture ? rd32(ee, e.envTexture) : 0u) + rd32(ee, kGp - 0x798Cu);
                    const uint64_t kl = ee[(envExt + 20u) & 0x1FFFFFFu] | (static_cast<uint64_t>(ee[(envExt + 21u) & 0x1FFFFFFu]) << 8);
                    e.envTex1 = (tex1 & 0xFFFFF000FFE7FDE3ull) | ((kl & 0xFFFu) << 32) | ee[(envExt + 22u) & 0x1FFFFFFu] |
                                (((kl >> 12) & 3u) << 19);
                }
            }
            std::memcpy(g_nativeVu[idx & (kNativeVuRing - 1u)], ex.vu, sizeof(ex.vu));
        }
        if (mode == 2)
        {
            nstat(kNsNative); // would have been drawn natively
            return false;
        }
        writeMarkers(ee, memory, slot, idxs, numMeshes);
        nstat(kNsNative);
        return true;
    }

    void marker(PS2Memory &memory, uint32_t index)
    {
        if (!g_gs)
            return;
        const Entry &e = g_table[index & (kTableSize - 1u)];
        {
            if (g_p3Trace > 0) // PS2X_P3_TRACE: native markers in the PATH3 mask / upload sequence
            {
                --g_p3Trace;
                std::printf("[p3] marker kind=%u native=%d regs=%d tex=%08x env=%08x masked=%d\n", e.kind, e.native ? 1 : 0,
                            e.gsFromRegs ? 1 : 0, e.texture, e.envTexture, memory.isPath3Masked() ? 1 : 0);
            }
        }
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
        if (e.native)
        {
            std::memcpy(job.vu, g_nativeVu[index & (kNativeVuRing - 1u)], sizeof(job.vu));
            job.gsTest = e.gsTest;
            job.gsTex1 = e.gsTex1;
            job.gsClamp = e.gsClamp;
            job.stateFlags = e.gsFromRegs ? 0u : kStateNative;
            job.reserved = e.geomGen;
            job.ringIndices = e.ringIndices;
            job.numIndices = e.numIndices;
            job.envTexture = e.envTexture;
            job.envFlags = e.native ? e.envFlags : 0u;
            if (e.envTexture)
            {
                job.envCoef = e.envCoef;
                for (int k = 0; k < 3; ++k)
                {
                    job.envR[k] = e.envR[k];
                    job.envU[k] = e.envU[k];
                }
                job.envAlpha = e.envAlpha;
                job.envTex1 = e.envTex1;
                job.envFlags = e.envFlags;
            }
            if (e.kind == kIm2DMesh && (e.code & kPGenEntry) && !pgenMarker(e, job))
                return; // no live particles
            if (e.kind == kIm2DMesh && (e.code & kCrowdEntry) && !crowdMarker(e, job))
                return; // nothing visible
        }
        else if (const uint8_t *vu = memory.getVU1Data())
            std::memcpy(job.vu, vu + kVuFirstQw * 16u, sizeof(job.vu));
        if ((objVerifyMode() || nodeMode() == 2) && !e.native)
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
        {
            static const bool s_log = std::getenv("PS2X_NRW_ENVLOG") != nullptr;
            static uint64_t s_m = 0, s_mNative = 0, s_mEnv = 0;
            if (s_log)
            {
                ++s_m;
                s_mNative += e.native ? 1u : 0u;
                s_mEnv += e.envTexture ? 1u : 0u;
                if ((s_m % 20000u) == 0u)
                {
                    std::printf("[nrw-markers] total=%llu native=%llu env=%llu nodeMarkersWritten=%llu ptr=%08x spr=%08x inMem=%08x\n",
                                (unsigned long long)s_m, (unsigned long long)s_mNative, (unsigned long long)s_mEnv,
                                (unsigned long long)g_nodeMarkersWritten, rd32(g_rdram, kDmaPktPtr), rd32(g_rdram, kDmaSprBase),
                                rd32(g_rdram, kDmaSprInMem));
                    std::fflush(stdout);
                }
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
