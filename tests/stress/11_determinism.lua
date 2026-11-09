-- 11 determinism: a mixed workload aggregated to an order-independent sum.
-- Iteration order of pairs() is not guaranteed, so every computation here
-- reduces to an aggregate -- and the runner additionally runs each stress
-- program twice to catch run-to-run nondeterminism.
local function agg(n)
  local t = {}
  for i = 1, n do t['k' .. i] = i end
  local s = 0
  for k, v in pairs(t) do s = s + v end          -- order-independent sum
  local u = {}
  for i = 1, n do u[i] = i * i end
  local j = table.concat(u, ',')
  return s, #j
end
local a1, b1 = agg(50000)
print('det', a1, b1)
