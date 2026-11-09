# Wua math: the "better than Lua, and warned" contract

Every math function that Lua 5.4 defines is implemented to match the reference
exactly (verified by the differential grids and the showcase). On top of that
Wua deliberately does **more**, because it is a runtime for game worldspaces,
not a general-purpose host. Where Wua is better-than-Lua, it is called out
below so nobody mistakes an extension or an intentional difference for a bug.

Everything in this file is a hard claim: each intentional difference or
extension has a `WARN` marker, and tests/asserts exist for the parity items.

## Intentional divergence from Lua 5.4 (WARN)

### `math.random` / `math.randomseed` are DETERMINISTIC

Stock Lua 5.4 delegates `math.random` to the platform C library's `rand()`
(mapped through `lua_newstate`'s `randf`). The same seed therefore produces a
**different** sequence on glibc vs musl vs MSVCRT, and `math.random()` with no
seed is seeded by the OS, so it is not reproducible.

Wua instead uses a fixed, self-contained PRNG (an xoshiro-style 128-bit
generator) seeded only by `math.randomseed`. Consequences:

- `math.randomseed(x); ...` then produces the **same** sequence on every
  platform, every run, every build. This is the guarantee a worldspace engine
  needs for replays, rollback, lockstep multiplayer and deterministic
  simulation, and stock Lua cannot offer it.
- `WARN` **the actual values differ from stock Lua for the same seed** -- a
  script that hard-codes an expected random sequence from stock Lua will see
  different numbers under Wua. Scripts that only require *reproducible*
  randomness are strictly better off. There is intentionally no
  "match the C library" mode: matching the platform `rand()` would reinject
  the non-determinism Wua exists to remove, and no two Lua builds agree on it
  anyway.
- Bounds match Lua: `math.random()` in `[0,1)`, `math.random(m)` in `[1,m]`,
  `math.random(m,n)` in `[m,n]` (both inclusive, `m<=n` required).

## Wua math EXTENSIONS (not in Lua 5.4) (WARN)

These are additive only: they do not shadow or alter any Lua 5.4 name, and a
script that references them on stock Lua gets a hard "attempt to index a nil
value" error rather than silently wrong behaviour. Worldspace game math stays
in one line instead of four.

| function | semantic |
|---|---|
| `math.clamp(x, lo, hi)` | `x < lo ? lo : (x > hi ? hi : x)`; swaps lo/hi if inverted. Returns a number. |
| `math.lerp(a, b, t)` | `a + (b - a) * t` (float). Safe at t=0/1; no clamping. |
| `math.sign(x)` | `-1`, `0`, or `1` (integer) for negative / zero / positive. |
| `math.round(x)` | nearest integer, halves away from zero (`floor(x + 0.5)`). Integer result. |
| `math.map(x, a1, b1, a2, b2)` | remap `x` from `[a1,b1]` into `[a2,b2]`; degenerate `a1==b1` returns the `[a2,b2]` midpoint. |
| `math.between(x, lo, hi)` | `lo <= x and x <= hi` (inclusive), boolean. |

`WARN` all six are non-standard: portable scripts must not rely on them, and
projects shared with stock-Lua peers should polyfill them (one line each).

## Float determinism (recommendation + build flag)

All arithmetic is IEEE-754 `binary64` through libc `double`. To make results
bit-identical across compiler builds (the thing most likely to differ is
FMA-contraction of `a*b + c`), Wua's own Makefile pins `-ffp-contract=off`.
Hosts embedding the runtime should do the same if they need bit-exactness
across builds; Lua makes no such promise.

## Parity: exact matches (no warning)

`floor ceil abs sqrt max min fmod modf tointeger type ult pow log log10
deg rad exp` and the trig/hyperbolic set match the reference; `pi huge
maxinteger mininteger` constants and int/float semantics (integer division,
`%` sign, Lua-border `#`, 1-ulp-exact literal parsing) are covered by the
grids in `tests/`. Only the PRNG above and the six extensions differ.
