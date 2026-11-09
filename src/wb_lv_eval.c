/*
 * wb_lv_eval.c -- the tree-walking evaluator: scopes, calls, operators with
 * metamethod dispatch, multiple returns, varargs, and error isolation.
 * See wb_lv_internal.h.
 */
#include "wb_lv_internal.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- errors */

void wb_lv_set_err(wb_lv *L, const char *fmt, ...) {
    if (!L || L->has_err) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(L->err, sizeof L->err, fmt, ap);
    va_end(ap);
    L->has_err = 1;
}

/* Raise. If a pcall catch point is active, unwind to it; otherwise just mark
 * the state and let the eval loop stop at the next check. */
void wb_lv_error(wb_lv *L, const char *fmt, ...) {
    if (!L->has_err) {
        va_list ap; va_start(ap, fmt);
        vsnprintf(L->err, sizeof L->err, fmt, ap);
        va_end(ap);
        L->has_err = 1;
    }
    if (L->njmp > 0) longjmp(L->jmp[L->njmp - 1], 1);
}

jmp_buf *wb_lv_push_catch(wb_lv *L) {
    jmp_buf *n = (jmp_buf *)realloc(L->jmp, (size_t)(L->njmp + 1) * sizeof(jmp_buf));
    if (!n) return NULL;
    L->jmp = n;
    return &L->jmp[L->njmp++];
}
void wb_lv_pop_catch(wb_lv *L) { if (L->njmp > 0) L->njmp--; }

/* --------------------------------------------------------------- scopes */

static void env_set(wb_lv *L, env *e, const char *name, wb_lv_value v) {
    if (!e) return;
    wb_lv_table_set(L, e->t, wb_lv_str(name), v);
}

static wb_lv_value env_get(wb_lv *L, env *e, const char *name) {
    for (env *c = e; c; c = c->parent) {
        wb_lv_value r = wb_lv_table_get(L, c->t, wb_lv_str(name));
        if (r.t != WB_LV_NIL) return r;
    }
    return wb_lv_table_get(L, L->globals, wb_lv_str(name));
}

static void env_assign(wb_lv *L, env *e, const char *name, wb_lv_value v) {
    for (env *c = e; c; c = c->parent) {
        wb_lv_value r = wb_lv_table_get(L, c->t, wb_lv_str(name));
        if (r.t != WB_LV_NIL) { env_set(L, c, name, v); return; }
    }
    wb_lv_table_set(L, L->globals, wb_lv_str(name), v);
}

static env *new_env(wb_lv *L, env *parent) {
    env *e = WB_LV_ALLOC(env, L);
    if (!e) return NULL;
    e->t = wb_lv_table_new(L, 0, 8);
    e->parent = parent;
    return e;
}

/* ---------------------------------------------------------- value stack */

static void stk_push(wb_lv *L, wb_lv_value v) {
    if (L->sp >= L->scap) {
        int nc = L->scap ? L->scap * 2 : 64;
        wb_lv_value *n = (wb_lv_value *)wb_lv_arena_alloc(L, (size_t)nc * sizeof(*n));
        memcpy(n, L->stk, (size_t)L->sp * sizeof(*n));
        L->stk = n; L->scap = nc;
    }
    L->stk[L->sp++] = v;
}

/* Build a WB_LV_MULTI value from an array (arena-allocated). */
wb_lv_value wb_lv_multi_value(wb_lv *L, wb_lv_value *v, int n) {
    if (n <= 0) return wb_lv_nil();
    struct wb_lv_multi *m = (struct wb_lv_multi *)
        wb_lv_arena_alloc(L, sizeof(struct wb_lv_multi) + (size_t)n * sizeof(wb_lv_value));
    m->n = n;
    if (v) memcpy(m->v, v, (size_t)n * sizeof(wb_lv_value));
    wb_lv_value r; r.t = WB_LV_MULTI; r.as.multi = m;
    return r;
}

/* ------------------------------------------------------------- operators */

static int64_t to_int(wb_lv *L, wb_lv_value v, int *ok) {
    if (v.t == WB_LV_INT) { if (ok) *ok = 1; return v.as.i; }
    if (v.t == WB_LV_NUM) {
        if (v.as.n >= -9.2233720368547758e18 && v.as.n < 9.2233720368547758e18) {
            if (ok) *ok = 1; return (int64_t)v.as.n;
        }
    }
    if (ok) *ok = 0;
    return 0;
}

static const char *arith_event(int op) {
    switch (op) {
    case '+': return "__add";
    case '-': return "__sub";
    case '*': return "__mul";
    case '/': return "__div";
    case T_IDIV: return "__idiv";
    case '%': return "__mod";
    case '^': return "__pow";
    case '&': return "__band";
    case '|': return "__bor";
    case '~': return "__bxor";
    case T_SHL: return "__shl";
    case T_SHR: return "__shr";
    }
    return NULL;
}

static wb_lv_value call_meta(wb_lv *L, wb_lv_value f, wb_lv_value a, wb_lv_value b) {
    wb_lv_value args[2]; args[0] = a; args[1] = b;
    return wb_lv_call(L, f, args, 2);
}

/* Lua coerces numeric strings in arithmetic. Returns 1 when `out` is usable. */
static int coerce_num(wb_lv_value v, wb_lv_value *out) {
    if (v.t == WB_LV_INT || v.t == WB_LV_NUM) { *out = v; return 1; }
    if (v.t != WB_LV_STR) return 0;
    const char *str = v.as.s;
    char *end = NULL;
    double d = strtod(str, &end);
    if (end == str) return 0;
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') end++;
    if (*end != 0) return 0;
    /* a plain integer literal becomes an integer */
    const char *q = str;
    while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
    if (*q == '+' || *q == '-') q++;
    int alldig = (*q != 0);
    for (const char *r = q; *r; r++) if (*r < '0' || *r > '9') { alldig = 0; break; }
    if (alldig) {
        errno = 0;
        long long iv = strtoll(str, NULL, 10);
        *out = wb_lv_integer((int64_t)iv);
        return 1;
    }
    *out = wb_lv_number(d);
    return 1;
}

wb_lv_value wb_lv_arith(wb_lv *L, int op, wb_lv_value a, wb_lv_value b) {
    /* coerce numeric strings, as Lua does */
    {
        wb_lv_value ca, cb;
        int an = coerce_num(a, &ca), bn = coerce_num(b, &cb);
        if (an && bn) { a = ca; b = cb; }
    }

    /* metamethods first, when neither operand is a plain number */
    if (!(a.t == WB_LV_INT || a.t == WB_LV_NUM) || !(b.t == WB_LV_INT || b.t == WB_LV_NUM)) {
        const char *ev = arith_event(op);
        if (ev) {
            wb_lv_table *ma = wb_lv_getmetatable(L, a);
            wb_lv_table *mb = wb_lv_getmetatable(L, b);
            wb_lv_value f = ma ? wb_lv_table_get(L, ma, wb_lv_str(ev)) : wb_lv_nil();
            if (f.t == WB_LV_NIL && mb) f = wb_lv_table_get(L, mb, wb_lv_str(ev));
            if (f.t == WB_LV_FUNCTION || f.t == WB_LV_CFUNCTION) return call_meta(L, f, a, b);
        }
        wb_lv_error(L, "attempt to perform arithmetic on a %s value (line %d)",
                    wb_lv_type_name((a.t == WB_LV_INT || a.t == WB_LV_NUM) ? b.t : a.t),
                    L->curline);
        return wb_lv_nil();
    }

    int ai = (a.t == WB_LV_INT), bi = (b.t == WB_LV_INT);

    /* bitwise operations are integer-only */
    switch (op) {
    case '&': case '|': case '~': case T_SHL: case T_SHR: {
        int ok1, ok2;
        int64_t x = to_int(L, a, &ok1), y = to_int(L, b, &ok2);
        if (!ok1 || !ok2) {
            wb_lv_error(L, "number has no integer representation (line %d)", L->curline);
            return wb_lv_nil();
        }
        int64_t r = 0;
        switch (op) {
        case '&': r = x & y; break;
        case '|': r = x | y; break;
        case '~': r = x ^ y; break;
        case T_SHL: r = (y >= 0 && y < 64) ? (int64_t)((uint64_t)x << y) : (y >= 64 ? 0 : (int64_t)((uint64_t)x >> -y)); break;
        case T_SHR: r = (y >= 0 && y < 64) ? (int64_t)((uint64_t)x >> y) : (y >= 64 ? 0 : (int64_t)((uint64_t)x << -y)); break;
        }
        return wb_lv_integer(r);
    }
    }

    if (ai && bi) {
        int64_t x = a.as.i, y = b.as.i, r = 0;
        switch (op) {
        case '+': r = (int64_t)((uint64_t)x + (uint64_t)y); break;
        case '-': r = (int64_t)((uint64_t)x - (uint64_t)y); break;
        case '*': r = (int64_t)((uint64_t)x * (uint64_t)y); break;
        case '/': return wb_lv_number((double)x / (double)y);
        case T_IDIV:
            if (y == 0) { wb_lv_error(L, "attempt to divide by zero (line %d)", L->curline); return wb_lv_nil(); }
            r = x / y;
            if ((x % y != 0) && ((x < 0) != (y < 0))) r--;
            return wb_lv_integer(r);
        case '%':
            if (y == 0) { wb_lv_error(L, "attempt to perform 'n%%0' (line %d)", L->curline); return wb_lv_nil(); }
            r = x % y;
            if (r != 0 && ((r < 0) != (y < 0))) r += y;
            return wb_lv_integer(r);
        case '^': return wb_lv_number(pow((double)x, (double)y));
        }
        return wb_lv_integer(r);
    }

    double x = (ai) ? (double)a.as.i : a.as.n;
    double y = (bi) ? (double)b.as.i : b.as.n;
    switch (op) {
    case '+': return wb_lv_number(x + y);
    case '-': return wb_lv_number(x - y);
    case '*': return wb_lv_number(x * y);
    case '/': return wb_lv_number(x / y);
    case '^': return wb_lv_number(pow(x, y));
    case T_IDIV: {
        double r = floor(x / y);
        return wb_lv_number(r);
    }
    case '%': {
        double r = x - floor(x / y) * y;
        return wb_lv_number(r);
    }
    }
    return wb_lv_nil();
}

wb_lv_value wb_lv_compare(wb_lv *L, int op, wb_lv_value a, wb_lv_value b) {
    /* equality is defined across numeric types */
    if (op == T_EQ || op == T_NE) {
        int eq;
        if ((a.t == WB_LV_INT || a.t == WB_LV_NUM) && (b.t == WB_LV_INT || b.t == WB_LV_NUM)) {
            double x = (a.t == WB_LV_INT) ? (double)a.as.i : a.as.n;
            double y = (b.t == WB_LV_INT) ? (double)b.as.i : b.as.n;
            eq = (x == y);
        } else eq = wb_lv_equal(L, a, b);
        return wb_lv_bool(op == T_EQ ? eq : !eq);
    }

    /* ordering */
    if (a.t == WB_LV_STR && b.t == WB_LV_STR) {
        int c = strcmp(a.as.s, b.as.s);
        switch (op) {
        case '<': return wb_lv_bool(c < 0);
        case '>': return wb_lv_bool(c > 0);
        case T_LE: return wb_lv_bool(c <= 0);
        case T_GE: return wb_lv_bool(c >= 0);
        }
    }
    if ((a.t == WB_LV_INT || a.t == WB_LV_NUM) && (b.t == WB_LV_INT || b.t == WB_LV_NUM)) {
        double x = (a.t == WB_LV_INT) ? (double)a.as.i : a.as.n;
        double y = (b.t == WB_LV_INT) ? (double)b.as.i : b.as.n;
        switch (op) {
        case '<': return wb_lv_bool(x < y);
        case '>': return wb_lv_bool(x > y);
        case T_LE: return wb_lv_bool(x <= y);
        case T_GE: return wb_lv_bool(x >= y);
        }
    }
    /* metamethods __lt / __le. `a > b` is `b < a`, `a >= b` is `b <= a`. */
    const char *ev = (op == '<' || op == '>') ? "__lt" : "__le";
    wb_lv_value x = a, y = b;
    if (op == '>' || op == T_GE) { x = b; y = a; }
    wb_lv_table *mx = wb_lv_getmetatable(L, x);
    wb_lv_table *my = wb_lv_getmetatable(L, y);
    wb_lv_value f = mx ? wb_lv_table_get(L, mx, wb_lv_str(ev)) : wb_lv_nil();
    if (f.t == WB_LV_NIL && my) f = wb_lv_table_get(L, my, wb_lv_str(ev));
    if (f.t == WB_LV_FUNCTION || f.t == WB_LV_CFUNCTION) {
        wb_lv_value r = call_meta(L, f, x, y);
        return wb_lv_bool(wb_lv_truthy(r));
    }
    wb_lv_error(L, "attempt to compare %s with %s (line %d)",
                wb_lv_type_name(a.t), wb_lv_type_name(b.t), L->curline);
    return wb_lv_nil();
}

int wb_lv_len(wb_lv *L, wb_lv_value v, size_t *out) {
    if (v.t == WB_LV_STR) { *out = strlen(v.as.s); return 1; }
    if (v.t == WB_LV_TABLE) {
        wb_lv_table *m = wb_lv_getmetatable(L, v);
        wb_lv_value f = m ? wb_lv_table_get(L, m, wb_lv_str("__len")) : wb_lv_nil();
        if (f.t == WB_LV_FUNCTION || f.t == WB_LV_CFUNCTION) {
            wb_lv_value r = wb_lv_call(L, f, &v, 1);
            *out = (size_t)wb_lv_tonumber(r, NULL);
            return 1;
        }
        *out = wb_lv_table_len(v.as.tab);
        return 1;
    }
    wb_lv_error(L, "attempt to get length of a %s value (line %d)", wb_lv_type_name(v.t), L->curline);
    return 0;
}

wb_lv_value wb_lv_concat(wb_lv *L, wb_lv_value a, wb_lv_value b) {
    int an = (a.t == WB_LV_STR || a.t == WB_LV_INT || a.t == WB_LV_NUM);
    int bn = (b.t == WB_LV_STR || b.t == WB_LV_INT || b.t == WB_LV_NUM);
    if (an && bn) {
        const char *sa = wb_lv_tostring_intern(L, a);
        const char *sb = wb_lv_tostring_intern(L, b);
        size_t la = strlen(sa), lb = strlen(sb);
        char *buf = (char *)malloc(la + lb + 1);
        if (!buf) { wb_lv_error(L, "out of memory"); return wb_lv_nil(); }
        memcpy(buf, sa, la); memcpy(buf + la, sb, lb); buf[la + lb] = 0;
        const char *r = wb_lv_intern(L, buf, la + lb);
        free(buf);
        return wb_lv_str(r);
    }
    wb_lv_table *ma = wb_lv_getmetatable(L, a);
    wb_lv_table *mb = wb_lv_getmetatable(L, b);
    wb_lv_value f = ma ? wb_lv_table_get(L, ma, wb_lv_str("__concat")) : wb_lv_nil();
    if (f.t == WB_LV_NIL && mb) f = wb_lv_table_get(L, mb, wb_lv_str("__concat"));
    if (f.t == WB_LV_FUNCTION || f.t == WB_LV_CFUNCTION) return call_meta(L, f, a, b);
    wb_lv_error(L, "attempt to concatenate a %s value (line %d)",
                wb_lv_type_name(an ? b.t : a.t), L->curline);
    return wb_lv_nil();
}

/* ------------------------------------------------------------- indexing */

wb_lv_value wb_lv_index(wb_lv *L, wb_lv_value obj, wb_lv_value key) {
    for (int loop = 0; loop < 100; loop++) {
        if (obj.t == WB_LV_TABLE) {
            wb_lv_value v = wb_lv_table_get(L, obj.as.tab, key);
            if (v.t != WB_LV_NIL) return v;
            wb_lv_table *m = obj.as.tab->meta;
            if (!m) return wb_lv_nil();
            wb_lv_value h = wb_lv_table_get(L, m, wb_lv_str("__index"));
            if (h.t == WB_LV_NIL) return wb_lv_nil();
            if (h.t == WB_LV_FUNCTION || h.t == WB_LV_CFUNCTION) {
                wb_lv_value args[2]; args[0] = obj; args[1] = key;
                return wb_lv_call(L, h, args, 2);
            }
            obj = h;                     /* __index is a table: repeat */
            continue;
        }
        if (obj.t == WB_LV_STR) {
            /* string methods live on the string metatable */
            wb_lv_table *m = L->string_meta;
            if (m) {
                wb_lv_value v = wb_lv_table_get(L, m, key);
                if (v.t != WB_LV_NIL) return v;
            }
            wb_lv_error(L, "attempt to index a string value (line %d)", L->curline);
            return wb_lv_nil();
        }
        wb_lv_table *m = wb_lv_getmetatable(L, obj);
        if (m) {
            wb_lv_value h = wb_lv_table_get(L, m, wb_lv_str("__index"));
            if (h.t == WB_LV_FUNCTION || h.t == WB_LV_CFUNCTION) {
                wb_lv_value args[2]; args[0] = obj; args[1] = key;
                return wb_lv_call(L, h, args, 2);
            }
        }
        wb_lv_error(L, "attempt to index a %s value (line %d)", wb_lv_type_name(obj.t), L->curline);
        return wb_lv_nil();
    }
    wb_lv_error(L, "'__index' chain too long; possible loop (line %d)", L->curline);
    return wb_lv_nil();
}

void wb_lv_setindex(wb_lv *L, wb_lv_value obj, wb_lv_value key, wb_lv_value v) {
    for (int loop = 0; loop < 100; loop++) {
        if (obj.t == WB_LV_TABLE) {
            wb_lv_table *m = obj.as.tab->meta;
            if (m) {
                wb_lv_value h = wb_lv_table_get(L, m, wb_lv_str("__newindex"));
                if (h.t == WB_LV_FUNCTION || h.t == WB_LV_CFUNCTION) {
                    wb_lv_value args[3]; args[0] = obj; args[1] = key; args[2] = v;
                    wb_lv_call(L, h, args, 3);
                    return;
                }
                if (h.t == WB_LV_TABLE) { obj = h; continue; }
            }
            wb_lv_table_set(L, obj.as.tab, key, v);
            return;
        }
        wb_lv_error(L, "attempt to index a %s value (line %d)", wb_lv_type_name(obj.t), L->curline);
        return;
    }
}

/* ---------------------------------------------------------------- eval */

static wb_lv_value eval_impl(wb_lv *L, ex *e, env *en);
static wb_lv_value exec(wb_lv *L, ex *e, env *en, int *was_return);
static wb_lv_value exec(wb_lv *L, ex *e, env *en, int *was_return) {
    return wb_lv_exec(L, e, en, was_return);
}

static wb_lv_value eval(wb_lv *L, ex *e, env *en) {
    if (!e) return wb_lv_nil();
    if (L->eval_depth >= WB_LV_MAX_EVAL_DEPTH) {
        wb_lv_error(L, "expression nested too deeply (limit %d)", WB_LV_MAX_EVAL_DEPTH);
        return wb_lv_nil();
    }
    int save = L->curline;
    L->curline = e->line;
    L->eval_depth++;
    wb_lv_value r = eval_impl(L, e, en);
    L->eval_depth--;
    L->curline = save;
    return r;
}

/* call a function value. Results are copied into an arena buffer and left to
 * the caller; returns the result count. */
int wb_lv_call_value(wb_lv *L, wb_lv_value fn, wb_lv_value *args, int nargs, wb_lv_value **out) {
    *out = NULL;
    if (fn.t == WB_LV_CFUNCTION) {
        wb_lv_value r = fn.as.cf.f(L, args, nargs, fn.as.cf.ud);
        if (L->has_err) return 0;
        if (r.t == WB_LV_MULTI && r.as.multi) {
            int n = r.as.multi->n;
            if (n <= 0) return 0;
            wb_lv_value *res = WB_LV_ALLOCN(wb_lv_value, n, L);
            memcpy(res, r.as.multi->v, (size_t)n * sizeof(*res));
            *out = res; return n;
        }
        wb_lv_value *res = WB_LV_ALLOCN(wb_lv_value, 1, L);
        res[0] = r;
        *out = res; return 1;
    }
    if (fn.t == WB_LV_FUNCTION) {
        int base = L->sp;
        int n = wb_lv_call_lua_results(L, fn.as.fn, args, nargs);
        if (L->has_err) { L->sp = base; return 0; }
        if (n <= 0) { L->sp = base; return 0; }
        wb_lv_value *res = WB_LV_ALLOCN(wb_lv_value, n, L);
        memcpy(res, L->stk + base, (size_t)n * sizeof(*res));
        L->sp = base;
        *out = res; return n;
    }
    /* __call metamethod */
    wb_lv_table *m = wb_lv_getmetatable(L, fn);
    if (m) {
        wb_lv_value h = wb_lv_table_get(L, m, wb_lv_str("__call"));
        if (h.t == WB_LV_FUNCTION || h.t == WB_LV_CFUNCTION) {
            wb_lv_value *a2 = (wb_lv_value *)malloc((size_t)(nargs + 1) * sizeof(*a2));
            if (!a2) { wb_lv_error(L, "out of memory"); return 0; }
            a2[0] = fn;
            if (nargs) memcpy(a2 + 1, args, (size_t)nargs * sizeof(*a2));
            int r = wb_lv_call_value(L, h, a2, nargs + 1, out);
            free(a2);
            return r;
        }
    }
    wb_lv_error(L, "attempt to call a %s value (line %d)", wb_lv_type_name(fn.t), L->curline);
    return 0;
}

/* growable argument vector for calls */
static wb_lv_value *args_push(wb_lv_value *a, int *n, int *cap, wb_lv_value v) {
    if (*n >= *cap) {
        *cap = *cap ? *cap * 2 : 8;
        a = (wb_lv_value *)realloc(a, (size_t)(*cap) * sizeof(*a));
    }
    a[(*n)++] = v;
    return a;
}

wb_lv_value wb_lv_eval_multi(wb_lv *L, ex *e, env *en, wb_lv_value **out, int *nout) {
    *out = NULL; *nout = 0;
    if (!e) return wb_lv_nil();

    if (e->t == N_CALL || e->t == N_METHOD) {
        wb_lv_value callee;
        wb_lv_value *args = NULL; int na = 0, acap = 0;

        if (e->t == N_METHOD) {
            wb_lv_value self = eval(L, e->e1->e1, en);
            if (L->has_err) return wb_lv_nil();
            wb_lv_value key = (e->e1->e2 && e->e1->e2->t == N_STR)
                                  ? wb_lv_str(e->e1->e2->str) : wb_lv_nil();
            callee = wb_lv_index(L, self, key);
            if (L->has_err) { free(args); return wb_lv_nil(); }
            args = args_push(args, &na, &acap, self);      /* implicit self */
            for (int i = 0; i < e->nlist; i++) {
                if (i == e->nlist - 1) {
                    wb_lv_value *o2 = NULL; int n2 = 0;
                    wb_lv_eval_multi(L, e->list[i], en, &o2, &n2);
                    if (L->has_err) { free(args); return wb_lv_nil(); }
                    for (int k = 0; k < n2; k++) args = args_push(args, &na, &acap, o2[k]);
                    if (n2 == 0) args = args_push(args, &na, &acap, wb_lv_nil());
                } else {
                    wb_lv_value v = eval(L, e->list[i], en);
                    if (L->has_err) { free(args); return wb_lv_nil(); }
                    args = args_push(args, &na, &acap, v);
                }
            }
        } else {
            callee = eval(L, e->e1, en);
            if (L->has_err) { free(args); return wb_lv_nil(); }
            for (int i = 0; i < e->nlist; i++) {
                if (i == e->nlist - 1) {
                    /* the last argument expands to all of its values */
                    wb_lv_value *o2 = NULL; int n2 = 0;
                    wb_lv_eval_multi(L, e->list[i], en, &o2, &n2);
                    if (L->has_err) { free(args); return wb_lv_nil(); }
                    for (int k = 0; k < n2; k++) args = args_push(args, &na, &acap, o2[k]);
                    if (n2 == 0) args = args_push(args, &na, &acap, wb_lv_nil());
                } else {
                    wb_lv_value v = eval(L, e->list[i], en);
                    if (L->has_err) { free(args); return wb_lv_nil(); }
                    args = args_push(args, &na, &acap, v);
                }
            }
        }
        int nr = wb_lv_call_value(L, callee, args, na, out);
        free(args);
        if (L->has_err) return wb_lv_nil();
        *nout = nr;
        return (nr > 0) ? (*out)[0] : wb_lv_nil();
    }

    if (e->t == N_VARARG) {
        *out = L->va; *nout = L->nva;
        return (L->nva > 0) ? L->va[0] : wb_lv_nil();
    }

    wb_lv_value v = eval(L, e, en);
    *nout = 1;
    /* Arena slot, NOT the value stack: stk_push may realloc L->stk and
     * invalidate a pointer into it. */
    wb_lv_value *slot = WB_LV_ALLOC(wb_lv_value, L);
    if (slot) *slot = v;
    *out = slot;
    return v;
}

static wb_lv_value eval_impl(wb_lv *L, ex *e, env *en) {
    if (!e) return wb_lv_nil();
    switch (e->t) {
    case N_CALL: case N_METHOD: {
        /* a call used as a VALUE: take its first result. Statement position
         * goes through exec(), which discards it. */
        wb_lv_value *out = NULL; int no = 0;
        wb_lv_eval_multi(L, e, en, &out, &no);
        return (no > 0) ? out[0] : wb_lv_nil();
    }
    case N_NIL: return wb_lv_nil();
    case N_TRUE: return wb_lv_bool(1);
    case N_FALSE: return wb_lv_bool(0);
    case N_INT: return wb_lv_integer(e->i);
    case N_NUM: return wb_lv_number(e->num);
    case N_STR: return wb_lv_str(e->str);
    case N_NAME: return env_get(L, en, e->name);
    case N_PAREN: {
        /* parenthesised: exactly one value */
        if (e->e1 && (e->e1->t == N_CALL || e->e1->t == N_METHOD || e->e1->t == N_VARARG)) {
            wb_lv_value *out = NULL; int n = 0;
            wb_lv_eval_multi(L, e->e1, en, &out, &n);
            return (n > 0) ? out[0] : wb_lv_nil();
        }
        return eval(L, e->e1, en);
    }
    case N_INDEX: {
        wb_lv_value obj = eval(L, e->e1, en);
        wb_lv_value key = eval(L, e->e2, en);
        if (L->has_err) return wb_lv_nil();
        return wb_lv_index(L, obj, key);
    }
    case N_AND: {
        wb_lv_value a = eval(L, e->e1, en);
        if (!wb_lv_truthy(a)) return a;
        return eval(L, e->e2, en);
    }
    case N_OR: {
        wb_lv_value a = eval(L, e->e1, en);
        if (wb_lv_truthy(a)) return a;
        return eval(L, e->e2, en);
    }
    case N_UNARY: {
        wb_lv_value v = eval(L, e->e1, en);
        if (L->has_err) return wb_lv_nil();
        switch (e->op) {
        case T_NOT: return wb_lv_bool(!wb_lv_truthy(v));
        case '-':
            if (v.t == WB_LV_INT) return wb_lv_integer((int64_t)(0 - (uint64_t)v.as.i));
            if (v.t == WB_LV_NUM) return wb_lv_number(-v.as.n);
            {
                wb_lv_value cv;
                if (coerce_num(v, &cv)) {
                    if (cv.t == WB_LV_INT) return wb_lv_integer((int64_t)(0 - (uint64_t)cv.as.i));
                    return wb_lv_number(-cv.as.n);
                }
            }
            /* __unm */
            {
                wb_lv_table *m = wb_lv_getmetatable(L, v);
                if (m) {
                    wb_lv_value f = wb_lv_table_get(L, m, wb_lv_str("__unm"));
                    if (f.t == WB_LV_FUNCTION || f.t == WB_LV_CFUNCTION) return call_meta(L, f, v, v);
                }
            }
            wb_lv_error(L, "attempt to perform arithmetic on a %s value (line %d)", wb_lv_type_name(v.t), L->curline);
            return wb_lv_nil();
        case '~':
            if (v.t == WB_LV_INT) return wb_lv_integer(~v.as.i);
            if (v.t == WB_LV_NUM) {
                int ok; int64_t x = to_int(L, v, &ok);
                if (!ok) { wb_lv_error(L, "number has no integer representation (line %d)", L->curline); return wb_lv_nil(); }
                return wb_lv_integer(~x);
            }
            wb_lv_error(L, "attempt to perform bitwise operation on a %s value (line %d)", wb_lv_type_name(v.t), L->curline);
            return wb_lv_nil();
        case '#': {
            size_t n;
            if (!wb_lv_len(L, v, &n)) return wb_lv_nil();
            return wb_lv_integer((int64_t)n);
        }
        }
        return wb_lv_nil();
    }
    case N_BINOP: {
        wb_lv_value a = eval(L, e->e1, en);
        if (L->has_err) return wb_lv_nil();
        wb_lv_value b = eval(L, e->e2, en);
        if (L->has_err) return wb_lv_nil();
        if (e->op == T_CONCAT) return wb_lv_concat(L, a, b);
        if (e->op == T_EQ || e->op == T_NE || e->op == '<' || e->op == '>' ||
            e->op == T_LE || e->op == T_GE)
            return wb_lv_compare(L, e->op, a, b);
        return wb_lv_arith(L, e->op, a, b);
    }
    case N_TABLE: {
        wb_lv_table *t = wb_lv_table_new(L, e->nlist, e->nlist);
        for (int i = 0; i < e->nlist; i++) {
            if (e->keys[i]) {
                wb_lv_value k = eval(L, e->keys[i], en);
                wb_lv_value v = eval(L, e->list[i], en);
                if (L->has_err) return wb_lv_nil();
                wb_lv_table_set(L, t, k, v);
            } else if (i == e->nlist - 1 && (e->list[i]->t == N_CALL || e->list[i]->t == N_METHOD || e->list[i]->t == N_VARARG)) {
                wb_lv_value *out = NULL; int n = 0;
                wb_lv_eval_multi(L, e->list[i], en, &out, &n);
                for (int j = 0; j < n; j++) {
                    wb_lv_value k; k.t = WB_LV_INT; k.as.i = (int64_t)(i + j + 1);
                    wb_lv_table_set(L, t, k, out[j]);
                }
            } else {
                wb_lv_value v = eval(L, e->list[i], en);
                if (L->has_err) return wb_lv_nil();
                wb_lv_value k; k.t = WB_LV_INT; k.as.i = (int64_t)(i + 1);
                wb_lv_table_set(L, t, k, v);
            }
        }
        wb_lv_value r; r.t = WB_LV_TABLE; r.as.tab = t;
        return r;
    }
    case N_FUNC: {
        wb_lv_func *fn = WB_LV_ALLOC(wb_lv_func, L);
        if (!fn) { wb_lv_error(L, "out of memory"); return wb_lv_nil(); }
        fn->body = e->e1; fn->name = e->name; fn->defining = en;
        fn->nparams = e->nlist;
        fn->params = e->names;
        fn->varargs = NULL; fn->nvarargs = 0;
        wb_lv_value r; r.t = WB_LV_FUNCTION; r.as.fn = fn;
        return r;
    }
    default:
        /* statements and calls reach here only when used as expressions */
        return exec(L, e, en, &(int){0});
    }
}

/* ------------------------------------------------------- function calls */

/* Run a Lua function body. Results are PUSHED onto the value stack (the
 * caller reads L->stk[base..sp) and rewinds); returns the result count. */
int wb_lv_call_lua_results(wb_lv *L, wb_lv_func *fn, wb_lv_value *args, int nargs) {
    if (L->depth >= WB_LV_MAX_CALL) {
        wb_lv_error(L, "call nesting too deep (limit %d)", WB_LV_MAX_CALL);
        return 0;
    }
    L->depth++;

    env *e = new_env(L, fn->defining);
    if (!e) { L->depth--; wb_lv_error(L, "out of memory"); return 0; }
    for (int i = 0; i < fn->nparams; i++)
        env_set(L, e, fn->params[i], (i < nargs) ? args[i] : wb_lv_nil());

    /* varargs: everything past the named parameters */
    int nva = (nargs > fn->nparams) ? nargs - fn->nparams : 0;
    wb_lv_value *va = NULL;
    if (nva > 0) {
        va = WB_LV_ALLOCN(wb_lv_value, nva, L);
        memcpy(va, args + fn->nparams, (size_t)nva * sizeof(*va));
    }
    wb_lv_value *save_va = L->va; int save_nva = L->nva;
    L->va = va; L->nva = nva;

    int base = L->sp;
    if (fn->body && fn->body->t == N_BLOCK) {
        int wr = 0;
        exec(L, fn->body, e, &wr);
    }
    int n = L->sp - base;

    L->va = save_va; L->nva = save_nva;
    L->depth--;
    return n;
}

/* single-value contract used by the public wb_lv_call */
wb_lv_value wb_lv_call_lua(wb_lv *L, wb_lv_func *fn, wb_lv_value *args, int nargs) {
    int base = L->sp;
    int n = wb_lv_call_lua_results(L, fn, args, nargs);
    wb_lv_value r = wb_lv_nil();
    if (n > 0) { r = L->stk[base]; L->sp = base; }
    return r;
}

/* ------------------------------------------------------------- executor */

static int block_has_label(ex *b, const char *label) {
    if (!b) return 0;
    for (int i = 0; i < b->nlist; i++)
        if (b->list[i] && b->list[i]->t == N_LABEL && !strcmp(b->list[i]->label, label)) return 1;
    return 0;
}

wb_lv_value wb_lv_exec(wb_lv *L, ex *e, env *en, int *was_return) {
    *was_return = 0;
    if (!e) return wb_lv_nil();
    int save = L->curline;
    L->curline = e->line;

    switch (e->t) {
    case N_BLOCK: {
        env *be = new_env(L, en);           /* every block gets its own scope */
        int n = e->nlist;
        for (int i = 0; i < n; i++) {
            int wr = 0;
            wb_lv_value r = exec(L, e->list[i], be, &wr);
            if (wr) { *was_return = 1; L->curline = save; return r; }
            if (L->has_err) { L->curline = save; return wb_lv_nil(); }
            if (L->has_goto) {
                /* A goto whose label is in THIS block jumps; anything else
                 * (a break, or a goto to an outer block) propagates. */
                if (L->goto_label && block_has_label(e, L->goto_label)) {
                    int j = i + 1;
                    for (; j < n; j++)
                        if (e->list[j] && e->list[j]->t == N_LABEL &&
                            !strcmp(e->list[j]->label, L->goto_label)) break;
                    L->has_goto = 0; L->goto_label = NULL;
                    if (j < n) { i = j; continue; }   /* resume after the label */
                }
                L->curline = save;
                return wb_lv_nil();
            }
        }
        L->curline = save;
        return wb_lv_nil();
    }
    case N_LOCAL: {
        int nvals = (int)(intptr_t)e->e4;
        ex **vals = (ex **)e->keys;
        int nn = e->nlist;
        if (nvals == 0) {
            for (int i = 0; i < nn; i++) env_set(L, en, e->names[i], wb_lv_nil());
            L->curline = save;
            return wb_lv_nil();
        }
        /* all but the last value expression contribute exactly one value; the
         * last expands to fill whatever names remain (multiple returns). */
        wb_lv_value tmp[32]; int nt = 0;
        for (int i = 0; i < nvals - 1 && nt < 32; i++) {
            tmp[nt++] = eval(L, vals[i], en);
            if (L->has_err) { L->curline = save; return wb_lv_nil(); }
        }
        wb_lv_value *out = NULL; int no = 0;
        wb_lv_eval_multi(L, vals[nvals - 1], en, &out, &no);
        if (L->has_err) { L->curline = save; return wb_lv_nil(); }
        for (int i = 0; i < no && nt < 32; i++) tmp[nt++] = out[i];
        for (int i = 0; i < nn; i++)
            env_set(L, en, e->names[i], (i < nt) ? tmp[i] : wb_lv_nil());
        L->curline = save;
        return wb_lv_nil();
    }
    case N_ASSIGN: {
        int nt = e->nlist, nv = (int)(intptr_t)e->e4;
        ex **vals = (ex **)e->keys;
        /* evaluate the right-hand side first */
        wb_lv_value rhs[32];
        int nr = 0;
        for (int i = 0; i < nv - 1 && i < 32; i++) rhs[nr++] = eval(L, vals[i], en);
        wb_lv_value *out = NULL; int no = 0;
        if (nv > 0) wb_lv_eval_multi(L, vals[nv - 1], en, &out, &no);
        for (int i = 0; i < no && nr < 32; i++) rhs[nr++] = out[i];

        for (int i = 0; i < nt; i++) {
            ex *t = e->list[i];
            wb_lv_value v = (i < nr) ? rhs[i] : wb_lv_nil();
            if (t->t == N_NAME) env_assign(L, en, t->name, v);
            else if (t->t == N_INDEX) {
                wb_lv_value obj = eval(L, t->e1, en);
                wb_lv_value key = eval(L, t->e2, en);
                if (!L->has_err) wb_lv_setindex(L, obj, key, v);
            }
            if (L->has_err) break;
        }
        L->curline = save;
        return wb_lv_nil();
    }
    case N_CALL: case N_METHOD: {
        wb_lv_value *out = NULL; int no = 0;
        wb_lv_eval_multi(L, e, en, &out, &no);
        L->curline = save;
        return wb_lv_nil();
    }
    case N_FUNCSTAT: {
        wb_lv_value f = eval(L, e->e2, en);        /* the closure */
        if (L->has_err) { L->curline = save; return wb_lv_nil(); }
        if (e->nlist == 0) {
            env_assign(L, en, e->name, f);
        } else {
            /* function t.f() / function t:f() -- set the field on t */
            wb_lv_value obj = env_get(L, en, e->e1->name);
            for (int i = 0; i < e->nlist - 1; i++) {
                obj = wb_lv_index(L, obj, wb_lv_str(e->names[i]));
                if (L->has_err) break;
            }
            if (!L->has_err)
                wb_lv_setindex(L, obj, wb_lv_str(e->names[e->nlist - 1]), f);
        }
        L->curline = save;
        return wb_lv_nil();
    }
    case N_LOCALFUNC: {
        wb_lv_value f = eval(L, e->e1, en);
        env_set(L, en, e->name, f);
        L->curline = save;
        return wb_lv_nil();
    }
    case N_IF: {
        wb_lv_value c = eval(L, e->e1, en);
        if (L->has_err) { L->curline = save; return wb_lv_nil(); }
        if (wb_lv_truthy(c)) { wb_lv_value r = exec(L, e->e2, en, was_return); L->curline = save; return r; }
        for (int i = 0; i < e->nlist; i++) {
            wb_lv_value cc = eval(L, e->list[i * 2], en);
            if (L->has_err) { L->curline = save; return wb_lv_nil(); }
            if (wb_lv_truthy(cc)) { wb_lv_value r = exec(L, e->list[i * 2 + 1], en, was_return); L->curline = save; return r; }
        }
        if (e->e3) { wb_lv_value r = exec(L, e->e3, en, was_return); L->curline = save; return r; }
        L->curline = save;
        return wb_lv_nil();
    }
    case N_WHILE: {
        for (;;) {
            wb_lv_value c = eval(L, e->e1, en);
            if (L->has_err) break;
            if (!wb_lv_truthy(c)) break;
            int wr = 0;
            wb_lv_value r = exec(L, e->e2, en, &wr);
            if (wr) { *was_return = 1; L->curline = save; return r; }
            if (L->has_err) break;
            if (L->has_goto) {
                if (L->goto_label == NULL) L->has_goto = 0;   /* break */
                break;
            }
        }
        L->curline = save;
        return wb_lv_nil();
    }
    case N_REPEAT: {
        for (;;) {
            int wr = 0;
            wb_lv_value r = exec(L, e->e2, en, &wr);
            if (wr) { *was_return = 1; L->curline = save; return r; }
            if (L->has_err) break;
            if (L->has_goto) {
                if (L->goto_label == NULL) L->has_goto = 0;   /* break */
                break;
            }
            wb_lv_value c = eval(L, e->e1, en);
            if (L->has_err) break;
            if (wb_lv_truthy(c)) break;
        }
        L->curline = save;
        return wb_lv_nil();
    }
    case N_FORNUM: {
        wb_lv_value v1 = eval(L, e->e1, en);
        wb_lv_value v2 = eval(L, e->e2, en);
        wb_lv_value v3 = e->e3 ? eval(L, e->e3, en) : wb_lv_integer(1);
        if (L->has_err) { L->curline = save; return wb_lv_nil(); }
        int ok1, ok2, ok3;
        double start = wb_lv_tonumber(v1, &ok1);
        double stop  = wb_lv_tonumber(v2, &ok2);
        double step  = wb_lv_tonumber(v3, &ok3);
        if (!ok1 || !ok2 || !ok3) {
            wb_lv_error(L, "'for' initial value must be a number (line %d)", e->line);
            L->curline = save; return wb_lv_nil();
        }
        if (step == 0) { wb_lv_error(L, "'for' step is zero (line %d)", e->line); L->curline = save; return wb_lv_nil(); }
        for (double i = start; (step > 0) ? (i <= stop) : (i >= stop); i += step) {
            env *fe = new_env(L, en);
            if (!fe) break;
            env_set(L, fe, e->names[0], wb_lv_number(i));
            int wr = 0;
            wb_lv_value r = exec(L, e->e4, fe, &wr);
            if (wr) { *was_return = 1; L->curline = save; return r; }
            if (L->has_err) break;
            if (L->has_goto) {
                if (L->goto_label == NULL) L->has_goto = 0;   /* break */
                break;
            }
        }
        L->curline = save;
        return wb_lv_nil();
    }
    case N_FORIN: {
        /* evaluate the iterator triple */
        int nv = (int)(intptr_t)e->e4;
        wb_lv_value f, s, ctrl;
        wb_lv_value *out = NULL; int no = 0;
        wb_lv_eval_multi(L, e->list[nv - 1], en, &out, &no);
        f = (no > 0) ? out[0] : wb_lv_nil();
        s = (no > 1) ? out[1] : wb_lv_nil();
        ctrl = (no > 2) ? out[2] : wb_lv_nil();
        while (!L->has_err) {
            wb_lv_value args[2]; args[0] = s; args[1] = ctrl;
            wb_lv_value *res = NULL; int nr = wb_lv_call_value(L, f, args, 2, &res);
            if (L->has_err) break;
            if (nr == 0 || res[0].t == WB_LV_NIL) break;
            ctrl = res[0];
            env *fe = new_env(L, en);
            if (!fe) break;
            for (int i = 0; i < e->nlist && i < nr; i++) {
                env_set(L, fe, e->names[i], res[i]);
            }
            int wr = 0;
            wb_lv_value r = exec(L, e->e1, fe, &wr);
            if (wr) { *was_return = 1; L->curline = save; return r; }
            if (L->has_err) break;
            if (L->has_goto) {
                if (L->goto_label == NULL) L->has_goto = 0;   /* break */
                break;
            }
        }
        L->curline = save;
        return wb_lv_nil();
    }
    case N_DO: {
        env *de = new_env(L, en);
        wb_lv_value r = exec(L, e->e1, de ? de : en, was_return);
        L->curline = save;
        return r;
    }
    case N_RETURN: {
        int n = e->nlist;
        ex **vals = e->list;
        if (n == 0) { *was_return = 1; L->curline = save; return wb_lv_nil(); }
        /* push results onto the value stack */
        for (int i = 0; i < n - 1; i++) stk_push(L, eval(L, vals[i], en));
        wb_lv_value *out = NULL; int no = 0;
        wb_lv_eval_multi(L, vals[n - 1], en, &out, &no);
        for (int i = 0; i < no; i++) stk_push(L, out[i]);
        if (no == 0) stk_push(L, wb_lv_nil());
        *was_return = 1;
        L->curline = save;
        return wb_lv_nil();
    }
    case N_BREAK:
        /* break is signalled by a sentinel: reuse has_goto with a NULL label */
        L->goto_label = NULL; L->has_goto = 1;
        L->curline = save;
        return wb_lv_nil();
    case N_GOTO:
        L->goto_label = e->label; L->has_goto = 1;
        L->curline = save;
        return wb_lv_nil();
    case N_LABEL:
        /* labels are no-ops at run time */
        L->curline = save;
        return wb_lv_nil();
    default: {
        wb_lv_value r = eval(L, e, en);
        L->curline = save;
        return r;
    }
    }
}

wb_lv_value wb_lv_exec_root(wb_lv *L, ex *root) {
    env *top = new_env(L, NULL);
    if (!top) { wb_lv_error(L, "out of memory"); return wb_lv_nil(); }
    int wr = 0;
    exec(L, root, top, &wr);
    return wb_lv_nil();
}
