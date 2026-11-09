/*
 * wb_lv_value.c -- values, string interning and tables. See wb_lv_internal.h.
 *
 * Tables are open-addressed with no deletion: entries are replaced on hit and
 * the table grows by doubling. `#t` is the array-part border, cached and
 * invalidated on every write.
 */
#include "wb_lv_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- values */

int wb_lv_truthy(wb_lv_value v) {
    if (v.t == WB_LV_NIL) return 0;
    if (v.t == WB_LV_BOOL) return v.as.b;
    return 1;
}

int wb_lv_rawequal(wb_lv_value a, wb_lv_value b) {
    if (a.t != b.t) return 0;
    switch (a.t) {
    case WB_LV_NIL: return 1;
    case WB_LV_BOOL: return a.as.b == b.as.b;
    case WB_LV_INT: return a.as.i == b.as.i;
    case WB_LV_NUM: return a.as.n == b.as.n;
    case WB_LV_STR: return a.as.s == b.as.s;
    case WB_LV_TABLE: return a.as.tab == b.as.tab;
    case WB_LV_FUNCTION: return a.as.fn == b.as.fn;
    case WB_LV_CFUNCTION: return a.as.cf.f == b.as.cf.f;
    case WB_LV_MULTI: return 0;
    }
    return 0;
}

/* `==` with `__eq` dispatch for tables/userdata. */
int wb_lv_equal(wb_lv *L, wb_lv_value a, wb_lv_value b) {
    if (a.t == b.t && (a.t == WB_LV_TABLE)) {
        wb_lv_table *ma = wb_lv_getmetatable(L, a);
        wb_lv_table *mb = wb_lv_getmetatable(L, b);
        if (ma && mb && wb_lv_rawequal(wb_lv_table_get(L, ma, wb_lv_str("__eq")),
                                      wb_lv_table_get(L, mb, wb_lv_str("__eq")))) {
            wb_lv_value f = wb_lv_table_get(L, ma, wb_lv_str("__eq"));
            wb_lv_value args[2]; args[0] = a; args[1] = b;
            wb_lv_value r = wb_lv_call(L, f, args, 2);
            return wb_lv_truthy(r);
        }
    }
    if (a.t == WB_LV_INT && b.t == WB_LV_NUM) return (double)a.as.i == b.as.n;
    if (a.t == WB_LV_NUM && b.t == WB_LV_INT) return a.as.n == (double)b.as.i;
    return wb_lv_rawequal(a, b);
}

double wb_lv_tonumber(wb_lv_value v, int *ok) {
    if (v.t == WB_LV_NUM) { if (ok) *ok = 1; return v.as.n; }
    if (v.t == WB_LV_INT) { if (ok) *ok = 1; return (double)v.as.i; }
    if (ok) *ok = 0;
    return 0.0;
}

int64_t wb_lv_tointeger(wb_lv_value v, int *ok) {
    if (v.t == WB_LV_INT) { if (ok) *ok = 1; return v.as.i; }
    if (v.t == WB_LV_NUM) {
        if (v.as.n >= -9.2233720368547758e18 && v.as.n < 9.2233720368547758e18) {
            if (ok) *ok = 1;
            return (int64_t)v.as.n;
        }
    }
    if (ok) *ok = 0;
    return 0;
}

/* Lua 5.4 renders floats with 14 significant digits and integers in full. */
const char *wb_lv_tostring_intern(wb_lv *L, wb_lv_value v) {
    char buf[64];
    switch (v.t) {
    case WB_LV_NIL:   return wb_lv_intern(L, "nil", 3);
    case WB_LV_BOOL:  return wb_lv_intern(L, v.as.b ? "true" : "false", v.as.b ? 4 : 5);
    case WB_LV_INT:   snprintf(buf, sizeof buf, "%lld", (long long)v.as.i); return wb_lv_intern(L, buf, strlen(buf));
    case WB_LV_NUM:
        snprintf(buf, sizeof buf, "%.14g", v.as.n);
        /* Lua renders integral floats with a trailing ".0" */
        if (!strpbrk(buf, ".eEnN")) strncat(buf, ".0", sizeof buf - strlen(buf) - 1);
        return wb_lv_intern(L, buf, strlen(buf));
    case WB_LV_STR:   return v.as.s;
    case WB_LV_TABLE: snprintf(buf, sizeof buf, "table: %p", (void *)v.as.tab); return wb_lv_intern(L, buf, strlen(buf));
    case WB_LV_FUNCTION: snprintf(buf, sizeof buf, "function: %p", (void *)v.as.fn); return wb_lv_intern(L, buf, strlen(buf));
    case WB_LV_CFUNCTION: snprintf(buf, sizeof buf, "function: %p", (void *)v.as.cf.f); return wb_lv_intern(L, buf, strlen(buf));
    case WB_LV_MULTI: return wb_lv_intern(L, "(multi)", 8);
    }
    return wb_lv_intern(L, "(?)", 3);
}

/* Compatibility form: a shared buffer. Callers that need two values at once
 * must copy the first (wb_lv_concat does). */
const char *wb_lv_tostring(wb_lv_value v) {
    static char fb[64];
    if (v.t == WB_LV_STR) return v.as.s;
    if (v.t == WB_LV_INT) { snprintf(fb, sizeof fb, "%lld", (long long)v.as.i); return fb; }
    if (v.t == WB_LV_NUM) {
        snprintf(fb, sizeof fb, "%.14g", v.as.n);
        if (!strpbrk(fb, ".eEnN")) strncat(fb, ".0", sizeof fb - strlen(fb) - 1);
        return fb;
    }
    switch (v.t) {
    case WB_LV_NIL:   return "nil";
    case WB_LV_BOOL:  return v.as.b ? "true" : "false";
    case WB_LV_TABLE: snprintf(fb, sizeof fb, "table: %p", (void *)v.as.tab); return fb;
    case WB_LV_FUNCTION: snprintf(fb, sizeof fb, "function: %p", (void *)v.as.fn); return fb;
    case WB_LV_CFUNCTION: snprintf(fb, sizeof fb, "function: %p", (void *)v.as.cf.f); return fb;
    default: return "(?)";
    }
}

const char *wb_lv_type_name(wb_lv_type t) {
    switch (t) {
    case WB_LV_NIL: return "nil";
    case WB_LV_BOOL: return "boolean";
    case WB_LV_INT:
    case WB_LV_NUM: return "number";
    case WB_LV_STR: return "string";
    case WB_LV_TABLE: return "table";
    case WB_LV_FUNCTION:
    case WB_LV_CFUNCTION: return "function";
    case WB_LV_MULTI: return "multi";
    }
    return "unknown";
}

/* --------------------------------------------------------- string interning */

static unsigned strhash(const char *s, size_t n) {
    unsigned h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h;
}

const char *wb_lv_intern(wb_lv *L, const char *bytes, size_t len) {
    if (!L || !L->buckets) return "";
    unsigned h = strhash(bytes, len), b = h % L->nbuckets;
    for (istr *it = L->buckets[b]; it; it = it->next)
        if (it->len == len && memcmp(it->s, bytes, len) == 0) return it->s;
    istr *n = (istr *)wb_lv_arena_alloc(L, sizeof(*n) + len + 1);
    if (!n) return "";
    n->len = len; n->hash = h; n->s = (char *)(n + 1);
    memcpy(n->s, bytes, len); n->s[len] = 0;
    n->next = L->buckets[b]; L->buckets[b] = n;
    return n->s;
}

/* ---------------------------------------------------------------- tables */

static unsigned valhash(wb_lv_value k) {
    if (k.t == WB_LV_STR) return strhash(k.as.s, strlen(k.as.s));
    if (k.t == WB_LV_NUM) { double d = k.as.n; return (unsigned)((unsigned long long)(d * 2654435761.0)); }
    if (k.t == WB_LV_INT) return (unsigned)((unsigned long long)k.as.i * 2654435761u);
    return (unsigned)(uintptr_t)k.as.tab | (unsigned)(uintptr_t)k.as.fn;
}

static int keyeq(wb_lv_value a, wb_lv_value b) {
    if (a.t != b.t) {
        /* 1 and 1.0 are the same key in Lua */
        if ((a.t == WB_LV_INT && b.t == WB_LV_NUM) || (a.t == WB_LV_NUM && b.t == WB_LV_INT)) {
            double x = (a.t == WB_LV_INT) ? (double)a.as.i : a.as.n;
            double y = (b.t == WB_LV_INT) ? (double)b.as.i : b.as.n;
            return x == y;
        }
        return 0;
    }
    if (a.t == WB_LV_NUM) return a.as.n == b.as.n;
    if (a.t == WB_LV_INT) return a.as.i == b.as.i;
    /* content equality: callers may hand us a non-interned C string */
    if (a.t == WB_LV_STR) return strcmp(a.as.s, b.as.s) == 0;
    return wb_lv_rawequal(a, b);
}

static void tgrow(wb_lv *L, wb_lv_table *t) {
    size_t nc = t->cap ? t->cap * 2 : 16;
    wb_lv_kv *ne = (wb_lv_kv *)wb_lv_arena_alloc(L, nc * sizeof(*ne));
    if (t->e) for (size_t i = 0; i < t->cap; i++)
        if (t->e[i].used) {
            unsigned hi = valhash(t->e[i].k) % nc, j = hi;
            while (ne[j].used) j = (j + 1) % nc;
            ne[j] = t->e[i];
        }
    t->e = ne; t->cap = nc;
}

static wb_lv_kv *tfind(wb_lv_table *t, wb_lv_value k) {
    if (!t->cap) return NULL;
    unsigned hi = valhash(k) % t->cap, j = hi;
    while (t->e[j].used) {
        if (keyeq(t->e[j].k, k)) return &t->e[j];
        j = (j + 1) % t->cap;
    }
    return NULL;
}

wb_lv_table *wb_lv_table_new(wb_lv *L, int narr, int nrec) {
    (void)narr; (void)nrec;
    return WB_LV_ALLOC(wb_lv_table, L);
}

void wb_lv_table_set(wb_lv *L, wb_lv_table *t, wb_lv_value k, wb_lv_value v) {
    if (k.t == WB_LV_NIL) { wb_lv_error(L, "table index is nil (line %d)", L->curline); return; }
    if (k.t == WB_LV_NUM && isnan(k.as.n)) { wb_lv_error(L, "table index is NaN (line %d)", L->curline); return; }
    if (t->count * 2 >= t->cap) tgrow(L, t);
    wb_lv_kv *f = tfind(t, k);
    if (f) { f->v = v; t->seq_dirty = 1; return; }
    unsigned hi = valhash(k) % t->cap, j = hi;
    while (t->e[j].used) j = (j + 1) % t->cap;
    t->e[j].k = k; t->e[j].v = v; t->e[j].used = 1; t->count++;
    t->seq_dirty = 1;
}

wb_lv_value wb_lv_table_get(wb_lv *L, const wb_lv_table *tt, wb_lv_value k) {
    (void)L;
    wb_lv_kv *f = tfind((wb_lv_table *)tt, k);
    return f ? f->v : wb_lv_nil();
}

/* `#t`: the array-part border. Lua's rule is any n with t[n] ~= nil and
 * t[n+1] == nil; for a sequence that is the length. */
size_t wb_lv_table_len(const wb_lv_table *tt) {
    wb_lv_table *t = (wb_lv_table *)tt;
    if (!t) return 0;
    if (!t->seq_dirty) return t->seq;
    size_t n = 0;
    if (t->cap) {
        /* linear walk from the current hint; good for the common sequence */
        while (n + 1 < t->cap * 2) {
            wb_lv_value k; k.t = WB_LV_INT; k.as.i = (int64_t)(n + 1);
            if (tfind(t, k) == NULL) break;
            n++;
            if (n > t->count) break;
        }
    }
    t->seq = n; t->seq_dirty = 0;
    return n;
}

/* `next` support: iterate the backing array. */
int wb_lv_table_next(wb_lv_table *t, wb_lv_value *k, wb_lv_value *v) {
    if (!t || !t->cap) return 0;
    static size_t slot;   /* single-step iterator; fine for non-reentrant use */
    if (k && k->t != WB_LV_NIL) {
        wb_lv_kv *f = tfind(t, *k);
        if (!f) return 0;
        slot = (size_t)(f - t->e) + 1;
    } else slot = 0;
    for (size_t i = slot; i < t->cap; i++) {
        if (t->e[i].used) { if (k) *k = t->e[i].k; if (v) *v = t->e[i].v; return 1; }
    }
    return 0;
}

/* ------------------------------------------------------------- metatables */

wb_lv_table *wb_lv_getmetatable(wb_lv *L, wb_lv_value v) {
    (void)L;
    if (v.t == WB_LV_TABLE) return v.as.tab->meta;
    if (v.t == WB_LV_STR) return NULL;   /* the string metatable is per-state */
    return NULL;
}

void wb_lv_setmetatable(wb_lv *L, wb_lv_table *t, wb_lv_table *meta) {
    (void)L;
    if (t) t->meta = meta;
}

int wb_lv_has_meta(wb_lv *L, wb_lv_value v, const char *event) {
    wb_lv_table *m = wb_lv_getmetatable(L, v);
    if (!m) return 0;
    return wb_lv_table_get(L, m, wb_lv_str(event)).t != WB_LV_NIL;
}
