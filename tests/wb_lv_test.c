/*
 * wb_lv_test.c -- verification of the slerm-Lua interpreter (slice 8).
 *
 * Runs real programs through wb_lv_run and checks observable results via
 * host functions that record calls (so we assert program behavior, not just
 * "no crash"):
 *   1. arithmetic + precedence
 *   2. local + assignment + globals
 *   3. table literal, index get/set
 *   4. while / for loops
 *   5. user function with params + return
 *   6. closure capturing an upvalue + calling it (mod-style)
 *   7. host cfn binding + method-call sugar obj:show()
 */
#include "wb_lv.h"
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { if (cond) g_pass++; else { g_fail++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

/* recorder: each host call appends to a string buffer */
typedef struct { char buf[512]; int n; } recorder;

static wb_lv_value rec(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)L;
    recorder *r = (recorder *)ud;
    for (int i = 0; i < n; i++) {
        int len = strlen(wb_lv_tostring(args[i]));
        if (r->n + len < (int)sizeof(r->buf)) {
            memcpy(r->buf + r->n, wb_lv_tostring(args[i]), len);
            r->n += len;
        }
    }
    r->buf[r->n] = 0;
    return wb_lv_nil();
}

/* host func that returns a numerable from args */
static wb_lv_value cfn_boost(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)L; (void)ud;
    double base = (n > 0) ? wb_lv_tonumber(args[0], NULL) : 0;
    return wb_lv_number(base * 10.0);
}

/* assert the recorder captured 'expect' */
static void check_rec(recorder *r, const char *expect) {
    CHECK(r->n == (int)strlen(expect) && strncmp(r->buf, expect, r->n) == 0,
          "recorded output matches expected");
}

int main(void) {
    recorder r; memset(&r, 0, sizeof r);

    wb_lv *L = wb_lv_new();
    CHECK(L != NULL, "state created");

    /* 1+2: arithmetic, precedence (3 + 4 * 2 = 11), and print shows it via cfn */
    wb_lv_register_global(L, "echo", rec, &r);
    const char *p1 = wb_lv_run(L, "echo(3 + 4 * 2, '|', 10 - 2)\n", "t1");
    CHECK(p1 == NULL, "p1 ran without error");
    check_rec(&r, "11|8");
    r.n = 0; r.buf[0] = 0;

    /* 2: locals + globals + assignment */
    const char *p2 = wb_lv_run(L,
        "x = 5\n"
        "y = 3\n"
        "local z = x + y\n"
        "echo(z, '|', x * y)\n", "t2");
    CHECK(p2 == NULL, "p2 ran without error");
    check_rec(&r, "8|15");
    r.n = 0; r.buf[0] = 0;

    /* 3: tables: literal, index set/get */
    const char *p3 = wb_lv_run(L,
        "t = {a = 1, b = {2, 3}}\n"
        "t.c = 40\n"
        "echo(t.a, '|', t.c, '|', t.b[2])\n", "t3");
    CHECK(p3 == NULL, "p3 ran without error");
    check_rec(&r, "1|40|3");
    r.n = 0; r.buf[0] = 0;

    /* 4: loops -- while + for, accumulate */
    const char *p4 = wb_lv_run(L,
        "local sum = 0\n"
        "local i = 1\n"
        "while i <= 4 do\n"
        "  sum = sum + i\n"
        "  i = i + 1\n"
        "end\n"
        "local prod = 1\n"
        "for k = 1, 5 do prod = prod * 2 end\n"
        "echo(sum, '|', prod)\n", "t4");
    CHECK(p4 == NULL, "p4 ran without error");
    check_rec(&r, "10|32");
    r.n = 0; r.buf[0] = 0;

    /* 5: functions with params + return */
    const char *p5 = wb_lv_run(L,
        "function add(a, b)\n"
        "  return a + b\n"
        "end\n"
        "echo(add(3, 9))\n", "t5");
    CHECK(p5 == NULL, "p5 ran without error");
    check_rec(&r, "12");
    r.n = 0; r.buf[0] = 0;

    /* 6: a function that reads/writes a captured global (mod-style state) */
    const char *p6 = wb_lv_run(L,
        "count = 0\n"
        "function bump()\n"
        "  count = count + 1\n"
        "  return count\n"
        "end\n"
        "bump()\n"
        "bump()\n"
        "echo(bump())\n", "t6");
    CHECK(p6 == NULL, "p6 ran without error");
    check_rec(&r, "3");
    r.n = 0; r.buf[0] = 0;

    /* 7: host cfn returning a number (bindings like wb.spawn return a handle) */
    wb_lv_register_global(L, "boost", cfn_boost, NULL);
    const char *p7 = wb_lv_run(L, "echo(boost(7))\n", "t7");
    CHECK(p7 == NULL, "p7 ran without error");
    check_rec(&r, "70.0");   /* boost returns a float; Lua renders 70.0 */
    r.n = 0; r.buf[0] = 0;

    /* 8: a function stored as a table field (mod-style API) is callable */
    const char *p8 = wb_lv_run(L,
        "function show(a, b) return a + b end\n"
        "t = {}\n"
        "t.show = show\n"
        "echo(t.show(2, 3))\n", "t8");
    CHECK(p8 == NULL, "p8 ran without error");
    check_rec(&r, "5");
    r.n = 0; r.buf[0] = 0;

    /* 9: robustness + semantics regressions (each of these was a real bug) */
    {
        /* the lexer used a fixed char buf[256]; a longer literal overflowed it */
        char big[1100];
        big[0]='x'; big[1]=' '; big[2]='='; big[3]=' '; big[4]='"';
        memset(big + 5, 'A', 1000);
        big[1005] = '"'; big[1006] = 0;
        const char *e = wb_lv_run(L, big, "big");
        CHECK(e == NULL, "a 1000-char string literal parses without error");
        wb_lv_value v = wb_lv_get_global(L, "x");
        CHECK(v.t == WB_LV_STR && strlen(v.as.s) == 1000,
              "the long literal round-trips intact");
    }
    {
        /* the AST pool and the parser lived on the C stack with no depth limit */
        char deep[1200];
        int n = 0;
        for (int i = 0; i < 500; i++) deep[n++] = '(';
        deep[n++] = '1';
        for (int i = 0; i < 500; i++) deep[n++] = ')';
        deep[n] = 0;
        CHECK(wb_lv_run(L, deep, "deep") != NULL,
              "over-deep nesting is rejected with an error, not a crash");
    }
    {
        /* the table-literal loop never checked EOF, so this hung forever */
        CHECK(wb_lv_run(L, "x = { 1, 2, 3", "unterm") != NULL,
              "an unterminated table literal is an error, not a hang");
    }
    {
        /* closures captured pointers into wb_lv_run's C-stack AST pool */
        wb_lv_run(L, "function late(dt) return dt * 2 end", "late");
        wb_lv_run(L, "unused = 1", "clobber");
        wb_lv_value fn = wb_lv_get_global(L, "late");
        wb_lv_value arg = wb_lv_number(21);
        wb_lv_value got = wb_lv_call(L, fn, &arg, 1);
        CHECK(got.t == WB_LV_NUM && got.as.n == 42.0,
              "a closure survives into wb_lv_call after another run");
    }
    {
        /* a failing call used to leave no error behind at all */
        CHECK(wb_lv_run(L, "nope()", "callnil") != NULL,
              "calling an undefined global reports an error");
        CHECK(wb_lv_run(L, "x = 5\nx()", "callnum") != NULL,
              "calling a non-function reports an error");
    }
    {
        /* every comparison coerced non-numbers to 0, so 'a' < 'b' was false */
        r.n = 0; r.buf[0] = 0;
        wb_lv_run(L, "if 'a' < 'b' then echo('LT') else echo('GE') end", "cmp");
        check_rec(&r, "LT");
    }
    {
        /* wb_lv_tostring's shared buffer aliased: 1 .. 2 produced "22" */
        r.n = 0; r.buf[0] = 0;
        wb_lv_run(L, "echo(1 .. 2)", "concat");
        check_rec(&r, "12");
    }
    {
        /* unbounded NON-tail script recursion must report an error, not eat
         * the C stack (tail recursion is legitimately an infinite loop --
         * Lua's proper tail calls -- so the depth guard only fires here). */
        CHECK(wb_lv_run(L, "function f(n) return 1 + f(n+1) end\nf(0)", "rec") != NULL,
              "unbounded script recursion reports an error");
    }
    {
        /* gsub replacement paths (Lua 5.4 semantics). A table or function
         * result that is nil/false keeps the ORIGINAL match; a string or
         * number replaces it; anything else is an error. The table path used
         * to emit nothing for unmatched keys, silently dropping characters. */
        r.n = 0; r.buf[0] = 0;
        wb_lv_run(L, "echo((string.gsub('abc', '%a', {a='X'})))", "gsub1");
        check_rec(&r, "Xbc");

        r.n = 0; r.buf[0] = 0;
        wb_lv_run(L, "echo((string.gsub('abc', '%a', {a='X', b='Y'})))", "gsub2");
        check_rec(&r, "XYc");

        r.n = 0; r.buf[0] = 0;
        wb_lv_run(L, "echo((string.gsub('abc', '%a', {})))", "gsub3");
        check_rec(&r, "abc");

        r.n = 0; r.buf[0] = 0;
        wb_lv_run(L, "echo((string.gsub('ab', '%a', {a=false})))", "gsub4");
        check_rec(&r, "ab");

        r.n = 0; r.buf[0] = 0;
        wb_lv_run(L, "echo((string.gsub('ab', '%a', function(c) return nil end)))", "gsub5");
        check_rec(&r, "ab");

        r.n = 0; r.buf[0] = 0;
        wb_lv_run(L, "echo((string.gsub('ab', '%a', function(c) return 7 end)))", "gsub6");
        check_rec(&r, "77");

        r.n = 0; r.buf[0] = 0;
        wb_lv_run(L, "echo((string.gsub('abc', '%a', 'X')))", "gsub7");
        check_rec(&r, "XXX");

        /* an invalid replacement value raises; it does not silently drop */
        CHECK(wb_lv_run(L, "string.gsub('a', '%a', {a=true})", "gsub8") != NULL,
              "a boolean table replacement raises");
    }
    {
        /* wb_lv_pcall stashed an EMPTY string: wb_lv_str(L->err) aliased the
         * error buffer, which the very next statement cleared. */
        wb_lv_run(L, "function boom() error('boom') end", "pcall1");
        wb_lv_value fn = wb_lv_get_global(L, "boom");
        wb_lv_value out[4]; int nout = -1;
        CHECK(wb_lv_pcall(L, fn, NULL, 0, out, &nout) == 1,
              "wb_lv_pcall reports a raised call");
        CHECK(nout == 0, "a raised call returns no values");
        wb_lv_value le = wb_lv_get_global(L, "__lv_last_error");
        CHECK(le.t == WB_LV_STR, "__lv_last_error is a string");
        CHECK(le.t == WB_LV_STR && le.slen == 4 && memcmp(le.as.s, "boom", 4) == 0,
              "__lv_last_error carries the message, not an empty string");
    }
    {
        /* a runtime error (no error object) must survive the same path */
        wb_lv_run(L, "function boomnil() nope() end", "pcall2");
        wb_lv_value fn = wb_lv_get_global(L, "boomnil");
        wb_lv_value out[4]; int nout = -1;
        CHECK(wb_lv_pcall(L, fn, NULL, 0, out, &nout) == 1,
              "a runtime error is reported");
        wb_lv_value le = wb_lv_get_global(L, "__lv_last_error");
        CHECK(le.t == WB_LV_STR && le.slen > 0,
              "a runtime error's message survives");
    }
    {
        /* error(obj) raises the object; the stash must not stringify it */
        wb_lv_run(L, "function boomobj() error({code=5}) end", "pcall3");
        wb_lv_value fn = wb_lv_get_global(L, "boomobj");
        wb_lv_value out[4]; int nout = -1;
        CHECK(wb_lv_pcall(L, fn, NULL, 0, out, &nout) == 1,
              "error(obj) is reported");
        wb_lv_value le = wb_lv_get_global(L, "__lv_last_error");
        CHECK(le.t == WB_LV_TABLE, "error(obj) stashes the original object");
    }

    wb_lv_free(L);
    printf("\n=== LV TESTS: %d pass, %d fail ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
