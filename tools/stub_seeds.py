"""Undetected entries that generated code reaches through no-op stubs, filtered to real entries.

recomp_stubs_unresolved.c lists every direct call/jmp target that is not a detected function; at
runtime each one silently returns. A target is kept only when (a) a referencing direct branch is a
clean instruction in the disassembly listing, not an overlapping decode inside a previous
instruction, (b) the target is itself a clean instruction start or lies in undecoded bytes, so a
seed can never split an instruction, and (c) some reference is a call, or an unconditional jmp from
outside every detected function that contains the target. A target reached only by short jumps from just before it is a label inside a function
whose detected end stopped early (iteration 8: seeding 0x1863C0 split a hash-table resize and its
epilogue became a stub); those need boundary coalescing, not a seed. Usage: stub_seeds.py <pipeline-N> <gen dir> <out.json>
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
def inside_instruction(a):  # strictly inside some clean instruction's bytes
    import bisect
    i = bisect.bisect_right(junk_cover, (a, float('inf'))) - 1
    return i >= 0 and junk_cover[i][0] < a < junk_cover[i][1]
# A branch from inside the function that also contains the target is intra-function, however far:
# distance rules let through 0xE7200 (a loop back-edge `jne` 0x243 bytes away; its split froze
# the battle) and 0xEFE0C (a long `jmp` inside a 7.5 KB function; its split broke a switch).
# Real cross-function branches are calls and tail `jmp`s from another function.
# A jump-only target must also look like a function start (16-byte aligned, after ret/jmp): a cold
# chunk detected as its own function jumps back into its owner's labels (0x17DD3A, an epilogue,
# from sub_0017DD52), and a jump over alignment padding lands on a loop head (0x1863C0).
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
