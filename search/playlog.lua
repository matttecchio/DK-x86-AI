-- passive logger for -playback: one line each time play starts on a board (mode 0x0C after any other mode), and one per score change
local mac = manager.machine
local sp, n, pm, last, ps = nil, 0, nil, 0, nil
local function r(a) return sp:readv_u8(a) end
local function bcd(v) return (v >> 4) * 10 + (v & 15) end
local function score()
  local v = bcd(r(0x60B4)) * 10000 + bcd(r(0x60B3)) * 100 + bcd(r(0x60B2))
  while v < last - 400000 do v = v + 1000000 end
  last = v; return v
end
local L = io.open(os.getenv("LOG"), "w")
emu.register_frame_done(function()
  if not sp then local c = mac.devices[":maincpu"]; if not c then return end; sp = c.spaces["program"] end
  n = n + 1
  local m = r(0x600A)
  if m == 0x0C and pm ~= 0x0C then L:write(string.format("start %d %d %d %d %d frame=%d\n", score(), r(0x6228), r(0x6229), r(0x6227), r(0x62B1), n)); L:flush() end
  pm = m
  local s = score(); if s ~= ps then L:write(string.format("score %d frame=%d\n", s, n)); ps = s end
end)
emu.add_machine_stop_notifier(function() L:write(string.format("stop %d frame=%d\n", score(), n)); L:close() end)
