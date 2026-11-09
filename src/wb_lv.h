/*
 * wb_lv.h -- WuBu LV: slerm-Lua.
 *
 * A from-scratch Lua 5.4 reimplementation for C11 speed + control: the
 * substrate that lets people write mods and lets other games be slermed into
 * the world space. No third-party runtime, no GC dependency -- one arena.
 *
 * The language surface is Lua 5.4: integers and floats, the full operator
 * set, multiple returns, varargs, metatables and metamethods, `goto`, and
 * pcall-based error isolation. The standard library is the base library plus
 * string (with Lua patterns), table, math, and a minimal os/io.
 *
 * Coroutines are not implemented; they need a separate execution model.
 */
#ifndef WB_LV_H
#define WB_LV_H

#include <stdint.h>
#include <stddef.h>

typedef struct wb_lv       wb_lv;
typedef struct wb_lv_value wb_lv_value;
typedef struct wb_lv_func  wb_lv_func;
typedef struct wb_lv_table wb_lv_table;

typedef wb_lv_value (*wb_lv_cfn)(wb_lv *L, wb_lv_value *args, int nargs, void *ud);

/* --------------------------------------------------------------- values */
typedef enum {
    WB_LV_NIL = 0, WB_LV_BOOL, WB_LV_INT, WB_LV_NUM, WB_LV_STR, WB_LV_TABLE,
    WB_LV_FUNCTION, WB_LV_CFUNCTION, WB_LV_MULTI,
} wb_lv_type;

struct wb_lv_value {
    wb_lv_type t;
    union {
        int         b;
        int64_t     i;
        double      n;
        const char *s;
        wb_lv_table *tab;
        wb_lv_func  *fn;
        struct { wb_lv_cfn f; void *ud; } cf;
        struct wb_lv_multi *multi;
    } as;
};

#define wb_lv_nil()      ((wb_lv_value){.t=WB_LV_NIL})
#define wb_lv_bool(B)    ((wb_lv_value){.t=WB_LV_BOOL, .as.b=(B)})
#define wb_lv_integer(N) ((wb_lv_value){.t=WB_LV_INT,  .as.i=(int64_t)(N)})
#define wb_lv_number(N)  ((wb_lv_value){.t=WB_LV_NUM,  .as.n=(N)})
#define wb_lv_str(S)     ((wb_lv_value){.t=WB_LV_STR,  .as.s=(S)})

int    wb_lv_truthy(wb_lv_value v);
int    wb_lv_equal(wb_lv *L, wb_lv_value a, wb_lv_value b);
int    wb_lv_rawequal(wb_lv_value a, wb_lv_value b);

/* --------------------------------------------------------------- state */
wb_lv *wb_lv_new(void);          /* state + base library                        */
wb_lv *wb_lv_alloc(void);        /* state only (tests / embedders)               */
void   wb_lv_free(wb_lv *L);

const char *wb_lv_intern(wb_lv *L, const char *bytes, size_t len);

/* number / string coercion. `ok` (may be NULL) reports whether the value WAS
 * a number; string parsing happens only where Lua allows it. */
double      wb_lv_tonumber(wb_lv_value v, int *ok);
int64_t     wb_lv_tointeger(wb_lv_value v, int *ok);
const char *wb_lv_tostring(wb_lv_value v);            /* shared buffer; copy */
const char *wb_lv_tostring_intern(wb_lv *L, wb_lv_value v);  /* interned */

/* --------------------------------------------------------------- tables */
wb_lv_table *wb_lv_table_new(wb_lv *L, int narr, int nrec);
void         wb_lv_table_set(wb_lv *L, wb_lv_table *t, wb_lv_value k, wb_lv_value v);
wb_lv_value  wb_lv_table_get(wb_lv *L, const wb_lv_table *t, wb_lv_value k);
size_t       wb_lv_table_len(const wb_lv_table *t);

/* metatables */
wb_lv_table *wb_lv_getmetatable(wb_lv *L, wb_lv_value v);
void         wb_lv_setmetatable(wb_lv *L, wb_lv_table *t, wb_lv_table *meta);
int          wb_lv_has_meta(wb_lv *L, wb_lv_value v, const char *event);

/* --------------------------------------------------------------- driver */
const char *wb_lv_run(wb_lv *L, const char *src, const char *chunkname);
void        wb_lv_register_global(wb_lv *L, const char *name, wb_lv_cfn fn, void *ud);
wb_lv_value wb_lv_get_global(wb_lv *L, const char *name);
void        wb_lv_set_global(wb_lv *L, const char *name, wb_lv_value v);

/* host-driven call: invoke a function value (e.g. step) with args */
wb_lv_value wb_lv_call(wb_lv *L, wb_lv_value fn, wb_lv_value *args, int nargs);

/* --------------------------------------------------------------- library */
void wb_lv_register_builtins(wb_lv *L);   /* base library                      */
void wb_lv_openlibs(wb_lv *L);            /* base + string + table + math + io  */
const char *wb_lv_version(void);

/* error isolation: returns 0 on success, 1 if the call raised. */
int wb_lv_pcall(wb_lv *L, wb_lv_value fn, wb_lv_value *args, int nargs,
                wb_lv_value *out, int *nout);

#endif /* WB_LV_H */
