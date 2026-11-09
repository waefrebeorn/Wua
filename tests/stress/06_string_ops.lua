-- 06 string ops on a ~300KB buffer: length-aware, gsub, match, gsub count.
local big = string.rep('ab12 %_xy', 30000)
print('s1', #big)
print('s2', big:gsub('%d', '#'))
print('s3', #big:match('%d+'), big:gsub('ab', '_'))
print('s4', big:gsub('(%w)(%w)', '%2%1'))
