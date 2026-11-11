# Wua

**slerm-Lua** — a ground-up Lua 5.4 reimplementation in C11.

> **SLERM (verb):** to take someone's full work and make your own version from
> scratch. Not a fork — a ground-up reimplementation.

Wua is the scripting substrate for the ~WuBu~ world space: the thing that lets
people write mods, GUI logic and game rules without a third-party runtime, and
lets other games be slermed into the space. It lives inside
[**WWW**](https://github.com/waefrebeorn/WWW) as a submodule at
`~WuBu~/src/lv`.

- **Version: 5.4** — the target is Lua 5.4 language parity plus the
  base / string / table / math libraries. Every behaviour is checked against
  the real Lua 5.4 interpreter, not against a memory of the manual.
- **C11 only.** No C++, no external deps, no GC dependency.
- One non-intrusive arena, one string interner, one open-addressed table.
- Opaque handles: `wb_lv`, `wb_lv_value`, `wb_lv_func`, `wb_lv_table`.


> **~WuBu~ AGI** — one system, many repositories. This is the **script substrate**. The Body is [WuBuOS](https://github.com/waefrebeorn/WuBuOS), the Brain wubuwizard, the Compiler WuBuNOS.


> **~WuBu~ AGI** — one system, many repositories. This is the **script substrate**. The Body is [WuBuOS](https://github.com/waefrebeorn/WuBuOS), the Brain wubuwizard, the Compiler WuBuNOS.
## What is implemented

**Language.** Integers and floats with the Lua 5.3+ numeric model (integer
arithmetic stays integer, `/` is float, `//` and `%` are floored), the full
operator set (`+ - * / // % ^ # & | ~ << >>`), comparison and equality with
metamethod dispatch, multiple return values, varargs, `and`/`or` with operand
semantics, `if/elseif/else`, `while`, `repeat/until`, numeric and generic
`for`, `do/end`, `break`, `goto`/labels, `local function`, anonymous function
expressions, method definitions (`function t:f()`), table constructors,
`local`/assignment/multiple assignment.

**Values.** nil, boolean, integer, float, interned string, table, function,
C function, multi-value. Metatables with `__index`, `__newindex`, `__add`
family, `__eq`, `__lt`, `__le`, `__concat`, `__call`, `__tostring`, `__len`.

**Errors.** `pcall` / `xpcall` with real unwinding (setjmp catch points), so a
bad script cannot take down the host. Runtime errors carry a line number.

**Libraries.**
- **base** — `print`, `type`, `tostring`, `tonumber`, `assert`, `error`,
  `pcall`, `xpcall`, `select`, `next`, `pairs`, `ipairs`, `rawget`, `rawset`,
  `rawequal`, `rawlen`, `setmetatable`, `getmetatable`, `load`,
  `collectgarbage`, `_G`, `_VERSION`
- **string** — `len`, `sub`, `rep`, `upper`, `lower`, `reverse`, `byte`,
  `char`, `format`, `pack`, `unpack`, `packsize`, and a full Lua-pattern
  matcher: `find`, `match`, `gmatch`, `gsub` (classes, sets, ranges,
  quantifiers `* + - ?`, anchors, captures, position captures,
  back-references, `%b`, `%f`)
- **table** — `insert`, `remove`, `concat`, `sort` (with a comparator),
  `unpack`, `pack`, `move`
- **math** — `floor`, `ceil`, `abs`, `sqrt`, `max`, `min`, `fmod`, `modf`,
  `tointeger`, `type`, `ult`, `random`, `randomseed`, `pow`, `log`, `log10`,
  `exp`, the trig family, `deg`, `rad`, `pi`, `huge`, `maxinteger`,
  `mininteger`
- **os** — `time`, `clock`, `date`, `getenv`, `exit`, `remove`, `rename`,
  `tmpname`, `execute`, `difftime`, `setlocale`
- **io** — `write`, `read`, `open`, `lines`, `input`, `output`, `close`,
  `type`, `popen`, `tmpfile`, `stdin`, `stdout`, `stderr`
- **coroutine** — `create`, `resume`, `yield`, `status`, `running`,
  `isyieldable`, `wrap` (Windows fibers)
- **utf8** — `len`, `char`, `codepoint`, `offset`, `codes`
- **debug** — `traceback`, `getinfo`, `getlocal`, `sethook`, `gethook`,
  `getmetatable`, `setmetatable`, `getupvalue`, `upvalueid`, `upvaluejoin`,
  `getregistry`

**Strings are length-aware**: embedded NUL bytes survive literals, the
library, `string.format %s`, pattern matching, concatenation, comparison
and `#`. Binary data works.

**Finalizers**: tables with a `__gc` metamethod are reclaimed by a
conservative mark/sweep on `collectgarbage("collect")` and at state close,
newest-first in Lua's order. Reachable objects are never finalized.

## Not implemented

The C API (`lua_State`-style embedding surface) and a package/`require`
system. Everything language- and library-level is covered; the differential
probe against the reference interpreter shows no non-random divergences.

## Build and test

```bash
mkdir -p build      # make cannot create build/ itself on Windows
make                # builds and runs both gates
make test           # 27 core assertions + 5 script assertions
```

Requires a C11 compiler. On Windows use MinGW-w64 (`gcc`/`mingw32-make`).

`make fuzz` runs the differential fuzzer (`tests/fuzz_lua.py`): it generates
random type-consistent Lua programs and compares Wua's output to the reference
Lua 5.4 interpreter. Run it after interpreter edits; 1000+ cases across seeds
pass clean. The official `github.com/lua/tests` 5.4 suite also loads and runs
through Wua (a few edge-conformance files still diverge).

Objects are cached per source, so an edit rebuilds one file and relinks
(~0.5s) instead of recompiling everything (~6s).

## Use as a submodule

```bash
git submodule add git@github.com:waefrebeorn/Wua.git '~WuBu~/src/lv'
git submodule update --init --recursive
```

The public header is `src/wb_lv.h`; everything else is private.

## Layout

```
src/wb_lv.h            public API
src/wb_lv_internal.h   state, AST, lexer, parser, runtime internals
src/wb_lv_arena.c      the non-intrusive arena
src/wb_lv_value.c      values, interning, tables, metatables
src/wb_lv_lex.c        the lexer
src/wb_lv_parse.c      the parser + AST pool
src/wb_lv_eval.c       the evaluator, operators, calls, GC mark/sweep
src/wb_lv_host.c       state lifecycle + the base library
src/wb_lv_string.c     the string library + Lua patterns + pack/unpack
src/wb_lv_stdlib.c     table / math / os / io
src/wb_lv_coro.c       coroutines (Windows fibers)
tests/                 the two gates
docs/slerm-lua-gaps.md the measured Lua 5.4 gap list
```

## The gap list

`docs/slerm-lua-gaps.md` records every measured divergence from real Lua 5.4,
each one produced by running the same chunk through both interpreters. It is
the roadmap: when an item is fixed, the case is re-run and the line is
corrected, never deleted.
