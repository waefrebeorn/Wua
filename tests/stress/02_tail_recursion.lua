-- 02 proper tail recursion: must not grow the stack, N deep iterations.
local function sum(n, acc)
  if n == 0 then return acc end
  return sum(n - 1, acc + n)
end
print('tail', sum(500000, 0))
