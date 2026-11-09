-- games/gamemode_demo.lua -- a deterministic gamemode.
-- Spawns a FIXED set of entities at load, then does nothing per frame. This
-- makes per-instance isolation verifiable: the only way an instance's entity
-- count changes is an external join, so A vs B differ exactly by what was
-- joined into each -- never by nondeterministic auto-growth.
--
-- This is the script tests/wb_lv_script_test.c loads: it proves the VM meets
-- the gamemode contract (a `wb` table of host functions, a step() the host
-- drives from C).

local mob1 = wb.spawn("mob_1")
local mob2 = wb.spawn("mob_2")

function step(dt)
  -- no ambient spawning; entities only change via external join
end
