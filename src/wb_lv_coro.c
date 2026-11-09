/*
 * wb_lv_coro.c -- coroutines, on Windows fibers.
 *
 * A tree-walker lives on the C stack, so a coroutine needs a stack of its own.
 * Windows fibers give each coroutine one; switching between them is a context
 * switch. Only one coroutine runs at a time, so the interpreter's shared state
 * (value stack, varargs, error flag) is saved and restored around every switch.
 *
 * Not implemented on non-Windows hosts: coroutine.* reports "not supported".
 */
#include "wb_lv_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#define WB_LV_FIBERS 1
#endif

#define CO_SUSPENDED 0
#define CO_RUNNING   1
#define CO_DEAD      2
#define CO_NORMAL    3

/* stack reserved for each coroutine fiber */
#define CO_STACK (512 * 1024)

typedef struct wb_lv_coro {
#ifdef WB_LV_FIBERS
    void *fiber;
#endif
    int state;
    wb_lv *L;
    wb_lv_value fn;
    wb_lv_value *args; int nargs;      /* arguments for the next resume  */
    wb_lv_value *rets; int nrets;      /* values returned by resume()     */
    wb_lv_value *yrets; int nyrets;    /* values passed to yield()         */
    struct wb_lv_coro *resumer;
    struct wb_lv_table *handle_tab;
#ifdef WB_LV_FIBERS
    void *resumer_fiber;
#endif
    int started;
    /* interpreter state saved across a switch */
    int sp, curline, eval_depth, depth;
    wb_lv_value *va; int nva;
    int has_err; int has_err_obj; wb_lv_value err_obj;
    char err[512];
} wb_lv_coro;

/* ------------------------------------------------------------------ state */

#ifdef WB_LV_FIBERS
static void *main_fiber;
#endif
static wb_lv_coro *current;          /* the running coroutine, NULL for main */

static wb_lv_coro *co_from_value(wb_lv *L, wb_lv_value v) {
    if (v.t != WB_LV_TABLE) return NULL;
    wb_lv_value h = wb_lv_table_get(L, v.as.tab, wb_lv_str("_co"));
    if (h.t != WB_LV_INT) return NULL;
    return (wb_lv_coro *)(intptr_t)h.as.i;
}

static void co_save(wb_lv_coro *co, wb_lv *L) {
    co->sp = L->sp;
    co->curline = L->curline;
    co->eval_depth = L->eval_depth;
    co->depth = L->depth;
    co->va = L->va; co->nva = L->nva;
    co->has_err = L->has_err;
    co->has_err_obj = L->has_err_obj;
    co->err_obj = L->err_obj;
    memcpy(co->err, L->err, sizeof co->err);
}

static void co_restore(wb_lv_coro *co, wb_lv *L) {
    L->sp = co->sp;
    L->curline = co->curline;
    L->eval_depth = co->eval_depth;
    L->depth = co->depth;
    L->va = co->va; L->nva = co->nva;
    L->has_err = co->has_err;
    L->has_err_obj = co->has_err_obj;
    L->err_obj = co->err_obj;
    memcpy(L->err, co->err, sizeof L->err);
}

/* the fiber entry: run the body, then hand control back for good */
#ifdef WB_LV_FIBERS
static void __stdcall coro_entry(void *param) {
    wb_lv_coro *co = (wb_lv_coro *)param;
    wb_lv *L = co->L;
    for (;;) {
        /* first entry runs the body; later entries only re-yield nothing */
        if (!co->started) {
            co->started = 1;
            wb_lv_value *out = NULL; int nr = 0;
            if (co->fn.t == WB_LV_FUNCTION || co->fn.t == WB_LV_CFUNCTION)
                nr = wb_lv_call_value(L, co->fn, co->args, co->nargs, &out);
            co->nrets = nr;
            if (out) {
                co->rets = (wb_lv_value *)malloc((size_t)(nr ? nr : 1) * sizeof(*co->rets));
                memcpy(co->rets, out, (size_t)nr * sizeof(*co->rets));
            }
            co->state = CO_DEAD;
        } else {
            co->nrets = 0;
        }
        SwitchToFiber(co->resumer_fiber);
    }
}
#endif

/* --------------------------------------------------------------- yield/resume */

static wb_lv_value cfn_yield(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)ud;
    wb_lv_coro *co = current;          /* the running coroutine */
    if (!co) { wb_lv_error(L, "attempt to yield from outside a coroutine"); return wb_lv_nil(); }
    free(co->yrets);
    co->yrets = (wb_lv_value *)malloc((size_t)(n ? n : 1) * sizeof(*co->yrets));
    if (n) memcpy(co->yrets, args, (size_t)n * sizeof(*co->yrets));
    co->nyrets = n;
    co->state = CO_SUSPENDED;
#ifdef WB_LV_FIBERS
    SwitchToFiber(co->resumer_fiber);
#endif
    /* resumed again: the new arguments are ours */
    free(co->rets);
    co->rets = (wb_lv_value *)malloc((size_t)(co->nargs ? co->nargs : 1) * sizeof(*co->rets));
    if (co->nargs) memcpy(co->rets, co->args, (size_t)co->nargs * sizeof(*co->rets));
    co->nrets = co->nargs;

    return wb_lv_multi_value(L, co->rets, co->nrets);
}

static wb_lv_value cfn_resume(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)ud;
    wb_lv_coro *co = co_from_value(L, (n >= 1) ? args[0] : wb_lv_nil());
    if (!co) { wb_lv_error(L, "bad argument #1 to 'resume' (coroutine expected)"); return wb_lv_nil(); }
    if (co->state == CO_DEAD) {
        wb_lv_value out[2]; out[0] = wb_lv_bool(0);
        out[1] = wb_lv_str(wb_lv_intern(L, "cannot resume dead coroutine", 29));
        return wb_lv_multi_value(L, out, 2);
    }
    if (co->state == CO_RUNNING || co->state == CO_NORMAL) {
        wb_lv_value out[2]; out[0] = wb_lv_bool(0);
        out[1] = wb_lv_str(wb_lv_intern(L, "cannot resume non-suspended coroutine", 36));
        return wb_lv_multi_value(L, out, 2);
    }
#ifdef WB_LV_FIBERS
    if (!co->started) {
        /* first resume: create the fiber */
        co->fiber = CreateFiber(CO_STACK, coro_entry, co);
        if (!co->fiber) { wb_lv_error(L, "cannot create a coroutine"); return wb_lv_nil(); }
    }
    co->args = args + 1;
    co->nargs = n - 1;

    co->resumer = current;
    co->resumer_fiber = current ? current->fiber : main_fiber;
    co->state = CO_RUNNING;

    wb_lv_coro *prev = current;          /* NULL when resuming from main */
    if (prev) { co_save(prev, L); prev->state = CO_NORMAL; }
    current = co;
    co_restore(co, L);

    SwitchToFiber(co->fiber);

    /* back from a yield or a return */
    current = co->resumer;
    if (current) current->state = CO_RUNNING;
    if (co->state != CO_DEAD) co->state = CO_SUSPENDED;
    co_save(co, L);
    current = prev;
    if (prev) co_restore(prev, L);

    wb_lv_value *out = NULL; int nr = 0;
    if (L->has_err) {
        wb_lv_value msg = L->has_err_obj ? L->err_obj : wb_lv_str(L->err[0] ? L->err : "error");
        L->has_err = 0; L->err[0] = 0; L->has_err_obj = 0;
        wb_lv_value all[2]; all[0] = wb_lv_bool(0); all[1] = msg;
        return wb_lv_multi_value(L, all, 2);
    }
    if (co->state == CO_DEAD) { nr = co->nrets; out = co->rets; }
    else { nr = co->nyrets; out = co->yrets; }
    wb_lv_value *res = WB_LV_ALLOCN(wb_lv_value, nr + 1, L);
    res[0] = wb_lv_bool(1);
    for (int i = 0; i < nr; i++) res[i + 1] = out[i];
    return wb_lv_multi_value(L, res, nr + 1);
#else
    (void)co;
    wb_lv_error(L, "coroutines are not supported on this platform");
    return wb_lv_nil();
#endif
}

static wb_lv_value cfn_status(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)ud;
    wb_lv_coro *co = co_from_value(L, (n >= 1) ? args[0] : wb_lv_nil());
    if (!co) { wb_lv_error(L, "bad argument #1 to 'status' (coroutine expected)"); return wb_lv_nil(); }
    if (co == current) return wb_lv_str(wb_lv_intern(L, "running", 7));
    switch (co->state) {
    case CO_SUSPENDED: return wb_lv_str(wb_lv_intern(L, "suspended", 9));
    case CO_DEAD:      return wb_lv_str(wb_lv_intern(L, "dead", 4));
    case CO_NORMAL:    return wb_lv_str(wb_lv_intern(L, "normal", 6));
    case CO_RUNNING:   return wb_lv_str(wb_lv_intern(L, "running", 7));
    }
    return wb_lv_str(wb_lv_intern(L, "suspended", 9));
}

static wb_lv_value cfn_running(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)a; (void)n; (void)ud;
    if (!current) {
        wb_lv_value out[2];
        out[0] = wb_lv_nil();
        out[1] = wb_lv_bool(1);
        return wb_lv_multi_value(L, out, 2);
    }
    /* the running coroutine's handle is not directly reachable; report main */
    wb_lv_value out[2];
    out[0] = wb_lv_nil();
    out[1] = wb_lv_bool(0);
    return wb_lv_multi_value(L, out, 2);
}

static wb_lv_value cfn_isyieldable(wb_lv *L, wb_lv_value *a, int n, void *ud) {
    (void)L; (void)a; (void)n; (void)ud;
#ifdef WB_LV_FIBERS
    return wb_lv_bool(current != NULL);
#else
    return wb_lv_bool(0);
#endif
}

/* wrap: a function that resumes and raises on error */
static wb_lv_value cfn_wrap_call(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    wb_lv_coro *co = (wb_lv_coro *)ud;
    wb_lv_value ca[64];
    int nn = n + 1;
    if (nn > 64) nn = 64;
    ca[0].t = WB_LV_TABLE; ca[0].as.tab = co->handle_tab;
    for (int i = 1; i < nn; i++) ca[i] = args[i - 1];
    wb_lv_value r = cfn_resume(L, ca, nn, NULL);
    /* r = true, results... | false, err */
    if (r.t == WB_LV_MULTI && r.as.multi && r.as.multi->n >= 1) {
        if (!wb_lv_truthy(r.as.multi->v[0])) {
            wb_lv_value err = r.as.multi->n >= 2 ? r.as.multi->v[1] : wb_lv_nil();
            wb_lv_error_value(L, err);
            return wb_lv_nil();
        }
        return wb_lv_multi_value(L, r.as.multi->v + 1, r.as.multi->n - 1);
    }
    return r;
}

static wb_lv_value cfn_wrap(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)ud;
    if (n < 1) { wb_lv_error(L, "bad argument #1 to 'wrap' (function expected)"); return wb_lv_nil(); }
#ifdef WB_LV_FIBERS
    wb_lv_coro *co = (wb_lv_coro *)calloc(1, sizeof *co);
    co->L = L; co->fn = args[0]; co->state = CO_SUSPENDED;
    co->handle_tab = wb_lv_table_new(L, 0, 2);
    wb_lv_table_set(L, co->handle_tab, wb_lv_str("_co"), wb_lv_integer((int64_t)(intptr_t)co));
    wb_lv_value cf; cf.t = WB_LV_CFUNCTION; cf.as.cf.f = cfn_wrap_call; cf.as.cf.ud = co;
    return cf;
#else
    (void)co;
    wb_lv_error(L, "coroutines are not supported on this platform");
    return wb_lv_nil();
#endif
}

static wb_lv_value cfn_create(wb_lv *L, wb_lv_value *args, int n, void *ud) {
    (void)ud;
    if (n < 1) { wb_lv_error(L, "bad argument #1 to 'create' (function expected)"); return wb_lv_nil(); }
#ifdef WB_LV_FIBERS
    wb_lv_coro *co = (wb_lv_coro *)calloc(1, sizeof *co);
    co->L = L; co->fn = args[0]; co->state = CO_SUSPENDED;
    co->handle_tab = wb_lv_table_new(L, 0, 2);
    wb_lv_table_set(L, co->handle_tab, wb_lv_str("_co"), wb_lv_integer((int64_t)(intptr_t)co));
    wb_lv_value r; r.t = WB_LV_TABLE; r.as.tab = co->handle_tab;
    return r;
#else
    (void)co;
    wb_lv_error(L, "coroutines are not supported on this platform");
    return wb_lv_nil();
#endif
}

/* ------------------------------------------------------------ registration */

void wb_lv_register_coro(wb_lv *L) {
#ifdef WB_LV_FIBERS
    if (!main_fiber) {
        main_fiber = ConvertThreadToFiber(NULL);
        if (!main_fiber) { /* already a fiber (we are inside one) */ }
    }
#endif
    wb_lv_table *t = wb_lv_table_new(L, 0, 8);
#define C(name, fn) { wb_lv_value v; v.t = WB_LV_CFUNCTION; v.as.cf.f = fn; v.as.cf.ud = NULL; \
    wb_lv_table_set(L, t, wb_lv_str(name), v); }
    C("create", cfn_create);
    C("resume", cfn_resume);
    C("yield", cfn_yield);
    C("status", cfn_status);
    C("running", cfn_running);
    C("isyieldable", cfn_isyieldable);
    C("wrap", cfn_wrap);
#undef C
    wb_lv_value tv; tv.t = WB_LV_TABLE; tv.as.tab = t;
    wb_lv_set_global(L, "coroutine", tv);
}
