# Stress runner: run every tests/stress/*.lua under BOTH the reference
# interpreter and Wua, and require:
#   1. no hang            (per-case timeout -> fail)
#   2. no crash           (nonzero exit WITHOUT the program's own handled-Error
#                          marker is a crash/fail)
#   3. identical stdout   (value/behaviour parity, modulo known RNG draws)
#   4. deterministic      (each stress program is run twice; outputs must match)
#
# Each stress program must finish on its own: it wraps anything that might
# error in pcall and prints a deterministic summary, so a clean run exits 0.
# A nonzero exit on the REFERENCE is tolerated only if Wua exits nonzero too
# with the same status class -- but in practice the programs are all handled.
#
#   python3 tests/stress_runner.py [N-repeats]
import subprocess, sys, os, time, glob, re

SCRATCH = r"C:/Users/eman5/AppData/Local/hermes/cache/scratch"
WF   = SCRATCH + "/wf.exe"
REF  = r"c:/Users/eman5/WuBu/repos/WWW/~WuBu~/thirdparty/build/lua54ref.exe"
STRESS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "stress")
TIMEOUT = 30   # generous; sized to run well under this on Wua's slower walker

def run(exe, path, timeout=TIMEOUT):
    try:
        r = subprocess.run([exe, path], capture_output=True, timeout=timeout, cwd=SCRATCH)
        return (r.stdout or b"").decode("utf-8", "surrogateescape").replace("\r", ""), r.returncode
    except subprocess.TimeoutExpired:
        return "__HANG__", -1

def main():
    files = sorted(glob.glob(os.path.join(STRESS, "*.lua")))
    if not files:
        print("no stress programs"); return 1
    fails = 0
    for path in files:
        name = os.path.basename(path)
        # validate determinism on Wua first (2 identical runs)
        o1, c1 = run(WF, path); o2, _ = run(WF, path)
        det = "ok" if (o1 == o2 and o1 != "__HANG__") else "NONDET"
        # reference then wua
        ro, rc = run(REF, path); wo, wc = run(WF, path)
        r_hang, w_hang = ro == "__HANG__", wo == "__HANG__"
        parity = "ok" if (ro == wo) else "DIFF"
        crash = ("CRASH" if wc not in (0, -1) else "ok") if not w_hang else "HANG"
        bad = parity == "DIFF" or det == "NONDET" or crash != "ok" or r_hang
        if bad:
            fails += 1
        print(f"[{'FAIL' if bad else ' ok '}] {name:34s} parity={parity} det={det} crash={crash} "
              f"rc=({rc}/{wc}) outlen=({len(ro)}/{len(wo)})")
        if parity == "DIFF" and not r_hang and not w_hang:
            rl, wl = ro.split("\n"), wo.split("\n")
            for k in range(max(len(rl), len(wl))):
                a = rl[k] if k < len(rl) else "<eof>"
                b = wl[k] if k < len(wl) else "<eof>"
                if a != b:
                    print(f"     ref : {a[:110]}")
                    print(f"     wua : {b[:110]}")
                    break
    print(f"=== stress: {len(files)} programs, {fails} failing ===")
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
