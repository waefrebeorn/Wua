-- 08 coroutine stress: one coroutine yielding 100k times, resumed from main.
local co = coroutine.create(function()
  local sum = 0
  for i = 1, 100000 do
    sum = sum + i
    if i % 1000 == 0 then coroutine.yield(i) end
  end
  return sum
end)
local ok, last = true, nil
while true do
  ok, last = coroutine.resume(co)
  if not ok then break end
end
print('co', last)
-- second: a fan of 500 concurrent coroutines
local fan = {}
for i = 1, 500 do
  fan[i] = coroutine.wrap(function(n) local s = 0 for j = 1, n do s = s + j end return s end)
end
local total = 0
for i = 1, 500 do total = total + fan[i](i * 10) end
print('fan', total)
