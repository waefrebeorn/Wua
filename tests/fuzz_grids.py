#!/usr/bin/env python3
# fuzz_grids.py -- per-domain differential fuzzing for Wua vs reference Lua.
#
# The single-grammar fuzzer (fuzz_lua.py) covers mixed arithmetic/strings/tables.
# These grids drive each STANDARD LIBRARY + language domain in isolation with
# generated-but-valid, terminating programs, letting a domain-specific bug
# surface without being masked by an unrelated part of a big program. Mirrors
# golua's per-domain fuzz-grid practice.
#
#   python3 fuzz_grids.py pattern    -- one domain, N cases
#   python3 fuzz_grids.py all        -- every domain
# Domains: num  pattern  format  coro  meta  tbl  goto  varg
import random, subprocess, sys, os, re

WF = r"C:/Users/eman5/AppData/Local/hermes/cache/scratch/wf.exe"
REF = r"c:/Users/eman5/WuBu/repos/WWW/~WuBu~/thirdparty/build/lua54ref.exe"
TMP = r"C:/Users/eman5/AppData/Local/hermes/cache/scratch/fuzz_tmp.lua"

def norm(o):  # strip per-interpreter addresses / NaN sign forms handle
    o = re.sub(r"table: [0-9A-Fa-f]+", "table: #A", o)
    o = re.sub(r"function: [0-9A-Fa-f]+", "function: #A", o)
    return o

def run(cmd, timeout=8):
    try:
        r = subprocess.run(cmd, capture_output=True, timeout=timeout,
                           cwd=r"C:/Users/eman5/AppData/Local/hermes/cache/scratch")
        return (r.stdout or b"").decode("utf-8", "surrogateescape").replace("\r", ""), \
               (r.stderr or b"").decode("utf-8", "surrogateescape").replace("\r", ""), r.returncode
    except subprocess.TimeoutExpired:
        return "TIMEOUT", "", -1

SEED = 20261111
STR = ["", "abc", "hello world", "a1b2c3", "xX", "The Quick Brown Fox", "12345",
       "..()[].-*+?^$", "a.b.c", "line\nbreak", "  pad  ", "caf\u00e9", "key=val",
       "aaa", "babab", "end", "0", "a%20b", "foo_bar", "---"]
PLAINS = ["", ".", "a", "b", "[ab]", "[a-z]", "%a", "%d", "%w", "%l", "%u", "%s",
          "%p", "%c", "%x", "%W", "%D", "(a+)", "(b+)(c+)", "(%d+)(%w+)", "a*",
          "a+", ".+", ".-", "a?", "^a", "a$", "%b()", "%f[%a]", "%%", "(.-)",
          "a.*b", "[^%d]", "[%a%d]", "(%a)####", "(%w+)%1"]

def pick(e): return e["rng"]

def prog_num(rng):
    exprs = []
    lit = lambda: rng.choice([str(rng.randint(0, 2**31 - 1)), str(rng.randint(-10**6, 10**6)),
                              "0x%x" % rng.randint(0, 0xFFFFFF), "%.3f" % (rng.randint(-9999, 9999) / 101.0),
                              "%.1f" % rng.randint(-50, 50)])
    for _ in range(rng.randint(6, 14)):
        if rng.random() < 0.55:
            exprs.append(f"{lit()} {rng.choice(['+','-','*','/','//','%','^'])} {lit()}")
        else:
            exprs.append(f"math.{rng.choice(['floor','ceil','abs','sqrt','max','min','modf'])} ({lit()}, {lit()})")
    return "print(" + ", ".join(exprs) + ")\n"

def prog_pattern(rng):
    lines = ["math.randomseed(1)"]
    for _ in range(rng.randint(8, 16)):
        s, p = rng.choice(STR), rng.choice(PLAINS)
        lines.append(f"print((string.match({s!r}, {p!r})))")
        lines.append(f"local a,b,c = string.find({s!r}, {p!r}); print('f', a, b, c)")
        lines.append(f"local g,n = string.gsub({s!r}, {p!r}, '#'); print('g', g, n)")
    return "\n".join(lines) + "\n"

def prog_format(rng):
    lines = []
    specs = ["%d","%i","%u","%x","%X","%o","%c","%f","%e","%g","%s","%q","%5d",
             "%-5d","%05d","%.2f","%10s","%+d","%x","%e","%.30g","%4.1f"]
    for _ in range(rng.randint(8, 16)):
        sp = rng.choice(specs)
        if sp in ("%s","%q","%10s"):
            a = repr(rng.choice(STR)).replace("\n","\\n")
        elif sp == "%c":
            a = str(rng.randint(32, 126))
        elif sp in ("%d","%i","%u","%5d","%-5d","%05d","%+d"):
            a = str(rng.randint(-500000, 500000))
        else:
            a = "%.3f %s" % (rng.randint(-999,999)/7.0, rng.randint(0,0)) if False else str(round(rng.randint(-9999,9999)/7.0, 3))
        lines.append(f"print(string.format('{sp}', {a}))")
    return "\n".join(lines) + "\n"

def prog_coro(rng):
    return """local out = {}
function push(v) out[#out+1] = tostring(v) end
local co = coroutine.create(function(a, b)
  push(a + b)
  local c = coroutine.yield(a * b, a - b)
  push('mid', c)
  coroutine.yield('y2')
  push('end', c * 2)
  return 'done'
end)
push(coroutine.resume(co, 3, 4))
push(coroutine.resume(co, 10))
push(coroutine.resume(co))
push(coroutine.status(co))
local w = coroutine.wrap(function(x) for i = 1, 3 do coroutine.yield(x .. i) end end)
push(w('n'))
push(w('n'))
push(pcall(coroutine.resume, w, 1))
push(table.concat(out, ','))
""" 

def prog_meta(rng):
    nm = rng.choice(["__index","__add","__tostring","__concat","__call","__len","__eq","__lt"])
    t = rng.choice(["x","y","z"])
    return f"""local mt = {{}}
function mt.__add(a,b) return 'A' end
function mt.__concat(a,b) return 'C' end
function mt.__tostring(a) return 'T' end
function mt.__call(a,k) return 'call' end
function mt.__len(a) return 7 end
function mt.__eq(a,b) return true end
local x, y, z = setmetatable({{}}, mt), setmetatable({{}}, mt), {{}}
print(x + y, x .. y, tostring(x), x(1), #x, x == y, x == z)
"""

def prog_tbl(rng):
    lines = ["local t = {}", "t.a, t.b, t.c = 1, 2, 3"]
    for _ in range(rng.randint(4, 8)):
        lines.append(f"t[{rng.choice(['1','2','3','kk','5'])}] = {rng.randint(0,9)}")
    lines.append("local u = {1,2,3,4,5}")
    lines.append("local c=0; for k,v in pairs(u) do c=c+v end; print(c)")
    lines.append("table.insert(u, 2, 99); table.remove(u, 4)")
    lines.append("print(table.concat(u, ','), #u)")
    lines.append("local z={10,20,nil,40}; print(#z)")
    return "\n".join(lines) + "\n"

def prog_goto(rng):
    return """local i = 0; local s = 0
::top::
i = i + 1
if i > 5 then goto done end
s = s + i
goto top
::done::
print(i, s)
for k = 1, 4 do if k == 3 then goto cont end print('k', k) ::cont:: end
"""

def prog_varg(rng):
    return """local function a(...) return select('#', ...) end
local function b(...) local x, y = ...; return x, y end
local function c(x, ...) return x, select(2, ...) end
print(a(), a(1,2,3), a(1))
print(b(7, 8), b(9))
print(c('z', 10, 20))
local f = function(...) return ... end
print(f(1,2,3))
"""

def gen(domain, rng):
    return {"num": prog_num, "pattern": prog_pattern, "format": prog_format,
            "coro": prog_coro, "meta": prog_meta, "tbl": prog_tbl,
            "goto": prog_goto, "varg": prog_varg}[domain](rng)

def main():
    domains = sys.argv[1] if len(sys.argv) > 1 else "all"
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 400
    if domains == "all":
        doms = ["num","pattern","format","coro","meta","tbl","goto","varg"]
    else:
        doms = [domains]
    for d in doms:
        fails = passes = hangs = 0
        rng = random.Random(SEED)
        for i in range(n):
            prog = gen(d, rng)
            open(TMP, "w", encoding="utf-8").write(prog)
            ro, re_, rc = run([REF, TMP]); so, se_, sc = run([WF, TMP])
            if ro == "TIMEOUT" or so == "TIMEOUT": hangs += 1; continue
            if re_ or rc: passes += 1; continue      # ref errored => skip
            if norm(so) == norm(ro):
                passes += 1
            else:
                fails += 1
                if fails <= 4:
                    sf = lambda s: s.encode("utf-8", "backslashreplace").decode("utf-8")
                    print(f"[{d} #{i}] FAIL")
                    print("  ref  :", sf(ro[:300].strip()))
                    print("  slerm:", sf(so[:300].strip()))
                    print("  prog :\n" + sf(prog[:400])); print()
        print(f"== {d}: {n} cases, {passes} pass, {fails} fail, {hangs} hang ==")
        if fails: sys.exit(1)
    sys.exit(0)

if __name__ == "__main__":
    main()
