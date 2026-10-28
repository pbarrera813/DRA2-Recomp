"""Missed function entries that generated code reaches through no-op stubs.

recomp_stubs_unresolved.c lists every direct call/jmp target that is not a detected function. A
target is kept when its referencing branch is a clean instruction, the target cannot split an
instruction, and some reference is a call or a tail jmp from another function. A target reached
only by jumps from its own function is a label; seeding it splits that function.

Usage: stub_seeds.py <pipeline dir> <gen dir> <out.json>
"""
import json
from pathlib import Path
import re
import sys

pipe, gen, out = map(Path, sys.argv[1:4])
stubs = {int(a, 16) for a in re.findall(r'^void sub_([0-9A-F]{8})\(void\) \{ g_esp', (gen/'recomp_stubs_unresolved.c').read_text(), re.M)}
line = re.compile(r'^  0x([0-9A-F]{8})  ([0-9a-f]+)\s+(\S+)\s*(.*?)\s*$')
clean, junk_cover, refs, order = set(), [], {}, []
end = 0
for asm in sorted((pipe/'disasm/asm').glob('*.asm')):
    for text in asm.read_text(errors='replace').splitlines():
        if text.startswith(';') or text.endswith(':'):
            end = 0 if text.startswith('; Function:') else end
            continue
        m = line.match(text)
        if not m:
            continue
        addr, size, mnem, op = int(m[1], 16), len(m[2]) // 2, m[3], m[4]
        if addr < end:          # overlapping decode inside the previous instruction
            continue
        clean.add(addr); junk_cover.append((addr, addr + size)); end = addr + size
        order.append((addr, mnem))
        t = re.fullmatch(r'0x([0-9a-f]+)', op)
        if (mnem == 'call' or mnem.startswith('j')) and t and int(t[1], 16) in stubs:
            refs.setdefault(int(t[1], 16), []).append((addr, mnem))
junk_cover.sort()
def inside_instruction(a):
    import bisect
    i = bisect.bisect_right(junk_cover, (a, float('inf'))) - 1
    return i >= 0 and junk_cover[i][0] < a < junk_cover[i][1]
# A branch within the target's own function is intra-function however far it reaches. A jump-only
# target must also look like a function start (16-byte aligned, after ret/jmp): cold chunks jump
# back into their owner's labels, and jumps over padding land on loop heads.
spans = [(int(f['start'], 16), int(f['end'], 16)) for f in json.loads((pipe/'disasm/functions.json').read_text())]
def same_function(a, t):
    return any(s <= a < e and s <= t < e for s, e in spans)
order.sort()
prev = {}
last = None
for a, mn in order:
    prev[a] = last
    if mn not in ('nop', 'int3'):
        last = mn
def entry_shape(t):
    return t % 16 == 0 and prev.get(t) in ('ret', 'retn', 'jmp')
far = lambda t: any(mn == 'call' or (mn == 'jmp' and entry_shape(t) and not same_function(a, t)
                                     and not 0 < t - a <= 32) for a, mn in refs[t])
seeds = sorted(t for t in refs if (t in clean or not inside_instruction(t)) and far(t))
rejected = sorted(stubs - set(seeds))
json.dump([{'start': f'0x{s:08x}'} for s in seeds], out.open('x'), indent=1)
print(len(stubs), 'stubs;', len(seeds), 'seeded;', len(rejected), 'rejected:', ' '.join(f'{r:08X}' for r in rejected))
assert all(s in seeds for s in (0x1252B0, 0x124C80, 0x124EF0, 0x125050)), 'save-task handlers must qualify'
assert not {0x1863C0, 0xE7200, 0xEFE0C, 0x17DD3A} & set(seeds), 'intra-function labels must not qualify'
