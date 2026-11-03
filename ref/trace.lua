-- Reference tracer (stock MAME): per frame, Z80 registers + port values + RAM 6000-6bff and 7000-77ff, as fixed-size binary records.
local mac = manager.machine
local NF = tonumber(os.getenv("NF") or 3000)
local out = io.open(os.getenv("OUT"), "wb")
local cpu, sp, ports, n = nil, nil, nil, 0
local REGS = {"PC", "SP", "AF", "BC", "DE", "HL", "IX", "IY", "AF2", "BC2", "DE2", "HL2", "WZ", "R", "I", "IM", "IFF1", "IFF2"}
emu.register_frame_done(function()
  if not cpu then
    cpu = mac.devices[":maincpu"]; sp = cpu.spaces["program"]; ports = mac.ioport.ports
  end
  n = n + 1
  local rec = {string.pack("<Id", n, mac.time:as_double())}
  for _, r in ipairs(REGS) do rec[#rec+1] = string.pack("<H", cpu.state[r].value & 0xffff) end
  for _, p in ipairs({":IN0", ":IN1", ":IN2", ":DSW0"}) do rec[#rec+1] = string.pack("<B", ports[p]:read() & 0xff) end
  rec[#rec+1] = sp:read_range(0x6000, 0x6bff, 8)
  rec[#rec+1] = sp:read_range(0x7000, 0x77ff, 8)
  out:write(table.concat(rec))
  if n >= NF then out:close(); mac:exit() end
end)
