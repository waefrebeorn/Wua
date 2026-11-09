-- 05 concat storm: 200k parts joined once vs a quadratic .. loop (bounded).
local parts = {}
for i = 1, 200000 do parts[i] = tostring(i % 10) end
print('join', #table.concat(parts))
local s = ''
for i = 1, 20000 do s = s .. 'ab' end
print('ccat', #s)
