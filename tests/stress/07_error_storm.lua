-- 07 error storm: 100k raised+caught errors must run clean and count right.
local okc, errc = 0, 0
for i = 1, 100000 do
  local r, e = pcall(function(v) if v % 2 == 0 then error('boom' .. v) end return v end, i)
  if r then okc = okc + 1 else errc = errc + 1 end
end
print('estorm', okc, errc)
-- nested pcall with error objects
local r2 = pcall(function() pcall(error, {code = 5}) end)
print('nest', r2)
