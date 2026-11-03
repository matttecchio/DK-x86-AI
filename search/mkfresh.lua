-- power-on -> coin -> 1P start -> first frame of play on L1 barrels; save state b000, write META
local mac = manager.machine
local sp, n = nil, 0
local function r(a) return sp:readv_u8(a) end
local function bcd(v) return (v >> 4) * 10 + (v & 15) end
local function score() return bcd(r(0x60B4)) * 10000 + bcd(r(0x60B3)) * 100 + bcd(r(0x60B2)) end
emu.register_frame_done(function()
  if not sp then local c = mac.devices[":maincpu"]; if not c then return end; sp = c.spaces["program"] end
  n = n + 1
  local p = mac.ioport.ports[":IN2"]
  if n >= 800 and n < 860 then p.fields["Coin 1"]:set_value(1) elseif n == 860 then p.fields["Coin 1"]:set_value(0) end
  if n >= 1000 and n < 1060 then p.fields["1 Player Start"]:set_value(1) elseif n == 1060 then p.fields["1 Player Start"]:set_value(0) end
  if n > 1060 and r(0x600A) == 0x0C then
    mac:save("b000")
    local f = io.open(os.getenv("META"), "w")
    f:write(string.format("0 %d %d %d %d %d %d\n", score(), r(0x6228), r(0x6229), r(0x6227), r(0x62B1), n)); f:close()
    mac:exit()
  end
end)
