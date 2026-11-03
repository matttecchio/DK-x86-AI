local started = false
emu.register_periodic(function()
  if started then return end
  local d = manager.machine.debugger
  if not d then print("NO DEBUGGER"); return end
  d:command('trace /home/claw3/dkong-ai/native/ref/itrace.log,maincpu,noloop,{tracelog "%X ",totalcycles}')
  d:command('go'); started = true
end)
