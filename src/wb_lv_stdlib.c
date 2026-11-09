/*
 * wb_lv_stdlib.c -- the table / math / os / io libraries.
 * See wb_lv_internal.h.
 */
#include "wb_lv_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------------------------------------------------------- table */

static int64_t check_int(wb_lv *L, wb_lv_value v, const char *who, int n) {
    int ok;
    int64_t i = wb_lv_tointeger(v, &ok);
    if (!ok) wb_lv_error(L, "bad argument #%d to '%s' (number expected)", n, who);
    return i;
}

static wb_lv_value t_insert(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    if (n < 2 || a[0].t != WB_LV_TABLE) { wb_lv_error(L, "bad argument #1 to 'insert' (table expected)"); return wb_lv_nil(); }
    wb_lv_table *t = a[0].as.tab;
    size_t len = wb_lv_table_len(t);
    if (n == 2) {
        wb_lv_value k; k.t = WB_LV_INT; k.as.i = (int64_t)len + 1;
        wb_lv_table_set(L, t, k, a[1]);
    } else {
        int64_t pos = check_int(L, a[1], "insert", 2);
        if (pos < 1 || pos > (int64_t)len + 1) { wb_lv_error(L, "bad argument #2 to 'insert' (position out of bounds)"); return wb_lv_nil(); }
        for (int64_t i = (int64_t)len; i >= pos; i--) {
            wb_lv_value k; k.t = WB_LV_INT; k.as.i = i;
            wb_lv_value v = wb_lv_table_get(L, t, k);
            k.as.i = i + 1;
            wb_lv_table_set(L, t, k, v);
        }
        wb_lv_value k; k.t = WB_LV_INT; k.as.i = pos;
        wb_lv_table_set(L, t, k, a[2]);
    }
    return wb_lv_nil();
}

static wb_lv_value t_remove(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    if (n < 1 || a[0].t != WB_LV_TABLE) { wb_lv_error(L, "bad argument #1 to 'remove' (table expected)"); return wb_lv_nil(); }
    wb_lv_table *t = a[0].as.tab;
    size_t len = wb_lv_table_len(t);
    int64_t pos = (n >= 2) ? check_int(L, a[1], "remove", 2) : (int64_t)len;
    if (len == 0) return wb_lv_nil();
    if (pos < 1 || pos > (int64_t)len) pos = (int64_t)len;
    wb_lv_value k; k.t = WB_LV_INT; k.as.i = pos;
    wb_lv_value v = wb_lv_table_get(L, t, k);
    for (int64_t i = pos; i < (int64_t)len; i++) {
        wb_lv_value k2; k2.t = WB_LV_INT; k2.as.i = i + 1;
        wb_lv_value v2 = wb_lv_table_get(L, t, k2);
        k2.as.i = i;
        wb_lv_table_set(L, t, k2, v2);
    }
    wb_lv_value k3; k3.t = WB_LV_INT; k3.as.i = (int64_t)len;
    wb_lv_table_set(L, t, k3, wb_lv_nil());
    return v;
}

static wb_lv_value t_concat(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    if (n < 1 || a[0].t != WB_LV_TABLE) { wb_lv_error(L, "bad argument #1 to 'concat' (table expected)"); return wb_lv_nil(); }
    const char *sep = (n >= 2 && a[1].t == WB_LV_STR) ? a[1].as.s : "";
    int64_t i = (n >= 3) ? check_int(L, a[2], "concat", 3) : 1;
    int64_t j = (n >= 4) ? check_int(L, a[3], "concat", 4) : (int64_t)wb_lv_table_len(a[0].as.tab);
    char *buf = NULL; size_t len = 0, cap = 0;
    for (int64_t k = i; k <= j; k++) {
        wb_lv_value key; key.t = WB_LV_INT; key.as.i = k;
        wb_lv_value v = wb_lv_table_get(L, a[0].as.tab, key);
        if (v.t != WB_LV_STR && v.t != WB_LV_INT && v.t != WB_LV_NUM) {
            free(buf);
            wb_lv_error(L, "invalid value (at index %lld) in table for 'concat'", (long long)k);
            return wb_lv_nil();
        }
        const char *s = wb_lv_tostring_intern(L, v);
        size_t sl = strlen(s);
        if (len + sl + strlen(sep) + 1 > cap) { cap = cap * 2 + sl + strlen(sep) + 1; buf = (char *)realloc(buf, cap); }
        memcpy(buf + len, s, sl); len += sl;
        if (k < j) { size_t sepl = strlen(sep); memcpy(buf + len, sep, sepl); len += sepl; }
    }
    if (buf) buf[len] = 0;
    wb_lv_value r = wb_lv_str(wb_lv_intern(L, buf ? buf : "", len));
    free(buf);
    return r;
}

static wb_lv_value t_unpack(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    if (n < 1 || a[0].t != WB_LV_TABLE) { wb_lv_error(L, "bad argument #1 to 'unpack' (table expected)"); return wb_lv_nil(); }
    int64_t i = (n >= 2) ? check_int(L, a[1], "unpack", 2) : 1;
    int64_t j = (n >= 3) ? check_int(L, a[2], "unpack", 3) : (int64_t)wb_lv_table_len(a[0].as.tab);
    int cnt = (int)(j - i + 1);
    if (cnt <= 0) return wb_lv_nil();
    if (cnt > 4096) { wb_lv_error(L, "too many results to unpack"); return wb_lv_nil(); }
    wb_lv_value *out = WB_LV_ALLOCN(wb_lv_value, cnt, L);
    for (int k = 0; k < cnt; k++) {
        wb_lv_value key; key.t = WB_LV_INT; key.as.i = i + k;
        out[k] = wb_lv_table_get(L, a[0].as.tab, key);
    }
    return wb_lv_multi_value(L, out, cnt);
}

static wb_lv_value t_pack(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    wb_lv_table *t = wb_lv_table_new(L, n, n);
    for (int i = 0; i < n; i++) {
        wb_lv_value k; k.t = WB_LV_INT; k.as.i = i + 1;
        wb_lv_table_set(L, t, k, a[i]);
    }
    wb_lv_table_set(L, t, wb_lv_str("n"), wb_lv_integer(n));
    wb_lv_value r; r.t = WB_LV_TABLE; r.as.tab = t;
    return r;
}

static int cmp_less(wb_lv *L, wb_lv_value a, wb_lv_value b) {
    if (a.t == WB_LV_STR && b.t == WB_LV_STR) return strcmp(a.as.s, b.as.s) < 0;
    if ((a.t == WB_LV_INT || a.t == WB_LV_NUM) && (b.t == WB_LV_INT || b.t == WB_LV_NUM)) {
        double x = (a.t == WB_LV_INT) ? (double)a.as.i : a.as.n;
        double y = (b.t == WB_LV_INT) ? (double)b.as.i : b.as.n;
        return x < y;
    }
    return 0;
}

static void quicksort(wb_lv *L, wb_lv_value *arr, int lo, int hi, wb_lv_value cmp) {
    while (lo < hi) {
        int i = lo, j = hi;
        wb_lv_value pivot = arr[(lo + hi) / 2];
        while (i <= j) {
            int less;
            if (cmp.t != WB_LV_NIL) {
                wb_lv_value args[2]; args[0] = arr[i]; args[1] = pivot;
                less = wb_lv_truthy(wb_lv_call(L, cmp, args, 2));
            } else less = cmp_less(L, arr[i], pivot);
            while (less) {
                i++;
                if (cmp.t != WB_LV_NIL) {
                    wb_lv_value args[2]; args[0] = arr[i]; args[1] = pivot;
                    less = wb_lv_truthy(wb_lv_call(L, cmp, args, 2));
                } else less = cmp_less(L, arr[i], pivot);
            }
            if (cmp.t != WB_LV_NIL) {
                wb_lv_value args[2]; args[0] = pivot; args[1] = arr[j];
                less = wb_lv_truthy(wb_lv_call(L, cmp, args, 2));
            } else less = cmp_less(L, pivot, arr[j]);
            while (less) {
                j--;
                if (cmp.t != WB_LV_NIL) {
                    wb_lv_value args[2]; args[0] = pivot; args[1] = arr[j];
                    less = wb_lv_truthy(wb_lv_call(L, cmp, args, 2));
                } else less = cmp_less(L, pivot, arr[j]);
            }
            if (i <= j) { wb_lv_value tmp = arr[i]; arr[i] = arr[j]; arr[j] = tmp; i++; j--; }
        }
        if (j - lo < hi - i) { quicksort(L, arr, lo, j, cmp); lo = i; }
        else { quicksort(L, arr, i, hi, cmp); hi = j; }
    }
}

static wb_lv_value t_sort(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    if (n < 1 || a[0].t != WB_LV_TABLE) { wb_lv_error(L, "bad argument #1 to 'sort' (table expected)"); return wb_lv_nil(); }
    wb_lv_table *t = a[0].as.tab;
    size_t len = wb_lv_table_len(t);
    if (len < 2) return wb_lv_nil();
    wb_lv_value *arr = WB_LV_ALLOCN(wb_lv_value, len, L);
    for (size_t i = 0; i < len; i++) {
        wb_lv_value k; k.t = WB_LV_INT; k.as.i = (int64_t)i + 1;
        arr[i] = wb_lv_table_get(L, t, k);
    }
    wb_lv_value cmp = (n >= 2) ? a[1] : wb_lv_nil();
    quicksort(L, arr, 0, (int)len - 1, cmp);
    for (size_t i = 0; i < len; i++) {
        wb_lv_value k; k.t = WB_LV_INT; k.as.i = (int64_t)i + 1;
        wb_lv_table_set(L, t, k, arr[i]);
    }
    return wb_lv_nil();
}

static wb_lv_value t_move(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    if (n < 3 || a[0].t != WB_LV_TABLE) { wb_lv_error(L, "bad argument #1 to 'move' (table expected)"); return wb_lv_nil(); }
    int64_t f = check_int(L, a[1], "move", 2);
    int64_t e = check_int(L, a[2], "move", 3);
    int64_t t = (n >= 4) ? check_int(L, a[3], "move", 4) : f;
    wb_lv_table *dst = (n >= 5 && a[4].t == WB_LV_TABLE) ? a[4].as.tab : a[0].as.tab;
    if (e >= f) {
        if (t > e || t <= f || dst != a[0].as.tab) {
            for (int64_t i = 0; i <= e - f; i++) {
                wb_lv_value k; k.t = WB_LV_INT; k.as.i = f + i;
                wb_lv_value v = wb_lv_table_get(L, a[0].as.tab, k);
                k.as.i = t + i;
                wb_lv_table_set(L, dst, k, v);
            }
        } else {
            for (int64_t i = e - f; i >= 0; i--) {
                wb_lv_value k; k.t = WB_LV_INT; k.as.i = f + i;
                wb_lv_value v = wb_lv_table_get(L, a[0].as.tab, k);
                k.as.i = t + i;
                wb_lv_table_set(L, dst, k, v);
            }
        }
    }
    if (n >= 5 && a[4].t == WB_LV_TABLE) return a[4];
    return a[0];
}

void wb_lv_register_table(wb_lv *L) {
    wb_lv_table *t = wb_lv_table_new(L, 0, 8);
#define T(name, fn) { wb_lv_value v; v.t = WB_LV_CFUNCTION; v.as.cf.f = fn; v.as.cf.ud = NULL; \
    wb_lv_table_set(L, t, wb_lv_str(name), v); }
    T("insert", t_insert);
    T("remove", t_remove);
    T("concat", t_concat);
    T("unpack", t_unpack);
    T("pack", t_pack);
    T("sort", t_sort); T("move", t_move);
#undef T
    wb_lv_value tv; tv.t = WB_LV_TABLE; tv.as.tab = t;
    wb_lv_set_global(L, "table", tv);
    /* deprecated global `unpack`, still used by Lua-5.4-era tests */
    { wb_lv_value guv; guv.t = WB_LV_CFUNCTION; guv.as.cf.f = t_unpack; guv.as.cf.ud = NULL;
      wb_lv_set_global(L, "unpack", guv); }
    /* 5.1 compat */
    wb_lv_value g; g.t = WB_LV_TABLE; g.as.tab = t;
    (void)g;
}

/* ---------------------------------------------------------------- math */

static wb_lv_value m_floor(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)ud;
    if (n < 1) return wb_lv_nil();
    if (a[0].t == WB_LV_INT) return a[0];
    int ok; double d = wb_lv_tonumber(a[0], &ok);
    if (!ok) return wb_lv_nil();
    return wb_lv_integer((int64_t)floor(d));
}
static wb_lv_value m_ceil(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)ud;
    if (n < 1) return wb_lv_nil();
    if (a[0].t == WB_LV_INT) return a[0];
    int ok; double d = wb_lv_tonumber(a[0], &ok);
    if (!ok) return wb_lv_nil();
    return wb_lv_integer((int64_t)ceil(d));
}
static wb_lv_value m_abs(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)ud;
    if (n < 1) return wb_lv_nil();
    if (a[0].t == WB_LV_INT) return wb_lv_integer(a[0].as.i < 0 ? -a[0].as.i : a[0].as.i);
    int ok; double d = wb_lv_tonumber(a[0], &ok);
    if (!ok) return wb_lv_nil();
    return wb_lv_number(fabs(d));
}
static wb_lv_value m_sqrt(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)ud;
    if (n < 1) return wb_lv_nil();
    return wb_lv_number(sqrt(wb_lv_tonumber(a[0], NULL)));
}
static wb_lv_value m_max(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)ud;
    if (n < 1) return wb_lv_nil();
    wb_lv_value best = a[0];
    for (int i = 1; i < n; i++) {
        int swap;
        if (a[i].t == WB_LV_INT || a[i].t == WB_LV_NUM) {
            double x = (a[i].t == WB_LV_INT) ? (double)a[i].as.i : a[i].as.n;
            double y = (best.t == WB_LV_INT) ? (double)best.as.i : best.as.n;
            swap = x > y;
        } else swap = 0;
        if (swap) best = a[i];
    }
    return best;
}
static wb_lv_value m_min(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)ud;
    if (n < 1) return wb_lv_nil();
    wb_lv_value best = a[0];
    for (int i = 1; i < n; i++) {
        int swap;
        if (a[i].t == WB_LV_INT || a[i].t == WB_LV_NUM) {
            double x = (a[i].t == WB_LV_INT) ? (double)a[i].as.i : a[i].as.n;
            double y = (best.t == WB_LV_INT) ? (double)best.as.i : best.as.n;
            swap = x < y;
        } else swap = 0;
        if (swap) best = a[i];
    }
    return best;
}
static wb_lv_value m_fmod(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)ud;
    if (n < 2) return wb_lv_nil();
    if (a[0].t == WB_LV_INT && a[1].t == WB_LV_INT) {
        if (a[1].as.i == 0) { wb_lv_error(L, "attempt to perform 'n%%0'"); return wb_lv_nil(); }
        int64_t r = a[0].as.i % a[1].as.i;
        if (r != 0 && ((r < 0) != (a[1].as.i < 0))) r += a[1].as.i;
        return wb_lv_integer(r);
    }
    return wb_lv_number(fmod(wb_lv_tonumber(a[0], NULL), wb_lv_tonumber(a[1], NULL)));
}
static wb_lv_value m_modf(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)ud;
    if (n < 1) return wb_lv_nil();
    double d = wb_lv_tonumber(a[0], NULL);
    double ip;
    double fp = modf(d, &ip);
    wb_lv_value out[2];
    out[0] = wb_lv_number(ip);
    out[1] = wb_lv_number(fp);
    return wb_lv_multi_value(L, out, 2);
}
static wb_lv_value m_tointeger(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)ud;
    if (n < 1) return wb_lv_nil();
    int ok; int64_t v = wb_lv_tointeger(a[0], &ok);
    return ok ? wb_lv_integer(v) : wb_lv_nil();
}
static wb_lv_value m_type(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)ud;
    if (n < 1) return wb_lv_nil();
    if (a[0].t == WB_LV_INT) return wb_lv_str("integer");
    if (a[0].t == WB_LV_NUM) return wb_lv_str("float");
    return wb_lv_nil();
}
static wb_lv_value m_ult(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)ud;
    if (n < 2) return wb_lv_nil();
    int ok1, ok2;
    int64_t x = wb_lv_tointeger(a[0], &ok1), y = wb_lv_tointeger(a[1], &ok2);
    if (!ok1 || !ok2) return wb_lv_nil();
    return wb_lv_bool((uint64_t)x < (uint64_t)y);
}
static wb_lv_value m_random(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    /* xoshiro256** */
    uint64_t *st = L->rand_state;
    uint64_t r = st[0] * 5;
    r = ((r >> 7) * 5) * 9;
    uint64_t t = (uint64_t)(st[1] << 17);
    st[1] ^= st[0];
    st[0] ^= t;
    st[0] = (st[0] << 45) | (st[0] >> 19);
    r = ((r >> 7) * 9) & 0xFFFFFFFFFFFFFFFFULL;

    if (n == 0) return wb_lv_number((double)r / 18446744073709551616.0);
    /* math.random(m)      -> 1..m
     * math.random(m, n)   -> m..n   (m is the LOWER bound) */
    int ok1 = 0, ok2 = 0;
    int64_t lo = wb_lv_tointeger(a[0], &ok1);
    int64_t hi = lo;
    if (n >= 2) { hi = wb_lv_tointeger(a[1], &ok2); if (!ok2) hi = lo; }
    if (!ok1) { wb_lv_error(L, "bad argument to 'random' (number expected)"); return wb_lv_nil(); }
    if (lo > hi) { wb_lv_error(L, "bad argument to 'random' (interval is empty)"); return wb_lv_nil(); }
    uint64_t span = (uint64_t)(hi - lo) + 1;
    return wb_lv_integer(lo + (int64_t)(r % span));
}

static wb_lv_value m_randomseed(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    int64_t s = (n >= 1) ? wb_lv_tointeger(a[0], NULL) : 0;
    L->rand_state[0] = (uint64_t)s;
    L->rand_state[1] = (uint64_t)s ^ 0x9E3779B97F4A7C15ULL;
    if (L->rand_state[0] == 0 && L->rand_state[1] == 0) L->rand_state[0] = 1;
    return wb_lv_nil();
}

static wb_lv_value m_trig1(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)ud;
    if (n < 1) return wb_lv_nil();
    return wb_lv_number(((double (*)(double))ud)(wb_lv_tonumber(a[0], NULL)));
}
static wb_lv_value m_deg(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    if (n < 1) return wb_lv_nil();
    return wb_lv_number(wb_lv_tonumber(a[0], NULL) * (180.0 / 3.14159265358979323846));
}
static wb_lv_value m_rad(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    if (n < 1) return wb_lv_nil();
    return wb_lv_number(wb_lv_tonumber(a[0], NULL) * (3.14159265358979323846 / 180.0));
}
static wb_lv_value m_exp(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    if (n < 1) return wb_lv_nil();
    return wb_lv_number(exp(wb_lv_tonumber(a[0], NULL)));
}
static wb_lv_value m_log10(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    if (n < 1) return wb_lv_nil();
    return wb_lv_number(log10(wb_lv_tonumber(a[0], NULL)));
}
static wb_lv_value m_pow(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)ud;
    if (n < 2) return wb_lv_nil();
    return wb_lv_number(pow(wb_lv_tonumber(a[0], NULL), wb_lv_tonumber(a[1], NULL)));
}
static wb_lv_value m_log(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)ud;
    if (n < 1) return wb_lv_nil();
    double x = wb_lv_tonumber(a[0], NULL);
    if (n >= 2) {
        double b = wb_lv_tonumber(a[1], NULL);
        return wb_lv_number(log(x) / log(b));
    }
    return wb_lv_number(log(x));
}

void wb_lv_register_math(wb_lv *L) {
    wb_lv_table *t = wb_lv_table_new(L, 0, 24);
#define M(name, fn) { wb_lv_value v; v.t = WB_LV_CFUNCTION; v.as.cf.f = fn; v.as.cf.ud = NULL; \
    wb_lv_table_set(L, t, wb_lv_str(name), v); }
    M("floor", m_floor); M("ceil", m_ceil); M("abs", m_abs); M("sqrt", m_sqrt);
    M("max", m_max); M("min", m_min); M("fmod", m_fmod); M("modf", m_modf);
    M("tointeger", m_tointeger); M("type", m_type); M("ult", m_ult);
    M("random", m_random); M("randomseed", m_randomseed);
    M("pow", m_pow); M("log", m_log);
    M("sin", m_trig1); M("cos", m_trig1); M("tan", m_trig1);
    M("asin", m_trig1); M("acos", m_trig1); M("atan", m_trig1);
    M("sinh", m_trig1); M("cosh", m_trig1); M("tanh", m_trig1);
    M("deg", m_deg); M("rad", m_rad); M("exp", m_exp); M("log10", m_log10);
#undef M
    /* trig handlers dispatch the libc function through ud */
#define TU(name, libfn) { wb_lv_value v; v.t = WB_LV_CFUNCTION; v.as.cf.f = m_trig1; v.as.cf.ud = (void *)(libfn); \
    wb_lv_table_set(L, t, wb_lv_str(name), v); }
    TU("sin", sin); TU("cos", cos); TU("tan", tan);
    TU("asin", asin); TU("acos", acos); TU("atan", atan);
    TU("sinh", sinh); TU("cosh", cosh); TU("tanh", tanh);
#undef TU
    /* constants: pi, huge, maxinteger, mininteger, inf */
    { wb_lv_value v; v.t = WB_LV_NUM; v.as.n = 3.14159265358979323846;
      wb_lv_table_set(L, t, wb_lv_str("pi"), v); }
    { wb_lv_value v; v.t = WB_LV_NUM; v.as.n = HUGE_VAL;
      wb_lv_table_set(L, t, wb_lv_str("huge"), v); }
    { wb_lv_value v; v.t = WB_LV_INT; v.as.i = INT64_MAX;
      wb_lv_table_set(L, t, wb_lv_str("maxinteger"), v);
      v.as.i = INT64_MIN;
      wb_lv_table_set(L, t, wb_lv_str("mininteger"), v); }
#define T(name, fn) { wb_lv_value v; v.t = WB_LV_CFUNCTION; v.as.cf.f = m_trig1; v.as.cf.ud = (void *)(fn); \
    wb_lv_table_set(L, t, wb_lv_str(name), v); }
    T("sin", sin); T("cos", cos); T("tan", tan);
    T("asin", asin); T("acos", acos); T("exp", exp);
#undef T
    { wb_lv_value v; v.t = WB_LV_CFUNCTION; v.as.cf.f = m_trig1; v.as.cf.ud = (void *)atan;
      wb_lv_table_set(L, t, wb_lv_str("atan"), v); }
    wb_lv_table_set(L, t, wb_lv_str("pi"), wb_lv_number(3.141592653589793));
    wb_lv_table_set(L, t, wb_lv_str("huge"), wb_lv_number(HUGE_VAL));
    wb_lv_table_set(L, t, wb_lv_str("maxinteger"), wb_lv_integer(INT64_MAX));
    wb_lv_table_set(L, t, wb_lv_str("mininteger"), wb_lv_integer(INT64_MIN));
    wb_lv_value tv; tv.t = WB_LV_TABLE; tv.as.tab = t;
    wb_lv_set_global(L, "math", tv);
}

/* ------------------------------------------------------------------ os */

static wb_lv_value o_time(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)a; (void)n; (void)ud;
    return wb_lv_integer((int64_t)time(NULL));
}
static wb_lv_value o_clock(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)a; (void)n; (void)ud;
    return wb_lv_number((double)clock() / CLOCKS_PER_SEC);
}
static wb_lv_value o_date(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    const char *fmt = (n >= 1 && a[0].t == WB_LV_STR) ? a[0].as.s : "%c";
    time_t t = (n >= 2) ? (time_t)wb_lv_tointeger(a[1], NULL) : time(NULL);
    struct tm tmv;
    time_t tt = t;
    struct tm *r = localtime(&tt);
    if (r) tmv = *r;
    char buf[512];
    strftime(buf, sizeof buf, fmt, &tmv);
    return wb_lv_str(wb_lv_intern(L, buf, strlen(buf)));
}
static wb_lv_value o_getenv(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    if (n < 1 || a[0].t != WB_LV_STR) return wb_lv_nil();
    const char *v = getenv(a[0].as.s);
    return v ? wb_lv_str(wb_lv_intern(L, v, strlen(v))) : wb_lv_nil();
}
static wb_lv_value o_exit(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)ud;
    int code = (n >= 1) ? (int)wb_lv_tointeger(a[0], NULL) : 0;
    exit(code);
}
static wb_lv_value o_remove(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    if (n < 1 || a[0].t != WB_LV_STR) return wb_lv_nil();
    int r = remove(a[0].as.s);
    if (r == 0) return wb_lv_bool(1);
    wb_lv_value out[2]; out[0] = wb_lv_nil(); out[1] = wb_lv_str(a[0].as.s);
    return wb_lv_multi_value(L, out, 2);
}
static wb_lv_value o_rename(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    if (n < 2) return wb_lv_nil();
    int r = rename(a[0].as.s, a[1].as.s);
    if (r == 0) return wb_lv_bool(1);
    wb_lv_value out[2]; out[0] = wb_lv_nil(); out[1] = wb_lv_str(a[0].as.s);
    return wb_lv_multi_value(L, out, 2);
}
static wb_lv_value o_tmpname(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)a; (void)n; (void)ud;
    char buf[64];
    snprintf(buf, sizeof buf, "wb_lv_tmp_%ld", (long)time(NULL));
    return wb_lv_str(wb_lv_intern(L, buf, strlen(buf)));
}

static wb_lv_value o_execute(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)ud;
    if (n < 1 || a[0].t != WB_LV_STR) return wb_lv_nil();
    int r = system(a[0].as.s);
    wb_lv_value out[3];
    out[0] = wb_lv_bool(r == 0);
    out[1] = wb_lv_str(wb_lv_intern(L, "exit", 4));
    out[2] = wb_lv_integer(r);
    return wb_lv_multi_value(L, out, 3);
}
static wb_lv_value o_difftime(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)ud;
    double t2 = (n >= 1) ? wb_lv_tonumber(a[0], NULL) : 0;
    double t1 = (n >= 2) ? wb_lv_tonumber(a[1], NULL) : 0;
    return wb_lv_number(t2 - t1);
}
static wb_lv_value o_setlocale(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    if (n < 1 || a[0].t == WB_LV_NIL) return wb_lv_str(wb_lv_intern(L, "C", 1));
    return wb_lv_str(wb_lv_intern(L, "C", 1));
}

void wb_lv_register_os(wb_lv *L) {
    wb_lv_table *t = wb_lv_table_new(L, 0, 8);
#define O(name, fn) { wb_lv_value v; v.t = WB_LV_CFUNCTION; v.as.cf.f = fn; v.as.cf.ud = NULL; \
    wb_lv_table_set(L, t, wb_lv_str(name), v); }
    O("time", o_time); O("clock", o_clock); O("date", o_date);
    O("getenv", o_getenv); O("exit", o_exit);
    O("remove", o_remove); O("rename", o_rename); O("tmpname", o_tmpname);
    O("execute", o_execute); O("difftime", o_difftime); O("setlocale", o_setlocale);
#undef O
    wb_lv_value tv; tv.t = WB_LV_TABLE; tv.as.tab = t;
    wb_lv_set_global(L, "os", tv);
}

/* ------------------------------------------------------------------ io */
/*
 * A file handle is a table { f = <int64 pointer>, closed = bool }. The methods
 * live on a shared metatable keyed by "f", so there is no userdata type.
 */

static FILE *get_file(wb_lv *L, wb_lv_value v, int argn) {
    if (v.t != WB_LV_TABLE) {
        wb_lv_error(L, "bad argument #%d to 'read' (file expected)", argn);
        return NULL;
    }
    wb_lv_value f = wb_lv_table_get(L, v.as.tab, wb_lv_str("f"));
    if (f.t != WB_LV_INT) {
        wb_lv_error(L, "bad argument #%d (file expected)", argn);
        return NULL;
    }
    return (FILE *)(intptr_t)f.as.i;
}

static wb_lv_value i_open(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    if (n < 1 || a[0].t != WB_LV_STR) {
        wb_lv_error(L, "bad argument #1 to 'open' (string expected)");
        return wb_lv_nil();
    }
    const char *mode = (n >= 2 && a[1].t == WB_LV_STR) ? a[1].as.s : "r";
    FILE *f = fopen(a[0].as.s, mode);
    if (!f) {
        wb_lv_value out[2];
        out[0] = wb_lv_nil();
        out[1] = wb_lv_str(wb_lv_intern(L, "cannot open file", 17));
        return wb_lv_multi_value(L, out, 2);
    }
    wb_lv_table *t = wb_lv_table_new(L, 0, 4);
    wb_lv_table_set(L, t, wb_lv_str("f"), wb_lv_integer((int64_t)(intptr_t)f));
    wb_lv_table_set(L, t, wb_lv_str("closed"), wb_lv_bool(0));
    /* the file metatable is registered by wb_lv_register_io; look it up as a
     * global so the handle carries its methods */
    wb_lv_value mt = wb_lv_get_global(L, "__file_meta");
    if (mt.t == WB_LV_TABLE) wb_lv_setmetatable(L, t, mt.as.tab);
    wb_lv_value r; r.t = WB_LV_TABLE; r.as.tab = t;
    return r;
}

static wb_lv_value f_close(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    FILE *f = get_file(L, (n >= 1) ? a[0] : wb_lv_nil(), 1);
    if (!f) return wb_lv_nil();
    fclose(f);
    if (n >= 1 && a[0].t == WB_LV_TABLE)
        wb_lv_table_set(L, a[0].as.tab, wb_lv_str("closed"), wb_lv_bool(1));
    return wb_lv_bool(1);
}

static wb_lv_value f_write(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    FILE *f = get_file(L, (n >= 1) ? a[0] : wb_lv_nil(), 1);
    if (!f) return wb_lv_nil();
    for (int i = 1; i < n; i++) {
        wb_lv_value v = a[i];
        if (v.t == WB_LV_STR) fputs(v.as.s, f);
        else if (v.t == WB_LV_INT || v.t == WB_LV_NUM) fputs(wb_lv_tostring_intern(L, v), f);
    }
    return a[0];
}

static wb_lv_value f_read(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    FILE *f = get_file(L, (n >= 1) ? a[0] : wb_lv_nil(), 1);
    if (!f) return wb_lv_nil();
    const char *fmt = (n >= 2 && a[1].t == WB_LV_STR) ? a[1].as.s : "l";
    if (fmt[0] == 'n') {   /* read a number */
        double d;
        if (fscanf(f, "%lf", &d) == 1) return wb_lv_number(d);
        return wb_lv_nil();
    }
    if (fmt[0] == 'a') {   /* read the whole file */
        char *buf = NULL; size_t len = 0, cap = 0; int c;
        while ((c = fgetc(f)) != EOF) {
            if (len + 1 >= cap) { cap = cap ? cap * 2 : 256; buf = (char *)realloc(buf, cap); }
            buf[len++] = (char)c;
        }
        wb_lv_value r = wb_lv_str(wb_lv_intern(L, buf ? buf : "", len));
        free(buf);
        return r;
    }
    /* "l" (default) or "L": one line */
    char buf[4096];
    if (!fgets(buf, sizeof buf, f)) return wb_lv_nil();
    size_t l = strlen(buf);
    if (l && buf[l-1] == '\n') buf[--l] = 0;
    return wb_lv_strl(wb_lv_intern(L, buf, l), l);
}

static wb_lv_value f_seek(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    FILE *f = get_file(L, (n >= 1) ? a[0] : wb_lv_nil(), 1);
    if (!f) return wb_lv_nil();
    const char *whence = (n >= 3 && a[2].t == WB_LV_STR) ? a[2].as.s : "cur";
    int w = SEEK_SET;
    if (whence[0] == 'c') w = SEEK_CUR;
    else if (whence[0] == 'e') w = SEEK_END;
    long off = (n >= 2) ? (long)wb_lv_tointeger(a[1], NULL) : 0;
    if (fseek(f, off, w) != 0) return wb_lv_nil();
    return wb_lv_integer((int64_t)ftell(f));
}

typedef struct { FILE *f; } line_state;

static wb_lv_value iter_lines(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)a; (void)n;
    line_state *st = (line_state *)ud;
    if (!st || !st->f) return wb_lv_nil();
    char buf[4096];
    if (!fgets(buf, sizeof buf, st->f)) { fclose(st->f); st->f = NULL; return wb_lv_nil(); }
    size_t l = strlen(buf);
    if (l && buf[l-1] == '\n') buf[--l] = 0;
    return wb_lv_strl(wb_lv_intern(L, buf, l), l);
}

static wb_lv_value i_lines(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    FILE *f = stdin;
    line_state *st = NULL;
    if (n >= 1 && a[0].t == WB_LV_STR) {
        f = fopen(a[0].as.s, "r");
        if (!f) { wb_lv_error(L, "cannot open file"); return wb_lv_nil(); }
    }
    st = (line_state *)calloc(1, sizeof *st);
    st->f = f;
    wb_lv_value it; it.t = WB_LV_CFUNCTION; it.as.cf.f = iter_lines; it.as.cf.ud = st;
    return it;
}

static wb_lv_value i_write(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    for (int i = 0; i < n; i++) {
        wb_lv_value v = a[i];
        if (v.t == WB_LV_STR) fwrite(v.as.s, 1, v.slen, stdout);
        else if (v.t == WB_LV_INT || v.t == WB_LV_NUM) fputs(wb_lv_tostring_intern(L, v), stdout);
    }
    return wb_lv_nil();
}

static wb_lv_value i_read(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    char buf[4096];
    if (!fgets(buf, sizeof buf, stdin)) return wb_lv_nil();
    size_t l = strlen(buf);
    if (l && buf[l-1] == '\n') buf[--l] = 0;
    return wb_lv_strl(wb_lv_intern(L, buf, l), l);
}

static wb_lv_value i_input(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)a; (void)n; (void)ud;
    return wb_lv_nil();   /* the default input handle */
}
static wb_lv_value i_output(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)a; (void)n; (void)ud;
    return wb_lv_nil();   /* the default output handle */
}
static wb_lv_value i_close(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    if (n < 1 || a[0].t != WB_LV_TABLE) return wb_lv_nil();
    FILE *f = get_file(L, a[0], 1);
    if (!f) return wb_lv_nil();
    fclose(f);
    wb_lv_table_set(L, a[0].as.tab, wb_lv_str("closed"), wb_lv_bool(1));
    return wb_lv_bool(1);
}
static wb_lv_value i_type(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    if (n < 1 || a[0].t != WB_LV_TABLE) return wb_lv_nil();
    wb_lv_value f = wb_lv_table_get(L, a[0].as.tab, wb_lv_str("f"));
    if (f.t != WB_LV_INT) return wb_lv_nil();
    wb_lv_value closed = wb_lv_table_get(L, a[0].as.tab, wb_lv_str("closed"));
    return wb_lv_str(wb_lv_intern(L, wb_lv_truthy(closed) ? "closed file" : "file", 5));
}
static wb_lv_value i_popen(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)a; (void)n; (void)ud;
    return wb_lv_nil();
}
static wb_lv_value i_tmpfile(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)a; (void)n; (void)ud;
    return wb_lv_nil();
}

/* ---------------------------------------------------------------- debug */

static wb_lv_value d_traceback(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    const char *msg = (n >= 1 && a[0].t == WB_LV_STR) ? a[0].as.s : "";
    char buf[1024];
    snprintf(buf, sizeof buf, "%s\nstack traceback:\n", msg);
    return wb_lv_str(wb_lv_intern(L, buf, strlen(buf)));
}
static wb_lv_value d_getinfo(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)a; (void)ud;
    wb_lv_table *t = wb_lv_table_new(L, 0, 8);
    wb_lv_table_set(L, t, wb_lv_str("currentline"), wb_lv_integer(L->curline));
    wb_lv_table_set(L, t, wb_lv_str("source"), wb_lv_str(L->chunkname ? L->chunkname : "?"));
    wb_lv_table_set(L, t, wb_lv_str("short_src"), wb_lv_str(L->chunkname ? L->chunkname : "?"));
    wb_lv_table_set(L, t, wb_lv_str("what"), wb_lv_str("Lua"));
    wb_lv_table_set(L, t, wb_lv_str("linedefined"), wb_lv_integer(0));
    wb_lv_table_set(L, t, wb_lv_str("lastlinedefined"), wb_lv_integer(0));
    wb_lv_value r; r.t = WB_LV_TABLE; r.as.tab = t;
    return r;
}
static wb_lv_value d_getlocal(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)a; (void)n; (void)ud;
    return wb_lv_nil();
}
static wb_lv_value d_sethook(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)a; (void)n; (void)ud;
    return wb_lv_nil();
}
static wb_lv_value d_gethook(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)a; (void)n; (void)ud;
    return wb_lv_bool(0);
}
static wb_lv_value d_getmetatable(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    if (n < 1) return wb_lv_nil();
    wb_lv_table *m = (a[0].t == WB_LV_STR) ? L->string_meta : wb_lv_getmetatable(L, a[0]);
    if (!m) return wb_lv_nil();
    wb_lv_value r; r.t = WB_LV_TABLE; r.as.tab = m;
    return r;
}
static wb_lv_value d_setmetatable(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    if (n >= 2 && a[0].t == WB_LV_TABLE) {
        wb_lv_setmetatable(L, a[0].as.tab, (a[1].t == WB_LV_TABLE) ? a[1].as.tab : NULL);
    }
    return (n >= 1) ? a[0] : wb_lv_nil();
}
static wb_lv_value d_getupvalue(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)a; (void)n; (void)ud;
    return wb_lv_nil();
}
static wb_lv_value d_upvalueid(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)a; (void)n; (void)ud;
    return wb_lv_nil();
}
static wb_lv_value d_upvaluejoin(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)a; (void)n; (void)ud;
    return wb_lv_nil();
}
static wb_lv_value d_getregistry(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)a; (void)n; (void)ud;
    return wb_lv_nil();
}
static wb_lv_value d_traceback_noop(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)a; (void)n; (void)ud;
    return wb_lv_nil();
}

void wb_lv_register_debug(wb_lv *L) {
    wb_lv_table *t = wb_lv_table_new(L, 0, 8);
#define D(name, fn) { wb_lv_value v; v.t = WB_LV_CFUNCTION; v.as.cf.f = fn; v.as.cf.ud = NULL; \
    wb_lv_table_set(L, t, wb_lv_str(name), v); }
    D("traceback", d_traceback); D("getinfo", d_getinfo); D("getlocal", d_getlocal);
    D("sethook", d_sethook); D("gethook", d_gethook);
    D("getmetatable", d_getmetatable); D("setmetatable", d_setmetatable);
    D("getupvalue", d_getupvalue); D("setupvalue", d_traceback_noop);
    D("upvalueid", d_upvalueid); D("upvaluejoin", d_upvaluejoin);
    D("getregistry", d_getregistry);
#undef D
    wb_lv_value tv; tv.t = WB_LV_TABLE; tv.as.tab = t;
    wb_lv_set_global(L, "debug", tv);
}

/* ---------------------------------------------------------------- utf8 */

/* Decode one UTF-8 sequence at s[i]; returns its length, or 0 for invalid. */
static int utf8_decode(const unsigned char *s, size_t len, size_t i, long *cp) {
    if (i >= len) return 0;
    unsigned c = s[i];
    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0 && i + 1 < len) {
        *cp = ((long)(c & 0x1F) << 6) | (s[i+1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && i + 2 < len) {
        *cp = ((long)(c & 0x0F) << 12) | ((long)(s[i+1] & 0x3F) << 6) | (s[i+2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && i + 3 < len) {
        *cp = ((long)(c & 0x07) << 18) | ((long)(s[i+1] & 0x3F) << 12) |
              ((long)(s[i+2] & 0x3F) << 6) | (s[i+3] & 0x3F);
        return 4;
    }
    return 0;
}

static wb_lv_value u_len(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    if (n < 1 || a[0].t != WB_LV_STR) return wb_lv_nil();
    const unsigned char *s = (const unsigned char *)a[0].as.s;
    size_t ls = strlen(a[0].as.s);
    int64_t i = (n >= 2) ? wb_lv_tointeger(a[1], NULL) : 1;
    int64_t j = (n >= 3) ? wb_lv_tointeger(a[2], NULL) : (int64_t)ls;
    if (i < 1) i = 1;
    if (j > (int64_t)ls) j = (int64_t)ls;
    int64_t count = 0;
    for (int64_t k = i - 1; k < j; ) {
        long cp;
        int l = utf8_decode(s, ls, (size_t)k, &cp);
        if (l == 0) {
            wb_lv_value out[2]; out[0] = wb_lv_nil(); out[1] = wb_lv_integer(k + 1);
            return wb_lv_multi_value(L, out, 2);
        }
        k += l; count++;
    }
    return wb_lv_integer(count);
}

static wb_lv_value u_char(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    char buf[256]; int k = 0;
    for (int i = 0; i < n && k < 250; i++) {
        long cp = (long)wb_lv_tointeger(a[i], NULL);
        if (cp < 0x80) buf[k++] = (char)cp;
        else if (cp < 0x800) {
            buf[k++] = (char)(0xC0 | (cp >> 6));
            buf[k++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            buf[k++] = (char)(0xE0 | (cp >> 12));
            buf[k++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[k++] = (char)(0x80 | (cp & 0x3F));
        } else {
            buf[k++] = (char)(0xF0 | (cp >> 18));
            buf[k++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            buf[k++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[k++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    return wb_lv_str(wb_lv_intern(L, buf, (size_t)k));
}

static wb_lv_value u_codepoint(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    if (n < 1 || a[0].t != WB_LV_STR) return wb_lv_nil();
    const unsigned char *s = (const unsigned char *)a[0].as.s;
    size_t ls = strlen(a[0].as.s);
    int64_t i = (n >= 2) ? wb_lv_tointeger(a[1], NULL) : 1;
    int64_t j = (n >= 3) ? wb_lv_tointeger(a[2], NULL) : i;
    if (i < 1) i = 1;
    if (j > (int64_t)ls) j = (int64_t)ls;
    int cnt = (int)(j - i + 1);
    if (cnt <= 0) return wb_lv_nil();
    if (cnt > 64) cnt = 64;
    wb_lv_value *out = WB_LV_ALLOCN(wb_lv_value, cnt, L);
    int k = 0;
    for (int64_t p = i - 1; p < j && k < cnt; ) {
        long cp;
        int l = utf8_decode(s, ls, (size_t)p, &cp);
        if (l == 0) break;
        out[k++] = wb_lv_integer(cp);
        p += l;
    }
    return wb_lv_multi_value(L, out, k);
}

static wb_lv_value u_offset(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    if (n < 2 || a[0].t != WB_LV_STR) return wb_lv_nil();
    const unsigned char *s = (const unsigned char *)a[0].as.s;
    size_t ls = strlen(a[0].as.s);
    int64_t nreq = wb_lv_tointeger(a[1], NULL);
    int64_t i = (n >= 3) ? wb_lv_tointeger(a[2], NULL) : (nreq >= 0 ? 1 : (int64_t)ls + 1);
    if (i < 1) i = 1;
    if (i > (int64_t)ls + 1) i = (int64_t)ls + 1;
    if (nreq == 0) {
        for (int64_t p = i - 1; p < (int64_t)ls; ) {
            long cp; int l = utf8_decode(s, ls, (size_t)p, &cp);
            if (l == 0) { p++; continue; }
            if (p + 1 == i - 1) return wb_lv_integer(p + 1);
            p += l;
        }
        return wb_lv_integer((int64_t)ls + 1);
    }
    int64_t count = 0;
    for (int64_t p = i - 1; p < (int64_t)ls; ) {
        long cp; int l = utf8_decode(s, ls, (size_t)p, &cp);
        if (l == 0) return wb_lv_nil();
        count++;
        if (count == nreq) return wb_lv_integer(p + 1);
        p += l;
    }
    return wb_lv_nil();
}

typedef struct { const char *s; size_t ls; int64_t pos; } ucodes_state;

static wb_lv_value iter_ucodes(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)a; (void)n;
    ucodes_state *st = (ucodes_state *)ud;
    if (!st || st->pos >= (int64_t)st->ls) return wb_lv_nil();
    long cp;
    int l = utf8_decode((const unsigned char *)st->s, st->ls, (size_t)st->pos, &cp);
    if (l == 0) { st->pos++; return iter_ucodes(L, a, n, ud); }
    wb_lv_value out[2];
    out[0] = wb_lv_integer(st->pos + 1);
    out[1] = wb_lv_integer(cp);
    st->pos += l;
    return wb_lv_multi_value(L, out, 2);
}

static wb_lv_value u_codes(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    if (n < 1 || a[0].t != WB_LV_STR) return wb_lv_nil();
    ucodes_state *st = (ucodes_state *)calloc(1, sizeof *st);
    st->s = a[0].as.s; st->ls = strlen(a[0].as.s); st->pos = 0;
    wb_lv_value it; it.t = WB_LV_CFUNCTION; it.as.cf.f = iter_ucodes; it.as.cf.ud = st;
    return wb_lv_multi_value(L, (wb_lv_value[]){ it, a[0], wb_lv_integer(0) }, 3);
}

void wb_lv_register_utf8(wb_lv *L) {
    wb_lv_table *t = wb_lv_table_new(L, 0, 8);
#define U(name, fn) { wb_lv_value v; v.t = WB_LV_CFUNCTION; v.as.cf.f = fn; v.as.cf.ud = NULL; \
    wb_lv_table_set(L, t, wb_lv_str(name), v); }
    U("len", u_len); U("char", u_char); U("codepoint", u_codepoint);
    U("offset", u_offset); U("codes", u_codes);
#undef U
    wb_lv_value tv; tv.t = WB_LV_TABLE; tv.as.tab = t;
    wb_lv_set_global(L, "utf8", tv);
}

void wb_lv_register_io(wb_lv *L) {
    wb_lv_table *t = wb_lv_table_new(L, 0, 8);
#define I(name, fn) { wb_lv_value v; v.t = WB_LV_CFUNCTION; v.as.cf.f = fn; v.as.cf.ud = NULL; \
    wb_lv_table_set(L, t, wb_lv_str(name), v); }
    I("write", i_write); I("read", i_read); I("open", i_open); I("lines", i_lines);
    I("input", i_input); I("output", i_output); I("close", i_close);
    I("type", i_type); I("popen", i_popen); I("tmpfile", i_tmpfile);
    { wb_lv_value v; v.t = WB_LV_TABLE; v.as.tab = wb_lv_table_new(L,0,2);
      wb_lv_table_set(L, v.as.tab, wb_lv_str("f"), wb_lv_integer((int64_t)(intptr_t)stdin));
      wb_lv_table_set(L, v.as.tab, wb_lv_str("closed"), wb_lv_bool(0));
      wb_lv_table_set(L, t, wb_lv_str("stdin"), v); }
    { wb_lv_value v; v.t = WB_LV_TABLE; v.as.tab = wb_lv_table_new(L,0,2);
      wb_lv_table_set(L, v.as.tab, wb_lv_str("f"), wb_lv_integer((int64_t)(intptr_t)stdout));
      wb_lv_table_set(L, v.as.tab, wb_lv_str("closed"), wb_lv_bool(0));
      wb_lv_table_set(L, t, wb_lv_str("stdout"), v); }
    { wb_lv_value v; v.t = WB_LV_TABLE; v.as.tab = wb_lv_table_new(L,0,2);
      wb_lv_table_set(L, v.as.tab, wb_lv_str("f"), wb_lv_integer((int64_t)(intptr_t)stderr));
      wb_lv_table_set(L, v.as.tab, wb_lv_str("closed"), wb_lv_bool(0));
      wb_lv_table_set(L, t, wb_lv_str("stderr"), v); }
#undef I
    wb_lv_value tv; tv.t = WB_LV_TABLE; tv.as.tab = t;
    wb_lv_set_global(L, "io", tv);

    /* the file metatable: file:read / write / close / seek / lines */
    wb_lv_table *fm = wb_lv_table_new(L, 0, 6);
#define M(name, fn) { wb_lv_value v; v.t = WB_LV_CFUNCTION; v.as.cf.f = fn; v.as.cf.ud = NULL; \
    wb_lv_table_set(L, fm, wb_lv_str(name), v); }
    M("read", f_read); M("write", f_write); M("close", f_close); M("seek", f_seek);
    M("lines", i_lines);
#undef M
    wb_lv_value idx; idx.t = WB_LV_TABLE; idx.as.tab = fm;
    wb_lv_table_set(L, fm, wb_lv_str("__index"), idx);
    wb_lv_value mv; mv.t = WB_LV_TABLE; mv.as.tab = fm;
    wb_lv_set_global(L, "__file_meta", mv);
}
