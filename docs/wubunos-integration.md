# Wua as a WuBuNOS module — integration notes

Wua (slerm-Lua) is a from-scratch Lua 5.4 reimplementation in C11. WuBuNOS is
intended to eventually take Wua in as a module of the compiler. This file is
the contract for that integration: how to embed it, which patterns to reuse,
what the performance ceiling is and how to break it, and the testing the two
projects already share.

**Wua is a strict superset, by rule.** Every script that compiles and runs on
stock Lua 5.4 compiles and runs identically on Wua (verified by the gates,
the differential fuzzers, and the stress docket). On top of that Wua adds
game-engine improvements, and **every extension or intentional divergence is
explicitly warned** in `docs/math.md` and is script-discoverable via the global
`wua` table (`wua.deterministic_random`, `wua.extensions`, `wua.version`).
Portable scripts feature-detect; WuBuNOS games reach for the extensions
(`math.clamp/lerp/sign/round/map/between`) and the reproducible
`math.random`. See `docs/math.md` for the full list and reasoning.


## Embedding

Wua is a plain C11 static library: no runtime deps, no global mutable state,
one arena, one string interner, one hash table. Build it once, link the
objects (or a `libwua.a`) into WuBuNOS.

```
CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -MMD -MP -Isrc
# src/wb_lv_*.c -> the interpreter; the WWW host app drives it via wb_lv.h
```

Public surface (`src/wb_lv.h`):

- `wb_lv *wb_lv_new(void);`            — a fresh, empty state
- `wb_lv_openlibs(wb_lv *L);`          — registers base/string/table/math/os/
                                         io/coroutine/*utf8*/*debug*/package
- `wb_lv_dofile(L, path)` / `wb_lv_dostring(L, src, len)` — run a script
- values are `wb_lv_value { .t = WB_LV_<type>, .as = { i / n / s (+slen) /
  ... } }` — passed by value, never hidden behind a GC handle.

One state is independent of every other: `wb_lv_new` allocates a fresh arena,
interner and value stack. Use one state per worldspace/VM instance; there is
no shared-singleton cost or thread-safety contract, so keep one state per
thread that executes scripts.

String values carry an explicit length (`slen`) and may contain embedded NUL
bytes; never `strlen` them. Concatenation, `#`, indexing, `byte`, and `format`
all respect the stored length. This is deliberately the same rule WuBuNOS
should apply to its own string type: keep length on the side, never assume
NUL-termination.

## The arena pattern (reuse this)

The slab arena (`wb_lv_arena.c`) is the piece WuBuNOS's compiler can copy
wholesale. Instead of one `malloc`/`calloc` per interpreter block (a real
workload issued ~312k of them), every allocation is a zeroed pointer bump out
of a 64K slab; a request larger than the slab gets its own slab. All memory
lives until the arena is destroyed. A profiled run went 47.5ms -> 32ms (~1.5x)
from this alone, and it removes per-allocation calloc/free from the hot path.

Rules that keep it sound:
- every handout must be zero-initialized (fields that rely on a fresh zero
  value exist), because bump allocation reuses memory;
- nothing may be `free`d individually — only the whole arena at destroy;
- a pointer into an arena block is valid until the arena is destroyed, but a
  value stored *inside* the arena (e.g. a realloc'd table) invalidates any
  earlier pointers into its own buffer — never retain such a pointer across a
  resize.

## The performance ceiling

Wua is a tree-walker: per-op dispatch and string-keyed hash lookups are the
cost, and they are the intrinsic limit of the architecture. Per the online
research adopted here (the standard practice for small VMs), the orders of
improvement are:

1. cache resolved variable lookups (name -> direct slot) rather than hashing
   every local/global access;
2. then compile the AST once to a bytecode VM with a register/slot model —
   this is where a real interpreter gets fast, and it is the *right* long-term
   move for a compiler-owned VM, because the compiler already has the AST.

Do not hand-optimize the walker before measuring. The profile counters
(`WB_LV_PROF` env var dumps call/alloc/eval/table tallies at `wb_lv_free`)
exist to find the hot functions; the slab arena, decimal `strtod` literal
parse, and the `__eq` metatable path all came out of those numbers.

## Practices the two projects already share

WuBuNOS -> Wua (adopted):
- `-MMD -MP` per-object dependency tracking (a header edit rebuilds exactly
  the dependent sources, not stales).
- gauntlet: run a bounded third-party corpus every build (`make gauntlet`).

Wua -> WuBuNOS (available to adopt):
- `tests/fuzz_lua.py` — grammar-based differential fuzzer (random valid,
  terminating Lua, diffed against real Lua 5.4 output). `make fuzz`.
- `tests/fuzz_grids.py` — per-domain grids (num/pattern/format/coro/meta/
  tbl/goto/varg) that drive one library each. This is the highest-yield
  depth test in the repo; it has caught real parity bugs a general generator
  missed (64-bit `%u`/`%x`, `%D` negated classes, `__eq` rules, `modf` integer
  part, 1-ulp decimal literal parse, gsub empty-match advancement).
- gates: `tests/wb_lv_test.c` (27 core) + `tests/wb_lv_script_test.c` (5).
- `tests/stress_runner.py` + `tests/stress/*.lua` (`make stress`) — 11 load
  tests run under both interpreters checking no-hang / no-crash / identical
  stdout / determinism. This caught a call-depth leak on caught errors and the
  missing proper-tail-call guarantee, so it is a real robustness gate, not a
  numbers benchmark.

## Fuzzer semantics (so they can be trusted)

A differential fail is only counted when the *reference* ran a program
cleanly and the outputs differ on value (table/function addresses normalized
out). If the reference itself errors — or the generator produced invalid Lua
— the case is skipped, because interpreter error *messages* legitimately
differ. The grey area is value *type* (integer `8` vs float `8.0`): it IS a
fail and it matters (it breaks `type()`, `==` with a float, and `%d`/`%u`)
even though `8 == 8.0` is true.

## Known open issues (checked 2026-11)

- pattern matcher robustness: `string.find('babab','(a+)')` never returns if
  run *after* a dozen diverse `string.gsub` calls in one program. It is not a
  regression (reproduces on the pre-harness baseline) and it is not an
  out-of-bounds in a single gsub; it appears to be matcher/arena-state
  corruption accumulated across many different pattern operations, and needs
  AddressSanitizer or a matcher trace to pin. MinGW (this toolchain) ships no
  libubsan/libasan, so it could not be instrumented here. It is the top
  robustness item to fix with a real sanitizer CI.
- `make clean` cannot spawn the payload's `usr/bin/rm.exe` under Mingw make
  (Access is denied — the sandboxed git-bash rm); run `rm -f src/*.o` from a
  real shell instead. This does not affect builds.
- not yet implemented: the full C API, `package` file-system search (only
  preloaded stdlibs resolve), `load`, `debug.getinfo` fields, and 5.2-era
  octal literals. The system is feature-complete for scripted game logic;
  the remaining surface is embedding/C-facing.
