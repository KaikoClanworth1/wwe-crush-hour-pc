#pragma once

// Native RenderWare rendering (PS2X_NATIVE_WORLD=1).
//
// RenderWare's PS2All pipeline normally hands every mesh to VU1 as a DMA CALL into the mesh's instanced
// VIF data (unpack + MSCAL). For meshes rendered natively the bridge callback emits a marker instead
// (VIF MARK 0xC0DE, then a NOP whose immediate indexes the EE-side side table). When the VIF reaches the
// marker, the job below is built and passed down the GS pipeline in stream order, so it sees the same
// GS state (texture, blending, frame/Z buffers) and the same VU1 constants (camera/object matrix,
// screen scale, lights) that the replaced VU1 program would have seen. The backend draws it from
// vertex/index buffers built once from the RenderWare geometry arrays in EE memory.

#include <cstdint>

class GS;
class PS2Memory;

namespace nrw
{
    constexpr uint32_t kMarkerMark = 0xC0DEu;         // VIF MARK immediate
    constexpr uint64_t kJobMagic = 0x4E52574A0000C0DEull; // first qword of a job packet ("JWRN")

    enum JobKind : uint32_t
    {
        kWorldSectorMesh = 1,
        kAtomicMesh = 2, // rigid RpAtomic (one morph target) on RenderWare's generic PS2All VU1 program
        kIm3DMesh = 3,   // RwIm3D primitive: vertices/indices snapshotted into the job ring (ring()), GS state from registers
    };

    // VU1 data memory snapshot range: lights (976..) through per-object constants (..1023).
    constexpr uint32_t kVuFirstQw = 976u;
    constexpr uint32_t kVuQwCount = 48u;

    struct MeshJob
    {
        uint64_t magic = kJobMagic;
        uint64_t reserved = 0;        // native node path: geometry edit generation (vertex buffer cache key)
        uint32_t kind = kWorldSectorMesh;
        uint32_t object = 0;          // RpWorldSector (EE address)
        uint32_t mesh = 0;            // RpMesh (EE address)
        uint32_t meshHeaderFlags = 0; // rpMESHHEADERTRISTRIP = 1
        uint32_t primType = 0;
        uint32_t transType = 0;
        uint32_t matCol = 0;          // RGBA
        uint32_t texture = 0;
        uint32_t vu1CodeIndex = 0;
        uint32_t surfProps = 0;       // RwSurfaceProperties (EE address)
        uint32_t pad[2]{};
        // Vertex source (EE addresses; 0 = absent), taken from the sector or the atomic's geometry.
        uint32_t verts = 0, normals = 0, uvs = 0, prelit = 0, numVerts = 0, geomFlags = 0;
        uint32_t ringIndices = 0, numIndices = 0; // kIm3DMesh: index offset / count in ring() (verts = vertex offset)
        float vu[kVuQwCount][4]{};    // VU1 data memory kVuFirstQw..
        // Native node path (RenderWare's PS2All pipeline skipped): the GS state its mesh bridge would have
        // set, applied by the backend to this draw only (the emulated GS registers stay untouched).
        uint64_t gsTest = 0, gsTex1 = 0, gsClamp = 0;
        uint32_t stateFlags = 0; // kStateNative
        uint32_t pad3 = 0;
        // PDS G3x Gem (RpMatFX environment map, native node path): a second, additive pass with the env raster.
        // UVs from the vertex normals, 0.5 * (N.envR, N.envU) - 0.5 with envR/envU the env frame's right/up in
        // object space; colour = coefficient * the lit colour. GS state of that pass: ALPHA_2 / TEX1_2 below,
        // TEST 0x50002, CLAMP 0 (repeat), FOGCOL 0, PRIM ABE|TME|IIP (+FGE).
        uint32_t envTexture = 0; // RwTexture, 0 = no env pass
        float envCoef = 0.0f;
        uint32_t envFlags = 0;   // kEnvRenderTarget: the env raster is a camera texture (sampled from its GS render target)
        uint32_t pad4 = 0;
        float envR[4]{}, envU[4]{};
        uint64_t envAlpha = 0, envTex1 = 0;
    };
    static_assert(sizeof(MeshJob) % 16u == 0u, "job must be qword sized");
    constexpr uint32_t kStateNative = 1u;
    constexpr uint32_t kEnvRenderTarget = 1u;

    // Vertex normal i as the packed form the VU1 program gets (x, y, z * 128 as signed bytes, w 0).
    // Sectors store that already; atomic morph targets store RwV3d floats.
    inline void readNormal(const uint8_t *ee, uint32_t normals, uint32_t i, bool floats, int8_t out[4])
    {
        if (!floats)
        {
            const uint8_t *p = ee + ((normals + i * 4u) & 0x1FFFFFFu);
            for (int k = 0; k < 4; ++k)
                out[k] = static_cast<int8_t>(p[k]);
            return;
        }
        for (int k = 0; k < 3; ++k)
        {
            float f;
            const uint8_t *p = ee + ((normals + i * 12u + k * 4u) & 0x1FFFFFFu);
            for (int b = 0; b < 4; ++b)
                reinterpret_cast<uint8_t *>(&f)[b] = p[b];
            const float v = f * 128.0f;
            out[k] = static_cast<int8_t>(v >= 127.0f ? 127 : (v <= -128.0f ? -128 : static_cast<int>(v)));
        }
        out[3] = 0;
    }

    // Which quadword of MeshJob::vu holds VU1 address a.
    constexpr uint32_t vuIndex(uint32_t a) { return a - kVuFirstQw; }

    bool enabled();
    // EE side (called from patched generated code).
    // RenderWare's PS2All node body (RxPipelineNodeInstance self, RxPipelineNodeParam params): draws the object
    // natively -- markers appended to RW's open VIF packet, constants computed from the RW structures -- and
    // returns true, or false to let RenderWare run its own PS2 pipeline (unsupported object / no packet room).
    bool nodeBody(uint8_t *rdram, PS2Memory &memory, uint32_t self, uint32_t params);
    // RpPDS_G3x_ADL_LightingUpload(ambient, directional): the lights PDS G3x objects are lit with from now on.
    void g3xLights(uint32_t ambient, uint32_t directional);
    void sectorBegin(uint32_t sector);
    void sectorEnd();
    // Bridge callback, about to store the geometry CALL tag (callLo/callHi): returns true and the marker
    // qword to store instead when this mesh is rendered natively.
    // insertBefore: keep the CALL and store the marker in front of it (PS2X_NATIVE_WORLD=verify).
    bool bridge(uint8_t *rdram, uint32_t pipeData, uint64_t callLo, uint64_t callHi, uint64_t &lo, uint64_t &hi,
                bool &insertBefore);
    // Verify mode: GIF packet kicked by VU1 (compared with the native result for the last marker).
    void observeKick(const uint8_t *data, uint32_t size);
    // VIF side: marker reached.
    void marker(PS2Memory &memory, uint32_t index);
    // GS side: a packet from the GS queue -- a native job or an ordinary GIF packet.
    void dispatchPacket(GS &gs, const uint8_t *data, uint32_t size);
    // VU1 light list (980..) -> per light colour*coefficient (w: 1 ambient, 2 directional, 3 point) and object-space
    // direction or position (w: 1/radius); returns the count. Stops at the terminator or a spot light.
    int parseLights(const MeshJob &job, float *colOut, float *dirOut, int maxLights);
    // Snapshot ring of transient vertex data (kIm3DMesh jobs; written on the EE side, read in stream order).
    const uint8_t *ring();
    constexpr uint32_t kIm3DVertexStride = 36u; // RxObjSpace3DVertex: pos, RGBA, normal, u, v
    // Runtime wiring.
    void setGs(GS *gs);
    void setRdram(const uint8_t *rdram);
    const uint8_t *rdram();
}
