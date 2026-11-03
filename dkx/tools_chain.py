#!/usr/bin/env python3
"""Rebuild every board-start state in dkx (mkfresh + advance on each final route) and check each step against MAME's meta files.
RUNDIR: the search run directory holding MAME's meta_NNN.txt and seg/bNNN_best.txt (default ~/dkong-ai/fullgame).
ROOTS_OUT: where to write the dkx board states (default roots/ here)."""
import os, subprocess, sys
FG = os.environ.get("RUNDIR", os.path.expanduser("~/dkong-ai/fullgame")); HERE = os.path.dirname(os.path.abspath(__file__))
DKX = os.path.join(HERE, "build", "dkx"); ROOTS = os.environ.get("ROOTS_OUT", os.path.join(HERE, "roots"))
def run(mode, **e): subprocess.run([DKX, mode], env=dict(os.environ, DKXST=ROOTS, **e), check=True)
run("mkfresh", META=os.path.join(ROOTS, "meta_000.txt"))
ok = open(os.path.join(ROOTS, "meta_000.txt")).read().strip() == open(os.path.join(FG, "meta_000.txt")).read().strip()
print("b000", "OK" if ok else "MISMATCH"); bad = 0 if ok else 1
i = 0
while os.path.exists(os.path.join(FG, f"meta_{i+1:03d}.txt")) or os.path.exists(os.path.join(FG, "final.txt")):
    mf = os.path.join(FG, f"meta_{i+1:03d}.txt")
    want = open(mf).read().strip() if os.path.exists(mf) else open(os.path.join(FG, "final.txt")).read().strip()
    s0 = open(os.path.join(FG, f"meta_{i:03d}.txt")).read().split()[1]
    out = os.path.join(ROOTS, f"meta_{i+1:03d}.txt")
    run("advance", S0=s0, START=f"b{i:03d}", NEXT=f"b{i+1:03d}", PATHF=os.path.join(FG, "seg", f"b{i:03d}_best.txt"), META=out,
        **({"SACR": "1"} if "sac=1" in want or want.startswith("GAMEOVER") else {}))
    got = open(out).read().strip()
    if got != want: bad += 1; print(f"b{i:03d} MISMATCH\n  dkx : {got}\n  MAME: {want}")
    if not os.path.exists(mf): break
    i += 1
print(f"boards checked: {i + 1}, mismatches: {bad}")
