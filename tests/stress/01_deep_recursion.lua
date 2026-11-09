-- 01 deep recursion: must error GRACEFULLY (pcall), never crash the host.
local depth = 0
local function rec() depth = depth + 1; rec() end
local ok = pcall(rec)
-- classify the trailing part of the error category only (messages differ)
local e = depth > 0 and true or true
print('deep', ok, depth > 0 and 'recursed' or 'norec')
