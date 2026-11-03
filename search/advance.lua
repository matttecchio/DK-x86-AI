-- load START, replay PATHF line 2, idle until next board's first play frame; save NEXT, write META line
local mac = manager.machine
local sp, port, n = nil, nil, 0
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
local pf = io.open(os.getenv("PATHF")); pf:read("l"); local P = pf:read("l"); pf:close()
local f = io.open(os.getenv("META"), "w")
local cleared, lives0, s0, clr = false, nil, nil, nil
local SACR = os.getenv("SACR") == "1"   -- path ends in a deliberate death: wait for the board to restart (or game over on the last life)
local died, left = nil, false
emu.register_frame_done(function()
  if not sp then local c = mac.devices[":maincpu"]; if not c then return end; sp = c.spaces["program"]; port = mac.ioport.ports[":IN0"] end
  if n == 0 then apply(P:byte(1) - 48); mac:load(os.getenv("START")); n = 1; return end
  if n == 1 then lives0 = r(0x6228); s0 = score() end
  local m = r(0x600A)
  if SACR then
    if not died then
      if m == 0x16 then f:write("FAIL cleared in sacrifice mode\n"); f:close(); mac:exit(); return end
      if m ~= 0x0C or r(0x6200) == 0 or r(0x639D) ~= 0 or r(0x6228) < lives0 then
        died = {n, score()}
        if n < #P - 1 then f:write(string.format("FAIL early death at %d of %d\n", n, #P)); f:close(); mac:exit(); return end
      end
      if n < #P then apply(P:byte(n + 1) - 48) else apply(0) end
      if n > #P + 5 and not died then f:write("FAIL no death by path end\n"); f:close(); mac:exit(); return end
    else
      apply(0)
      if m ~= 0x0C then left = true end
      if left and m == 0x0C and r(0x6228) == lives0 - 1 then
        mac:save(os.getenv("NEXT"))
        f:write(string.format("%s %d %d %d %d %d %d gain=%d deathf=%d pathlen=%d lives0=%d sac=1\n", os.getenv("NEXT"), score(), r(0x6228), r(0x6229), r(0x6227), r(0x62B1), n,
          (score() - s0) % 1000000, died[1], #P, lives0)); f:close(); mac:exit(); return
      end
      if lives0 == 1 and n > died[1] + 600 then   -- last life (kill screen): game over, record the final score
        f:write(string.format("GAMEOVER %d gain=%d deathf=%d end=%d\n", score(), (score() - s0) % 1000000, died[1], n)); f:close(); mac:exit(); return
      end
      if n > died[1] + 3000 then f:write("FAIL board did not restart\n"); f:close(); mac:exit(); return end
    end
    n = n + 1; return
  end
  if n <= #P then
    if m == 0x16 and not cleared then cleared = true; clr = {n, score(), r(0x62B1)} end
    if not cleared and (m ~= 0x0C or r(0x6200) == 0) then f:write(string.format("FAIL dead at %d mode=%02X\n", n, m)); f:close(); mac:exit(); return end
    if n < #P then apply(P:byte(n + 1) - 48) else apply(0) end
  else
    if m == 0x16 and not cleared then cleared = true; clr = {n, score(), r(0x62B1)} end
    apply(0)
    if not cleared then f:write("FAIL no clear by path end\n"); f:close(); mac:exit(); return end
    if m == 0x0C then
      mac:save(os.getenv("NEXT"))
      f:write(string.format("%s %d %d %d %d %d %d gain=%d clearf=%d pathlen=%d lives0=%d\n", os.getenv("NEXT"), score(), r(0x6228), r(0x6229), r(0x6227), r(0x62B1), n,
        (score() - s0) % 1000000, clr[1], #P, lives0)); f:close(); mac:exit(); return
    end
    if n > #P + 3000 then f:write("FAIL no next board\n"); f:close(); mac:exit(); return end
  end
  n = n + 1
end)
