import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
import re
root = runtime('src', 'lib')

# 1) VIF const trace: also print scratchpad offset of the unpack source.
p = os.path.join(root, 'ps2_vif1_interpreter.cpp')
s = open(p, encoding='utf-8').read()
s = s.replace('''                            const long long ee = (srcVec >= m_rdram && srcVec < m_rdram + PS2_RAM_SIZE) ? (long long)(srcVec - m_rdram) : -1;''',
'''                            long long ee = (srcVec >= m_rdram && srcVec < m_rdram + PS2_RAM_SIZE) ? (long long)(srcVec - m_rdram) : -1;
                            if (ee < 0 && srcVec >= m_scratchpad && srcVec < m_scratchpad + PS2_SCRATCHPAD_SIZE)
                                ee = 0x70000000ll + (long long)(srcVec - m_scratchpad);''')
open(p, 'w', encoding='utf-8').write(s)

# 2) EE store watchpoint in PS2Runtime::StoreN (special addresses incl. scratchpad).
p = os.path.join(root, 'ps2_runtime.cpp')
s = open(p, encoding='utf-8').read()
helper = '''
// Debug aid: PS2X_EE_WATCH=<hex vaddr> logs guest stores (special-address path) that cover it.
static void eeWatchStore(const R5900Context *ctx, uint32_t vaddr, uint32_t size, uint64_t lo, uint64_t hi)
{
    static const long long s_watch = std::getenv("PS2X_EE_WATCH") ? std::strtoll(std::getenv("PS2X_EE_WATCH"), nullptr, 16) : -1;
    static int s_count = 0;
    if (s_watch < 0 || s_count >= 60)
        return;
    const uint32_t w = static_cast<uint32_t>(s_watch);
    if (w < vaddr || w >= vaddr + size)
        return;
    ++s_count;
#if defined(PS2X_NATIVE_HOST)
    const char *fn = guestSymbolName(ctx ? ctx->pc : 0u);
#else
    const char *fn = "?";
#endif
    std::printf("[ee-watch] pc=0x%x (%s) addr=0x%x size=%u lo=%016llx hi=%016llx\\n", ctx ? ctx->pc : 0u, fn, vaddr, size,
                static_cast<unsigned long long>(lo), static_cast<unsigned long long>(hi));
}
'''
i = s.index('void PS2Runtime::Store8(')
s = s[:i] + helper + '\n' + s[i:]
s = s.replace('''void PS2Runtime::Store32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint32_t value)
{''', '''void PS2Runtime::Store32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint32_t value)
{
    eeWatchStore(ctx, vaddr, 4u, value, 0u);''')
s = s.replace('''void PS2Runtime::Store64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint64_t value)
{''', '''void PS2Runtime::Store64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint64_t value)
{
    eeWatchStore(ctx, vaddr, 8u, value, 0u);''')
s = s.replace('''    alignas(16) uint64_t _parts[2];
    _mm_storeu_si128(reinterpret_cast<__m128i *>(_parts), value);
    ps2TraceGuestWrite(rdram, vaddr, 16u, _parts[0], _parts[1], "WRITE128", ctx);''', '''    alignas(16) uint64_t _parts[2];
    _mm_storeu_si128(reinterpret_cast<__m128i *>(_parts), value);
    eeWatchStore(ctx, vaddr, 16u, _parts[0], _parts[1]);
    ps2TraceGuestWrite(rdram, vaddr, 16u, _parts[0], _parts[1], "WRITE128", ctx);''')
open(p, 'w', encoding='utf-8').write(s)
print('ok')
