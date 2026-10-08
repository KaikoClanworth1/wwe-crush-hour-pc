# VFTOIn saturates on the VU (>= 2^31 -> 0x7FFFFFFF); _mm_cvttps_epi32 returns 0x80000000 for any
# out-of-range value. Used by Havok's MOPP query machines (ray/OBB/sphere -> integer MOPP space): a huge
# positive bound became the most negative int and the query rejected the arena floor (wheel raycasts
# missed on HARDCORE). Recompiler fixed too (vu_translation_helpers.cpp translateVU_VFTOI).
import glob
old = '__m128i res_i = _mm_cvttps_epi32(src); '
new = ('__m128i res_i = _mm_cvttps_epi32(src); '
       'res_i = _mm_xor_si128(res_i, _mm_castps_si128(_mm_cmpge_ps(src, _mm_set1_ps(2147483648.0f)))); ')
n = 0
for f in glob.glob(r'D:\Xbox Games Ports\WWE Crush Hour\recomp\output\*.cpp'):
    s = open(f, encoding='utf-8', errors='surrogateescape').read()
    if old not in s or new in s:
        continue
    s = s.replace(old, new)
    open(f, 'w', encoding='utf-8', errors='surrogateescape', newline='').write(s)
    n += 1
print('patched files', n)
