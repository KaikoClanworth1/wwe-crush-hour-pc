p='ps2_vu1_upper.cpp'
s=open(p,encoding='utf-8').read()
helper='''namespace
{
    // VU MAX/MINI compare raw sign-magnitude bit patterns (no denormal flush): microcode
    // routinely clamps FTOI'd integers with them.
    int32_t vuOrderKey(float v)
    {
        uint32_t b;
        std::memcpy(&b, &v, 4);
        return (b & 0x80000000u) ? -static_cast<int32_t>(b & 0x7FFFFFFFu) : static_cast<int32_t>(b);
    }
    float vuMaxRaw(float a, float b) { return vuOrderKey(a) >= vuOrderKey(b) ? a : b; }
    float vuMinRaw(float a, float b) { return vuOrderKey(a) <= vuOrderKey(b) ? a : b; }
'''
s=s.replace('namespace\n{\n',helper,1)
rawVs='m_state.vf[fs]'; rawVt='m_state.vf[ft]'
reps=[
('''        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] > bc) ? vs[c] : bc;''','''        const float bc = m_state.vf[ft][op & 3];
        for (int c = 0; c < 4; c++)
            result[c] = vuMaxRaw(m_state.vf[fs][c], bc);'''),
('''        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] < bc) ? vs[c] : bc;''','''        const float bc = m_state.vf[ft][op & 3];
        for (int c = 0; c < 4; c++)
            result[c] = vuMinRaw(m_state.vf[fs][c], bc);'''),
('''            result[c] = (vs[c] > i) ? vs[c] : i;''','''            result[c] = vuMaxRaw(m_state.vf[fs][c], m_state.i);'''),
('''            result[c] = (vs[c] < i) ? vs[c] : i;''','''            result[c] = vuMinRaw(m_state.vf[fs][c], m_state.i);'''),
('''            result[c] = (vs[c] > vt[c]) ? vs[c] : vt[c];''','''            result[c] = vuMaxRaw(m_state.vf[fs][c], m_state.vf[ft][c]);'''),
('''            result[c] = (vs[c] < vt[c]) ? vs[c] : vt[c];''','''            result[c] = vuMinRaw(m_state.vf[fs][c], m_state.vf[ft][c]);'''),
]
for a,b in reps:
    assert a in s, a
    s=s.replace(a,b)
open(p,'w',encoding='utf-8').write(s)
print("ok")
