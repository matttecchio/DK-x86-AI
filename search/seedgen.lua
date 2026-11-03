-- replay the user's inputs (INP line 2) from the root state; save a seed state every STEP frames until death/clear
local mac = manager.machine
local sp, port, n = nil, nil, 0
local BT = tonumber(os.getenv("BT")); local T0 = tonumber(os.getenv("T0")); local L0 = tonumber(os.getenv("L0"))
local STEP = tonumber(os.getenv("STEP") or 12)
local function r(a) return sp:readv_u8(a) end
local function bcd(v) return (v >> 4) * 10 + (v & 15) end
local SREF = tonumber(os.getenv("S0") or 0)
local function score()
  local v = bcd(r(0x60B4)) * 10000 + bcd(r(0x60B3)) * 100 + bcd(r(0x60B2))
  while v < SREF - 400000 do v = v + 1000000 end
  return v
end
local FIELDS = {"P1 Left", "P1 Right", "P1 Up", "P1 Down", "P1 Button 1"}
local function apply(m) for i, fn in ipairs(FIELDS) do port.fields[fn]:set_value((m >> (i - 1)) & 1) end end
local pf = io.open(os.getenv("INP")); pf:read("l"); local P = pf:read("l"); pf:close()
local f = io.open(os.getenv("OUT"), "w")
emu.register_frame_done(function()
  if not sp then local c = mac.devices[":maincpu"]; if not c then return end; sp = c.spaces["program"]; port = mac.ioport.ports[":IN0"] end
  if n == 0 then apply(P:byte(1) - 48); mac:load("root"); n = 1; return end
  local m = r(0x600A)
  if m == 0x16 then f:write(string.format("CLEAR %d %d %d\n", n, score(), r(0x62B1))); f:close(); mac:exit(); return end
  if m ~= 0x0C or r(0x6200) == 0 or r(0x639D) ~= 0 or r(0x6228) < L0 or r(0x62B1) > T0 or n >= #P then
    f:write(string.format("END %d\n", n)); f:close(); mac:exit(); return end
  if n % STEP == 0 and r(0x6216) == 0 then
    local name = string.format("%s_%d", os.getenv("PFX") or "sd", n)
    mac:save(name)
    local hc = (r(0x6A18) > 0 and 1 or 0) + (r(0x6A1C) > 0 and 1 or 0)
    local nb, nn, my, pg = 0, 0, r(0x6205), 0
    if BT == 1 then
      for b = 0, 9 do local a = 0x6700 + 0x20 * b
        if r(a) & 1 == 1 then nb = nb + 1; local dy = r(a + 5) - my; if dy > -20 and dy < 12 then nn = nn + 1 end end end
    else
      for b = 0, 4 do local a = 0x6400 + 0x20 * b
        if r(a) & 1 == 1 then nb = nb + 1; local dy = r(a + 5) - my; if dy > -20 and dy < 12 then nn = nn + 1 end end end
      for _, a in ipairs({0x6A0C, 0x6A10, 0x6A14}) do if r(a) == 0 then pg = pg + 1 end end
      if BT == 4 then pg = pg + 4 * r(0x6290); for k = 0, 7 do if r(0x6292 + k) ~= 0 then pg = pg + (64 << k) end end end
    end
    f:write(string.format("S %d %s %d %d %d %d %d %d %d %d %d %d %d %d\n", n, name, score(), r(0x62B1), r(0x6203), r(0x6205), r(0x6217),
      r(0x6395) * 256 + r(0x6394), hc, r(0x6216), r(0x6215), nb, nn, pg))
  end
  apply(P:byte(n + 1) - 48)
  n = n + 1
end)
