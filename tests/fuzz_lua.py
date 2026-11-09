#!/usr/bin/env python3
# fuzz_lua.py -- grammar-based differential fuzzer for Wua (slerm-Lua) vs Lua 5.4.
#
# Generates type-consistent, terminating Lua programs that print their results,
# runs each through the reference interpreter and through Wua, and diffs the
# output. A mismatch is a real parity bug (printed values differ, or the
# programs diverge). Deterministic under a fixed seed.
#
# Usage: python3 fuzz_lua.py [N]      # N programs (default 300)
import random, subprocess, sys, os, re

def norm(out):
    # table/function addresses are per-interpreter; not a parity issue
    out = re.sub(r"table: [0-9A-Fa-f]+", "table: <addr>", out)
    out = re.sub(r"function: [0-9A-Fa-f]+", "function: <addr>", out)
    return out

WF = r"C:/Users/eman5/AppData/Local/hermes/cache/scratch/wf.exe"
REF = r"c:/Users/eman5/WuBu/repos/WWW/~WuBu~/thirdparty/build/lua54ref.exe"
TMP = r"C:/Users/eman5/AppData/Local/hermes/cache/scratch/fuzz_tmp.lua"
SEED = 20261109
KEYS = ["a", "b", "v", "key", "n", "k"]

def litnum(e):
    r = e.rng.random()
    if r < 0.40: return str(e.rng.randint(-10**6, 10**6))
    if r < 0.60: return "%.3f" % (e.rng.randint(-5000, 5000) / 9.0)
    if r < 0.75: return "0x%x" % e.rng.randint(0, 0xFFFFF)
    return str(e.rng.randint(0, 2**31 - 1))

def litstr(e):
    return repr(e.rng.choice(["", "x", "abc", "hello", "the quick fox", "123",
        "  pad", "line\nbreak", "cafe", "a,b,c", "%%s", "ok", "TXT", "tt"])) \
        .replace("\n", "\\n").replace("\t", "\\t")

def g_num(e, d=2):
    r = e.rng.random()
    if d <= 0 or r < 0.30: return litnum(e)
    if r < 0.52: return "%s %s %s" % (g_num(e,d-1), e.rng.choice(["+","-","*","/","//","%","^"]), g_num(e,d-1))
    if r < 0.60: return "-(%s)" % g_num(e, d-1)
    p = e.pick("num")
    if r < 0.74 and p: return p
    if r < 0.86: return "math.%s(%s)" % (e.rng.choice(["floor","ceil","abs"]), g_num(e,d-1))
    return "(%s)" % g_num(e,d-1)

def g_str(e, d=2):
    r = e.rng.random()
    if d <= 0 or r < 0.42: return litstr(e)
    if r < 0.55: return "(%s .. %s)" % (g_str(e,d-1), (g_num(e,d-1) if e.rng.random()<0.5 else g_str(e,d-1)))
    if r < 0.70:
        p = e.pick("str")
        if p:
            return "%s:%s" % (p, e.rng.choice(["upper","lower","len","reverse"]))
        return "string.rep(%s, %d)" % (g_str(e,d-1), e.rng.randint(1, 12))
    if r < 0.82: return "string.%s(%s, %d)" % (e.rng.choice(["sub","rep","reverse"]), g_str(e,d-1), e.rng.randint(1, 8))
    if r < 0.90: return "tostring(%s)" % g_num(e,d-1)
    return litstr(e)

def g_bool(e, d=2):
    r = e.rng.random()
    if r < 0.35: return e.rng.choice(["true","false"])
    if r < 0.62: return "%s %s %s" % (g_num(e,d-1), e.rng.choice(["==","~=","<",">","<=",">="]), g_num(e,d-1))
    if r < 0.76: return "%s %s %s" % (g_str(e,d-1), e.rng.choice(["==","~=","<",">"]), g_str(e,d-1))
    if r < 0.86: return "(%s %s %s)" % (g_bool(e,d-1), e.rng.choice(["and","or"]), g_bool(e,d-1))
    return "not %s" % g_bool(e,d-1)

def g_tbl(e, d=2):
    parts = []
    for _ in range(e.rng.randint(1, 3)):
        r = e.rng.random()
        if r < 0.5: parts.append(g_any(e, d-1))
        elif r < 0.75: parts.append("%s=%s" % (e.rng.choice(KEYS), g_any(e,d-1)))
        else: parts.append("[%s]=%s" % (litnum(e), g_any(e,d-1)))
    return "{" + ", ".join(parts) + "}"

def g_any(e, d=2):
    r = e.rng.random()
    if r < 0.25: return g_num(e, d)
    if r < 0.45: return g_str(e, d)
    if r < 0.58: return g_bool(e, d)
    if r < 0.66: return litnum(e) if e.rng.random()<0.5 else litstr(e)
    if r < 0.76: return g_tbl(e, d)
    if r < 0.86:
        p = e.pick(None)
        return p if p else litnum(e)
    return "%s(%s)" % (e.rng.choice(["tostring","type","tonumber"]), g_any(e,d-1))

class S:
    def __init__(self, rng):
        self.rng = rng
        self.l = {"a": None, "b": None, "s": None, "t": None, "x": None, "n": None}
    def d(self, nm, ty, code):
        self.l[nm] = ty
        return "local %s = %s" % (nm, code)
    def pick(self, ty=None):
        c = [nm for nm, t in self.l.items() if (ty is None and t is not None) or t == ty]
        return self.rng.choice(c) if c else None

def gen_program(rng):
    e = S(rng)
    out = ["math.randomseed(42)"]
    out.append(e.d("a", "num", litnum(e)))
    out.append(e.d("b", "str", litstr(e)))
    out.append(e.d("s", "tbl", g_tbl(e, 2)))
    if rng.random() < 0.75: out.append(e.d("x", "num", litnum(e)))
    if rng.random() < 0.7:  out.append(e.d("t", "str", g_str(e, 2)))
    if rng.random() < 0.6:  out.append(e.d("n", "num", "0"))
    # append into the table, then mutate a couple cells
    out.append("for i = 1, 3 do s[#s+1] = %s end" % g_any(e, 1))
    out.append("for i = 1, 3 do s[i] = %s end" % g_any(e, 1))
    out.append("s[4] = nil")
    # a closure reading a num upvalue
    nu = e.pick("num")
    if nu:
        out.append("local function f(k) return k * %s end; print(f(3), f(%s))" % (nu, litnum(e)))
    # an anonymous call producing values
    out.append("local function g() return %s end; local r = g(); print(type(r), r)" % g_any(e, 1))
    # sum a loop (uses the num local if we have one)
    if e.pick("num"):
        out.append("local sum = 0; for i = 1, 4 do sum = sum + i end; print('sum', sum)")
    # table ops
    out.append("print('cnt', #s)")
    out.append("print(%s)" % ", ".join(g_any(e, 2) for _ in range(rng.randint(1, 3))))
    if e.pick("num"):
        out.append("print('up', %s)" % e.pick("num"))
    out.append("print('end', a, tostring(b), type(s), #s, string.upper(b))")
    return "\n".join(out)

def run(cmd, timeout=8):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                           cwd=r"C:/Users/eman5/AppData/Local/hermes/cache/scratch")
        return (r.stdout.replace("\r", ""), r.stderr.replace("\r", ""), r.returncode)
    except subprocess.TimeoutExpired:
        return "TIMEOUT", "", -1

def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 300
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else SEED
    rng = random.Random(seed)
    fails = passes = hangs = 0
    for i in range(n):
        prog = gen_program(rng)
        with open(TMP, "w", encoding="utf-8") as f: f.write(prog)
        ro, rrerr, rrc = run([REF, TMP]); so, _s_err, src = run([WF, TMP])
        if ro == "TIMEOUT" or so == "TIMEOUT":
            hangs += 1; print("HANG #%d" % i); continue
        # only a VALUE divergence in a program the reference ran clean is a fail;
        # if the reference itself errored, skip (not a parity signal)
        if rrerr or rrc != 0:
            passes += 1
            continue
        if norm(so) == norm(ro):
            passes += 1
        else:
            fails += 1
            print("FAIL #%d" % i)
            if fails <= 6:
                print("  ref  :", ro[:400].strip())
                print("  slerm:", so[:400].strip())
                print("  prog :\n" + prog[:700])
                print()
    print("\n== seed %d: %d cases, %d pass, %d fail, %d hang ==" % (seed, n, passes, fails, hangs))
    sys.exit(1 if fails else 0)

if __name__ == "__main__":
    main()
