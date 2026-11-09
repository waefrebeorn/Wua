-- 09 pathological patterns: hostile patterns must TERMINATE on both. The
-- runner's timeout turns a hang into a fail; a correct engine finishes fast.
local s = string.rep('a', 20000) .. 'b'
local x, y = s:find('a*b$')
print('p1', x, y, y == 20001)
local x2, y2 = s:find('a+b$')
print('p2', x2, y2)
print('p3', #(s:gsub('a*', '#')))
print('p4', ('12345'):find('%f[%a]%a'))
print('p5', ('key=val'):gsub('%a+', '(%1)'))
print('p6', ('a.b.c'):gsub('(.-)%.(.-)%.(.-)', '%2%1%3'))
