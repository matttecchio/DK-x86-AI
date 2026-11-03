import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rd import frames
A = list(frames(sys.argv[1])); B = list(frames(sys.argv[2]))
bad = 0
for (n, t, r, p, ram, vram), (n2, t2, r2, p2, ram2, vram2) in zip(A, B):
    d = []
    if r != r2: d.append("regs " + str({k: (hex(r[k]), hex(r2[k])) for k in r if r[k] != r2[k]}))
    if p != p2: d.append(f"ports {p} {p2}")
    if ram != ram2: d.append("ram@" + ",".join(hex(0x6000 + i) for i in range(len(ram)) if ram[i] != ram2[i])[:100])
    if vram != vram2: d.append("vram@" + ",".join(hex(0x7000 + i) for i in range(len(vram)) if vram[i] != vram2[i])[:100])
    if d:
        bad += 1
        if bad <= 3: print("frame", n, "; ".join(d))
print("frames differing:", bad, "of", min(len(A), len(B)))
