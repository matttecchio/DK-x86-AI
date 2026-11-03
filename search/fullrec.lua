-- one continuous game from power-on: coin, 1P start, then every board's route back to back (as advance.lua plays them), under -record
-- LOG gets one line per board end: board score lives lvl bt timer (+ GAMEOVER line); NB = number of boards, sacrifice boards listed in SACS
local mac = manager.machine
local sp, port, n = nil, nil, 0
local function r(a) return sp:readv_u8(a) end
local function bcd(v) return (v >> 4) * 10 + (v & 15) end
local last = 0
local function score()
  local v = bcd(r(0x60B4)) * 10000 + bcd(r(0x60B3)) * 100 + bcd(r(0x60B2))
  while v < last - 400000 do v = v + 1000000 end
  return v
end
local FIELDS = {"P1 Left", "P1 Right", "P1 Up", "P1 Down", "P1 Button 1"}
local function apply(m) for i, fn in ipairs(FIELDS) do port.fields[fn]:set_value((m >> (i - 1)) & 1) end end
local DIR, NB = os.getenv("SEGDIR"), tonumber(os.getenv("NB"))
local SAC = {}
for s in string.gmatch(os.getenv("SACS"), "%d+") do SAC[tonumber(s)] = true end
local function path(i) local pf = io.open(string.format("%s/b%03d_best.txt", DIR, i)); pf:read("l"); local p = pf:read("l"); pf:close(); return p end
local L = io.open(os.getenv("LOG"), "w")
local function out(s) L:write(s .. "\n"); L:flush() end
local bi, P, nb, cleared, lives0, died, left = -1, nil, 0, false, nil, nil, false
local function start(i)   -- the frame where advance.lua would have saved this board's start state
  last = score(); out(string.format("b%03d %d %d %d %d %d %d", i, score(), r(0x6228), r(0x6229), r(0x6227), r(0x62B1), n))
  if i >= NB then out("END no more routes"); L:close(); mac:exit(); return end
  bi, P, nb, cleared, lives0, died, left = i, path(i), 1, false, nil, nil, false
  apply(P:byte(1) - 48)
end
local function fail(s) out(string.format("FAIL b%03d %s at %d", bi, s, nb)); L:close(); mac:exit() end
emu.register_frame_done(function()
  if not sp then local c = mac.devices[":maincpu"]; if not c then return end; sp = c.spaces["program"]; port = mac.ioport.ports[":IN0"] end
  n = n + 1
  if bi < 0 then   -- mkfresh.lua's power-on sequence
    local p = mac.ioport.ports[":IN2"]
    if n >= 800 and n < 860 then p.fields["Coin 1"]:set_value(1) elseif n == 860 then p.fields["Coin 1"]:set_value(0) end
    if n >= 1000 and n < 1060 then p.fields["1 Player Start"]:set_value(1) elseif n == 1060 then p.fields["1 Player Start"]:set_value(0) end
    if n > 1060 and r(0x600A) == 0x0C then start(0) end
    return
  end
  if nb == 1 then lives0 = r(0x6228) end
  local m = r(0x600A)
  if SAC[bi] then
    if not died then
      if m == 0x16 then return fail("cleared in sacrifice mode") end
      if m ~= 0x0C or r(0x6200) == 0 or r(0x639D) ~= 0 or r(0x6228) < lives0 then
        died = nb
        if nb < #P - 1 then return fail("early death") end
      end
      if nb < #P then apply(P:byte(nb + 1) - 48) else apply(0) end
      if nb > #P + 5 and not died then return fail("no death by path end") end
    else
      apply(0)
      if m ~= 0x0C then left = true end
      if left and m == 0x0C and r(0x6228) == lives0 - 1 then nb = 0; start(bi + 1); return end
      if lives0 == 1 and nb > died + 600 then
        out(string.format("GAMEOVER %d deathf=%d end=%d frame=%d", score(), died, nb, n)); L:close(); mac:exit(); return
      end
      if nb > died + 3000 then return fail("board did not restart") end
    end
    nb = nb + 1; return
  end
  if nb <= #P then
    if m == 0x16 then cleared = true end
    if not cleared and (m ~= 0x0C or r(0x6200) == 0) then return fail(string.format("dead mode=%02X", m)) end
    if nb < #P then apply(P:byte(nb + 1) - 48) else apply(0) end
  else
    if m == 0x16 then cleared = true end
    apply(0)
    if not cleared then return fail("no clear by path end") end
    if m == 0x0C then start(bi + 1); return end
    if nb > #P + 3000 then return fail("no next board") end
  end
  nb = nb + 1
end)
