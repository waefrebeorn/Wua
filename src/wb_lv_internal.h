/*
 * wb_lv_internal.h -- private interface of slerm-Lua.
 *
 * Included only by the wb_lv*.c translation units. The public header keeps
 * wb_lv / wb_lv_value / wb_lv_func / wb_lv_table opaque; this is where the
 * state, the AST, the lexer and the parser live, so no caller can reach in.
 */
#ifndef WB_LV_INTERNAL_H
#define WB_LV_INTERNAL_H

#include "wb_lv.h"

#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>

/* ---------------------------------------------------------------- arena */
/*
 * NON-intrusive arena: an 8-byte header precedes each block, so it works on
 * any struct. (An intrusive arena that writes `next` into the block's first
 * field silently corrupts interners and tables.)
 */
typedef struct anode { struct anode *next; } anode;

void *wb_lv_arena_alloc(wb_lv *L, size_t n);
#define WB_LV_ALLOC(T, L)     ((T *)wb_lv_arena_alloc((L), sizeof(T)))
#define WB_LV_ALLOCN(T, n, L) ((T *)wb_lv_arena_alloc((L), (n) * sizeof(T)))

/* ------------------------------------------------------------------ AST */

typedef enum {
    N_NIL, N_TRUE, N_FALSE, N_INT, N_NUM, N_STR, N_VARARG, N_NAME,
    N_INDEX, N_CALL, N_METHOD, N_FUNC, N_TABLE,
    N_BINOP, N_UNARY, N_AND, N_OR, N_PAREN,
    N_LOCAL, N_ASSIGN, N_LOCALFUNC, N_FUNCSTAT,
    N_IF, N_WHILE, N_REPEAT, N_FORNUM, N_FORIN, N_DO,
    N_RETURN, N_BREAK, N_GOTO, N_LABEL, N_BLOCK,
} ntype;

typedef struct ex {
    ntype t; int line;
    int64_t i; double num; const char *str; const char *name;
    int op;                          /* operator token for N_BINOP/N_UNARY  */
    struct ex *e1, *e2, *e3, *e4;    /* operands / cond / body / else / step */
    struct ex **list; int nlist;     /* stmts / args / params / fields       */
    struct ex **keys;                /* table: {list=values, keys=key-expr}  */
    const char **names;              /* param names / loop variables         */
    const char *label;               /* goto / label name                    */
    int is_method;
} ex;

#define WB_LV_MAX_EX 65536
/* Lua's own limits. All three are independent: the parser can recurse
 * iteratively (a postfix chain) while eval recurses per node. */
#define WB_LV_MAX_DEPTH 200
#define WB_LV_MAX_EVAL_DEPTH 200
#define WB_LV_MAX_CALL 190

typedef struct { ex n[WB_LV_MAX_EX]; int cnt; } pool;

/* ---------------------------------------------------------------- state */

struct wb_lv_multi { int n; wb_lv_value v[]; };

struct wb_lv {
    struct anode *root;
    struct istr **buckets; size_t nbuckets;
    wb_lv_table *globals;
    wb_lv_table *string_meta;        /* metatable for the string type        */
    char err[512]; int has_err;
    int has_err_obj;                /* error() raised a value, not a string */
    wb_lv_value err_obj;
    int curline;                     /* line of the node being executed      */
    const char *chunkname;
    int depth;                       /* Lua call nesting (recursion guard)   */
    int eval_depth;
    pool *pool;

    /* error isolation: a stack of catch points for pcall/xpcall */
    jmp_buf *jmp; int njmp;
    /* value stack, used for multiple returns and call frames */
    wb_lv_value *stk; int sp, scap;

    /* non-local control: goto */
    const char *goto_label; int has_goto;

    /* varargs of the currently executing Lua frame (saved/restored) */
    wb_lv_value *va; int nva;

    uint64_t rand_state[2];
};

/* --------------------------------------------------------- string pool */

typedef struct istr { char *s; size_t len; unsigned hash; struct istr *next; } istr;

/* --------------------------------------------------------------- tables */

typedef struct wb_lv_kv { wb_lv_value k, v; int used; int tomb; } wb_lv_kv;
struct wb_lv_table {
    wb_lv_kv *e; size_t cap, count;
    struct wb_lv_table *meta;
    size_t seq;                      /* cached array-part length for `#`     */
    int seq_dirty;
};

/* --------------------------------------------------------------- parser */

/* char tokens are (unsigned char)c, always < 256; the rest are >= 256. */
typedef enum {
    T_EOF = 0,
    T_INT = 256, T_NUM, T_STR, T_NAME,
    /* keywords */
    T_AND, T_BREAK, T_DO, T_ELSE, T_ELSEIF, T_END, T_FALSE, T_FOR,
    T_FUNCTION, T_GOTO, T_IF, T_IN, T_LOCAL, T_NIL, T_NOT, T_OR, T_REPEAT,
    T_RETURN, T_THEN, T_TRUE, T_UNTIL, T_WHILE,
    /* compound */
    T_CONCAT, T_DOTS, T_EQ, T_NE, T_LE, T_GE, T_IDIV, T_SHL, T_SHR, T_DBCOLON,
} tktype;

typedef struct { int type; int64_t i; double num; const char *str; int line; } tok;

#define WB_LV_TKCHAR(c) ((int)(unsigned char)(c))

typedef struct {
    const char *src; size_t p; int line;
    tok cur;
    wb_lv *L; pool *pl; int err;
    int depth;
} parser;

void wb_lv_set_err(wb_lv *L, const char *fmt, ...);
void wb_lv_error(wb_lv *L, const char *fmt, ...);   /* raises: longjmps or sets */
void wb_lv_error_value(wb_lv *L, wb_lv_value v); /* raises a value, as error(obj) does */
const char *wb_lv_type_name(wb_lv_type t);
void wb_lv_next_tok(parser *P);
void wb_lv_perr(parser *P, const char *msg);

ex *wb_lv_parse_expr(parser *P);
ex *wb_lv_parse_stmt(parser *P);
ex *wb_lv_parse_block(parser *P, int *matched);
const char *wb_lv_parse_driver(wb_lv *L, const char *src, pool *pl, ex **out);

/* -------------------------------------------------------------- runtime */

typedef struct env { struct env *parent; wb_lv_table *t; wb_lv_table *consts; } env;

struct wb_lv_func {
    ex *body;                  /* N_BLOCK                                    */
    const char *name;
    env *defining;
    int nparams;               /* number of param names                      */
    const char **params;
    wb_lv_value *varargs; int nvarargs;
};

wb_lv_value wb_lv_eval(wb_lv *L, ex *e, env *en);
wb_lv_value wb_lv_eval_multi(wb_lv *L, ex *e, env *en, wb_lv_value **out, int *nout);
wb_lv_value wb_lv_exec(wb_lv *L, ex *e, env *en, int *was_return);
wb_lv_value wb_lv_call_lua(wb_lv *L, wb_lv_func *fn, wb_lv_value *args, int nargs);
int         wb_lv_call_lua_results(wb_lv *L, wb_lv_func *fn, wb_lv_value *args, int nargs);
int         wb_lv_call_value(wb_lv *L, wb_lv_value fn, wb_lv_value *args, int nargs, wb_lv_value **out);
wb_lv_value wb_lv_multi_value(wb_lv *L, wb_lv_value *v, int n);
wb_lv_value wb_lv_exec_root(wb_lv *L, ex *root);

/* operators with metamethod dispatch */
wb_lv_value wb_lv_arith(wb_lv *L, int op, wb_lv_value a, wb_lv_value b);
wb_lv_value wb_lv_compare(wb_lv *L, int op, wb_lv_value a, wb_lv_value b);
int         wb_lv_len(wb_lv *L, wb_lv_value v, size_t *out);
wb_lv_value wb_lv_concat(wb_lv *L, wb_lv_value a, wb_lv_value b);
wb_lv_value wb_lv_index(wb_lv *L, wb_lv_value obj, wb_lv_value key);
void        wb_lv_setindex(wb_lv *L, wb_lv_value obj, wb_lv_value key, wb_lv_value v);
size_t      wb_lv_table_len(const wb_lv_table *t);
int         wb_lv_table_next(wb_lv_table *t, wb_lv_value *k, wb_lv_value *v);

/* catch points for pcall */
jmp_buf *wb_lv_push_catch(wb_lv *L);
void     wb_lv_pop_catch(wb_lv *L);

/* -------------------------------------------------------------- host */

void wb_lv_register_builtins(wb_lv *L);
void wb_lv_register_string(wb_lv *L);
void wb_lv_register_table(wb_lv *L);
void wb_lv_register_math(wb_lv *L);
void wb_lv_register_os(wb_lv *L);
void wb_lv_register_io(wb_lv *L);
void wb_lv_register_debug(wb_lv *L);
void wb_lv_register_utf8(wb_lv *L);
void wb_lv_register_coro(wb_lv *L);

#endif /* WB_LV_INTERNAL_H */
