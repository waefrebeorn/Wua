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

/* Raise a VALUE, as error(obj) does: Lua returns the original object, not a
 * stringified copy. */
void wb_lv_error_value(wb_lv *L, wb_lv_value v) {
    if (!L->has_err) {
        L->has_err = 1;
        L->err_obj = v;
        L->has_err_obj = 1;
        snprintf(L->err, sizeof L->err, "%s", wb_lv_tostring_intern(L, v));
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

static void env_set(wb_lv *L, env *e, const char *name, wb_lv_value v) { if (L->prof_on) L->prof.envs++;
    if (!e) return;
    wb_lv_table_set(L, e->t, wb_lv_str(name), v);
}

static wb_lv_value env_get(wb_lv *L, env *e, const char *name) { if (L->prof_on) L->prof.envg++;
    for (env *c = e; c; c = c->parent) {
        wb_lv_value r = wb_lv_table_get(L, c->t, wb_lv_str(name));
        if (r.t != WB_LV_NIL) return r;
    }
    return wb_lv_table_get(L, L->globals, wb_lv_str(name));
}

static void env_assign(wb_lv *L, env *e, const char *name, wb_lv_value v) {
    for (env *c = e; c; c = c->parent) {
        wb_lv_value r = wb_lv_table_get(L, c->t, wb_lv_str(name));
        if (r.t != WB_LV_NIL) {
            if (c->consts && wb_lv_table_get(L, c->consts, wb_lv_str(name)).t != WB_LV_NIL)
                wb_lv_error(L, "attempt to assign to a const variable '%s' (line %d)", name, L->curline);
            env_set(L, c, name, v);
            return;
        }
    }
    wb_lv_table_set(L, L->globals, wb_lv_str(name), v);
}

static void env_push(wb_lv *L, env *e) {
    if (L->envtop >= L->envcap) {
        int nc = L->envcap ? L->envcap * 2 : 32;
        env **nn = (env **)realloc(L->envst, (size_t)nc * sizeof(*nn));
        if (!nn) return;
        L->envst = nn; L->envcap = nc;
    }
    L->envst[L->envtop++] = e;
}
static env *new_env(wb_lv *L, env *parent) { if (L->prof_on) L->prof.newenv++;
    env *e = WB_LV_ALLOC(env, L);
    if (!e) return NULL;
    e->t = wb_lv_table_new(L, 0, 8);
    e->consts = wb_lv_table_new(L, 0, 4);
    e->parent = parent;
    env_push(L, e);                 /* track active scopes for the GC       */
    return e;
}
static void env_pop(wb_lv *L) {
    if (L->envtop > 0) { L->envtop--; L->envst[L->envtop] = NULL; }
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
        /* Lua's luaV_modfloat: exact fmod, then correct the sign so the
         * result has the divisor's sign. The naive `x - floor(x/y)*y`
         * rounds twice and diverges from Lua by ~1 ulp. */
        double r = fmod(x, y);
        if (r != 0 && ((r < 0) != (y < 0))) r += y;
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
    /* Lua 5.4: __le may be derived from __lt as `not (b < a)`. */
    if (op == T_LE || op == T_GE) {
        wb_lv_value x2 = a, y2 = b;
        if (op == T_GE) { x2 = b; y2 = a; }
        wb_lv_table *mx2 = wb_lv_getmetatable(L, x2);
        wb_lv_table *my2 = wb_lv_getmetatable(L, y2);
        wb_lv_value f2 = mx2 ? wb_lv_table_get(L, mx2, wb_lv_str("__lt")) : wb_lv_nil();
        if (f2.t == WB_LV_NIL && my2) f2 = wb_lv_table_get(L, my2, wb_lv_str("__lt"));
        if (f2.t == WB_LV_FUNCTION || f2.t == WB_LV_CFUNCTION) {
            wb_lv_value r = call_meta(L, f2, x2, y2);
            return wb_lv_bool(!wb_lv_truthy(r));
        }
    }
    wb_lv_error(L, "attempt to compare %s with %s (line %d)",
                wb_lv_type_name(a.t), wb_lv_type_name(b.t), L->curline);
    return wb_lv_nil();
}

int wb_lv_len(wb_lv *L, wb_lv_value v, size_t *out) {
    if (v.t == WB_LV_STR) { *out = v.slen; return 1; }
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
        size_t la, lb;
        const char *sa, *sb;
        if (a.t == WB_LV_STR) { sa = a.as.s; la = a.slen; }
        else { sa = wb_lv_tostring_intern(L, a); la = strlen(sa); }
        if (b.t == WB_LV_STR) { sb = b.as.s; lb = b.slen; }
        else { sb = wb_lv_tostring_intern(L, b); lb = strlen(sb); }
        char *buf = (char *)malloc(la + lb + 1);
        if (!buf) { wb_lv_error(L, "out of memory"); return wb_lv_nil(); }
        memcpy(buf, sa, la); memcpy(buf + la, sb, lb); buf[la + lb] = 0;
        const char *r = wb_lv_intern(L, buf, la + lb);
        free(buf);
        return wb_lv_strl(r, la + lb);
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
        /* every type with a metatable -- including strings -- resolves __index */
        wb_lv_table *m = (obj.t == WB_LV_STR) ? L->string_meta : wb_lv_getmetatable(L, obj);
        if (m) {
            wb_lv_value h = wb_lv_table_get(L, m, wb_lv_str("__index"));
            if (h.t == WB_LV_TABLE) {
                wb_lv_value v = wb_lv_table_get(L, h.as.tab, key);
                if (v.t != WB_LV_NIL) return v;
            } else if (h.t == WB_LV_FUNCTION || h.t == WB_LV_CFUNCTION) {
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
    if (L->prof_on) L->prof.eval++;
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

/* Proper-tail-call: evaluate a call EXPRESSION's callee + arguments without
 * invoking it, and record them as a pending tail call (L->tail). Only used for
 * a single-value `return f(...)` in tail position; the enclosing frame's loop
 * in wb_lv_call_lua_results consumes it and reuses its own depth, so deep tail
 * recursion does not grow the C stack. N_METHOD prepends self, like a normal
 * method call. Returns 1 on success (L->tail set), 0 on error (L->has_err). */
static int wb_lv_eval_multi_tail(wb_lv *L, ex *e, env *en) {
    wb_lv_value callee;
    wb_lv_value *args = NULL; int na = 0, acap = 0;
    if (e->t == N_METHOD) {
        wb_lv_value self = eval(L, e->e1->e1, en);
        if (L->has_err) return 0;
        wb_lv_value key = (e->e1->e2 && e->e1->e2->t == N_STR)
                              ? wb_lv_str(e->e1->e2->str) : wb_lv_nil();
        callee = wb_lv_index(L, self, key);
        if (L->has_err) { free(args); return 0; }
        args = args_push(args, &na, &acap, self);          /* implicit self */
        for (int i = 0; i < e->nlist; i++) {
            if (i == e->nlist - 1) {
                wb_lv_value *o2 = NULL; int n2 = 0;
                wb_lv_eval_multi(L, e->list[i], en, &o2, &n2);
                if (L->has_err) { free(args); return 0; }
                for (int k = 0; k < n2; k++) args = args_push(args, &na, &acap, o2[k]);
            } else {
                wb_lv_value v = eval(L, e->list[i], en);
                if (L->has_err) { free(args); return 0; }
                args = args_push(args, &na, &acap, v);
            }
        }
    } else {
        callee = eval(L, e->e1, en);
        if (L->has_err) { free(args); return 0; }
        for (int i = 0; i < e->nlist; i++) {
            if (i == e->nlist - 1) {
                wb_lv_value *o2 = NULL; int n2 = 0;
                wb_lv_eval_multi(L, e->list[i], en, &o2, &n2);
                if (L->has_err) { free(args); return 0; }
                for (int k = 0; k < n2; k++) args = args_push(args, &na, &acap, o2[k]);
            } else {
                wb_lv_value v = eval(L, e->list[i], en);
                if (L->has_err) { free(args); return 0; }
                args = args_push(args, &na, &acap, v);
            }
        }
    }
    L->tail_fn = callee; L->tail_args = args; L->tail_n = na;
    L->tail = 1;
    return 1;
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
    case N_STR: return wb_lv_strl(e->str, e->slen);
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
        /* Positional (array-part) items are numbered 1,2,3,... in the order
         * they appear, INDEPENDENTLY of keyed fields: in Lua,
         * {spacing=6,'a','b','c'} and {'a','b','c',spacing=6} are the same
         * table. Numbering them by their index in the field list instead
         * would let each keyed field shift every later item up one slot. */
        int64_t pos = 0;
        for (int i = 0; i < e->nlist; i++) {
            if (e->keys[i]) {
                wb_lv_value k = eval(L, e->keys[i], en);
                wb_lv_value v = eval(L, e->list[i], en);
                if (L->has_err) return wb_lv_nil();
                wb_lv_table_set(L, t, k, v);
            } else if (i == e->nlist - 1 && (e->list[i]->t == N_CALL || e->list[i]->t == N_METHOD || e->list[i]->t == N_VARARG)) {
                /* the last field, if it is a call or vararg, expands to all
                 * of its values (Lua's table-constructor rule) */
                wb_lv_value *out = NULL; int n = 0;
                wb_lv_eval_multi(L, e->list[i], en, &out, &n);
                for (int j = 0; j < n; j++) {
                    wb_lv_value k; k.t = WB_LV_INT; k.as.i = ++pos;
                    wb_lv_table_set(L, t, k, out[j]);
                }
            } else {
                wb_lv_value v = eval(L, e->list[i], en);
                if (L->has_err) return wb_lv_nil();
                wb_lv_value k; k.t = WB_LV_INT; k.as.i = ++pos;
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
 * caller reads L->stk[base..sp) and rewinds); returns the result count.
 *
 * Proper tail calls: a single-value `return f(...)` in this frame records a
 * pending tail call (L->tail) instead of recursing, and the loop below runs
 * the callee IN THE SAME frame -- same depth, same C frame -- so deep tail
 * recursion never grows the stack. Non-tail calls still recurse normally. */
int wb_lv_call_lua_results(wb_lv *L, wb_lv_func *fn, wb_lv_value *args, int nargs) { if (L->prof_on) L->prof.call++;
    if (L->depth >= WB_LV_MAX_CALL) {
        wb_lv_error(L, "call nesting too deep (limit %d)", WB_LV_MAX_CALL);
        return 0;
    }
    L->depth++;

    wb_lv_value *save_va = L->va; int save_nva = L->nva;
    int base = L->sp;
    int n = 0;
    wb_lv_value *pending = NULL;       /* malloc'd tail args, freed once bound */

    for (;;) {
        env *e = new_env(L, fn->defining);
        if (!e) {
            free(pending);
            L->va = save_va; L->nva = save_nva; L->depth--;
            wb_lv_error(L, "out of memory"); return 0;
        }
        for (int i = 0; i < fn->nparams; i++)
            env_set(L, e, fn->params[i], (i < nargs) ? args[i] : wb_lv_nil());

        /* varargs: everything past the named parameters */
        int nva = (nargs > fn->nparams) ? nargs - fn->nparams : 0;
        wb_lv_value *va = NULL;
        if (nva > 0) {
            va = WB_LV_ALLOCN(wb_lv_value, nva, L);
            memcpy(va, args + fn->nparams, (size_t)nva * sizeof(*va));
        }
        free(pending); pending = NULL;       /* this frame copied what it needed */
        L->va = va; L->nva = nva;

        int wr = 0;
        if (fn->body && fn->body->t == N_BLOCK) exec(L, fn->body, e, &wr);

        if (L->has_err) {
            env_pop(L);
            L->tail = 0; free(L->tail_args); L->tail_args = NULL;
            L->va = save_va; L->nva = save_nva; L->depth--;
            return 0;
        }

        if (L->tail) {
            wb_lv_value tfn = L->tail_fn;
            wb_lv_value *ta = L->tail_args; int tn = L->tail_n;
            L->tail = 0; L->tail_args = NULL;
            env_pop(L);       /* this frame's scope is done; reuse its depth */
            if (tfn.t == WB_LV_FUNCTION) {
                fn = tfn.as.fn; args = ta; nargs = tn;
                pending = ta;
                continue;     /* run the callee in this same flat frame */
            }
            /* C function / __call tail: an ordinary nested call */
            {
                wb_lv_value *out = NULL; int r = wb_lv_call_value(L, tfn, ta, tn, &out);
                free(ta);
                if (L->has_err) {
                    L->va = save_va; L->nva = save_nva; L->depth--;
                    return 0;
                }
                for (int i = 0; i < r; i++) stk_push(L, out[i]);
                n = r;
                L->va = save_va; L->nva = save_nva; L->depth--;
                return n;
            }
        }

        n = L->sp - base;
        env_pop(L);                     /* the call frame's scope is done   */
        L->va = save_va; L->nva = save_nva; L->depth--;
        return n;
    }
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
    if (L->prof_on) L->prof.exec++;
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
                    int j = 0;                        /* backward jumps included */
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
        env_pop(L);
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
        int *attrs = (int *)(intptr_t)e->e3;
        for (int i = 0; i < nn; i++) {
            env_set(L, en, e->names[i], (i < nt) ? tmp[i] : wb_lv_nil());
            if (attrs && (attrs[i] & 1) && en->consts)
                wb_lv_table_set(L, en->consts, wb_lv_str(e->names[i]), wb_lv_bool(1));
        }
        L->curline = save;
        return wb_lv_nil();
    }
    case N_ASSIGN: {
        int nt = e->nlist, nv = (int)(intptr_t)e->e4;
        ex **vals = (ex **)e->keys;
        /* Evaluate every target's table+key FIRST: in `i, t[i] = 2, 99` the
         * key t[i] must use the OLD i. */
        int is_idx[32];
        wb_lv_value objs[32], keys[32];
        int bounded = (nt < 32) ? nt : 32;
        for (int i = 0; i < bounded; i++) {
            ex *t = e->list[i];
            if (t->t == N_NAME) {
                is_idx[i] = 0;
            } else if (t->t == N_INDEX) {
                is_idx[i] = 1;
                objs[i] = eval(L, t->e1, en);
                keys[i] = eval(L, t->e2, en);
                if (L->has_err) { L->curline = save; return wb_lv_nil(); }
            } else {
                wb_lv_error(L, "invalid assignment target (line %d)", e->line);
                L->curline = save; return wb_lv_nil();
            }
        }
        wb_lv_value rhs[32]; int nr = 0;
        for (int i = 0; i < nv - 1 && nr < 32; i++) {
            rhs[nr++] = eval(L, vals[i], en);
            if (L->has_err) { L->curline = save; return wb_lv_nil(); }
        }
        wb_lv_value *out = NULL; int no = 0;
        if (nv > 0) wb_lv_eval_multi(L, vals[nv - 1], en, &out, &no);
        if (L->has_err) { L->curline = save; return wb_lv_nil(); }
        for (int i = 0; i < no && nr < 32; i++) rhs[nr++] = out[i];
        for (int i = 0; i < bounded; i++) {
            wb_lv_value v = (i < nr) ? rhs[i] : wb_lv_nil();
            if (is_idx[i]) wb_lv_setindex(L, objs[i], keys[i], v);
            else env_assign(L, en, e->list[i]->name, v);
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
        /* Lua 5.4: integer bounds (and step) give an integer control variable */
        int allint = (v1.t == WB_LV_INT) && (v2.t == WB_LV_INT) &&
                     (!e->e3 || v3.t == WB_LV_INT);
        if (allint) {
            int64_t i1 = v1.as.i, i2 = v2.as.i;
            int64_t i3 = e->e3 ? v3.as.i : 1;
            if (i3 == 0) { wb_lv_error(L, "'for' step is zero (line %d)", e->line); L->curline = save; return wb_lv_nil(); }
            for (int64_t i = i1; (i3 > 0) ? (i <= i2) : (i >= i2); i += i3) {
                env *fe = new_env(L, en);
                if (!fe) break;
                env_set(L, fe, e->names[0], wb_lv_integer(i));
                int wr = 0;
                wb_lv_value r = exec(L, e->e4, fe, &wr);
                if (wr) { *was_return = 1; L->curline = save; return r; }
                if (L->has_err) break;
                if (L->has_goto) { if (L->goto_label == NULL) L->has_goto = 0; break; }
            }
        } else {
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
                if (L->has_goto) { if (L->goto_label == NULL) L->has_goto = 0; break; }
            }
        }
        env_pop(L);
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
        env_pop(L);
        L->curline = save;
        return wb_lv_nil();
    }
    case N_DO: {
        env *de = new_env(L, en);
        wb_lv_value r = exec(L, e->e1, de ? de : en, was_return);
        env_pop(L);
        L->curline = save;
        return r;
    }
    case N_RETURN: {
        int n = e->nlist;
        ex **vals = e->list;
        if (n == 0) { *was_return = 1; L->curline = save; return wb_lv_nil(); }
        /* proper tail call: a single-value return of a call in tail position,
         * inside a function frame, becomes a pending tail call so the frame
         * reuses its depth instead of recursing. (depth>0 means we are inside
         * wb_lv_call_lua_results, which consumes the signal.) */
        if (n == 1 && L->depth > 0 &&
            (vals[0]->t == N_CALL || vals[0]->t == N_METHOD)) {
            L->curline = save;
            if (wb_lv_eval_multi_tail(L, vals[0], en)) {
                *was_return = 1;
                return wb_lv_nil();
            }
            if (L->has_err) { *was_return = 1; return wb_lv_nil(); }
            L->curline = save;
            /* not a tail candidate after all: continue below */
        }
        /* push results onto the value stack */
        for (int i = 0; i < n - 1; i++) stk_push(L, eval(L, vals[i], en));
        wb_lv_value *out = NULL; int no = 0;
        wb_lv_eval_multi(L, vals[n - 1], en, &out, &no);
        for (int i = 0; i < no; i++) stk_push(L, out[i]);
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

/* --------------------------------------------------------------- garbage */
/*
 * Finalizers, conservatively. Tables live in a non-reclaiming arena, so the
 * mark/sweep here only decides liveness to run __gc; it frees nothing. Roots
 * are the globals, every currently-active execution scope (env stack), the
 * value stack and the active varargs. Propagate through table entries,
 * metatables and function environments. A table that carries a __gc in its
 * metatable and is unreachable has its __gc called exactly once. Over-marking
 * is safe (a false live keeps it alive); under-marking would call a finalizer
 * on a live object, so the root set must be complete.
 */

typedef struct { wb_lv *L; wb_lv_table **wk; int wn, wcap; } gctx;

static void gcadd(gctx *g, wb_lv_table *t) {
    if (!t || t->gcgen == g->L->gc_epoch) return;
    t->gcgen = g->L->gc_epoch;
    if (g->wn >= g->wcap) {
        int nc = g->wcap * 2 + 16;
        wb_lv_table **nn = (wb_lv_table **)realloc(g->wk, (size_t)nc * sizeof(*nn));
        if (!nn) return;                 /* over-mark kept live: safe */
        g->wk = nn; g->wcap = nc;
    }
    if (g->wn < g->wcap) g->wk[g->wn++] = t;
}

static void gcmark(gctx *g, wb_lv_value v) {
    if (v.t == WB_LV_TABLE) gcadd(g, v.as.tab);
    else if (v.t == WB_LV_FUNCTION) {
        if (v.as.fn && v.as.fn->defining) {
            gcadd(g, v.as.fn->defining->t);
            gcadd(g, v.as.fn->defining->consts);
        }
    } else if (v.t == WB_LV_MULTI) {
        for (int i = 0; v.as.multi && i < v.as.multi->n; i++) gcmark(g, v.as.multi->v[i]);
    }
}

size_t wb_lv_gc_count(wb_lv *L) {
    return L ? (L->arena_bytes / 1024) : 0;   /* KB, as Lua's 'count' */
}

void wb_lv_gc_collect(wb_lv *L) {
    if (!L) return;
    L->gc_epoch++;
    if (L->gc_epoch == 0) L->gc_epoch = 1;

    gctx g; g.L = L; g.wk = NULL; g.wn = 0; g.wcap = 0;

    /* roots: globals, active scopes, value stack, varargs */
    gcadd(&g, L->globals);
    gcadd(&g, L->string_meta);
    for (int i = 0; i < L->envtop && L->envst; i++) {
        if (L->envst[i]) { gcadd(&g, L->envst[i]->t); gcadd(&g, L->envst[i]->consts); }
    }
    for (int i = 0; i < L->sp && i < L->scap; i++) gcmark(&g, L->stk[i]);
    for (int i = 0; i < L->nva && L->va; i++) gcmark(&g, L->va[i]);

    /* propagate through entries, metatables and function environments */
    for (int i = 0; i < g.wn; i++) {
        wb_lv_table *t = g.wk[i];
        if (t->meta) gcadd(&g, t->meta);
        if (t->e) for (size_t j = 0; j < t->cap; j++) {
            wb_lv_kv *kv = &t->e[j];
            if (kv->used && !kv->tomb) { gcmark(&g, kv->k); gcmark(&g, kv->v); }
        }
    }

    /* finalize unreachable tables that carry __gc */
    for (int i = L->mtab_n - 1; i >= 0; i--) {        /* newest first, like Lua */
        wb_lv_table *t = L->mtab[i];
        if (!t || t->gc_done || !t->meta) continue;
        if (t->gcgen == L->gc_epoch) continue;                 /* reachable */
        wb_lv_value f = wb_lv_table_get(L, t->meta, wb_lv_str("__gc"));
        if (f.t == WB_LV_FUNCTION || f.t == WB_LV_CFUNCTION) {
            wb_lv_value self; self.t = WB_LV_TABLE; self.as.tab = t;
            wb_lv_call(L, f, &self, 1);
            t->gc_done = 1;
        }
    }
    free(g.wk);
}
