# Wua — slerm-Lua
#
# A from-scratch Lua 5.4 reimplementation in C11. No third-party runtime, no
# GC dependency: one arena, one string interner, one hash table.
#
# Target: Lua 5.4 language parity + the base/string/table/math libraries.
# Not implemented: coroutines, utf8, debug, string.pack, the C API.
#
# Build:   make
# Test:    make test          (two gates: core + script)
#
# Test binaries are written to the repo root and gitignored -- no build/
# directory, so no mkdir step is needed (make's spawned mkdir.exe is restricted
# on Windows).
#
# As a submodule inside WWW:
#   git submodule add git@github.com:waefrebeorn/Wua.git '~WuBu~/src/lv'
#   git submodule update --init --recursive

CC      = gcc
# -MMD -MP emits per-object dependency files so a header edit rebuilds every
# dependent source (adopted from WuBuNOS, which commits its own .d tracking).
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -MMD -MP -Isrc
LDLIBS  ?= -lm

LV_SRC  := $(wildcard src/wb_lv_*.c)
LV_OBJS := $(patsubst src/%.c,src/%.o,$(LV_SRC))
DEPS    := $(LV_OBJS:.o=.d)
CORE    := wua_test.exe
SCRIPT  := wua_script_test.exe

-include $(DEPS)

.PHONY: all test test-core test-script fuzz gauntlet clean

all: test

# Each source compiles to its own object once, so an edit rebuilds exactly one
# file and relinks (and a cold build needs no build/ mkdir). Link serially.
src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(CORE): $(LV_OBJS) tests/wb_lv_test.c
	$(CC) $(CFLAGS) $(LV_OBJS) tests/wb_lv_test.c -o $@ $(LDLIBS)

$(SCRIPT): $(LV_OBJS) tests/wb_lv_script_test.c
	$(CC) $(CFLAGS) $(LV_OBJS) tests/wb_lv_script_test.c -o $@ $(LDLIBS)

test-core: $(CORE)
	./$(CORE)

test-script: $(SCRIPT)
	./$(SCRIPT)

test: test-core test-script

# differential fuzzer vs the reference interpreter (needs python3 + lua54ref)
fuzz:
	python3 tests/fuzz_lua.py 100

# stress docket: 11 tests/stress/*.lua under both interpreters, checking no
# hang / no crash / identical stdout / determinism (see stress_runner.py)
stress:
	python3 tests/stress_runner.py

# gauntlet: the deep differential corpus, one domain per interpreter build.
# Mirrors WuBuNOS's own "run a big third-party corpus" gauntlet practice.
gauntlet: test
	python3 tests/fuzz_lua.py 150
	python3 tests/fuzz_grids.py num 150
	python3 tests/fuzz_grids.py format 120
	python3 tests/fuzz_grids.py meta 120
	python3 tests/fuzz_grids.py coro 80
	python3 tests/fuzz_grids.py tbl 80
	python3 tests/fuzz_grids.py varg 120

clean:
	rm -f $(CORE) $(SCRIPT) $(LV_OBJS) $(DEPS)
