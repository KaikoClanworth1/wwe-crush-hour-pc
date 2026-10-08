import re
p = r'D:\Xbox Games Ports\WWE Crush Hour\PS2Recomp\ps2xRuntime\src\lib\vu\ps2_vu1_core.cpp'
s = open(p, encoding='utf-8').read()
s = s.replace('namespace { uint64_t g_vuExecCount = 0; }',
 'namespace { uint64_t g_vuExecCount = 0;\n'
 '// Earliest readyCycle of any queued pipeline entry, per unit (kept out of the class header so the\n'
 '// PCH is untouched). commitReadyPipelines() is a no-op until the cycle counter reaches it.\n'
 'uint64_t g_vuNextCommit[2] = {0, 0}; }', 1)
s = s.replace('''void VU1Interpreter::resetScheduler()
{
    m_flagPipeline = {};''', '''void VU1Interpreter::resetScheduler()
{
    g_vuNextCommit[m_unit == Unit::VU1] = 0;
    m_flagPipeline = {};''', 1)
# Every insertion lowers the hint.
n = 0
def hint(m):
    global n; n += 1
    return m.group(0) + '\n' + m.group(1) + 'g_vuNextCommit[m_unit == Unit::VU1] = std::min(g_vuNextCommit[m_unit == Unit::VU1], ' + m.group(2) + ');'
s = re.sub(r'([ \t]*)(?:entry->|entry\.|store\.|write\.|m_fdiv\.)readyCycle = ([^;]+);', lambda m: hint(re.match(r'([ \t]*)\S+ = (.*)', m.group(0)) and m) , s)
print('hints', n)
old = '''void VU1Interpreter::commitReadyPipelines()
{
'''
assert old in s
s = s.replace(old, old + '''    uint64_t &nextCommit = g_vuNextCommit[m_unit == Unit::VU1];
    if (m_cycle < nextCommit)
        return;
    uint64_t next = ~0ull;
''', 1)
# recompute: in each loop, entries that are valid but not ready contribute to next.
body_start = s.index('void VU1Interpreter::commitReadyPipelines()')
body_end = s.index('void VU1Interpreter::progressXgkick()')
body = s[body_start:body_end]
body = body.replace('''        if (!entry.valid || entry.readyCycle > m_cycle)
            continue;''', '''        if (!entry.valid)
            continue;
        if (entry.readyCycle > m_cycle)
        {
            next = std::min(next, entry.readyCycle);
            continue;
        }''')
body = body.replace('''        if (!store.valid || store.readyCycle > m_cycle)
            continue;''', '''        if (!store.valid)
            continue;
        if (store.readyCycle > m_cycle)
        {
            next = std::min(next, store.readyCycle);
            continue;
        }''')
body = body.replace('''        if (!write.valid || write.readyCycle > m_cycle)
            continue;''', '''        if (!write.valid)
            continue;
        if (write.readyCycle > m_cycle)
        {
            next = std::min(next, write.readyCycle);
            continue;
        }''')
body = body.replace('''    if (m_fdiv.valid && m_fdiv.readyCycle <= m_cycle)
    {''', '''    if (m_fdiv.valid && m_fdiv.readyCycle > m_cycle)
        next = std::min(next, m_fdiv.readyCycle);
    if (m_fdiv.valid && m_fdiv.readyCycle <= m_cycle)
    {''')
body = body.replace('''        if (entry.valid && entry.readyCycle <= m_cycle)
        {
            m_state.p = entry.value;''', '''        if (entry.valid && entry.readyCycle > m_cycle)
            next = std::min(next, entry.readyCycle);
        if (entry.valid && entry.readyCycle <= m_cycle)
        {
            m_state.p = entry.value;''')
# closing brace of function
idx = body.rstrip().rfind('}')
body = body[:idx] + '    nextCommit = next;\n' + body[idx:]
s = s[:body_start] + body + s[body_end:]
open(p, 'w', encoding='utf-8').write(s)
print(body.count('next = std::min'))
