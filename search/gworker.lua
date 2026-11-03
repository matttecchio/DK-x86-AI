-- Persistent Go-Explore worker: polls JOBDIR/w<WID>.job, runs rollouts, writes w<WID>.res
local WID = os.getenv("WID")
local JOBDIR = os.getenv("JOBDIR")
local STD = os.getenv("STD")
local L0 = tonumber(os.getenv("L0") or 3)
local T0 = tonumber(os.getenv("T0") or 80)
local BT = tonumber(os.getenv("BT") or 1)
local SACR = os.getenv("SACR") == "1"   -- sacrifice a life: a death is the goal (score kept, board restarts); a clear is not
math.randomseed(tonumber(os.getenv("RS") or 1))
local mac = manager.machine
local sp, port
local function r(a) return sp:readv_u8(a) end
local function bcd(v) return (v >> 4) * 10 + (v & 15) end
local SREF = tonumber(os.getenv("S0") or 0)
local function score()
  local v = bcd(r(0x60B4)) * 10000 + bcd(r(0x60B3)) * 100 + bcd(r(0x60B2))
  while v < SREF - 400000 do v = v + 1000000 end
  return v
end
local FIELDS = {"P1 Left", "P1 Right", "P1 Up", "P1 Down", "P1 Button 1"}
local FOBJ = {}   -- the five input fields, looked up once (port.fields builds a fresh table on every access)
local function apply(m) for i = 1, 5 do FOBJ[i]:set_value((m >> (i - 1)) & 1) end end
-- action table: {mask, weight, minhold, maxhold}
local ACTS = {{0, 2, 1, 40}, {1, 3, 2, 40}, {2, 3, 2, 40}, {4, 2, 4, 90}, {8, 0.4, 2, 16},
              {16, 1.2, 0, 0}, {17, 1.4, 0, 0}, {18, 1.4, 0, 0}}
if BT == 2 or BT == 4 then ACTS[5] = {8, 2, 4, 90} end   -- pies/rivets: descending ladders matters (barrel tuning made Down rare and short)
local WSUM = 0; for _, a in ipairs(ACTS) do WSUM = WSUM + a[2] end
local function pick()
  local x = math.random() * WSUM
  for _, a in ipairs(ACTS) do x = x - a[2]; if x <= 0 then return a end end
  return ACTS[1]
end
-- build a rollout: list of inputs + list of macro-end indices
local function mkroll()
  local inp, ends = {}, {}
  for _ = 1, math.random(2, 5) do
    local a = pick()
    if a[1] >= 16 then
      local press = math.random(1, 3)
      for _ = 1, press do inp[#inp+1] = a[1] end
      local after = ({0, 1, 2, a[1] - 16})[math.random(4)]
      for _ = 1, math.random(0, 36) do inp[#inp+1] = after end
    else
      for _ = 1, math.random(a[3], a[4]) do inp[#inp+1] = a[1] end
    end
    ends[#inp] = true
  end
  return inp, ends
end
local NS, J = tonumber(os.getenv("NS") or 4), 2
local function noisy(P, n)
  local runs, starts = {}, {}
  for i = 1, n do if i == 1 or P[i] ~= P[i - 1] then runs[#runs+1] = {i, P[i]} end end
  for k, rr in ipairs(runs) do
    local s = rr[1]
    if k > 1 then s = s + math.random(-J, J); if s <= starts[k - 1] then s = starts[k - 1] + 1 end end
    starts[k] = s
  end
  local o, k = {}, 1
  for i = 1, n do while k < #runs and i >= starts[k + 1] do k = k + 1 end; o[i] = runs[k][2] end
  return o
end
local cands, wrec, lastidx, nz, nzi, nseq, dthidx = {}, nil, 0, 0, 0, nil, {}
local jobs, ji, roll, nroll, out, cc = nil, 0, 0, 0, nil, 0
local phase, inp, ends, idx, node = "idle", nil, nil, 0, nil
local tick = 0
local function enc(t, n) local c = {}; for i = 1, n do c[i] = string.char(48 + t[i]) end; return table.concat(c) end
local function nextjob()
  ji = ji + 1
  if ji > #jobs then
    local f = io.open(JOBDIR .. "/w" .. WID .. ".tmp", "w"); f:write(table.concat(out)); f:write("END\n"); f:close()
    os.rename(JOBDIR .. "/w" .. WID .. ".tmp", JOBDIR .. "/w" .. WID .. ".res")
    jobs = nil; phase = "idle"; return
  end
  node = jobs[ji]; roll = 0; phase = "load"
end
emu.register_frame_done(function()
  if not sp then
    local c = mac.devices[":maincpu"]; if not c then return end; sp = c.spaces["program"]; port = mac.ioport.ports[":IN0"]
    local fl = port.fields; for i, fn in ipairs(FIELDS) do FOBJ[i] = fl[fn] end
  end
  if phase == "idle" then
    tick = tick + 1
    if tick % 2 ~= 0 then return end
    local f = io.open(JOBDIR .. "/w" .. WID .. ".job", "r")
    if not f then return end
    local txt = f:read("a"); f:close(); os.remove(JOBDIR .. "/w" .. WID .. ".job")
    jobs, out = {}, {}
    for line in txt:gmatch("[^\n]+") do
      if line == "Q" then mac:exit(); return end
      local id, file, n, pf, pi = line:match("^J (%S+) (%S+) (%d+) (%S+) (%S+)")
      if id then
        local pin = {}
        if pi ~= "-" then for k = 1, #pi do pin[k] = pi:byte(k) - 48 end end
        jobs[#jobs+1] = {id = id, file = file, n = tonumber(n), pfile = pf, pin = pin}
      end
    end
    ji = 0; nextjob()
    if phase == "idle" then return end
  end
  if phase == "load" then
    roll = roll + 1
    if roll > node.n then nextjob(); if phase ~= "load" then return end; roll = 1 end
    local fh = io.open(STD .. "/" .. node.file .. ".sta", "rb")
    if not fh then out[#out+1] = "M " .. node.file .. "\n"; roll = node.n; return end
    fh:close()
    inp, ends = mkroll(); idx = 0; FIRE = false
    apply(inp[1]); mac:load(node.file); phase = "run"; return
  end
  if phase == "nzstart" then
    if #cands == 0 and not wrec then phase = "load"; return end
    nz = 0; dthidx = {}; phase = "nzload"
    if NS == 0 then nz = 1 end
  end
  if phase == "nzload" then
    nz = nz + 1
    if nz > NS then
      for _, c in ipairs(cands) do
        local d = 0; for _, di in ipairs(dthidx) do if di <= c[1] then d = d + 1 end end
        out[#out+1] = string.format("%s %.3f %s\n", c[2], NS > 0 and d / NS or 0, enc(inp, c[1]))
      end
      if wrec then
        local d = 0; for _, di in ipairs(dthidx) do if di <= wrec[3] then d = d + 1 end end
        out[#out+1] = string.format("W %s %d %d %.3f %s\n", node.id, wrec[1], wrec[2], NS > 0 and d / NS or 0, enc(inp, wrec[3]))
      end
      cands = {}; wrec = nil; phase = "load"; return
    end
    local base, off, lf = inp, 0, node.file
    if node.pfile ~= "-" and #node.pin > 0 then
      local fh = io.open(STD .. "/" .. node.pfile .. ".sta", "rb")
      if fh then
        fh:close(); base = {}; off = #node.pin; lf = node.pfile
        for k = 1, off do base[k] = node.pin[k] end
        for k = 1, lastidx do base[off + k] = inp[k] end
      end
    end
    nzoff = off; nzlen = off + lastidx
    nseq = noisy(base, nzlen); nzi = 0; dthidx[nz] = 1e9
    apply(nseq[1]); mac:load(lf); phase = "nzrun"; return
  end
  if phase == "nzrun" then
    nzi = nzi + 1
    local mode = r(0x600A)
    if mode == 0x16 then phase = "nzload"; return end
    if mode ~= 0x0C or r(0x6200) == 0 or r(0x639D) ~= 0 or r(0x6228) < L0 or r(0x62B1) > T0 then dthidx[nz] = nzi - nzoff; phase = "nzload"; return end
    if nzi >= nzlen then phase = "nzload"; return end
    apply(nseq[nzi + 1]); return
  end
  if phase == "run" then
    idx = idx + 1
    local mode = r(0x600A)
    if mode == 0x16 then
      if SACR then out[#out+1] = "D\n"; lastidx = idx; phase = "nzstart"; return end
      wrec = {score(), r(0x62B1), idx}; lastidx = idx; phase = "nzstart"; return
    end
    if mode ~= 0x0C or r(0x6200) == 0 or r(0x639D) ~= 0 or r(0x6228) < L0 or r(0x62B1) > T0 then
      if SACR then wrec = {score(), 0, idx}; lastidx = idx; phase = "nzstart"; return end
      out[#out+1] = "D\n"; lastidx = idx; phase = "nzstart"; return
    end
    if ends[idx] then
      cc = cc + 1
      local name = string.format("w%s_%d", WID, cc)
      mac:save(name)
      local hc = (r(0x6A18) > 0 and 1 or 0) + (r(0x6A1C) > 0 and 1 or 0)
      local nb, nn, my, pg = 0, 0, r(0x6205), 0
      if BT == 1 then
        for b = 0, 9 do
          local a = 0x6700 + 0x20 * b
          if r(a) & 1 == 1 then nb = nb + 1; local dy = r(a + 5) - my; if dy > -20 and dy < 12 then nn = nn + 1 end end
        end
      else
        for b = 0, 4 do
          local a = 0x6400 + 0x20 * b
          if r(a) & 1 == 1 then nb = nb + 1; local dy = r(a + 5) - my; if dy > -20 and dy < 12 then nn = nn + 1 end end
        end
        for _, a in ipairs({0x6A0C, 0x6A10, 0x6A14}) do if r(a) == 0 then pg = pg + 1 end end
        if BT == 4 then pg = pg + 4 * r(0x6290); for k = 0, 7 do if r(0x6292 + k) ~= 0 then pg = pg + (64 << k) end end end
      end
      cands[#cands+1] = {idx, string.format("C %s %s %d %d %d %d %d %d %d %d %d %d %d %d", node.id, name, score(), r(0x62B1),
        r(0x6203), r(0x6205), r(0x6217), r(0x6395) * 256 + r(0x6394), hc, r(0x6216), r(0x6215), nb, nn, pg)}
    end
    if idx >= #inp then lastidx = idx; phase = "nzstart"; return end
    apply(inp[idx + 1])
  end
end)
