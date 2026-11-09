-- gamemode_showcase.lua
-- A self-asserting tour of the Lua 5.4 surface Wua now covers. Run it through
-- the interpreter; every section asserts its result. Deterministic, no I/O.

local ok = true
local function check(cond, label)
    if not cond then ok = false; error("FAIL: " .. label, 0) end
    print("  ok  " .. label)
end

print("== strings ==")
check(("hello world"):upper() == "HELLO WORLD", "method call via metatable")
check(("aBcD"):lower() == "abcd", "lower")
check(("hello"):sub(2, 4) == "ell", "sub")
check(string.rep("ab", 3) == "ababab", "rep")
check(string.len("héllo") == 6, "byte length counts multi-byte chars")
check(string.format("%d %s %.1f", 42, "x", 2.5) == "42 x 2.5", "format")
check(string.format("%q", 'say "hi"') == '"say \\"hi\\""', "format %q")
local m = string.match("k=v", "(%w+)=(%w+)")
check(m == "k" and select(2, string.match("k=v", "(%w+)=(%w+)")) == "v", "match captures")
check(string.gsub("hello", "l+", "L") == "heLo", "gsub")

print("== length-aware strings (embedded NUL) ==")
check(#("a\0b") == 3, "# counts bytes past the NUL")
local b1, b2, b3 = ("a\0b"):byte(1), ("a\0b"):byte(2), ("a\0b"):byte(3)
check(b1 == 97 and b2 == 0 and b3 == 98, "byte reads the NUL byte")
check(("a\0b" .. "c") == ("a" .. "\0" .. "bc"), "concat preserves NUL")
check(string.format("%s", "a\0b") == "a\0b", "format %s round-trips the NUL")

print("== string.pack / unpack ==")
local p = string.pack(">i2 i2 i1", 7, 255, 3)
local a, b, c = string.unpack(">i2 i2 i1", p)
check(a == 7 and b == 255 and c == 3, "pack/unpack round-trip")
local q = string.pack(">i4", 0x01020304)
check(q:byte(1) == 1 and q:byte(4) == 4, "big-endian pack")
check(string.packsize(">i2 d") == 10, "packsize without alignment")
local z, zp = string.unpack("z", string.pack("z", "hi"))
check(z == "hi" and zp == 4, "unpack z returns value + next position (past the NUL)")

print("== coroutines ==")
local co = coroutine.create(function(a)
    local b = coroutine.yield(a + 1)
    return b * 2
end)
local ok1, v1 = coroutine.resume(co, 10)   -- true, 11
local ok2, v2 = coroutine.resume(co, 5)    -- true, 10  (b=5, return 10)
check(ok1 and v1 == 11 and ok2 and v2 == 10, "resume/yield/return values")
check(coroutine.status(co) == "dead", "coroutine dead after return")
local w = coroutine.wrap(function() local x = 1 while x <= 3 do coroutine.yield(x) x = x + 1 end end)
check(w() == 1 and w() == 2 and w() == 3, "wrap iterator")

print("== tables ==")
local t = {10, 20, 30}
table.insert(t, 2, 15)
check(table.concat(t, ",") == "10,15,20,30", "insert + concat")
table.remove(t, 2)
check(table.concat(t, ",") == "10,20,30", "remove clears the trailing slot")
local u = {1, 2, 3}
table.move(u, 1, 3, 2)
check(u[2] == 1 and u[4] == 3, "table.move")
local p2 = table.pack(1, nil, 3)
check(p2.n == 3 and p2[3] == 3, "table.pack preserves n")
check(#t == 3, "# on a clean array")
local hole = {1, 2, nil, 4}
check(#hole == 4, "# reports Lua's border on a sparse array")

print("== math ==")
check(math.floor(-2.5) == -3 and math.ceil(-2.5) == -2, "floor/ceil negatives")
check(math.deg(math.pi) == 180 and math.rad(180) == math.pi, "deg/rad")
check(math.max(-1, -5, 0) == 0 and math.min(9, 2, -3) == -3, "max/min")
check(math.type(3) == "integer" and math.type(3.0) == "float", "integer/float model")
check(math.ult(1, 2) == true, "math.ult")
local rr = math.random(3, 5)
check(rr >= 3 and rr <= 5, "random in range")

print("== vtables / __gc ==")
local fired = false
do
    local obj = setmetatable({}, {__gc = function() fired = true end})
    obj = nil
    collectgarbage("collect")
end
check(fired == true, "__gc ran on an unreachable table")

print("== calls / varargs / returns ==")
local function multi() return 1, 2, 3 end
check(select("#", multi()) == 3, "multiple returns")
check(select(2, multi()) == 2, "select index")
local function vf(...) return select("#", ...) end
check(vf() == 0 and vf(1, 2) == 2, "empty varargs count 0")

print("== forward/backward goto ==")
local i = 0
::top::
i = i + 1
if i < 3 then goto top end
check(i == 3, "backward goto loops correctly")

if ok then print("\nSHOWCASE: all sections passed") end
return ok
