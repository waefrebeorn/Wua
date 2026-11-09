# Wua — slerm-Lua
#
# A from-scratch Lua 5.4 reimplementation in C11. No third-party runtime, no
# GC dependency: one arena, one string interner, one hash table.
#
# Target: Lua 5.4 language parity + the base/string/table/math libraries.
# Not implemented: coroutines, utf8, debug, string.pack, the C API.
#
# Build:   mkdir -p build && make
# Test:    make test          (two gates: core + script)
#
# As a submodule inside WWW:
#   git submodule add git@github.com:waefrebeorn/Wua.git '~WuBu~/src/lv'
#   git submodule update --init --recursive

CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Isrc
LDLIBS  ?= -lm

LV_SRC  := $(wildcard src/wb_lv_*.c)
CORE    := build/wua_test.exe
SCRIPT  := build/wua_script_test.exe

.PHONY: all test test-core test-script clean

all: test

build:
	@test -d build || mkdir build

$(CORE): $(LV_SRC) tests/wb_lv_test.c | build
	$(CC) $(CFLAGS) $(LV_SRC) tests/wb_lv_test.c -o $@ $(LDLIBS)

$(SCRIPT): $(LV_SRC) tests/wb_lv_script_test.c | build
	$(CC) $(CFLAGS) $(LV_SRC) tests/wb_lv_script_test.c -o $@ $(LDLIBS)

test-core: $(CORE)
	./$(CORE)

test-script: $(SCRIPT)
	./$(SCRIPT)

test: test-core test-script

clean:
	rm -rf build
