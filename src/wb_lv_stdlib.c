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

void wb_lv_register_table(wb_lv *L) {
    wb_lv_table *t = wb_lv_table_new(L, 0, 8);
#define T(name, fn) { wb_lv_value v; v.t = WB_LV_CFUNCTION; v.as.cf.f = fn; v.as.cf.ud = NULL; \
    wb_lv_table_set(L, t, wb_lv_str(name), v); }
    T("insert", t_insert);
    T("remove", t_remove);
    T("concat", t_concat);
    T("unpack", t_unpack);
    T("pack", t_pack);
    T("sort", t_sort);
#undef T
    wb_lv_value tv; tv.t = WB_LV_TABLE; tv.as.tab = t;
    wb_lv_set_global(L, "table", tv);
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
    uint64_t *s = L->rand_state;
    uint64_t result = (uint64_t)((s[0] + s[1]) & 0xFFFFFFFFFFFFFFFFULL);
    result = (uint64_t)(((result * 5) & 0xFFFFFFFFFFFFFFFFULL) >> 7) * 9;
    uint64_t t = (uint64_t)(s[1] << 17);
    s[1] ^= s[0]; s[0] ^= t; s[0] = (s[0] << 45) | (s[0] >> 19);
    (void)result;
    uint64_t r = s[0] * 5;
    r = ((r >> 7) * 9) & 0xFFFFFFFFFFFFFFFFULL;
    if (n == 0) return wb_lv_number((double)r / 18446744073709551616.0);
    int64_t lo = (n >= 2) ? wb_lv_tointeger(a[1], NULL) : 1;
    int64_t hi = (n >= 1) ? wb_lv_tointeger(a[0], NULL) : 1;
    if (n >= 2) { lo = wb_lv_tointeger(a[1], NULL); hi = wb_lv_tointeger(a[0], NULL); }
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
#undef M
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

void wb_lv_register_os(wb_lv *L) {
    wb_lv_table *t = wb_lv_table_new(L, 0, 8);
#define O(name, fn) { wb_lv_value v; v.t = WB_LV_CFUNCTION; v.as.cf.f = fn; v.as.cf.ud = NULL; \
    wb_lv_table_set(L, t, wb_lv_str(name), v); }
    O("time", o_time); O("clock", o_clock); O("date", o_date);
    O("getenv", o_getenv); O("exit", o_exit);
    O("remove", o_remove); O("rename", o_rename); O("tmpname", o_tmpname);
#undef O
    wb_lv_value tv; tv.t = WB_LV_TABLE; tv.as.tab = t;
    wb_lv_set_global(L, "os", tv);
}

/* ------------------------------------------------------------------ io */

static wb_lv_value i_write(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    for (int i = 0; i < n; i++) {
        wb_lv_value v = a[i];
        if (v.t == WB_LV_STR) fputs(v.as.s, stdout);
        else fputs(wb_lv_tostring_intern(L, v), stdout);
    }
    return wb_lv_nil();
}
static wb_lv_value i_read(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)ud;
    const char *fmt = (n >= 1 && a[0].t == WB_LV_STR) ? a[0].as.s : "l";
    char buf[4096];
    if (!fgets(buf, sizeof buf, stdin)) return wb_lv_nil();
    size_t l = strlen(buf);
    if (l && buf[l-1] == '\n') buf[--l] = 0;
    (void)fmt;
    return wb_lv_str(wb_lv_intern(L, buf, l));
}

void wb_lv_register_io(wb_lv *L) {
    wb_lv_table *t = wb_lv_table_new(L, 0, 4);
#define I(name, fn) { wb_lv_value v; v.t = WB_LV_CFUNCTION; v.as.cf.f = fn; v.as.cf.ud = NULL; \
    wb_lv_table_set(L, t, wb_lv_str(name), v); }
    I("write", i_write); I("read", i_read);
#undef I
    wb_lv_value tv; tv.t = WB_LV_TABLE; tv.as.tab = t;
    wb_lv_set_global(L, "io", tv);
}
