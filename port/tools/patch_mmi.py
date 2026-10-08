p = r'D:\Xbox Games Ports\WWE Crush Hour\PS2Recomp\ps2xRuntime\include\ps2_runtime_macros.h'
s = open(p, encoding='utf-8').read()
reps = [
    # PINTH rd = {rt.h0, rs.h4, rt.h1, rs.h5, rt.h2, rs.h6, rt.h3, rs.h7}
    ('#define PS2_PINTH(a, b) _mm_unpacklo_epi16(_mm_shuffle_epi32((__m128i)(b), _MM_SHUFFLE(3, 2, 1, 0)), _mm_shuffle_epi32((__m128i)(a), _MM_SHUFFLE(3, 2, 1, 0)))',
     '#define PS2_PINTH(a, b) _mm_unpacklo_epi16((__m128i)(b), _mm_srli_si128((__m128i)(a), 8))'),
    # PINTEH rd = {rt.h0, rs.h0, rt.h2, rs.h2, rt.h4, rs.h4, rt.h6, rs.h6}
    ('#define PS2_PINTEH(a, b) _mm_unpackhi_epi16(_mm_shuffle_epi32((__m128i)(b), _MM_SHUFFLE(3, 2, 1, 0)), _mm_shuffle_epi32((__m128i)(a), _MM_SHUFFLE(3, 2, 1, 0)))',
     '#define PS2_PINTEH(a, b) _mm_or_si128(_mm_and_si128((__m128i)(b), _mm_set1_epi32(0x0000FFFF)), _mm_slli_epi32((__m128i)(a), 16))'),
    # PEXEH: per doubleword swap halfwords 0 and 2 -> {h2, h1, h0, h3}
    ('#define PS2_PEXEH(rs) _mm_shufflelo_epi16(_mm_shufflehi_epi16(rs, _MM_SHUFFLE(2, 3, 0, 1)), _MM_SHUFFLE(2, 3, 0, 1))',
     '#define PS2_PEXEH(rs) _mm_shufflelo_epi16(_mm_shufflehi_epi16(rs, _MM_SHUFFLE(3, 0, 1, 2)), _MM_SHUFFLE(3, 0, 1, 2))'),
    # PEXEW: swap words 0 and 2 -> {w2, w1, w0, w3}
    ('#define PS2_PEXEW(rs) _mm_shuffle_epi32(rs, _MM_SHUFFLE(2, 3, 0, 1))',
     '#define PS2_PEXEW(rs) _mm_shuffle_epi32(rs, _MM_SHUFFLE(3, 0, 1, 2))'),
    # PROT3W: rotate low three words -> {w1, w2, w0, w3}
    ('#define PS2_PROT3W(rs) _mm_shuffle_epi32(rs, _MM_SHUFFLE(0, 3, 2, 1))',
     '#define PS2_PROT3W(rs) _mm_shuffle_epi32(rs, _MM_SHUFFLE(3, 0, 2, 1))'),
]
for a, b in reps:
    assert a in s, a
    s = s.replace(a, b)
open(p, 'w', encoding='utf-8').write(s)
print('ok')
