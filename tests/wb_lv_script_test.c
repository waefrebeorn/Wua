/*
 * wb_lv_script_test.c -- drive engine-style bindings through wb_lv.
 *
 * Proves the VM meets the gamemode contract: a `wb` table of host functions
 * (spawn/count as cfn) is set as a global; a gamemode script runs and uses
 * them; step() is fetched and invoked via wb_lv_call.
 */
#include "wb_lv.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { if (cond) g_pass++; else { g_fail++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

typedef struct { int count; } entreg;

static wb_lv_value cfn_spawn(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)L; (void)args; (void)n;
    entreg *eg = (entreg *)ud;
    eg->count++;
    return wb_lv_number(eg->count);   /* handle id */
}
static wb_lv_value cfn_count(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)L; (void)args; (void)n;
    entreg *eg = (entreg *)ud;
    return wb_lv_number(eg->count);
}

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1); if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, n, f); b[got] = 0; fclose(f);
    return b;
}

int main(void) {
    entreg reg = {0};

    wb_lv *L = wb_lv_new();

    /* build the wb.* API as a table of cfn fields, set as global "wb" */
    {
        wb_lv_value wb;
        wb.t = WB_LV_TABLE; wb.as.tab = wb_lv_table_new(L, 0, 2);
        wb_lv_value spawn, cnt;
        spawn.t = WB_LV_CFUNCTION; spawn.as.cf.f = cfn_spawn; spawn.as.cf.ud = &reg;
        cnt.t   = WB_LV_CFUNCTION; cnt.as.cf.f   = cfn_count; cnt.as.cf.ud   = &reg;
        wb_lv_table_set(L, wb.as.tab, wb_lv_str(wb_lv_intern(L,"spawn",5)), spawn);
        wb_lv_table_set(L, wb.as.tab, wb_lv_str(wb_lv_intern(L,"count",5)), cnt);
        wb_lv_set_global(L, "wb", wb);
    }

    char *src = slurp("games/gamemode_demo.lua");
    CHECK(src != NULL, "gamemode file loaded");
    if (!src) { printf("=== LV-SCRIPT TESTS: %d pass, %d fail ===\n", g_pass, g_fail); return 1; }

    const char *err = wb_lv_run(L, src, "gamemode_demo");
    CHECK(err == NULL, "gamemode compiled + ran without error");

    CHECK(reg.count == 2, "gamemode spawned its 2 load-time mobs via wb.spawn");

    /* fetch and drive step() from C (the engine loop contract) */
    wb_lv_value step = wb_lv_get_global(L, "step");
    CHECK(step.t == WB_LV_FUNCTION, "step() is a callable Lua function");
    for (int i = 0; i < 30; i++) {
        wb_lv_value dt = wb_lv_number(1.0/60.0);
        wb_lv_call(L, step, &dt, 1);
    }

    CHECK(reg.count == 2, "deterministic gamemode did not spawn during step");

    /* Wua math improvements (docs/math.md): deterministic PRNG + extensions.
     * These are intentionally MORE than Lua 5.4 and are feature-detected. */
    {
        const char *s =
            "math.randomseed(7)\n"
            "local a = math.random()\n"
            "math.randomseed(7)\n"
            "local b = math.random()\n"
            "c1 = a == b\n"
            "c2 = math.random(100) >= 1 and math.random(100) <= 100\n"
            "c3 = math.clamp(9,0,5) == 5 and math.clamp(-1,0,5) == 0\n"
            "c4 = math.sign(-2) == -1 and math.sign(4) == 1 and math.sign(0) == 0\n"
            "c5 = math.lerp(0,10,0.5) == 5\n"
            "c6 = math.round(2.5) == 3 and math.round(-2.5) == -3\n"
            "c7 = math.map(0.5,0,1,10,20) == 15 and math.between(2,1,3)\n"
            "c8 = wua.deterministic_random == true and #wua.extensions == 6\n";
        const char *e2 = wb_lv_run(L, s, "wua_math");
        CHECK(e2 == NULL, "Wua math extensions compiled + ran");
        if (!e2) {
            CHECK(wb_lv_truthy(wb_lv_get_global(L, "c1")), "math.random reproducible across reseed");
            CHECK(wb_lv_truthy(wb_lv_get_global(L, "c2")), "math.random(m) in [1,m]");
            CHECK(wb_lv_truthy(wb_lv_get_global(L, "c3")), "math.clamp extension");
            CHECK(wb_lv_truthy(wb_lv_get_global(L, "c4")), "math.sign extension");
            CHECK(wb_lv_truthy(wb_lv_get_global(L, "c5")), "math.lerp extension");
            CHECK(wb_lv_truthy(wb_lv_get_global(L, "c6")), "math.round extension");
            CHECK(wb_lv_truthy(wb_lv_get_global(L, "c7")), "math.map/between extensions");
            CHECK(wb_lv_truthy(wb_lv_get_global(L, "c8")), "wua feature table present");
        }
    }

    free(src);
    wb_lv_free(L);
    printf("\n=== LV-SCRIPT TESTS: %d pass, %d fail ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
