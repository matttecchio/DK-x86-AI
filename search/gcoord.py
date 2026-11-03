#!/usr/bin/env python3
"""Go-Explore coordinator: Pareto archive over (position, timer) -> best score."""
import os, sys, time, math, random, pickle, subprocess, signal

HERE = os.path.dirname(os.path.abspath(__file__))
MAMEBIN = os.environ.get("MAME", "/usr/games/mame" if os.path.exists("/usr/games/mame") else "mame")   # stock MAME 0.264
ROMPATH = os.environ.get("DK_ROMPATH", os.path.expanduser("~/dkong-ai/roms"))   # your own dkong ROM set (not included)
WORKDIR = os.environ.get("DK_WORKDIR", os.path.expanduser("~/dkong-ai") if os.path.isdir(os.path.expanduser("~/dkong-ai")) else HERE)
SHM = os.environ.get("SHM", "/dev/shm/dkge")
JOBDIR = os.path.join(SHM, "jobs")
STD = os.path.join(SHM, "dkong")
W = int(os.environ.get("W", 20))
HOURS = float(os.environ.get("HOURS", 1))
ROOT = os.environ.get("ROOT", "root")
BT = int(os.environ.get("BT", 1))
T0 = int(os.environ.get("T0", 80))
L0 = os.environ.get("L0", "3")
TARGET = int(os.environ.get("TARGET", 16000))
HMIN = float(os.environ.get("HMIN", 0.33))
HFAIL = float(os.environ.get("HFAIL", 2.0))
if os.environ.get("SACR") == "1": HFAIL = max(HFAIL, HOURS)   # sacrifice searches run their full HOURS
STALL = float(os.environ.get("STALL", 0))   # minutes: once 20 min in, stop when no new best for this long (0 = off)
X0 = int(os.environ.get("X0", 63)); Y0 = int(os.environ.get("Y0", 240))
YTOP = {1: 49, 2: 40, 3: 40, 4: 40}[BT]
NEEDY = float(os.environ.get("NEEDY", {1: 20, 2: 14, 3: 22, 4: 0}[BT]))
NEEDK = float(os.environ.get("NEEDK", 6))
NEEDW = float(os.environ.get("NEEDW", 1))  # value weight on remaining work (pruning still uses need())
S0 = int(os.environ.get("S0", 652200))
NODES_PER_JOB = int(os.environ.get("NJ", 6))
ROLLS = int(os.environ.get("NR", 4))
RTOL = float(os.environ.get("RTOL", 0.1))
LAMS = [0, 100, 300, 1000]
CAP = int(os.environ.get("CAP", 17000))
TOPFRAC = float(os.environ.get("TOPFRAC", 0.6))
FRONT = float(os.environ.get("FRONT", 0.3))
LOG = open(os.path.join(HERE, os.environ.get("GELOG", "ge.log")), "a")
random.seed(int(os.environ.get("SEED", 1)))

def log(*a):
    s = time.strftime("%H:%M:%S ") + " ".join(str(x) for x in a)
    print(s, file=LOG, flush=True)

RIVX = (75, 179); RIVY = (80, 120, 160, 200)   # rivet k at 0x6292+k: floor k//2 (top first), left/right k%2
PXU = float(os.environ.get("PXU", 70))          # pixels walked per timer unit (approx)
STARPEN = float(os.environ.get("STARPEN", 15))   # timer units (x DW) charged when top-right goes before the rest
DW = float(os.environ.get("DW", 3))              # extra weight on distance so walking toward the next rivet gains value
def riv(pg): return ((pg or 0) >> 2) & 15
def rivdist(x, y, pg):  # rough walking distance (timer units) to the nearest remaining rivet
    m = (pg or 0) >> 6
    if not m or x is None: return 0
    pen = 0
    if m != 2:                       # star pattern (user): the top-right rivet (bit 1) is normally the last one
        if m & 2: m &= ~2            # don't aim for it while others remain
        else: pen = STARPEN          # it was pulled early: penalise (soft, not enforced)
    d = []
    for k in range(8):
        if m >> k & 1:
            rx, ry = RIVX[k % 2], RIVY[k // 2]
            c = abs(x - rx) + 2 * abs(y - ry)
            if ry == 80 and (x < 128) != (rx < 128): c += 160   # Kong blocks the top floor: detour down and up
            d.append(c)
    return min(d) / PXU + pen
def need(y, pg=0, x=None):  # heuristic timer units needed to finish (x given: include distance to next rivet)
    if BT == 4: return NEEDK * riv(pg) + DW * rivdist(x, y, pg)
    return NEEDY * max(0, y - YTOP) / (240 - YTOP)

class Node:
    __slots__ = ("id", "file", "parent", "inp", "s", "t", "x", "y", "pos", "chosen", "alive", "kids", "risk", "pg")
    def __init__(self, **k):
        for a in Node.__slots__: setattr(self, a, k.get(a))

nodes = {}          # id -> Node (all accepted nodes, kept for path reconstruction)
cells = {}          # pos -> {t: node_id}
class Active:
    """Active node ids with O(1) add/discard/random pick, plus the timer buckets choose() exploits (kept up to date incrementally)."""
    def __init__(self): self.ids, self.at, self.byt, self.v0 = [], {}, {}, {}
    def add(self, i):
        if i in self.at: return
        self.at[i] = len(self.ids); self.ids.append(i)
        n = nodes[i]
        if n.t >= need(n.y, n.pg) - 2: self.byt.setdefault(n.t, set()).add(i)   # else cannot plausibly finish
        self.v0[i] = (n.s - S0) - 100 * NEEDW * need(n.y, n.pg, n.x)              # fixed part of the exploitation score
    def discard(self, i):
        k = self.at.pop(i, None)
        if k is None: return
        del self.v0[i]
        last = self.ids.pop()
        if last != i: self.ids[k] = last; self.at[last] = k
        g = self.byt.get(nodes[i].t)
        if g is not None:
            g.discard(i)
            if not g: del self.byt[nodes[i].t]
    def __len__(self): return len(self.ids)
    def __iter__(self): return iter(self.ids)
    def __contains__(self, i): return i in self.at
active = Active()
best = {"final": -1, "id": None, "inp": None, "s": 0, "t": 0}
clears = []
nid = 0

def posk(x, y, ham, hel, hc, jmp, lad, nb=0, nn=0, pg=0):
    return (x // 8, y // 8, ham, (hel // 64) if ham else 0, hc, 1 if jmp else 0, 1 if lad else 0, min(nb, 8) // 2, min(nn, 3), pg)

def value(n):
    return (n.s - S0) + 100 * n.t - 100 * NEEDW * need(n.y, n.pg, n.x)

def add_root():
    global nid
    n = Node(id="0", file=ROOT, parent=None, inp="", s=S0, t=T0, x=X0, y=Y0, pos=("root",), chosen=0, alive=True, kids=0, risk=0.0, pg=32 + (255 << 6) if BT == 4 else 0)
    nodes["0"] = n; cells[n.pos] = ["0"]; active.add("0"); nid = 1

TRASH = []
def add_expert():
    global nid
    total = 0
    for spec in os.environ.get("SEEDS", "").split(","):
        if spec.startswith("T:"):
            _, tf, K = spec.split(":")
            rows = [l.split() for l in open(os.path.join(HERE, tf))]
            rows.sort(key=lambda r: -int(r[0]))
            picked, seen = [], {}
            for r in rows:
                key = (int(r[8]) // 2, min(int(r[9]), 3))   # diversity by barrel layout
                if seen.get(key, 0) >= 3: continue
                seen[key] = seen.get(key, 0) + 1
                picked.append(r)
                if len(picked) >= int(K): break
            for r in picked:
                s, t, x, y, ham, hel, hc = map(int, r[1:8])
                i = str(nid); nid += 1
                pos = posk(x, y, ham, hel, hc, 0, 0, int(r[8]), int(r[9]))
                nodes[i] = Node(id=i, file=r[10], parent="0", inp=r[11], s=s, t=t, x=x, y=y, pos=pos, chosen=0, alive=True, kids=0, risk=0.0)
                cells.setdefault(pos, []).append(i); active.add(i); total += 1
            log(f"seeded {len(picked)} from {tf}; best {picked[0][0] if picked else None}")
            continue
        if not spec: continue
        sf, pf = spec.split(":")
        P = open(os.path.join(HERE, pf)).read().split("\n")[1]
        prev_id, prev_n = "0", 0
        for ln in open(os.path.join(HERE, sf)):
            p = ln.split()
            if p[0] != "S": continue
            n_, name = int(p[1]), p[2]
            if p[0] != "S": continue
            s, t, x, y, ham, hel, hc, jmp, lad, nb, nn, pg = map(int, p[3:15])
            pos = posk(x, y, ham, hel, hc, jmp, lad, nb, nn, pg)
            i = str(nid); nid += 1
            nodes[i] = Node(id=i, file=name, parent=prev_id, inp=P[prev_n:n_], s=s, t=t, x=x, y=y, pos=pos, chosen=0, alive=True, kids=0, risk=0.0, pg=pg)
            cells.setdefault(pos, []).append(i); active.add(i)
            prev_id, prev_n = i, n_
            total += 1
    log(f"added {total} seed nodes")

def rmfile(f):
    if f != ROOT and f.startswith("w"): TRASH.append((time.time(), f))

def empty_trash():
    now = time.time()
    while TRASH and now - TRASH[0][0] > 60:
        _, f = TRASH.pop(0)
        try: os.remove(os.path.join(STD, f + ".sta"))
        except FileNotFoundError: pass

STAGE = os.environ.get("STAGE", "D")
TBEST = [-10**9]
TF = None
def is_term(ham, hc, jmp, lad):
    if jmp or lad: return False
    if STAGE == "A": return hc == 1 and ham == 0
    if STAGE == "B": return hc == 1 and ham == 1
    if STAGE == "C": return hc == 0 and ham == 0
    return False

def consider(parent, name, s, t, x, y, ham, hel, hc, jmp, lad, nb, nn, pg, frag, inp):
    global nid
    if is_term(ham, hc, jmp, lad):
        v = s - S0 + 100 * t
        if v < TBEST[0] - 1000: rmfile(name); return True
        TBEST[0] = max(TBEST[0], v)
        TF.write(f"{s - S0 + 100 * t} {s} {t} {x} {y} {ham} {hel} {hc} {nb} {nn} {name} {path(parent, inp)}\n"); TF.flush()
        return True
    pos = posk(x, y, ham, hel, hc, jmp, lad, nb, nn, pg)
    risk = nodes[parent].risk + frag
    cell = cells.setdefault(pos, [])
    for oid in cell:
        o = nodes[oid]
        if o.t >= t and o.s >= s and o.risk <= risk + RTOL:
            rmfile(name); return False
    keep = []
    for oid in cell:
        o = nodes[oid]
        if o.t <= t and o.s <= s and o.risk >= risk - RTOL:
            o.alive = False; active.discard(o.id); rmfile(o.file)
        else: keep.append(oid)
    i = str(nid); nid += 1
    n = Node(id=i, file=name, parent=parent, inp=inp, s=s, t=t, x=x, y=y, pos=pos, chosen=0, alive=True, kids=0, risk=risk, pg=pg)
    nodes[i] = n; keep.append(i); cells[pos] = keep; active.add(i)
    return True

def path(i, tail=""):
    parts = [tail]
    while i is not None:
        n = nodes[i]; parts.append(n.inp); i = n.parent
    return "".join(reversed(parts))

FRONTC = [0.0, []]   # rivets: front nodes, rescanned at most every 2 s
def choose(k):
    if not len(active): return []
    out = []
    ntop = int(k * TOPFRAC)
    # time-stratified exploitation: pick a timer value, then the best-scoring node at that timer
    ts = list(active.byt)
    for _ in range(ntop):
        if not ts: break
        t = random.choice(ts)
        lam = random.choice(LAMS)
        v0 = active.v0
        bi = max(active.byt[t], key=lambda i: v0[i] - lam * nodes[i].risk - 150 * math.sqrt(nodes[i].chosen))
        best = nodes[bi]; out.append(best); best.chosen += 1
    if BT == 4:
        if time.time() - FRONTC[0] > 2:
            live = [nodes[i] for i in active if nodes[i].t > 0 and riv(nodes[i].pg) < 8]
            front = []
            if live:
                mr = min(riv(n.pg) for n in live)
                front = [(n, max(0.5, n.t + 1 - rivdist(n.x, n.y, n.pg)) * (3 if riv(n.pg) == mr else 1)) for n in live if riv(n.pg) <= mr + 1]
            FRONTC[:] = [time.time(), front, max((w for _, w in front), default=1)]
        front, wmax = FRONTC[1], FRONTC[2]
        if front:   # weight w/sqrt(1+chosen), drawn by rejection; nodes pruned since the rescan are skipped
            picks = []
            for _ in range(int(k * FRONT)):
                for _ in range(1000):
                    n, w = front[random.randrange(len(front))]
                    if n.alive and random.random() * wmax * math.sqrt(1 + n.chosen) < w: picks.append(n); break
            for n in picks: out.append(n); n.chosen += 1
    # the rest: all active nodes weighted 1/sqrt(1+chosen), drawn by rejection (weights are <= 1) instead of building a weight list per call
    ids, rest = active.ids, []
    for _ in range(max(0, k - len(out))):
        for _ in range(1000):
            n = nodes[ids[random.randrange(len(ids))]]
            if random.random() * math.sqrt(1 + n.chosen) < 1: break
        rest.append(n)
    for n in rest: n.chosen += 1
    return out + rest

DKX = os.environ.get("DKX")   # path to the dkx binary: run the MAME-exact native emulator instead of MAME workers
def launch(w):
    env = dict(os.environ, WID=str(w), JOBDIR=JOBDIR, STD=STD, RS=str(random.randrange(1 << 30)), L0=L0, T0=str(T0), BT=str(BT), NS="0")
    if DKX:
        return subprocess.Popen([DKX, "worker"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for v in ("DISPLAY", "WAYLAND_DISPLAY"): env.pop(v, None)
    env["SDL_VIDEODRIVER"] = "dummy"
    return subprocess.Popen([MAMEBIN, "dkong", "-rompath", ROMPATH, "-state_directory", SHM,
        "-skip_gameinfo", "-video", "none", "-sound", "none", "-nothrottle", "-numprocessors", "1", "-samplerate", os.environ.get("SR", "48000"),
        "-autoboot_script", os.path.join(HERE, os.environ.get("WORKER", "gworker.lua"))], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        cwd=WORKDIR)

def give(w):
    js = choose(NODES_PER_JOB)
    tmp = os.path.join(JOBDIR, f"w{w}.jtmp")
    with open(tmp, "w") as f:
        for n in js:
            pf, pi = "-", "-"
            if n.parent is not None and n.inp:
                pf, pi = nodes[n.parent].file, n.inp
            f.write(f"J {n.id} {n.file} {ROLLS} {pf} {pi}\n")
    os.rename(tmp, os.path.join(JOBDIR, f"w{w}.job"))
    return {n.id for n in js}

def save_ckpt():
    with open(os.path.join(HERE, "ckpt.pkl.tmp"), "wb") as f:
        pickle.dump({"nodes": {k: {a: getattr(v, a) for a in Node.__slots__} for k, v in nodes.items()}, "best": best, "clears": clears}, f)
    os.replace(os.path.join(HERE, "ckpt.pkl.tmp"), os.path.join(HERE, "ckpt.pkl"))

nfire = [0]
best_at = [0.0]
CF = open(os.path.join(HERE, os.environ.get("CLEARS", "clears.txt")), "a")

def main():
    global TF
    os.makedirs(JOBDIR, exist_ok=True)
    add_root()
    add_expert()
    procs = [launch(w) for w in range(W)]
    pending = {w: give(w) for w in range(W)}
    t0 = time.time(); lastlog = 0; lastck = time.time()
    nroll = ndead = ncand = nacc = 0
    while True:
        el_ = time.time() - t0
        if el_ >= HMIN * 3600 and best["final"] >= TARGET: break
        if el_ >= HOURS * 3600 and best["final"] >= 0: break
        if el_ >= HFAIL * 3600: break
        if STALL and el_ >= max(STALL, 20) * 60 and el_ - best_at[0] >= STALL * 60: log(f"stalled {STALL:.0f} min at best {best['final']}"); break
        got = False
        for w in range(W):
            rp = os.path.join(JOBDIR, f"w{w}.res")
            if not os.path.exists(rp): continue
            got = True
            with open(rp) as f: lines = f.read().split("\n")
            os.remove(rp)
            for ln in lines:
                if not ln or ln == "END": continue
                p = ln.split(" ")
                if p[0] == "M": log("MISSING state", p[1]); continue
                if p[0] == "F": nfire[0] += 1; nroll += 1; continue
                if p[0] == "D": ndead += 1; nroll += 1
                elif p[0] == "W":
                    nroll += 1
                    par, sc, t, frag, inp = p[1], int(p[2]), int(p[3]), float(p[4]), p[5]
                    if par not in nodes: continue
                    risk = nodes[par].risk + frag
                    final = sc - S0 + 100 * t
                    clears.append((final, par, sc, t))
                    if final >= best["final"] - 300:
                        CF.write(f"{final} {sc - S0} {t} {risk:.3f} {path(par, inp)}\n"); CF.flush()
                    if final > best["final"] and risk < 99:
                        best.update(final=final, id=par, inp=inp, s=sc - S0, t=t)
                        best_at[0] = time.time() - t0
                        log(f"NEW BEST CLEAR {final} (play {sc - S0} + bonus {100 * t}) parent={par}")
                        with open(os.path.join(HERE, os.environ.get("BESTF", "best_path.txt")), "w") as f:
                            f.write(f"{final} {sc - S0} {t}\n{path(par, inp)}\n")
                elif p[0] == "C":
                    ncand += 1
                    par = p[1]
                    if par not in nodes: rmfile(p[2]); continue
                    if consider(par, p[2], *map(int, p[3:15]), float(p[15]), p[16]): nacc += 1
            pending[w] = give(w)
        for w, pr in enumerate(procs):
            if pr.poll() is not None:
                log(f"worker {w} died rc={pr.returncode}; relaunching")
                for ext in (".job", ".res", ".tmp"):
                    try: os.remove(os.path.join(JOBDIR, f"w{w}{ext}"))
                    except FileNotFoundError: pass
                procs[w] = launch(w); pending[w] = give(w)
        empty_trash()
        if not got: time.sleep(0.005)
        el = time.time() - t0
        if el - lastlog > 60:
            lastlog = el
            act = [nodes[i] for i in active]
            top = max(act, key=value) if act else None
            if not act: continue
            hi = min(act, key=lambda n: n.y) if act else None
            maxs = max(act, key=lambda n: n.s) if act else None
            fr = min(act, key=lambda n: riv(n.pg)) if BT == 4 else None
            frs = f" | fewest riv={riv(fr.pg)} s={fr.s-S0} t={fr.t} items={fr.pg % 4}" if fr else ""
            if fr:
                fl = [n for n in act if riv(n.pg) == riv(fr.pg)]
                ft = max(fl, key=lambda n: n.t)
                frs += f" x={ft.x} y={ft.y} tmax={ft.t} n={len(fl)}"
                with open(os.path.join(HERE, os.environ.get("GELOG", "ge.log") + ".fewest"), "w") as f:
                    f.write(f"{riv(ft.pg)} {ft.s - S0} {ft.t} {ft.x} {ft.y}\n{path(ft.id)}\n")
            log(f"t={el/60:.1f}m rolls={nroll} ({nroll/el:.0f}/s) dead={ndead} cand={ncand} acc={nacc} active={len(active)} cells={len(cells)} "
                f"fire={nfire[0]} clears={len(clears)} best={best['final']} | topV s={top.s-S0} t={top.t} y={top.y} | highest y={hi.y} s={hi.s-S0} t={hi.t} | maxs={maxs.s-S0} t={maxs.t} y={maxs.y}" + frs)
    for w in range(W):
        with open(os.path.join(JOBDIR, f"w{w}.job"), "w") as f: f.write("Q\n")
    time.sleep(3)
    for pr in procs:
        if pr.poll() is None: pr.kill()
    log("DONE best", best["final"])

if __name__ == "__main__":
    main()
