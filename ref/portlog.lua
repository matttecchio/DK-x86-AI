-- Dump the live IN0/IN1/IN2 values at every frame_done (stock MAME, -playback): 3 bytes per frame.
local out = io.open(os.getenv("OUT"), "wb")
local ports
emu.register_frame_done(function()
  if not ports then ports = manager.machine.ioport.ports end
  out:write(string.char(ports[":IN0"]:read() & 0xff, ports[":IN1"]:read() & 0xff, ports[":IN2"]:read() & 0xff))
end)
emu.add_machine_stop_notifier(function() out:close() end)
