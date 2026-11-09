-- 04 large table load: 250k integer-keyed entries, indexed sum.
local t = {}
for i = 1, 250000 do t[i] = i * 2 end
local s = 0
for i = 1, 250000 do s = s + t[i] end
print('tload', s)
-- spot the sparse border afterwards
t[3] = nil
print('border', #t)
