/*
 * wb_lv_host.c -- the interpreter's public driver: state lifecycle, the base
 * library, run/call entry points and pcall-based error isolation.
 * See wb_lv.h.
 */
#include "wb_lv_internal.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- state */

const char *wb_lv_version(void) { return "Lua 5.4"; }

wb_lv *wb_lv_alloc(void) {
    wb_lv *L = (wb_lv *)calloc(1, sizeof(*L));
    if (!L) return NULL;
    L->nbuckets = 64;
    L->buckets = (istr **)wb_lv_arena_alloc(L, L->nbuckets * sizeof(*L->buckets));
    L->globals = wb_lv_table_new(L, 0, 32);
    L->scap = 64;
    L->stk = (wb_lv_value *)wb_lv_arena_alloc(L, (size_t)L->scap * sizeof(*L->stk));
    L->rand_state[0] = 0x853c49e6748fea9bULL;
    L->rand_state[1] = 0xda3e39cb94b95bdbULL;
    return L;
}

wb_lv *wb_lv_new(void) {
    wb_lv *L = wb_lv_alloc();
    if (L) wb_lv_openlibs(L);
    return L;
    L->prof_on = getenv("WB_LV_PROF") != NULL;
}

void wb_lv_free(wb_lv *L) {
    if (L && getenv("WB_LV_PROF"))
        fprintf(stderr, "[prof] eval=%lld exec=%lld envg=%lld envs=%lld newenv=%lld tg=%lld ts=%lld call=%lld alloc=%lld\n",
                L->prof.eval, L->prof.exec, L->prof.envg, L->prof.envs,
                L->prof.newenv, L->prof.tg, L->prof.ts, L->prof.call, L->prof.alloc);
    /* run any pending __gc finalizers before the arena is destroyed */
    if (L) {
        for (int i = L->mtab_n - 1; i >= 0; i--) {     /* newest first, like Lua */
            wb_lv_table *t = L->mtab[i];
            if (!t || t->gc_done || !t->meta) continue;
            wb_lv_value f = wb_lv_table_get(L, t->meta, wb_lv_str("__gc"));
            if (f.t == WB_LV_FUNCTION || f.t == WB_LV_CFUNCTION) {
                wb_lv_value self; self.t = WB_LV_TABLE; self.as.tab = t;
                wb_lv_call(L, f, &self, 1);
                t->gc_done = 1;
            }
        }
    }
    if (!L) return;
    anode *n = L->root, *nx;
    while (n) { nx = n->next; free(n); n = nx; }
    free(L->jmp);
    free(L);
}

void wb_lv_register_global(wb_lv *L, const char *name, wb_lv_cfn fn, void *ud) {
    wb_lv_value v; v.t = WB_LV_CFUNCTION; v.as.cf.f = fn; v.as.cf.ud = ud;
    wb_lv_table_set(L, L->globals, wb_lv_str(name), v);
}

wb_lv_value wb_lv_get_global(wb_lv *L, const char *name) {
    return wb_lv_table_get(L, L->globals, wb_lv_str(name));
}

void wb_lv_set_global(wb_lv *L, const char *name, wb_lv_value v) {
    wb_lv_table_set(L, L->globals, wb_lv_str(name), v);
}

const char *wb_lv_run(wb_lv *L, const char *src, const char *chunkname) {
    L->has_err = 0; L->err[0] = 0; L->has_err_obj = 0;
    L->chunkname = chunkname ? chunkname : "?";
    L->curline = 1;

    if (!L->pool) {
        L->pool = WB_LV_ALLOC(pool, L);
        if (!L->pool) { wb_lv_set_err(L, "out of memory"); return L->err; }
    }

    ex *root = NULL;
    if (wb_lv_parse_driver(L, src, L->pool, &root))
        return L->err[0] ? L->err : "parse error";
    wb_lv_exec_root(L, root);
    return L->has_err ? L->err : NULL;
}

wb_lv_value wb_lv_call(wb_lv *L, wb_lv_value fn, wb_lv_value *args, int nargs) {
    if (fn.t == WB_LV_FUNCTION) return wb_lv_call_lua(L, fn.as.fn, args, nargs);
    if (fn.t == WB_LV_CFUNCTION) return fn.as.cf.f(L, args, nargs, fn.as.cf.ud);
    return wb_lv_nil();
}

int wb_lv_pcall(wb_lv *L, wb_lv_value fn, wb_lv_value *args, int nargs,
                wb_lv_value *out, int *nout) {
    /* snapshot call/isolation state so a caught error unwinds cleanly: the
     * longjmp below skips the normal depth-- / env_pop of every frame in
     * between, which would otherwise LEAK +1 depth and a scope per caught
     * error (error-heavy scripts then hit a false 'nesting too deep'). */
    int sd = L->depth, sed = L->eval_depth, se = L->envtop;
    jmp_buf *jb = wb_lv_push_catch(L);
    if (!jb) { wb_lv_set_err(L, "out of memory"); return 1; }
    if (setjmp(*jb) == 0) {
        int nr = wb_lv_call_value(L, fn, args, nargs, &out);
        wb_lv_pop_catch(L);
        *nout = nr;
        return 0;
    }
    L->depth = sd; L->eval_depth = sed; L->envtop = se;   /* unwind state */
    wb_lv_pop_catch(L);
    *nout = 0;
    /* stash the message where the caller can find it */
    wb_lv_set_global(L, "__lv_last_error", wb_lv_str(L->err));
    L->has_err = 0; L->err[0] = 0;
    return 1;
}

/* ---------------------------------------------------------- base library */

static wb_lv_value cfn_print(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)ud;
    for (int i = 0; i < n; i++) {
        if (i) fwrite("\t", 1, 1, stdout);
        wb_lv_value v = args[i];
        /* __tostring */
        wb_lv_table *m = wb_lv_getmetatable(L, v);
        if (m) {
            wb_lv_value f = wb_lv_table_get(L, m, wb_lv_str("__tostring"));
            if (f.t == WB_LV_FUNCTION || f.t == WB_LV_CFUNCTION) {
                v = wb_lv_call(L, f, &args[i], 1);
            }
        }
        const char *sv = wb_lv_tostring_intern(L, v);
        fwrite(sv, 1, v.t == WB_LV_STR ? v.slen : strlen(sv), stdout);
    }
    fwrite("\n", 1, 1, stdout);
    return wb_lv_nil();
}

static wb_lv_value cfn_type(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)L; (void)ud;
    return wb_lv_str(wb_lv_type_name(n > 0 ? args[0].t : WB_LV_NIL));
}

static wb_lv_value cfn_tostring(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)ud;
    wb_lv_value v = (n > 0) ? args[0] : wb_lv_nil();
    wb_lv_table *m = wb_lv_getmetatable(L, v);
    if (m) {
        wb_lv_value f = wb_lv_table_get(L, m, wb_lv_str("__tostring"));
        if (f.t == WB_LV_FUNCTION || f.t == WB_LV_CFUNCTION) v = wb_lv_call(L, f, &args[0], 1);
    }
    return wb_lv_str(wb_lv_tostring_intern(L, v));
}

static int str2num(const char *s, double *out, int *isint) {
    char *end = NULL;
    /* Lua accepts leading/trailing spaces and a sign */
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        long long v = strtoll(s + 2, &end, 16);
        if (end == s + 2 || *end) return 0;
        *out = (double)v; if (isint) *isint = 1;
        return 1;
    }
    long long iv = strtoll(s, &end, 10);
    if (end != s && (*end == 0 || *end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') end++;
        if (*end == 0) { *out = (double)iv; if (isint) *isint = 1; return 1; }
    }
    double d = strtod(s, &end);
    if (end == s) return 0;
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') end++;
    if (*end != 0) return 0;
    *out = d; if (isint) *isint = 0;
    return 1;
}

static wb_lv_value cfn_tonumber(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)L; (void)ud;
    if (n < 1 || args[0].t == WB_LV_NIL) return wb_lv_nil();
    if (args[0].t == WB_LV_INT) return args[0];
    if (args[0].t == WB_LV_NUM) return args[0];
    if (args[0].t != WB_LV_STR) return wb_lv_nil();
    /* optional base */
    if (n >= 2) {
        int b = (int)wb_lv_tonumber(args[1], NULL);
        if (b < 2 || b > 36) { wb_lv_error(L, "base out of range"); return wb_lv_nil(); }
        char *end = NULL;
        long long v = strtoll(args[0].as.s, &end, b);
        if (end == args[0].as.s || *end) return wb_lv_nil();
        return wb_lv_integer(v);
    }
    double d; int isint;
    if (!str2num(args[0].as.s, &d, &isint)) return wb_lv_nil();
    return isint ? wb_lv_integer((int64_t)d) : wb_lv_number(d);
}

static wb_lv_value cfn_assert(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)ud;
    if (n < 1 || !wb_lv_truthy(args[0])) {
        wb_lv_value m = (n >= 2) ? args[1] : wb_lv_str("assertion failed!");
        wb_lv_error(L, "%s", wb_lv_tostring_intern(L, m));
        return wb_lv_nil();
    }
    return wb_lv_multi_value(L, args, n);
}

static wb_lv_value cfn_collectgarbage(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)ud;
    const char *opt = (n >= 1 && args[0].t == WB_LV_STR) ? args[0].as.s : "collect";
    if (!strcmp(opt, "count")) return wb_lv_number((double)wb_lv_gc_count(L));
    if (!strcmp(opt, "collect") || !strcmp(opt, "step")) { wb_lv_gc_collect(L); return wb_lv_integer(1); }
    if (!strcmp(opt, "stop") || !strcmp(opt, "restart")) return wb_lv_integer(0);
    if (!strcmp(opt, "isrunning")) return wb_lv_bool(1);
    if (!strcmp(opt, "incremental")) return wb_lv_integer(0);
    return wb_lv_integer(0);
}

static wb_lv_value cfn_error(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)ud;
    /* error(obj) raises the ORIGINAL value; only the message is stringified */
    wb_lv_error_value(L, (n >= 1) ? args[0] : wb_lv_nil());
    return wb_lv_nil();
}

static wb_lv_value cfn_pcall(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)ud;
    if (n < 1) { wb_lv_error(L, "bad argument #1 to 'pcall' (value expected)"); return wb_lv_nil(); }
    int sd = L->depth, sed = L->eval_depth, se = L->envtop;
    jmp_buf *jb = wb_lv_push_catch(L);
    if (!jb) { wb_lv_set_err(L, "out of memory"); return wb_lv_nil(); }
    if (setjmp(*jb) == 0) {
        wb_lv_value *out = NULL;
        int nr = wb_lv_call_value(L, args[0], args + 1, n - 1, &out);
        wb_lv_pop_catch(L);
        if (L->has_err) { L->has_err = 0; L->err[0] = 0; }
        wb_lv_value *all = WB_LV_ALLOCN(wb_lv_value, nr + 1, L);
        all[0] = wb_lv_bool(1);
        for (int i = 0; i < nr; i++) all[i + 1] = out[i];
        return wb_lv_multi_value(L, all, nr + 1);
    }
    L->depth = sd; L->eval_depth = sed; L->envtop = se;   /* unwind state */
    wb_lv_pop_catch(L);
    wb_lv_value msg = L->has_err_obj ? L->err_obj : wb_lv_str(L->err[0] ? L->err : "error");
    L->has_err = 0; L->err[0] = 0; L->has_err_obj = 0;
    wb_lv_value all[2]; all[0] = wb_lv_bool(0); all[1] = msg;
    return wb_lv_multi_value(L, all, 2);
}

static wb_lv_value cfn_xpcall(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)ud;
    if (n < 2) { wb_lv_error(L, "bad argument #2 to 'xpcall' (value expected)"); return wb_lv_nil(); }
    int sd = L->depth, sed = L->eval_depth, se = L->envtop;
    jmp_buf *jb = wb_lv_push_catch(L);
    if (!jb) { wb_lv_set_err(L, "out of memory"); return wb_lv_nil(); }
    if (setjmp(*jb) == 0) {
        wb_lv_value *out = NULL;
        int nr = wb_lv_call_value(L, args[0], args + 2, n - 2, &out);
        wb_lv_pop_catch(L);
        wb_lv_value *all = WB_LV_ALLOCN(wb_lv_value, nr + 1, L);
        all[0] = wb_lv_bool(1);
        for (int i = 0; i < nr; i++) all[i + 1] = out[i];
        return wb_lv_multi_value(L, all, nr + 1);
    }
    L->depth = sd; L->eval_depth = sed; L->envtop = se;   /* unwind state */
    wb_lv_pop_catch(L);
    wb_lv_value msg = L->has_err_obj ? L->err_obj : wb_lv_str(L->err[0] ? L->err : "error");
    L->has_err = 0; L->err[0] = 0; L->has_err_obj = 0;
    wb_lv_value hargs[1]; hargs[0] = msg;
    wb_lv_value h = wb_lv_call(L, args[1], hargs, 1);
    wb_lv_value all[2]; all[0] = wb_lv_bool(0); all[1] = h;
    return wb_lv_multi_value(L, all, 2);
}

static wb_lv_value cfn_select(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)ud;
    if (n < 1) { wb_lv_error(L, "bad argument #1 to 'select' (number expected)"); return wb_lv_nil(); }
    if (args[0].t == WB_LV_STR && args[0].as.s[0] == '#') {
        return wb_lv_integer(n - 1);
    }
    int64_t i = wb_lv_tointeger(args[0], NULL);
    if (i < 0) i = (int64_t)n + i;          /* negative index from the end */
    else if (i > (int64_t)n - 1) { wb_lv_error(L, "bad argument #1 to 'select' (index out of range)"); return wb_lv_nil(); }
    if (i < 1) { wb_lv_error(L, "bad argument #1 to 'select' (index out of range)"); return wb_lv_nil(); }
    int cnt = n - (int)i;
    if (cnt <= 0) return wb_lv_nil();
    return wb_lv_multi_value(L, args + i, cnt);
}

static wb_lv_value cfn_next(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)ud;
    if (n < 1 || args[0].t != WB_LV_TABLE) { wb_lv_error(L, "bad argument #1 to 'next' (table expected)"); return wb_lv_nil(); }
    wb_lv_value k = (n >= 2) ? args[1] : wb_lv_nil();
    wb_lv_value v;
    if (!wb_lv_table_next(args[0].as.tab, &k, &v)) return wb_lv_nil();
    wb_lv_value all[2]; all[0] = k; all[1] = v;
    return wb_lv_multi_value(L, all, 2);
}

/* the `pairs` iterator: next, t, nil */
static wb_lv_value iter_next(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    return cfn_next(L, args, n, ud);
}

static wb_lv_value cfn_pairs(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)ud;
    if (n < 1) { wb_lv_error(L, "bad argument #1 to 'pairs' (table expected)"); return wb_lv_nil(); }
    wb_lv_table *m = wb_lv_getmetatable(L, args[0]);
    if (m) {
        wb_lv_value f = wb_lv_table_get(L, m, wb_lv_str("__pairs"));
        if (f.t == WB_LV_FUNCTION || f.t == WB_LV_CFUNCTION) {
            wb_lv_value *out = NULL;
            int nr = wb_lv_call_value(L, f, args, 1, &out);
            return wb_lv_multi_value(L, out, nr);
        }
    }
    wb_lv_value all[3];
    all[0].t = WB_LV_CFUNCTION; all[0].as.cf.f = iter_next; all[0].as.cf.ud = NULL;
    all[1] = args[0];
    all[2] = wb_lv_nil();
    return wb_lv_multi_value(L, all, 3);
}

static wb_lv_value iter_ipairs(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)ud;
    if (n < 2) return wb_lv_nil();
    int64_t i = wb_lv_tointeger(args[1], NULL) + 1;
    wb_lv_value k; k.t = WB_LV_INT; k.as.i = i;
    wb_lv_value v = wb_lv_index(L, args[0], k);
    if (v.t == WB_LV_NIL) return wb_lv_nil();
    wb_lv_value all[2]; all[0] = k; all[1] = v;
    return wb_lv_multi_value(L, all, 2);
}

static wb_lv_value cfn_ipairs(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)ud;
    if (n < 1) { wb_lv_error(L, "bad argument #1 to 'ipairs' (table expected)"); return wb_lv_nil(); }
    wb_lv_value all[3];
    all[0].t = WB_LV_CFUNCTION; all[0].as.cf.f = iter_ipairs; all[0].as.cf.ud = NULL;
    all[1] = args[0];
    all[2] = wb_lv_integer(0);
    return wb_lv_multi_value(L, all, 3);
}

static wb_lv_value cfn_rawget(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)L; (void)ud;
    if (n < 2 || args[0].t != WB_LV_TABLE) return wb_lv_nil();
    return wb_lv_table_get(L, args[0].as.tab, args[1]);
}

static wb_lv_value cfn_rawset(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)ud;
    if (n < 3 || args[0].t != WB_LV_TABLE) return args[0];
    wb_lv_table_set(L, args[0].as.tab, args[1], args[2]);
    return args[0];
}

static wb_lv_value cfn_rawequal(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)L; (void)ud;
    return wb_lv_bool(n >= 2 && wb_lv_rawequal(args[0], args[1]));
}

static wb_lv_value cfn_rawlen(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)ud;
    size_t len = 0;
    if (n >= 1 && (args[0].t == WB_LV_TABLE || args[0].t == WB_LV_STR))
        wb_lv_len(L, args[0], &len);
    return wb_lv_integer((int64_t)len);
}

static wb_lv_value cfn_setmetatable(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)ud;
    if (n < 1 || args[0].t != WB_LV_TABLE) { wb_lv_error(L, "bad argument #1 to 'setmetatable' (table expected)"); return wb_lv_nil(); }
    wb_lv_table *m = (n >= 2 && args[1].t == WB_LV_TABLE) ? args[1].as.tab : NULL;
    wb_lv_setmetatable(L, args[0].as.tab, m);
    return args[0];
}

static wb_lv_value cfn_getmetatable(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)ud;
    if (n < 1) return wb_lv_nil();
    wb_lv_table *m = wb_lv_getmetatable(L, args[0]);
    if (!m) return wb_lv_nil();
    wb_lv_value mt = wb_lv_table_get(L, m, wb_lv_str("__metatable"));
    if (mt.t != WB_LV_NIL) return mt;
    wb_lv_value r; r.t = WB_LV_TABLE; r.as.tab = m;
    return r;
}

static wb_lv_value cfn_require(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)ud;
    if (n < 1 || args[0].t != WB_LV_STR) { wb_lv_error(L, "bad argument #1 to 'require' (string expected)"); return wb_lv_nil(); }
    const char *name = args[0].as.s;
    /* preloaded library modules: require returns the standard global table */
    static const char *mods[] = {"_G", "string", "table", "math", "os", "io",
                                 "coroutine", "utf8", "debug", "package", NULL};
    for (int i = 0; mods[i]; i++) {
        if (!strcmp(name, mods[i])) {
            wb_lv_value g = wb_lv_get_global(L, mods[i]);
            if (g.t != WB_LV_NIL) return g;
        }
    }
    wb_lv_error(L, "module '%s' not found", name);
    return wb_lv_nil();
}

/* `package` table: package.loaded is preloaded with the stdlibs so a later,
 * richer require has a cache to consult. */
static wb_lv_value cfn_pkg_loadlib(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)a; (void)n; (void)ud;
    return wb_lv_nil();
}
static wb_lv_value cfn_pkg_searchpath(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)a; (void)n; (void)ud;
    return wb_lv_nil();
}


static char *lv_readfile(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)sz + 1);
    if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, (size_t)sz, f);
    b[got] = 0; fclose(f);
    if (n) *n = got;
    return b;
}

static wb_lv_value cfn_loadfile(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)ud;
    if (n < 1 || args[0].t != WB_LV_STR) { wb_lv_error(L, "bad argument #1 to 'loadfile' (string expected)"); return wb_lv_nil(); }
    size_t len = 0;
    char *src = lv_readfile(args[0].as.s, &len);
    if (!src) {
        wb_lv_value all[2]; all[0] = wb_lv_nil();
        all[1] = wb_lv_strl(wb_lv_intern(L, "cannot open file", 16), 16);
        return wb_lv_multi_value(L, all, 2);
    }
    if (!L->pool) L->pool = WB_LV_ALLOC(pool, L);
    ex *root = NULL;
    const char *err = wb_lv_parse_driver(L, src, L->pool, &root);
    free(src);
    if (err) {
        wb_lv_value all[2]; all[0] = wb_lv_nil(); all[1] = wb_lv_str(err);
        L->has_err = 0; L->err[0] = 0;
        return wb_lv_multi_value(L, all, 2);
    }
    wb_lv_func *fn = WB_LV_ALLOC(wb_lv_func, L);
    fn->body = root; fn->name = args[0].as.s; fn->defining = NULL;
    fn->nparams = 0; fn->params = NULL;
    wb_lv_value r; r.t = WB_LV_FUNCTION; r.as.fn = fn;
    return r;
}

static wb_lv_value cfn_dofile(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)ud;
    wb_lv_value chunk = cfn_loadfile(L, args, n, ud);
    if (L->has_err) return wb_lv_nil();
    if (chunk.t != WB_LV_FUNCTION) {
        /* load failed: error on the message */
        const char *m = (chunk.t == WB_LV_MULTI && chunk.as.multi && chunk.as.multi->n >= 2)
                        ? wb_lv_tostring_intern(L, chunk.as.multi->v[1]) : "cannot open file";
        wb_lv_error(L, "%s", m);
        wb_lv_value all[2]; all[0] = wb_lv_bool(0); all[1] = wb_lv_str(m);
        return wb_lv_multi_value(L, all, 2);
    }
    wb_lv_value *out = NULL;
    int nr = wb_lv_call_value(L, chunk, NULL, 0, &out);
    if (L->has_err) return wb_lv_nil();
    return wb_lv_multi_value(L, out, nr);
}

static wb_lv_value cfn_load(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)ud;
    if (n < 1) return wb_lv_nil();
    const char *src = NULL;
    if (args[0].t == WB_LV_STR) src = args[0].as.s;
    else {
        /* a function chunk: call it repeatedly and concatenate */
        wb_lv_value f = args[0];
        char *buf = NULL; size_t len = 0, cap = 0;
        for (;;) {
            wb_lv_value *out = NULL;
            int nr = wb_lv_call_value(L, f, NULL, 0, &out);
            if (L->has_err) break;
            if (nr == 0 || out[0].t == WB_LV_NIL) break;
            if (out[0].t != WB_LV_STR) break;
            size_t l = strlen(out[0].as.s);
            if (len + l + 1 > cap) { cap = (len + l + 1) * 2; buf = (char *)realloc(buf, cap); }
            memcpy(buf + len, out[0].as.s, l); len += l;
            if (buf) buf[len] = 0;
        }
        src = buf ? buf : "";
        /* fall through: compile below */
        L->pool = L->pool;   /* no-op */
        wb_lv_value *out = NULL;
        int nr = wb_lv_call_value(L, wb_lv_nil(), NULL, 0, &out);
        (void)nr;
        /* compile */
        if (!L->pool) L->pool = WB_LV_ALLOC(pool, L);
        ex *root = NULL;
        const char *err = wb_lv_parse_driver(L, src, L->pool, &root);
        free(buf);
        if (err) {
            wb_lv_value all[2]; all[0] = wb_lv_nil(); all[1] = wb_lv_str(err);
            L->has_err = 0; L->err[0] = 0;
            return wb_lv_multi_value(L, all, 2);
        }
        wb_lv_func *fn = WB_LV_ALLOC(wb_lv_func, L);
        fn->body = root; fn->name = "=chunk"; fn->defining = NULL;
        fn->nparams = 0; fn->params = NULL;
        wb_lv_value r; r.t = WB_LV_FUNCTION; r.as.fn = fn;
        return r;
    }
    if (!L->pool) L->pool = WB_LV_ALLOC(pool, L);
    ex *root = NULL;
    const char *err = wb_lv_parse_driver(L, src, L->pool, &root);
    if (err) {
        wb_lv_value all[2]; all[0] = wb_lv_nil(); all[1] = wb_lv_str(err);
        L->has_err = 0; L->err[0] = 0;
        return wb_lv_multi_value(L, all, 2);
    }
    wb_lv_func *fn = WB_LV_ALLOC(wb_lv_func, L);
    fn->body = root; fn->name = "=chunk"; fn->defining = NULL;
    fn->nparams = 0; fn->params = NULL;
    wb_lv_value r; r.t = WB_LV_FUNCTION; r.as.fn = fn;
    return r;
}

static wb_lv_value cfn_unimplemented(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)args; (void)n;
    wb_lv_error(L, "%s is not implemented in slerm-Lua", (const char *)ud);
    return wb_lv_nil();
}

/* ------------------------------------------------------------ registration */

void wb_lv_register_builtins(wb_lv *L) {
    wb_lv_register_global(L, "print", cfn_print, NULL);
    wb_lv_register_global(L, "type", cfn_type, NULL);
    wb_lv_register_global(L, "tostring", cfn_tostring, NULL);
    wb_lv_register_global(L, "tonumber", cfn_tonumber, NULL);
    wb_lv_register_global(L, "assert", cfn_assert, NULL);
    wb_lv_register_global(L, "error", cfn_error, NULL);
    wb_lv_register_global(L, "pcall", cfn_pcall, NULL);
    wb_lv_register_global(L, "xpcall", cfn_xpcall, NULL);
    wb_lv_register_global(L, "select", cfn_select, NULL);
    wb_lv_register_global(L, "collectgarbage", cfn_collectgarbage, NULL);
    wb_lv_register_global(L, "next", cfn_next, NULL);
    wb_lv_register_global(L, "pairs", cfn_pairs, NULL);
    wb_lv_register_global(L, "ipairs", cfn_ipairs, NULL);
    wb_lv_register_global(L, "rawget", cfn_rawget, NULL);
    wb_lv_register_global(L, "rawset", cfn_rawset, NULL);
    wb_lv_register_global(L, "rawequal", cfn_rawequal, NULL);
    wb_lv_register_global(L, "rawlen", cfn_rawlen, NULL);
    wb_lv_register_global(L, "setmetatable", cfn_setmetatable, NULL);
    wb_lv_register_global(L, "getmetatable", cfn_getmetatable, NULL);
    wb_lv_register_global(L, "load", cfn_load, NULL);
    wb_lv_register_global(L, "require", cfn_require, NULL);

    /* package table with a loaded cache bound to the (already-registered) libs */
    {
        wb_lv_value pkg;
        pkg.t = WB_LV_TABLE; pkg.as.tab = wb_lv_table_new(L, 0, 4);
        wb_lv_value loaded;
        loaded.t = WB_LV_TABLE; loaded.as.tab = wb_lv_table_new(L, 0, 12);
        static const char *mods[] = {"string","table","math","os","io","coroutine","utf8","debug","_G",NULL};
        for (int i = 0; mods[i]; i++) {
            wb_lv_value g = wb_lv_get_global(L, mods[i]);
            if (g.t != WB_LV_NIL) wb_lv_table_set(L, loaded.as.tab, wb_lv_str(mods[i]), g);
        }
        wb_lv_table_set(L, pkg.as.tab, wb_lv_str("loaded"), loaded);
        wb_lv_value v; v.t = WB_LV_CFUNCTION; v.as.cf.f = cfn_pkg_loadlib; v.as.cf.ud = NULL;
        wb_lv_table_set(L, pkg.as.tab, wb_lv_str("loadlib"), v);
        v.as.cf.f = cfn_pkg_searchpath;
        wb_lv_table_set(L, pkg.as.tab, wb_lv_str("searchpath"), v);
        wb_lv_table_set(L, pkg.as.tab, wb_lv_str("path"), wb_lv_str("?.lua"));
        wb_lv_table_set(L, pkg.as.tab, wb_lv_str("cpath"), wb_lv_str(""));
        wb_lv_value pv; pv.t = WB_LV_TABLE; pv.as.tab = pkg.as.tab;
        wb_lv_set_global(L, "package", pv);
    }

    /* _VERSION */
    wb_lv_set_global(L, "_VERSION", wb_lv_str("Lua 5.4"));

    /* _G: the globals table, self-referential */
    wb_lv_value g; g.t = WB_LV_TABLE; g.as.tab = L->globals;
    wb_lv_table_set(L, L->globals, wb_lv_str("_G"), g);

    /* dofile / loadfile: load and run a script from the real filesystem.
     * require is registered for real above (a package table + preloaded libs). */
    { /* loadfile -> a function the official tests call */
        wb_lv_value lf; lf.t = WB_LV_CFUNCTION; lf.as.cf.f = cfn_loadfile; lf.as.cf.ud = NULL;
        wb_lv_set_global(L, "loadfile", lf);
        wb_lv_value df; df.t = WB_LV_CFUNCTION; df.as.cf.f = cfn_dofile; df.as.cf.ud = NULL;
        wb_lv_set_global(L, "dofile", df);
    }
}

void wb_lv_openlibs(wb_lv *L) {
    wb_lv_register_builtins(L);
    wb_lv_register_string(L);
    wb_lv_register_table(L);
    wb_lv_register_math(L);
    wb_lv_register_os(L);
    wb_lv_register_io(L);
    wb_lv_register_debug(L);
    wb_lv_register_utf8(L);
    wb_lv_register_coro(L);
    /* `wua` -- Wua feature table. A script can feature-detect every way Wua is
     * more than plain Lua (the "a wua is our improvements" contract). Every
     * key is an intentional divergence or an additive extension; scripts that
     * must stay portable to stock Lua 5.4 should test these before relying on
     * them. See docs/math.md for the full wording. */
    {
        wb_lv_table *t = wb_lv_table_new(L, 0, 8);
        wb_lv_table_set(L, t, wb_lv_str("version"), wb_lv_str("wua-1.0"));
        wb_lv_table_set(L, t, wb_lv_str("lua_standard"), wb_lv_str("5.4"));
        /* intentional divergence: math.random is reproducible across platforms
         * (splitmix64), unlike stock Lua's libc rand() */
        wb_lv_table_set(L, t, wb_lv_str("deterministic_random"), wb_lv_bool(1));
        /* additive math extensions not present in Lua 5.4 */
        static const char *ex[] = { "math.clamp", "math.lerp", "math.sign",
                                    "math.round", "math.map", "math.between",
                                    NULL };
        wb_lv_table *el = wb_lv_table_new(L, 6, 0);
        int k = 1;
        for (const char **p = ex; *p; p++, k++)
            wb_lv_table_set(L, el, wb_lv_integer(k), wb_lv_str(*p));
        wb_lv_value ev; ev.t = WB_LV_TABLE; ev.as.tab = el;
        wb_lv_table_set(L, t, wb_lv_str("extensions"), ev);
        wb_lv_value tv; tv.t = WB_LV_TABLE; tv.as.tab = t;
        wb_lv_set_global(L, "wua", tv);
    }
}
