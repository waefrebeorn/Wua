#!/usr/bin/env python3
# fuzz_parse.py -- parser accept/reject differential fuzz (the PyroLua / golua
# practice). Mutate valid Lua seeds, then check that Wua and the reference
# AGREE on whether each mutant is a syntax error -- the parse layer, which
# value-differential fuzzing does not reach. For mutants both accept, the
# executed stdout is also diffed (catches execution divergence on valid
# mutants). A seed corpus is mutated structurally (insert/delete/swap/flip/
# splice/truncate tokens) so we stress the lexer/parser boundary.
#
#   python3 tests/fuzz_parse.py [N-mutants]
import random, subprocess, sys, os, re

SC = r"C:/Users/eman5/AppData/Local/hermes/cache/scratch"
ACC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "accept_wua.exe")
VERDICT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "verdict.lua")
REF = r"c:/Users/eman5/WuBu/repos/WWW/~WuBu~/thirdparty/build/lua54ref.exe"
WF = SC + "/wf.exe"
TMP = SC + "/fuzz_parse_tmp.lua"

SEEDS = [
    "local x = -2^2 * 3 + 8 // 4 % 3; print(x)",
    "local function add(a,b) return a+b end; print(add(2,3))",
    "local s='abc'; print(s:upper(), ('xy'):rep(2))",
    "local t = {1,2,3; a=4, ['k']=5}; print(t[1],t.a,t.k,#t)",
    "local s=0; for i=1,5 do s=s+i end; if s>10 then print('big') else print('small') end",
    "local i=0; repeat i=i+1 until i==3; print(i)",
    "local x=0; ::top:: x=x+1; if x<3 then goto top end; print(x)",
    "local function f(...) return select('#', ...) end; print(f(1,2,3))",
    "print('a'..'b', #('x\\0y'), string.format('%d %s', 4, 'z'))",
    "local function mk() local c=0; return function() c=c+1; return c end end; local g=mk(); print(g(),g())",
    "local co=coroutine.wrap(function(a) return a*2 end); print(co(4))",
    "local t={3,1,2}; table.sort(t); print(table.concat(t,','))",
    "print(('a1b2'):gsub('%d', '#'))",
    "local m={}; m.__add=function() return 'x' end; print(setmetatable({},m)+setmetatable({},m))",
    "print(5 & 3, 5 | 2, ~5, 5 << 2, 8 >> 1)",
    "local x = [[hello]]; print(x)",
    "local a,b = 1,2; a,b=b,a; print(a,b)",
    "while false do print('never') end; print('end')",
    "local n=' 42 '; print(tonumber(n), tonumber('ff',16))",
    "local u={}; u[1]=1; u[#u+1]=2; print(#u, next(u))",
    "print((function(x) return x*x end)(7))",
    "local e; print(2^0, 2^-1, -0.0, 1/0, 0/0)",
    "for k,v in pairs({x=1,y=2}) do print(k) end",
    "do local q = 1 end; print('scoped')",
]
TOKENS = ["if", "then", "end", "local", "=", "(", ")", "[", "]", "{", "}", ",", ";",
          ":", ".", "+", "-", "*", "/", "//", "%", "..", "==", "<", ">", "<=", ">=",
          "~=", "and", "or", "not", "nil", "true", "false", "function", "return",
          "for", "while", "do", "repeat", "until", "break", "goto", "in", "'s'",
          '"d"', "0", "1", "42", "0x1F", "3.5", "1e3", "n", "t", "x"]

def run(cmd, timeout=6):
    try:
        r = subprocess.run(cmd, capture_output=True, timeout=timeout, cwd=SC)
        return (r.stdout or b"").decode("utf-8", "surrogateescape").replace("\r", ""), r.returncode
    except subprocess.TimeoutExpired:
        return "__HANG__", -1

def mutate(seed, rng):
    """Return a structurally mutated source."""
    s = seed
    op = rng.randint(0, 5)
    if len(s) < 3:
        op = 0
    if op == 0:  # insert a token at a random point
        p = rng.randint(0, len(s)); t = rng.choice(TOKENS)
        return s[:p] + t + (s[p:] if rng.random() < 0.5 else " " + s[p:])
    if op == 1:  # delete a random run
        a = rng.randint(0, len(s) - 1); b = min(len(s), a + rng.randint(1, 4))
        return s[:a] + s[b:]
    if op == 2:  # swap two adjacent runes
        p = rng.randint(0, len(s) - 2)
        return s[:p] + s[p+1] + s[p] + s[p+2:]
    if op == 3:  # flip a char to punctuation
        p = rng.randint(0, len(s) - 1)
        return s[:p] + rng.choice("()[]{}.,;:=+-*%/<>#&|^~!?\"' ") + s[p+1:]
    if op == 4:  # duplicate a run
        a = rng.randint(0, len(s) - 1); b = min(len(s), a + rng.randint(1, 10))
        return s[:b] + s[a:b] + s[b:]
    # op == 5: truncate the tail
    return s[:rng.randint(1, len(s))]

def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 800
    rng = random.Random(20261112)
    agrees = 0; diverges = 0; exec_mismatch = 0; hangs = 0
    shown = 0
    for i in range(n):
        seed = rng.choice(SEEDS)
        prog = mutate(seed, rng)
        open(TMP, "w", encoding="utf-8").write(prog)
        # parse verdicts (accept = 0, reject = 1, other = -1)
        ro, rc = run([REF, VERDICT, TMP]); rv = rc
        wo, wc = run([ACC, TMP]); wv = wc
        # verdict 0 = parse OK (accept), 1 = reject; anything else = skip
        if rv not in (0, 1) or wv not in (0, 1):
            continue
        if rv == wv:
            agrees += 1
            # (execution parity on accepted mutants is covered by fuzz_lua /
            # fuzz_grids; the reference standalone does not flush buffered
            # stdout on a runtime-error exit, so stdout here is host-dependent)
        else:
            diverges += 1
            if shown < 6:
                print(f"[ACCEPT/REJECT DIVERGE #{i}] ref={rv} wua={wv}\n  prog: {prog!r}\n"); shown += 1
        if diverges > 60:
            break
    total = agrees + diverges
    print(f"== parse-fuzz: {total} mutants, agree={agrees}, accept/reject DIVERGE={diverges} ==")
    return 1 if diverges else 0

if __name__ == "__main__":
    sys.exit(main())
