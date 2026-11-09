# slerm-Lua vs Lua 5.4 — the measured gap list

> **Method.** 156 differential cases, each run through real Lua 5.4.8 (built
> from the vendored `thirdparty/lua5.4`) and through slerm-Lua, same chunk, same
> interpreter entry. Nothing here is read off the source and guessed at: every
> line was observed. Cases that only differ in rendering (see "(value)" below)
> are marked, because a wrong *print* is not a wrong *computation*.

**Result: 27 of 156 cases match. 129 differ.** The interpreter is a genuine
subset — but the subset is narrower than the header admits, and three of the
differences are bugs rather than missing features.

The probe harness is `tests/wb_lv_conformance.c` (gate `make test-lv-conformance`).
Rebuild the reference any time: `thirdparty/lua5.4` is the ground truth.

---

## Tier 0 — bugs, not missing features

These are wrong answers for things slerm-Lua *claims* to do. Fix these before
adding anything new.

| # | Case | Lua 5.4 | slerm-Lua | Cause |
|---|------|---------|-----------|-------|
| 0.1 | `-"3"` | `-3` | `-1.54737e-311` | `wb_lv_tonumber` returns `v.as.n` for non-numbers. `as` is a union, so for a string it hands back the **pointer** reinterpreted as a double. |
| 0.2 | `tonumber("42")` | `42` | `0` | `cfn_tonumber` only accepts `WB_LV_NUM`; strings are never parsed. |
| 0.3 | `tonumber("abc")` | `nil` | `0` | same. No failure path at all. |
| 0.4 | `print(nil)` / `print(true)` / `print({})` | `nil` / `true` / `table: 0x...` | `(value)` | `wb_lv_tostring` returns the literal `"(value)"` for every non-number/non-string. Masks correct computation — `mutual_recursion` computes `true` correctly and prints `(value)`. |
| 0.5 | `print(1,2,3)` | `1 2 3` | `1` | `cfn_print` reads `args[0]` only. |
| 0.6 | `-1 % 3` | `2` | `-1` | `fmod` keeps the dividend's sign; Lua's `%` is **floored** (sign of the divisor). |
| 0.7 | `local = 5` | syntax error | accepted | `local` with no names parses as an empty `N_LOCAL`; the `= 5` is then a silent no-op. Malformed input is accepted, not rejected. |

## Tier 1 — safety

| # | Case | Lua 5.4 | slerm-Lua |
|---|------|---------|-----------|
| 1.1 | `while true do i=i+1 if i>2 then break end end` | terminates, prints `3` | **hangs forever** |
| 1.2 | `nil_key`: `t[nil]=1` | runtime error | silently accepted |
| 1.3 | `nan_key`: `t[0/0]=1` | runtime error | silently accepted |
| 1.4 | `1 < "2"` | runtime error | `true` (coerces to `0`) |
| 1.5 | `"2" + 1` | runtime error | `1` |
| 1.6 | `true .. ""` | runtime error | concatenates |

**1.1 is the one that matters.** `break` is not in the keyword table
(`wb_lv_lex.c`), so it parses as a `T_NAME` expression statement and does
nothing. Any loop that relies on `break` spins. This is reachable from any
script the engine loads, and a hung interpreter is a hung server. `break` must
become a keyword before anything else on this list.

## Tier 2 — language: expressions

| Feature | Lua | slerm-Lua | Notes |
|---|---|---|---|
| anonymous function expression `function() ... end` | yes | **parse error** | `parse_primary_impl` has no `T_FUNCTION` case. Only *named* `function f()` statements parse. This alone kills `pcall(function() ... end)`, callbacks, and every closure idiom. |
| `do ... end` block | yes | **parse error** | no `T_DO` case in `parse_stmt`. Kills block scoping (`local x = 5 do local x = 7 end`). |
| `local function f()` | yes | **parse error** | `local` expects names, gets `function`. |
| `#` length operator | yes | **parse error** | no unary `#`. |
| `^` power | yes | **parse error** | not in `binprec`. |
| `//` floor division | yes | **parse error** | | 
| `&` `|` `~` `>>` `<<` | yes | **parse error** | no bitwise at all. |
| unary `~` | yes | **nil** | lexer emits `~`, `parse_unary` ignores it. |
| hex literal `0xFF` | `255` | `0` | **confirmed by the user.** `0xFFFFFF` parses as `0` and the `xFFFFFF` is re-tokenised as a name. |
| hex float `0x1p4` | `16.0` | parse error | |
| `.5` leading-dot float | `0.5` | parse error | number lexing starts only on `0-9`. |
| `5.` trailing dot | `5.0` | `5` | accepted, value same; rendering differs. |
| `;` statement separator | yes | **parse error** | |
| empty statement `;;;` | yes | **parse error** | |
| `break` | yes | **not a keyword** | see 1.1 |
| `repeat ... until` | yes | **parse error** | |
| `goto` / `::label::` | yes | **parse error** | |
| numeric `for` step `for i=1,10,3` | `22` | **parse error** | `N_FOR` carries only start/stop. |
| generic `for ... in` | yes | **parse error** | `T_IN` exists in the token enum; nothing consumes it. |
| multiple returns `return 1,2` | yes | **single value** | `N_RETURN` evaluates one expr. |
| multiple assignment `local a,b = 1,2` | yes | **works** | ✓ measured match |
| varargs `...` | yes | **parse error** | no `...` in any param list. |
| metatables / metamethods | yes | **absent** | `__index` `__newindex` `__add` `__eq` `__lt` `__le` `__concat` `__call` `__tostring` `__len` `__close` `__gc` — all absent. `setmetatable` is nil. |
| integer subtype | yes | **absent** | one `double` for every number. Kills `math.type`, `string.format("%d")`, `string.pack`, integer for-loop overflow semantics, and every bitwise op. |

## Tier 3 — language: literals and strings

| Feature | Lua | slerm-Lua |
|---|---|---|
| string escapes `\n \t \r \a \b \f \v \\ \" \'` | interpreted | **copied verbatim** — `"a\nb"` is the four characters `a \ n b` |
| `\ddd` decimal escape | yes | verbatim |
| `\xXX` hex escape | yes | verbatim (`"\x41"` → `x41`) |
| `\u{XXX}` unicode escape | yes | verbatim |
| `\z` whitespace skip | yes | verbatim |
| long string `[[...]]` | yes | **parse error** |
| levelled long string `[=[...]=]` | yes | **parse error** |
| long comment `--[[...]]` | yes | line comment only (silently drops the rest of the line) |
| shebang `#!` first line | skipped | **parse error** |

The escape handling is a one-line fix in the lexer and it is the highest
value-per-effort item after `break`: right now *no* string containing a
backslash behaves like Lua.

## Tier 4 — standard library: entirely absent

Every global below is `nil` in slerm-Lua. Only `print`, `type` and the broken
`tonumber` exist.

| Library | Functions |
|---|---|
| **base** | `assert` `error` `pcall` `xpcall` `select` `rawget` `rawset` `rawequal` `rawlen` `setmetatable` `getmetatable` `next` `tostring` `tonumber` `type` `print` `unpack`/`table.unpack` `load` `loadfile` `dofile` `require` `package` `collectgarbage` `_G` `_VERSION` |
| **string** | `format` `sub` `rep` `gsub` `find` `match` `gmatch` `byte` `char` `upper` `lower` `len` `reverse` `dump` `pack` `packsize` `unpack` |
| **table** | `sort` `concat` `insert` `remove` `unpack` `pack` `move` `getn` `foreach` |
| **math** | `floor` `ceil` `abs` `sqrt` `max` `min` `huge` `pi` `random` `randomseed` `fmod` `modf` `tointeger` `type` `ult` `maxinteger` `mininteger` |
| **os** | `time` `clock` `date` `getenv` `remove` `rename` `tmpname` `exit` |
| **io** | `write` `read` `open` `lines` `close` `flush` |
| **coroutine** | `create` `resume` `yield` `status` `wrap` `isyieldable` `running` `close` |
| **utf8** | `len` `codepoint` `char` `codes` `offset` |
| **debug** | everything |

---

## What works (measured, do not break)

`local function`-free closures over shared locals · `type()` · nested and
variable indexing · recursion (`fact(5)`) · mutual recursion · wrong-arity and
extra-arg calls · global assignment · `local`/assignment/multi-assignment ·
`if/elseif/else` · `while` · numeric `for` (no step) · table literals incl.
`[k]=v` and `k=v` · method calls `t:f()` · `and`/`or` short-circuit and
operand-returning · `not` · string comparison · `..` concat · line comments ·
empty chunks.

---

## The test suite

### What exists

**The official Lua test suite** — <https://github.com/lua/tests> (canonical
mirror of <https://www.lua.org/tests/>; the tarballs there are the same files).
30 `.lua` files plus `all.lua` and `libs/`:

```
all.lua api.lua attrib.lua big.lua bitwise.lua calls.lua checktable.lua
closure.lua code.lua constructs.lua coroutine.lua db.lua errors.lua
events.lua files.lua gc.lua goto.lua heavy.lua literals.lua locals.lua
main.lua math.lua nextvar.lua pm.lua sort.lua strings.lua tpack.lua
utf8.lua vararg.lua verybig.lua
```

### Why it cannot be run as-is

Measured: **all 30 files use features slerm-Lua lacks.** Every file uses
`assert`, `#`, `^`, or `string.*`/`math.*` — the minimum a Lua test file needs.
The suite also assumes the C API (`api.lua`, `db.lua`, `code.lua`, `gc.lua`,
`tpack.lua`, `attrib.lua`, `nextvar.lua`) and `coroutine`. Running it raw
against slerm-Lua produces nothing but parse errors; it is not a usable gate
today and pretending otherwise would be worse than having no suite.

The precedent for doing this properly is **`arnodel/golua`** (a Go Lua
implementation): it passes an *adapted* 5.4.3 suite and publishes the
adaptation as a PR so the diff from upstream stays reviewable. That is the
model — fork the suite, keep the deltas visible, never silently edit a test.

### The plan

**1. A conformance gate that works now.** `tests/wb_lv_conformance.c` runs the
differential cases against slerm-Lua directly and reports
`N pass / M fail`. It is a real gate (`make test-lv-conformance`), it needs no
Lua stdlib, and it grows one case per feature landed. This is the suite that
exists and runs today.

**2. A Lua-side stdlib module.** `src/lv/lv_stdlib.lua` implements the base
library in Lua on top of the interpreter's core — `assert`, `error`, `pcall`,
`select`, `next`, `pairs`, `ipairs`, `tostring`, a working `tonumber`, then
`string.*`, `table.*`, `math.*` — and is loaded into every state at
`wb_lv_new`. Each function lands with cases ported from the official
`strings.lua` / `math.lua` / `sort.lua`. This is what makes the official suite
reachable at all.

**3. Then enable the official files, in dependency order.** As each feature
lands, its file joins the run:

| Order | Official file | Unlocks at |
|---|---|---|
| 1 | `literals.lua` | hex + escapes + long strings |
| 2 | `constructs.lua` | `do`, `break`, `;`, for-step, generic for |
| 3 | `locals.lua` | `local function`, scoping |
| 4 | `closure.lua` | anonymous function expressions |
| 5 | `calls.lua`, `vararg.lua` | multiple returns, varargs |
| 6 | `goto.lua` | `goto`/labels |
| 7 | `bitwise.lua` | integer subtype + bitwise ops |
| 8 | `strings.lua`, `math.lua`, `sort.lua`, `pm.lua` | the stdlib module |
| 9 | `utf8.lua` | utf8 library |

Until step 2 lands, the official suite is a *roadmap*, not a gate. The
conformance gate is the gate.

---

## Order of work

1. **`break` keyword** — the hang (1.1). Safety before features.
2. **Anonymous function expressions + `do ... end` + `local function`** — the
   three parse gaps that block every idiomatic Lua program.
3. **String escapes** — one lexer fix, enormous correctness win.
4. **Tier 0 bugs** — `tostring`, `tonumber`, `print` arity, `%`.
5. **`#`, `^`, `//`, `+ - * /` on a real integer subtype, bitwise** — the
   Lua 5.3+ numeric model.
6. **Multiple returns + varargs**.
7. **The stdlib module**, then the official suite files.
