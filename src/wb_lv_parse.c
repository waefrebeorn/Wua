/*
 * wb_lv_parse.c -- the grammar: expressions, statements and blocks, plus the
 * AST pool. Lua 5.4 syntax throughout. See wb_lv_internal.h.
 */
#include "wb_lv_internal.h"

#include <stdlib.h>
#include <string.h>

static ex *espawn(pool *p) {
    if (p->cnt >= WB_LV_MAX_EX) return NULL;
    memset(&p->n[p->cnt], 0, sizeof(ex));
    return &p->n[p->cnt++];
}

static int tok_is(parser *P, int type) { return P->cur.type == type; }

static void expect(parser *P, int type, const char *what) {
    if (!tok_is(P, type)) wb_lv_perr(P, what);
    if (!P->err) wb_lv_next_tok(P);
}

static ex *parse_expr(parser *P);
static ex *parse_stmt(parser *P);
static ex *parse_block(parser *P, int *matched);
static ex *parse_simple(parser *P);
static ex *parse_prefix(parser *P);
static ex *parse_funcbody(parser *P, const char *name, int is_method);

#define U_END    1
#define U_ELSE   2
#define U_ELSEIF 4
#define U_UNTIL  8

/* Parser nesting guard. A few thousand nested parens recurse until the C
 * stack is gone; Lua's own limit is 200, so match it. */
static ex *parse_primary(parser *P) {
    if (P->depth >= WB_LV_MAX_DEPTH) { wb_lv_perr(P, "expression nested too deeply"); return NULL; }
    P->depth++;
    ex *e = parse_prefix(P);
    P->depth--;
    return e;
}

static ex *parse_unary(parser *P) {
    static const int uni[] = { T_NOT, '#', '-', '~', 0 };
    for (int i = 0; uni[i]; i++) {
        if (tok_is(P, uni[i])) {
            int op = P->cur.type;
            int line = P->cur.line;
            wb_lv_next_tok(P);
            ex *operand = parse_unary(P);
            if (P->err) return NULL;
            ex *e = espawn(P->pl);
            if (!e) { wb_lv_perr(P, "out of AST pool"); return NULL; }
            e->t = N_UNARY; e->op = op; e->e1 = operand; e->line = line;
            return e;
        }
    }
    return parse_primary(P);
}

/* binary operator precedence, from the Lua 5.4 reference manual */
static int binprec(int type) {
    switch (type) {
    case T_OR: return 1;
    case T_AND: return 2;
    case '<': case '>': case T_LE: case T_GE: case T_NE: case T_EQ: return 3;
    case '|': return 4;
    case '~': return 5;
    case '&': return 6;
    case T_SHL: case T_SHR: return 7;
    case T_CONCAT: return 8;
    case '+': case '-': return 9;
    case '*': case '/': case T_IDIV: case '%': return 10;
    case '^': return 12;
    }
    return 0;
}
static int rightassoc(int type) { return type == T_CONCAT || type == '^'; }

static ex *parse_bin(parser *P, int limit) {
    ex *lhs = parse_unary(P);
    if (P->err) return lhs;
    for (;;) {
        int op = P->cur.type;
        int pr = binprec(op);
        if (pr == 0 || pr < limit) break;
        int line = P->cur.line;
        wb_lv_next_tok(P);
        int nextlimit = rightassoc(op) ? pr : pr + 1;
        ex *rhs = parse_bin(P, nextlimit);
        if (P->err) return lhs;
        ex *e = espawn(P->pl);
        if (!e) { wb_lv_perr(P, "out of AST pool"); return NULL; }
        e->t = (op == T_AND) ? N_AND : (op == T_OR) ? N_OR : N_BINOP;
        e->op = op; e->e1 = lhs; e->e2 = rhs; e->line = line;
        lhs = e;
    }
    return lhs;
}

static ex *parse_expr(parser *P) { return parse_bin(P, 1); }

/* --------------------------------------------------------- expr lists */

static ex **parse_explist(parser *P, int *n) {
    ex **v = NULL; int nv = 0, cap = 0;
    for (;;) {
        if (nv >= cap) { cap = cap ? cap * 2 : 4; v = (ex **)realloc(v, (size_t)cap * sizeof(*v)); }
        v[nv++] = parse_expr(P);
        if (P->err) { free(v); *n = 0; return NULL; }
        if (tok_is(P, ',')) { wb_lv_next_tok(P); continue; }
        break;
    }
    *n = nv;
    return v;
}

/* ------------------------------------------------------------ tables */

static ex *parse_table(parser *P) {
    int line = P->cur.line;
    wb_lv_next_tok(P);                       /* '{' */
    ex *t = espawn(P->pl);
    if (!t) { wb_lv_perr(P, "out of AST pool"); return NULL; }
    t->t = N_TABLE; t->line = line;
    ex **vals = NULL, **keys = NULL; int nv = 0, cap = 0;
    while (!tok_is(P, '}')) {
        if (tok_is(P, T_EOF)) { wb_lv_perr(P, "'}' expected"); break; }
        ex *key = NULL, *val;
        if (tok_is(P, '[')) {
            wb_lv_next_tok(P);
            key = parse_expr(P);
            if (P->err) break;
            expect(P, ']', "']' expected");
            expect(P, '=', "'=' expected");
            val = parse_expr(P);
        } else if (tok_is(P, T_NAME)) {
            /* lookahead for `name = value` */
            size_t save_p = P->p; tok save_cur = P->cur; int save_line = P->line;
            const char *nm = P->cur.str;
            wb_lv_next_tok(P);
            if (tok_is(P, '=')) {
                wb_lv_next_tok(P);
                key = espawn(P->pl);
                if (key) { key->t = N_STR; key->str = nm; key->slen = strlen(nm); }
                val = parse_expr(P);
            } else {
                /* not a keyed field: rewind and parse as a positional expr */
                P->p = save_p; P->cur = save_cur; P->line = save_line;
                val = parse_expr(P);
            }
        } else {
            val = parse_expr(P);
        }
        if (P->err) break;
        if (nv >= cap) {
            cap = cap ? cap * 2 : 8;
            vals = (ex **)realloc(vals, (size_t)cap * sizeof(*vals));
            keys = (ex **)realloc(keys, (size_t)cap * sizeof(*keys));
        }
        vals[nv] = val; keys[nv] = key; nv++;
        if (tok_is(P, ',') || tok_is(P, ';')) wb_lv_next_tok(P);
    }
    if (!P->err) expect(P, '}', "'}' expected");
    t->list = vals; t->keys = keys; t->nlist = nv;
    return t;
}

/* ------------------------------------------------------- function def */

static ex *parse_funcbody(parser *P, const char *name, int is_method) {
    int line = P->cur.line;
    ex *f = espawn(P->pl);
    if (!f) { wb_lv_perr(P, "out of AST pool"); return NULL; }
    f->t = N_FUNC; f->name = name; f->line = line; f->is_method = is_method;
    expect(P, '(', "'(' expected");
    const char **params = NULL; int np = 0, cap = 0;
    if (is_method) {
        /* the implicit `self` parameter */
        params = (const char **)wb_lv_arena_alloc(P->L, sizeof(char *));
        params[np++] = wb_lv_intern(P->L, "self", 4);
    }
    if (!tok_is(P, ')')) {
        for (;;) {
            if (tok_is(P, T_DOTS)) {
                f->op = 1;                      /* has varargs */
                wb_lv_next_tok(P);
                break;
            }
            if (!tok_is(P, T_NAME)) { wb_lv_perr(P, "parameter name expected"); break; }
            if (np >= cap) {
                const char **np2 = (const char **)wb_lv_arena_alloc(P->L, (size_t)(cap ? cap * 2 : 8) * sizeof(char *));
                memcpy(np2, params, (size_t)np * sizeof(char *));
                params = np2; cap = cap ? cap * 2 : 8;
            }
            params[np++] = P->cur.str;
            wb_lv_next_tok(P);
            if (tok_is(P, ',')) { wb_lv_next_tok(P); continue; }
            break;
        }
    }
    expect(P, ')', "')' expected");
    f->names = params; f->nlist = np;
    int m = 0;
    f->e1 = parse_block(P, &m);
    if (!(m & U_END)) wb_lv_perr(P, "'end' expected");
    return f;
}

/* --------------------------------------------------------- simple exp */

static ex *parse_simple(parser *P) {
    int line = P->cur.line;
    switch (P->cur.type) {
    case T_INT: { ex *e = espawn(P->pl); if(!e){wb_lv_perr(P,"pool");return NULL;} e->t=N_INT; e->i=P->cur.i; wb_lv_next_tok(P); e->line=line; return e; }
    case T_NUM: { ex *e = espawn(P->pl); if(!e){wb_lv_perr(P,"pool");return NULL;} e->t=N_NUM; e->num=P->cur.num; wb_lv_next_tok(P); e->line=line; return e; }
    case T_STR: { ex *e = espawn(P->pl); if(!e){wb_lv_perr(P,"pool");return NULL;} e->t=N_STR; e->str=P->cur.str; e->slen=P->cur.slen; wb_lv_next_tok(P); e->line=line; return e; }
    case T_NIL: { ex *e = espawn(P->pl); if(!e){wb_lv_perr(P,"pool");return NULL;} e->t=N_NIL; wb_lv_next_tok(P); e->line=line; return e; }
    case T_TRUE: { ex *e = espawn(P->pl); if(!e){wb_lv_perr(P,"pool");return NULL;} e->t=N_TRUE; wb_lv_next_tok(P); e->line=line; return e; }
    case T_FALSE: { ex *e = espawn(P->pl); if(!e){wb_lv_perr(P,"pool");return NULL;} e->t=N_FALSE; wb_lv_next_tok(P); e->line=line; return e; }
    case T_DOTS: { ex *e = espawn(P->pl); if(!e){wb_lv_perr(P,"pool");return NULL;} e->t=N_VARARG; wb_lv_next_tok(P); e->line=line; return e; }
    case T_FUNCTION: {
        wb_lv_next_tok(P);
        return parse_funcbody(P, NULL, 0);
    }
    case '{': return parse_table(P);
    default: return NULL;   /* not a simple expression */
    }
}

static ex *parse_prefix(parser *P) {
    ex *e;
    int line = P->cur.line;
    if (tok_is(P, '(')) {
        wb_lv_next_tok(P);
        e = parse_expr(P);
        if (P->err) return NULL;
        expect(P, ')', "')' expected");
        /* parenthesised: truncates multiple results to one -- but it is still
         * a prefix expression, so .field / () / :method suffixes apply */
        ex *p = espawn(P->pl);
        if (p) { p->t = N_PAREN; p->e1 = e; p->line = line; }
        e = p ? p : e;
    } else if (tok_is(P, T_NAME)) {
        e = espawn(P->pl);
        if (!e) { wb_lv_perr(P, "out of AST pool"); return NULL; }
        e->t = N_NAME; e->name = P->cur.str; e->line = line;
        wb_lv_next_tok(P);
    } else {
        e = parse_simple(P);
        if (!e && !P->err) wb_lv_perr(P, "unexpected symbol");
        return e;
    }
    /* suffixes */
    for (;;) {
        if (tok_is(P, '.')) {
            wb_lv_next_tok(P);
            if (!tok_is(P, T_NAME)) { wb_lv_perr(P, "field name expected"); return e; }
            ex *n = espawn(P->pl);
            ex *k = espawn(P->pl);
            if (!n || !k) { wb_lv_perr(P, "out of AST pool"); return NULL; }
            k->t = N_STR; k->str = P->cur.str; k->slen = P->cur.slen;
            n->t = N_INDEX; n->e1 = e; n->e2 = k; n->line = P->cur.line;
            wb_lv_next_tok(P);
            e = n;
        } else if (tok_is(P, '[')) {
            wb_lv_next_tok(P);
            ex *idx = parse_expr(P);
            if (P->err) return e;
            expect(P, ']', "']' expected");
            ex *n = espawn(P->pl);
            if (!n) { wb_lv_perr(P, "out of AST pool"); return NULL; }
            n->t = N_INDEX; n->e1 = e; n->e2 = idx; n->line = P->cur.line;
            e = n;
        } else if (tok_is(P, ':')) {
            wb_lv_next_tok(P);
            if (!tok_is(P, T_NAME)) { wb_lv_perr(P, "method name expected"); return e; }
            const char *mname = P->cur.str;
            wb_lv_next_tok(P);
            ex *call = espawn(P->pl);
            ex *mkey = espawn(P->pl);
            ex *base = espawn(P->pl);
            if (!call || !mkey || !base) { wb_lv_perr(P, "out of AST pool"); return NULL; }
            mkey->t = N_STR; mkey->str = mname; mkey->slen = strlen(mname);
            base->t = N_INDEX; base->e1 = e; base->e2 = mkey;
            call->t = N_METHOD; call->e1 = base; call->is_method = 1; call->line = P->cur.line;
            /* arguments */
            if (tok_is(P, '(')) {
                wb_lv_next_tok(P);
                if (tok_is(P, ')')) { call->nlist = 0; call->list = NULL; wb_lv_next_tok(P); }
                else {
                    int n = 0; call->list = parse_explist(P, &n); call->nlist = n;
                    if (!P->err) expect(P, ')', "')' expected");
                }
            } else if (tok_is(P, '{')) {
                call->list = (ex **)wb_lv_arena_alloc(P->L, sizeof(ex *));
                call->list[0] = parse_table(P); call->nlist = 1;
            } else if (tok_is(P, T_STR)) {
                call->list = (ex **)wb_lv_arena_alloc(P->L, sizeof(ex *));
                ex *s = espawn(P->pl);
                if (s) { s->t = N_STR; s->str = P->cur.str; s->slen = P->cur.slen; }
                call->list[0] = s; call->nlist = 1; wb_lv_next_tok(P);
            } else wb_lv_perr(P, "arguments expected");
            e = call;
        } else if (tok_is(P, '(') || tok_is(P, '{') || tok_is(P, T_STR)) {
            ex *call = espawn(P->pl);
            if (!call) { wb_lv_perr(P, "out of AST pool"); return NULL; }
            call->t = N_CALL; call->e1 = e; call->line = P->cur.line;
            if (tok_is(P, '(')) {
                wb_lv_next_tok(P);
                if (tok_is(P, ')')) { call->nlist = 0; call->list = NULL; wb_lv_next_tok(P); }
                else {
                    int n = 0; call->list = parse_explist(P, &n); call->nlist = n;
                    if (!P->err) expect(P, ')', "')' expected");
                }
            } else if (tok_is(P, '{')) {
                call->list = (ex **)wb_lv_arena_alloc(P->L, sizeof(ex *));
                call->list[0] = parse_table(P); call->nlist = 1;
            } else {
                ex *s = espawn(P->pl);
                if (s) { s->t = N_STR; s->str = P->cur.str; s->slen = P->cur.slen; }
                call->list = (ex **)wb_lv_arena_alloc(P->L, sizeof(ex *));
                call->list[0] = s; call->nlist = 1; wb_lv_next_tok(P);
            }
            e = call;
        } else break;
    }
    return e;
}

/* ----------------------------------------------------------- statements */

static ex *parse_stmt(parser *P) {
    int line = P->cur.line;
    ex *s = espawn(P->pl);
    if (!s) { wb_lv_perr(P, "out of AST pool"); return NULL; }
    s->line = line;

    switch (P->cur.type) {
    case ';': wb_lv_next_tok(P); s->t = N_BLOCK; s->nlist = 0; return s;

    case T_BREAK:
        wb_lv_next_tok(P);
        s->t = N_BREAK;
        return s;

    case T_GOTO:
        wb_lv_next_tok(P);
        if (!tok_is(P, T_NAME)) { wb_lv_perr(P, "label name expected"); return s; }
        s->t = N_GOTO; s->label = P->cur.str;
        wb_lv_next_tok(P);
        return s;

    case T_DBCOLON:
        wb_lv_next_tok(P);
        if (!tok_is(P, T_NAME)) { wb_lv_perr(P, "label name expected"); return s; }
        s->t = N_LABEL; s->label = P->cur.str;
        wb_lv_next_tok(P);
        expect(P, T_DBCOLON, "'::' expected");
        return s;

    case T_DO:
        wb_lv_next_tok(P);
        s->t = N_DO;
        { int m = 0; s->e1 = parse_block(P, &m); if (!(m & U_END)) wb_lv_perr(P, "'end' expected"); }
        return s;

    case T_WHILE:
        wb_lv_next_tok(P);
        s->t = N_WHILE;
        s->e1 = parse_expr(P);
        if (P->err) return s;
        expect(P, T_DO, "'do' expected");
        { int m = 0; s->e2 = parse_block(P, &m); if (!(m & U_END)) wb_lv_perr(P, "'end' expected"); }
        return s;

    case T_REPEAT:
        wb_lv_next_tok(P);
        s->t = N_REPEAT;
        { int m = 0; s->e2 = parse_block(P, &m); if (!(m & U_UNTIL)) wb_lv_perr(P, "'until' expected"); }
        s->e1 = parse_expr(P);
        return s;

    case T_IF: {
        wb_lv_next_tok(P);
        s->t = N_IF;
        s->e1 = parse_expr(P);
        if (P->err) return s;
        expect(P, T_THEN, "'then' expected");
        int m = 0;
        s->e2 = parse_block(P, &m);
        /* chain elseif/else into a list: e3 = else block, list = elseif pairs */
        ex **branches = NULL; int nb = 0, cap = 0;
        while (m & U_ELSEIF) {
            wb_lv_next_tok(P);               /* consume 'elseif' */
            ex *cond = parse_expr(P);
            if (P->err) break;
            expect(P, T_THEN, "'then' expected");
            int m2 = 0;
            ex *body = parse_block(P, &m2);
            if (nb >= cap) { cap = cap ? cap * 2 : 4; branches = (ex **)realloc(branches, (size_t)cap * 2 * sizeof(ex *)); }
            branches[nb * 2] = cond; branches[nb * 2 + 1] = body; nb++;
            m = m2;
        }
        if (m & U_ELSE) {
            /* parse_block already consumed the 'else' */
            int m3 = 0;
            s->e3 = parse_block(P, &m3);
            m = m3;
        }
        if (!(m & U_END)) wb_lv_perr(P, "'end' expected");
        s->list = branches; s->nlist = nb;
        return s;
    }

    case T_FOR: {
        wb_lv_next_tok(P);
        if (!tok_is(P, T_NAME)) { wb_lv_perr(P, "loop variable expected"); return s; }
        const char *v1 = P->cur.str;
        wb_lv_next_tok(P);
        if (tok_is(P, '=')) {
            wb_lv_next_tok(P);
            s->t = N_FORNUM;
            s->names = (const char **)wb_lv_arena_alloc(P->L, 2 * sizeof(char *));
            s->names[0] = v1;
            s->e1 = parse_expr(P);
            if (P->err) return s;
            expect(P, ',', "',' expected");
            s->e2 = parse_expr(P);
            if (P->err) return s;
            if (tok_is(P, ',')) { wb_lv_next_tok(P); s->e3 = parse_expr(P); }
            expect(P, T_DO, "'do' expected");
            int m = 0; s->e4 = parse_block(P, &m);
            if (!(m & U_END)) wb_lv_perr(P, "'end' expected");
            return s;
        }
        /* generic for */
        s->t = N_FORIN;
        const char **names = (const char **)wb_lv_arena_alloc(P->L, 8 * sizeof(char *));
        names[0] = v1; int nn = 1;
        while (tok_is(P, ',')) {
            wb_lv_next_tok(P);
            if (!tok_is(P, T_NAME)) { wb_lv_perr(P, "loop variable expected"); return s; }
            names[nn++] = P->cur.str;
            wb_lv_next_tok(P);
        }
        s->names = names; s->nlist = nn;      /* loop-variable count */
        expect(P, T_IN, "'in' expected");
        int n = 0;
        s->list = parse_explist(P, &n);
        s->e4 = (ex *)(intptr_t)n;           /* expression-list count */
        if (P->err) return s;
        expect(P, T_DO, "'do' expected");
        int m = 0; s->e1 = parse_block(P, &m);   /* the loop body */
        if (!(m & U_END)) wb_lv_perr(P, "'end' expected");
        return s;
    }

    case T_FUNCTION: {
        wb_lv_next_tok(P);
        if (!tok_is(P, T_NAME)) { wb_lv_perr(P, "function name expected"); return s; }
        const char *base = P->cur.str;
        wb_lv_next_tok(P);
        ex *base_node = espawn(P->pl);
        if (base_node) { base_node->t = N_NAME; base_node->name = base; base_node->line = line; }
        const char **fields = NULL; int nf = 0, cap = 0;
        int is_method = 0;
        while (tok_is(P, '.')) {
            wb_lv_next_tok(P);
            if (!tok_is(P, T_NAME)) { wb_lv_perr(P, "field name expected"); return s; }
            if (nf >= cap) {
                const char **nf2 = (const char **)wb_lv_arena_alloc(P->L, (size_t)(cap ? cap * 2 : 4) * sizeof(char *));
                memcpy(nf2, fields, (size_t)nf * sizeof(char *));
                fields = nf2; cap = cap ? cap * 2 : 4;
            }
            fields[nf++] = P->cur.str;
            wb_lv_next_tok(P);
        }
        if (tok_is(P, ':')) {
            wb_lv_next_tok(P);
            if (!tok_is(P, T_NAME)) { wb_lv_perr(P, "method name expected"); return s; }
            if (nf >= cap) {
                const char **nf2 = (const char **)wb_lv_arena_alloc(P->L, (size_t)(cap ? cap * 2 : 4) * sizeof(char *));
                memcpy(nf2, fields, (size_t)nf * sizeof(char *));
                fields = nf2; cap = cap ? cap * 2 : 4;
            }
            fields[nf++] = P->cur.str;
            is_method = 1;
            wb_lv_next_tok(P);
        }
        s->t = N_FUNCSTAT;
        s->e1 = base_node;
        s->names = fields; s->nlist = nf;
        s->name = (nf > 0) ? fields[nf - 1] : base;
        s->op = is_method;
        s->e2 = parse_funcbody(P, s->name, is_method);
        return s;
    }

    case T_LOCAL: {
        wb_lv_next_tok(P);
        if (tok_is(P, T_FUNCTION)) {
            wb_lv_next_tok(P);
            if (!tok_is(P, T_NAME)) { wb_lv_perr(P, "function name expected"); return s; }
            s->t = N_LOCALFUNC;
            s->name = P->cur.str;
            wb_lv_next_tok(P);               /* consume the name */
            ex *body = parse_funcbody(P, s->name, 0);
            s->e1 = body;
            return s;
        }
        const char **names = (const char **)wb_lv_arena_alloc(P->L, 8 * sizeof(char *));
        int *attrs = (int *)wb_lv_arena_alloc(P->L, 8 * sizeof(int));
        int nn = 0;
        for (;;) {
            if (!tok_is(P, T_NAME)) { wb_lv_perr(P, "name expected"); break; }
            if (nn < 8) { names[nn] = P->cur.str; attrs[nn] = 0; nn++; }
            wb_lv_next_tok(P);
            /* 5.4 attributes: <const> is enforced; <close> is accepted */
            if (tok_is(P, '<')) {
                wb_lv_next_tok(P);
                if (tok_is(P, T_NAME)) {
                    if (!strcmp(P->cur.str, "const")) attrs[nn - 1] |= 1;
                    wb_lv_next_tok(P);
                }
                expect(P, '>', "'>' expected");
            }
            if (tok_is(P, ',')) { wb_lv_next_tok(P); continue; }
            break;
        }
        s->e3 = (ex *)(intptr_t)attrs;   /* per-name attribute flags */
        s->t = N_LOCAL;
        s->names = names; s->nlist = nn;      /* name count lives in nlist */
        if (tok_is(P, '=')) {
            wb_lv_next_tok(P);
            int n = 0;
            s->keys = parse_explist(P, &n);   /* values live in keys */
            s->e4 = (ex *)(intptr_t)n;        /* value count */
            s->op = 1;                        /* has initialisers */
        }
        return s;
    }

    case T_RETURN: {
        wb_lv_next_tok(P);
        s->t = N_RETURN;
        if (!tok_is(P, T_END) && !tok_is(P, T_EOF) && !tok_is(P, ';') &&
            !tok_is(P, T_ELSE) && !tok_is(P, T_ELSEIF) && !tok_is(P, T_UNTIL)) {
            int n = 0;
            s->list = parse_explist(P, &n);
            s->nlist = n;
        }
        if (tok_is(P, ';')) wb_lv_next_tok(P);
        return s;
    }

    default: break;
    }

    /* assignment or expression statement */
    ex *e = parse_prefix(P);
    if (P->err) { s->t = N_CALL; s->e1 = e; return s; }
    if (!e) { wb_lv_perr(P, "unexpected symbol"); s->t = N_CALL; s->e1 = NULL; return s; }
    if (tok_is(P, '=') || tok_is(P, ',')) {
        ex **targets = (ex **)wb_lv_arena_alloc(P->L, 8 * sizeof(ex *));
        int nt = 0;
        targets[nt++] = e;
        while (tok_is(P, ',')) {
            wb_lv_next_tok(P);
            ex *t = parse_prefix(P);
            if (P->err) break;
            targets[nt++] = t;
        }
        expect(P, '=', "'=' expected");
        int n = 0;
        ex **vals = parse_explist(P, &n);
        s->t = N_ASSIGN; s->list = targets; s->nlist = nt;
        s->keys = vals; s->e4 = (ex *)(intptr_t)n;
        return s;
    }
    /* expression statement: the expression node IS the statement. Lua only
     * allows a call here; anything else is a syntax error. */
    if (e->t != N_CALL && e->t != N_METHOD) {
        wb_lv_perr(P, "syntax error near unexpected expression");
    }
    return e;
}

/* --------------------------------------------------------------- blocks */

static ex *parse_block_impl(parser *P, int until, int *matched) {
    ex *b = espawn(P->pl);
    if (!b) { wb_lv_perr(P, "out of AST pool"); return NULL; }
    b->t = N_BLOCK; b->line = P->cur.line;
    ex **stmts = NULL; int ns = 0, cap = 0;
    *matched = 0;
    for (;;) {
        int tt = P->cur.type;
        int hit = 0;
        if (tt == T_EOF) break;
        if ((until & U_END)    && tt == T_END)    { wb_lv_next_tok(P); *matched = U_END;    hit = 1; }
        if ((until & U_ELSE)   && tt == T_ELSE)   { wb_lv_next_tok(P); *matched = U_ELSE;   hit = 1; }
        if ((until & U_ELSEIF) && tt == T_ELSEIF) { *matched = U_ELSEIF;                    hit = 1; }
        if ((until & U_UNTIL)  && tt == T_UNTIL)  { wb_lv_next_tok(P); *matched = U_UNTIL;  hit = 1; }
        if (hit) break;
        ex *st = parse_stmt(P);
        if (ns >= cap) { cap = cap ? cap * 2 : 8; stmts = (ex **)realloc(stmts, (size_t)cap * sizeof(*stmts)); }
        stmts[ns++] = st;
        if (P->err) break;
    }
    b->list = stmts; b->nlist = ns;
    return b;
}

static ex *parse_block(parser *P, int *matched) {
    if (P->depth >= WB_LV_MAX_DEPTH) { wb_lv_perr(P, "block nested too deeply"); return NULL; }
    P->depth++;
    ex *b = parse_block_impl(P, U_END | U_ELSE | U_ELSEIF | U_UNTIL, matched);
    P->depth--;
    return b;
}

const char *wb_lv_parse_driver(wb_lv *L, const char *src, pool *pl, ex **out) {
    parser P;
    memset(&P, 0, sizeof P);
    P.src = src; P.line = 1; P.L = L; P.pl = pl; P.p = 0;
    wb_lv_next_tok(&P);
    int m = 0;
    ex *root = parse_block(&P, &m);
    if (P.err || L->has_err) {
        if (!L->err[0]) snprintf(L->err, sizeof L->err, "parse error");
        return L->err;
    }
    if (!tok_is(&P, T_EOF)) {
        snprintf(L->err, sizeof L->err, "unexpected symbol (line %d)", P.line);
        return L->err;
    }
    *out = root;
    return NULL;
}
