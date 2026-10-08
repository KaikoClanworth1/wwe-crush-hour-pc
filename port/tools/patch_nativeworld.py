# Generated code hooks for native RenderWare world rendering (PS2Recomp runtime/native_rw.h).
#
# _rpSectorDefaultRenderCallBack (0x2f4160): record the sector being rendered, so the PS2All bridge
#   callback can tell world-sector meshes from atomics (their source object is that sector).
# RpMeshPS2AllBridgeCallBack (0x2fdf80): at 0x2fe6c4 the bridge stores the DMA CALL into the mesh's
#   instanced VIF data (unpack + VU1 start). nrw::bridge() may swap that qword for a marker; the native
#   renderer then draws the mesh when the VIF reaches it. Everything else the bridge emits (texture
#   upload, GS registers via DIRECT, VU1 constants) is unchanged. Inactive unless PS2X_NATIVE_WORLD=1.
out = r'D:\Xbox Games Ports\WWE Crush Hour\recomp\output'
INC = '#include "runtime/native_rw.h"\n'


def patch(fn, anchor, block, tag):
    p = out + '\\' + fn
    s = open(p, encoding='utf-8').read()
    if tag in s:
        print(fn, 'already patched')
        return
    assert s.count(anchor) == 1, (fn, s.count(anchor))
    s = s.replace(anchor, anchor + block if block.startswith('\n') is False and anchor.endswith('\n\n') else block + anchor, 1)
    if INC not in s:
        s = s.replace('#include "ps2_runtime.h"\n', '#include "ps2_runtime.h"\n' + INC, 1)
    open(p, 'w', encoding='utf-8', newline='').write(s)
    print(fn, 'patched')


# Sector render entry: after the prologue pc assignment.
patch('_rpSectorDefaultRenderCallBack_0x2f4160.cpp', '    ctx->pc = 0x2f4160u;\n\n',
      '    nrw::sectorBegin(GPR_U32(ctx, 4)); // patch_nativeworld\n', 'patch_nativeworld')

# Bridge: replace the geometry CALL qword just before it is stored.
patch('RpMeshPS2AllBridgeCallBack_0x2fdf80.cpp', '    // 0x2fe6c4: 0x7c650000  sq          $a1, 0x0($v1)\n',
      '''    { // patch_nativeworld: native mesh -> marker instead of the VU1 geometry CALL
        uint64_t mlo, mhi;
        const uint64_t clo = static_cast<uint64_t>(_mm_cvtsi128_si64(ctx->r[5]));
        const uint64_t chi = static_cast<uint64_t>(_mm_extract_epi64(ctx->r[5], 1));
        bool insertBefore = false;
        if (nrw::bridge(rdram, GPR_U32(ctx, 17), clo, chi, mlo, mhi, insertBefore))
        {
            const __m128i marker = _mm_set_epi64x(static_cast<int64_t>(mhi), static_cast<int64_t>(mlo));
            if (insertBefore)
            {
                // Verify mode: marker first, then the original CALL (v1 = DMA write pointer, gp-0x7A44 = next).
                WRITE128(ADD32(GPR_U32(ctx, 3), 0), marker);
                SET_GPR_U32(ctx, 3, ADD32(GPR_U32(ctx, 3), 16));
                WRITE32(ADD32(GPR_U32(ctx, 28), 0xFFFF85BCu), ADD32(GPR_U32(ctx, 3), 16));
            }
            else
                ctx->r[5] = marker;
        }
    }
''', 'patch_nativeworld')

# RenderWare PDS G3x Generic pipeline (most of the game's models): its bridge ends exactly like the
# generic one (s1 = pipe data, CALL stored at 0x2dc9b8, DMA pointer at gp-0x7A44).
patch('RpPDS_G3x_Generic_MeshPS2AllBridgeCallBack_0x2dc320.cpp', '    // 0x2dc9b8: 0x7c650000  sq          $a1, 0x0($v1)\n',
      '''    { // patch_nativeworld: native mesh -> marker instead of the VU1 geometry CALL
        uint64_t mlo, mhi;
        const uint64_t clo = static_cast<uint64_t>(_mm_cvtsi128_si64(ctx->r[5]));
        const uint64_t chi = static_cast<uint64_t>(_mm_extract_epi64(ctx->r[5], 1));
        bool insertBefore = false;
        if (nrw::bridge(rdram, GPR_U32(ctx, 17), clo, chi, mlo, mhi, insertBefore))
        {
            const __m128i marker = _mm_set_epi64x(static_cast<int64_t>(mhi), static_cast<int64_t>(mlo));
            if (insertBefore)
            {
                // Verify mode: marker first, then the original CALL (v1 = DMA write pointer, gp-0x7A44 = next).
                WRITE128(ADD32(GPR_U32(ctx, 3), 0), marker);
                SET_GPR_U32(ctx, 3, ADD32(GPR_U32(ctx, 3), 16));
                WRITE32(ADD32(GPR_U32(ctx, 28), 0xFFFF85BCu), ADD32(GPR_U32(ctx, 3), 16));
            }
            else
                ctx->r[5] = marker;
        }
    }
''', 'patch_nativeworld')

# The game's own bridge_callback (0x2b0e00, a recompiled copy of RenderWare's generic bridge used by the game's
# object_setup_callback objects): CALL tag in v1, DMA write pointer in v0, the pointer variable (0x48fe2c) is
# reloaded and advanced after the store.
patch('bridge_callback__FP16RxPS2AllPipeData_0x2b0e00.cpp', '    // 0x2b1720: 0x7c430000  sq          $v1, 0x0($v0)\n',
      '''    { // patch_nativeworld: native mesh -> marker instead of the VU1 geometry CALL
        uint64_t mlo, mhi;
        const uint64_t clo = static_cast<uint64_t>(_mm_cvtsi128_si64(ctx->r[3]));
        const uint64_t chi = static_cast<uint64_t>(_mm_extract_epi64(ctx->r[3], 1));
        bool insertBefore = false;
        if (nrw::bridge(rdram, GPR_U32(ctx, 17), clo, chi, mlo, mhi, insertBefore))
        {
            const __m128i marker = _mm_set_epi64x(static_cast<int64_t>(mhi), static_cast<int64_t>(mlo));
            if (insertBefore)
            {
                // Verify mode: marker first, then the original CALL; the code below reloads the pointer and adds 16.
                WRITE128(ADD32(GPR_U32(ctx, 2), 0), marker);
                SET_GPR_U32(ctx, 2, ADD32(GPR_U32(ctx, 2), 16));
                WRITE32(0x48FE2Cu, GPR_U32(ctx, 2));
            }
            else
                ctx->r[3] = marker;
        }
    }
''', 'patch_nativeworld')
