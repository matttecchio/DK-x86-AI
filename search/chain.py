#!/usr/bin/env python3
"""Chain per-board Go-Explore searches from a fresh game to the L22 kill screen."""
import os, sys, time, shutil, subprocess, glob, json
HERE = os.path.dirname(os.path.abspath(__file__))
MAMEBIN = os.environ.get("MAME", "/usr/games/mame" if os.path.exists("/usr/games/mame") else "mame")   # stock MAME 0.264
ROMPATH = os.environ.get("DK_ROMPATH", os.path.expanduser("~/dkong-ai/roms"))   # your own dkong ROM set (not included)
WORKDIR = os.environ.get("DK_WORKDIR", os.path.expanduser("~/dkong-ai") if os.path.isdir(os.path.expanduser("~/dkong-ai")) else HERE)
ST = os.path.join(HERE, "st", "dkong")
SHM = "/dev/shm/gfull"
TARGET = {1: 16000, 2: 11000, 3: 8600, 4: 10000}
AIM = {1: 22000, 2: 20000, 4: 13000}   # extra-pass aims: barrels L4+ (kill screen is worth little, so push for 2M here), pies/rivets L7+
def cfg():   # live-tunable extra-pass rules (re-read every board, no restart needed)
    try: return json.load(open(os.path.join(HERE, "aims.json")))
    except Exception: return {}
NAME = {1: "barrels", 2: "pies", 3: "springs", 4: "rivets"}
W = int(os.environ.get("W", 22 if os.environ.get("DKX") else 20))   # dkx workers: all 22 threads (+4% over 20)
LOG = open(os.path.join(HERE, "chain.log"), "a")
def log(s):
    print(time.strftime("%m-%d %H:%M:%S ") + s, file=LOG, flush=True)

def meta(i):
    p = open(os.path.join(HERE, f"meta_{i:03d}.txt")).read().split()
    return dict(score=int(p[1]), lives=int(p[2]), lvl=int(p[3]), bt=int(p[4]), t0=int(p[5]), restart="sac=1" in p)

# DKX=<dkx binary>: search on the MAME-exact native emulator (~/dkong-ai/native/dkx). Its board states live in DKXROOTS;
# MAME stays the official verifier, and every board's dkx advance must reproduce MAME's meta line exactly.
DKX = os.environ.get("DKX")
DKXROOTS = os.environ.get("DKXROOTS", os.path.expanduser("~/dkong-ai/native/dkx/roots"))
def twin(i, bf, extra):   # rebuild the next board's low-sample-rate state and check it matches the official 48 kHz result
    if DKX:
        mf = os.path.join(HERE, f"meta_{i+1:03d}.txt"); tf = os.path.join(HERE, "seg", f"b{i:03d}_dkx.txt")
        subprocess.run([DKX, "advance"], env=dict(os.environ, DKXST=DKXROOTS, S0=str(meta(i)["score"]), START=f"b{i:03d}", NEXT=f"b{i+1:03d}", PATHF=bf, META=tf, **extra), timeout=900)
        a, b = open(mf).read().strip(), (open(tf).read().strip() if os.path.exists(tf) else "missing")
        if a != b: log(f"board {i:03d} DKX TWIN MISMATCH: {b} vs {a}")
        return a == b
    mf = os.path.join(HERE, f"meta_{i+1:03d}.txt"); tf = os.path.join(HERE, "seg", f"b{i:03d}_twin.txt")
    mame("advance.lua", dict(S0=str(meta(i)["score"]), START=f"b{i:03d}", NEXT=f"b{i+1:03d}", PATHF=bf, META=tf, **extra), sr=SR, stdir="st1k")
    a, b = open(mf).read().strip(), (open(tf).read().strip() if os.path.exists(tf) else "missing")
    if a != b: log(f"board {i:03d} LOW-SAMPLE-RATE TWIN MISMATCH: {b} vs {a}")
    return a == b

def halfline(tag, k, frac):   # the best route cut short: the search keeps its opening and must rebuild the rest from scratch
    bp = open(os.path.join(HERE, "seg", f"{tag}_best.txt")).read().split("\n")[1]
    f = os.path.join(HERE, "seg", f"{tag}_half{k}.txt")
    open(f, "w").write(f"0 0 0\n{bp[:int(len(bp) * frac)]}\n")
    return f

SACLVL = int(os.environ.get("SACLVL", 21))   # user: spend spare lives on high-scoring boards near the end, keep the last for the kill screen
def level_pos(i):  # (position of board i within its level, index of the matching user board)
    nr = [j for j in range(i + 1) if not meta(j)["restart"]]
    return sum(1 for j in nr if meta(j)["lvl"] == meta(i)["lvl"]), len(nr) - 1

# Search runs MAME at a low audio sample rate (~46% faster emulation; game logic proven identical over boards 0-62).
# States are not portable across sample rates, so a parallel state set st1k/ is kept; official states/verification stay at 48 kHz.
SR = os.environ.get("SR", "1000")
ST1K = os.path.join(HERE, "st1k", "dkong")
def mame(script, env, sr="48000", stdir="st"):
    e = dict(os.environ, **env); e.pop("DISPLAY", None); e.pop("WAYLAND_DISPLAY", None); e["SDL_VIDEODRIVER"] = "dummy"
    subprocess.run([MAMEBIN, "dkong", "-rompath", ROMPATH, "-state_directory", os.path.join(HERE, stdir),
        "-skip_gameinfo", "-video", "none", "-sound", "none", "-nothrottle", "-samplerate", sr, "-autoboot_script", os.path.join(HERE, script)],
        env=e, cwd=WORKDIR, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=900)

UIN = {}   # optional: a human recording (frame -> input mask) mapped to boards by userboards.txt, used as extra seeds
if os.path.exists(os.path.join(HERE, "xin_all.log")):
    for ln in open(os.path.join(HERE, "xin_all.log")):
        p = ln.split(); UIN[int(p[0])] = int(p[1])
i = int(sys.argv[1]) if len(sys.argv) > 1 else max(int(f[5:8]) for f in os.listdir(HERE) if f.startswith("meta_") and f[5:8].isdigit())
while True:
    m = meta(i)
    ks = m["lvl"] >= 22
    if ks:
        log(f"board {i:03d} is L{m['lvl']} {NAME[m['bt']]}: KILL SCREEN reached, score {m['score']} lives {m['lives']} -- scoring with the last life")
    bt = m["bt"]
    pos, uidx = level_pos(i)
    # sacrifice: on SACLVL barrel boards while a spare life remains; once per board, except the level's last barrel board takes all that are left
    sac = ks or (bt == 1 and m["lvl"] == SACLVL and m["lives"] > 1 and (not m["restart"] or pos == 5))
    tgt = 20000 if bt == 1 and m["lvl"] >= 4 else TARGET[bt]   # user: 16k floor, expects 18k every time; TAS reached 22k -> aim 20k
    shutil.rmtree(SHM, ignore_errors=True); os.makedirs(os.path.join(SHM, "dkong"))
    shutil.copy(os.path.join(DKXROOTS if DKX else ST1K, f"b{i:03d}.sta"), os.path.join(SHM, "dkong", "root.sta"))
    tag = f"b{i:03d}"
    # seed from the user's own play of the matching board
    UB = [l for l in open(os.path.join(HERE, "userboards.txt")).read().split("\n") if l.strip()] if UIN else []
    if uidx < len(UB):
        ub = UB[uidx].split()
        ua, ue = int(ub[0]), int(ub[1])
        with open(os.path.join(HERE, "seg", f"{tag}_user.txt"), "w") as f:
            f.write(f"user board L{ub[3]} type {ub[2]}\n" + "".join(chr(48 + UIN[k]) for k in range(ua + 1, ue + 1)) + "\n")
    def seed(inp, pfx):
        out = os.path.join(HERE, "seg", f"{tag}_seeds_{pfx}.txt")
        se = dict(os.environ, BT=str(bt), T0=str(m["t0"]), L0=str(m["lives"]), S0=str(m["score"]), INP=inp, OUT=out, PFX=pfx)
        se.pop("DISPLAY", None); se.pop("WAYLAND_DISPLAY", None); se["SDL_VIDEODRIVER"] = "dummy"
        if DKX:
            subprocess.run([DKX, "seedgen"], env=dict(se, STD=os.path.join(SHM, "dkong")), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=600)
        else:
          subprocess.run([MAMEBIN, "dkong", "-rompath", ROMPATH, "-state_directory", SHM,
            "-skip_gameinfo", "-video", "none", "-sound", "none", "-nothrottle", "-samplerate", SR, "-autoboot_script", os.path.join(HERE, "seedgen.lua")],
            env=se, cwd=WORKDIR, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=600)
        sl = open(out).read().strip().split("\n")
        log(f"board {i:03d} seed {pfx}: {len(sl) - 1} states, ends: {sl[-1]}")
        return f"{out}:{inp}"
    specs = [seed(os.path.join(HERE, "seg", f"{tag}_user.txt"), "u")] if uidx < len(UB) else []
    # our own best lines from the last 3 boards of the same type (strategy library)
    lib = [j for j in range(i - 1, -1, -1) if meta(j)["bt"] == bt and os.path.exists(os.path.join(HERE, "seg", f"b{j:03d}_best.txt"))][:3]
    if bt == 1:   # the earlier L13 TAS lines (13.7k-22k) as global barrel skeletons
        for g in ("t8_22000", "t7_20400", "t6_19400", "t4_17300"):
            if os.path.exists(os.path.join(HERE, "lib", f"{g}.txt")):   # optional library of earlier barrel lines
                specs.append(seed(os.path.join(HERE, "lib", f"{g}.txt"), "g" + g[1]))
    for j in lib:
        specs.append(seed(os.path.join(HERE, "seg", f"b{j:03d}_best.txt"), f"l{j}"))
    for k, xf in enumerate(sorted(glob.glob(os.path.join(HERE, "seg", f"{tag}_extra*.txt")))):   # earlier clears of this board
        specs.append(seed(xf, f"x{k}"))
    env = dict(os.environ, SR=SR, SHM=SHM, ROOT="root", S0=str(m["score"]), T0=str(m["t0"]), L0=str(m["lives"]), BT=str(bt), TARGET=str(tgt),
               W=str(W), HOURS=os.environ.get("HOURS", "1"), HMIN=os.environ.get("HMIN", {1: "1.0", 2: "0.75", 3: "0.33", 4: "0.75"}[bt]), HFAIL=os.environ.get("HFAIL", "1.5"),
               SEEDS=",".join(specs), GELOG=f"seg/{tag}.log", CLEARS=f"seg/{tag}_clears.txt", BESTF=f"seg/{tag}_best.txt", SEED=str(1000 + i))
    log(f"board {i:03d} L{m['lvl']} {NAME[bt]} start score {m['score']} lives {m['lives']} t0 {m['t0']} target {tgt}" + ("  ** SACRIFICE A LIFE **" if sac else ""))
    t0 = time.time()
    if sac:
        sh = str(cfg().get("sac_hours", 1.5))
        senv = dict(env, SACR="1", NEEDY="0", HOURS=sh if not ks else "0.5", HMIN=str(cfg().get("sac_hmin", 1.0)) if not ks else "0.5", HFAIL="2.5" if not ks else "0.6")
        subprocess.run(["python3", os.path.join(HERE, "gcoord.py")], env=senv, cwd=HERE)
        subprocess.run("killall -9 mame dkx 2>/dev/null", shell=True)
        bf = os.path.join(HERE, "seg", f"{tag}_best.txt")
        if not os.path.exists(bf):
            log(f"board {i:03d} sacrifice search found nothing -- stopping"); break
        best = open(bf).readline().split()
        mame("advance.lua", dict(S0=str(m["score"]), START=f"b{i:03d}", NEXT=f"b{i+1:03d}", PATHF=bf, META=os.path.join(HERE, f"meta_{i+1:03d}.txt"), SACR="1"))
        res = open(os.path.join(HERE, f"meta_{i+1:03d}.txt")).read().strip()
        if not res.startswith(("FAIL", "GAMEOVER")) and not twin(i, bf, {"SACR": "1"}):
            log("twin state mismatch -- stopping"); break
        log(f"board {i:03d} life spent in {(time.time()-t0)/60:.0f} min: scored {best[0]} | {res}")
        if res.startswith("FAIL"):
            log("advance FAILED -- stopping"); break
        if res.startswith("GAMEOVER"):
            os.rename(os.path.join(HERE, f"meta_{i+1:03d}.txt"), os.path.join(HERE, "final.txt"))
            log(f"GAME OVER on the kill screen: final score {res.split()[1]}"); break
        i += 1; continue
    if not os.path.exists(os.path.join(HERE, "seg", f"{tag}_try1.log")):   # a previous normal pass already failed here
        # best-of-N first passes (user: best of 2 on barrels): the first pass fixes the route, so run independent starts and polish the best
        bf = os.path.join(HERE, "seg", f"{tag}_best.txt")
        sc = lambda f: int(open(f).readline().split()[0]) if os.path.exists(f) else -1
        for j in range(int(cfg().get("starts", {}).get(str(bt), 1))):
            sf = bf if j == 0 else os.path.join(HERE, "seg", f"{tag}_s{j}_best.txt")
            jenv = env if j == 0 else dict(env, GELOG=f"seg/{tag}_s{j}.log", BESTF=f"seg/{tag}_s{j}_best.txt", SEED=str(30000 + 100 * i + j))
            subprocess.run(["python3", os.path.join(HERE, "gcoord.py")], env=jenv, cwd=HERE)
            subprocess.run("killall -9 mame dkx 2>/dev/null", shell=True)
            log(f"board {i:03d} start {j + 1}: best {sc(sf)}")
            if j and sc(sf) > sc(bf): shutil.copy(sf, bf)
    bf = os.path.join(HERE, "seg", f"{tag}_best.txt")
    if not os.path.exists(bf):
        # rescue: weight remaining work (rivets / height) 4x so the search finishes first, then keeps maximising
        log(f"board {i:03d} no clear after {(time.time()-t0)/60:.0f} min -- rescue pass (NEEDW=4)")
        renv = dict(env, NEEDW="4", FRONT="0.5", HOURS="0.75", HFAIL="2.5", GELOG=f"seg/{tag}_r.log", SEED=str(5000 + i))
        subprocess.run(["python3", os.path.join(HERE, "gcoord.py")], env=renv, cwd=HERE)
        subprocess.run("killall -9 mame dkx 2>/dev/null", shell=True)
        if os.path.exists(bf):
            # polish: normal weighting, seeded with the rescue line
            shutil.copy(bf, os.path.join(HERE, "seg", f"{tag}_rescue.txt"))
            log(f"board {i:03d} rescue clear {open(bf).readline().split()[0]} -- polish pass")
            penv = dict(env, SEEDS=env["SEEDS"] + "," + seed(os.path.join(HERE, "seg", f"{tag}_rescue.txt"), "r"),
                        HOURS="0.75", GELOG=f"seg/{tag}_p.log", SEED=str(9000 + i))
            subprocess.run(["python3", os.path.join(HERE, "gcoord.py")], env=penv, cwd=HERE)
            subprocess.run("killall -9 mame dkx 2>/dev/null", shell=True)
            rf = os.path.join(HERE, "seg", f"{tag}_rescue.txt")
            if not os.path.exists(bf) or int(open(bf).readline().split()[0]) < int(open(rf).readline().split()[0]):
                shutil.copy(rf, bf)
    if not os.path.exists(bf):
        log(f"board {i:03d} NO CLEAR after {(time.time()-t0)/60:.0f} min -- stopping"); break
    # barrels from L4 on must reach the target: keep searching (seeded with the best line so far) until they do
    def polish(rnd):
        k = 0; sfx = "" if rnd == 0 else f"r{rnd}"
        # pies/rivets from L7 on, springs always: extra passes up to aims.json (user NEEDS the total past 2M)
        c = cfg(); aim = {**AIM, 3: 8600, **{int(x): v for x, v in c.get("aim", {}).items()}}
        cap = {1: 2, 2: 2, 3: 1, 4: 2, **{int(x): v for x, v in c.get("passes", {}).items()}}
        ok = (bt == 1 and m["lvl"] >= 4) or (bt in (2, 4) and m["lvl"] >= 7) or bt == 3
        more = lambda b: ok and (bt == 1 and b < 18000 and k < 4 or b < aim[bt] and k < cap[bt])
        cur = lambda: int(open(bf).readline().split()[0])
        flat = 0   # a pass that gains nothing gets one diversity pass (all near-best clears share one route, so cut it and rebuild the end); a second blank ends the board
        while os.path.exists(bf) and more(cur()) and flat < (2 if c.get("halfpass") else 1):   # half-route rebuild tested on b064: 13.7k vs 20k -- off by default, so a blank pass ends the board
            k += 1; b0 = cur()
            kf = os.path.join(HERE, "seg", f"{tag}_more{sfx}{k}.txt"); shutil.copy(bf, kf)
            frac = 0.4 + 0.2 * ((i + k) % 3) / 2   # 40-60%
            seeds = env["SEEDS"] + "," + (seed(halfline(tag, k, frac), f"h{k}") if flat else seed(kf, f"m{sfx}{k}"))
            log(f"board {i:03d} best {b0} < {aim[bt]} -- extra pass {k}" + (f" (fresh second half: route kept to {frac:.0%})" if flat else ""))
            kenv = dict(env, SEEDS=seeds, TARGET=str(aim[bt]), HOURS=str(c.get("extra_hours", 1.5)), HMIN=str(c.get("extra_hmin", 0.5)), HFAIL="2.5",
                        STALL=str(c.get("extra_stall", 20)), GELOG=f"seg/{tag}_m{sfx}{k}.log", SEED=str(20000 + 100 * i + k + 50 * rnd))
            subprocess.run(["python3", os.path.join(HERE, "gcoord.py")], env=kenv, cwd=HERE)
            subprocess.run("killall -9 mame dkx 2>/dev/null", shell=True)
            if cur() < int(open(kf).readline().split()[0]): shutil.copy(kf, bf)
            flat = flat + 1 if cur() <= b0 else 0
    polish(0)
    cur = lambda: int(open(bf).readline().split()[0])
    sc = lambda f: int(open(f).readline().split()[0]) if os.path.exists(f) else -1
    # barrels that finish under the 20k floor get one more independent start; it is polished too and the better board is kept
    rf = cfg().get("retry_floor", {})   # user 2026-10-22: raise the retry floor to 21k from L19
    floor = max(tgt, rf.get("floor", 0)) if m["lvl"] >= rf.get("from_lvl", 99) else tgt
    if bt == 1 and not sac and cur() < floor and cfg().get("retry_below", True):
        keep = os.path.join(HERE, "seg", f"{tag}_keep.txt"); shutil.copy(bf, keep)
        sf = os.path.join(HERE, "seg", f"{tag}_s9_best.txt")
        log(f"board {i:03d} best {cur()} < {floor} -- one more independent start")
        subprocess.run(["python3", os.path.join(HERE, "gcoord.py")], env=dict(env, GELOG=f"seg/{tag}_s9.log", BESTF=f"seg/{tag}_s9_best.txt", SEED=str(40000 + i)), cwd=HERE)
        subprocess.run("killall -9 mame dkx 2>/dev/null", shell=True)
        log(f"board {i:03d} extra start: best {sc(sf)}")
        if sc(sf) >= sc(keep) - 3500:   # polishing has added 2.6-3.3k to a start; a start further back than that cannot catch up
            shutil.copy(sf, bf); polish(1)
            if cur() < sc(keep): shutil.copy(keep, bf)
            log(f"board {i:03d} keeping {cur()} (first route {sc(keep)})")
        else:
            shutil.copy(keep, bf)
    best = open(bf).readline().split()
    mame("advance.lua", dict(S0=str(m["score"]), START=f"b{i:03d}", NEXT=f"b{i+1:03d}", PATHF=bf, META=os.path.join(HERE, f"meta_{i+1:03d}.txt")))
    res = open(os.path.join(HERE, f"meta_{i+1:03d}.txt")).read().strip()
    if not res.startswith("FAIL") and not twin(i, bf, {}):
        log("twin state mismatch -- stopping"); break
    flag = "" if int(best[0]) >= tgt else "  ** BELOW TARGET **"
    log(f"board {i:03d} done in {(time.time()-t0)/60:.0f} min: best {best[0]} (play {best[1]} + bonus {100*int(best[2])}){flag} | {res}")
    if res.startswith("FAIL"):
        log("advance FAILED -- stopping"); break
    i += 1
shutil.rmtree(SHM, ignore_errors=True)
log("CHAIN END")
