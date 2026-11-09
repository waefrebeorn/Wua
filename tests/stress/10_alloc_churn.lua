-- 10 alloc churn: create + abandon many tables/strings. Under Lua this "stress
-- tests GC"; under Wua it exercises the arena and the conservative __gc mark
-- liveness path without ever running the C stacks out.
local sink = {}
for i = 1, 50000 do
  sink[i] = { i = i, tag = 'x' .. tostring(i % 7), tbl = { [i] = i } }
  if i % 1000 == 0 then sink = {} end          -- abandon batches periodically
end
print('alloc', #sink)
print('gc', collectgarbage('count') >= 0)
