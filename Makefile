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
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Isrc
LDLIBS  ?= -lm

LV_SRC  := $(wildcard src/wb_lv_*.c)
CORE    := wua_test.exe
SCRIPT  := wua_script_test.exe

.PHONY: all test test-core test-script clean

all: test

$(CORE): $(LV_SRC) tests/wb_lv_test.c
	$(CC) $(CFLAGS) $(LV_SRC) tests/wb_lv_test.c -o $@ $(LDLIBS)

$(SCRIPT): $(LV_SRC) tests/wb_lv_script_test.c
	$(CC) $(CFLAGS) $(LV_SRC) tests/wb_lv_script_test.c -o $@ $(LDLIBS)

test-core: $(CORE)
	./$(CORE)

test-script: $(SCRIPT)
	./$(SCRIPT)

test: test-core test-script

clean:
	rm -f $(CORE) $(SCRIPT)
