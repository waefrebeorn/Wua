/*
 * wb_lv_lex.c -- the token stream: whitespace/comments, numbers (decimal +
 * hex), names/keywords, strings (quoted with escapes, and long brackets), and
 * every Lua 5.4 operator. See wb_lv_internal.h.
 */
#include "wb_lv_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void wb_lv_perr(parser *P, const char *msg) {
    if (!P->err) {
        snprintf(P->L->err, sizeof P->L->err, "%s (line %d)", msg, P->line);
        P->L->has_err = 1;
        P->err = 1;
    }
}

/* ------------------------------------------------------------ char buffer */

typedef struct { char *p; size_t n, cap; } sbuf;

static int sb_need(sbuf *b, size_t extra) {
    if (b->n + extra + 1 <= b->cap) return 1;
    size_t nc = b->cap ? b->cap : 64;
    while (nc < b->n + extra + 1) nc *= 2;
    char *np = (char *)realloc(b->p, nc);
    if (!np) return 0;
    b->p = np; b->cap = nc;
    return 1;
}
static void sb_put(sbuf *b, char c) { sb_need(b, 1); b->p[b->n++] = c; }

/* --------------------------------------------------------------- comments */

/* Returns the long-bracket level if `p` starts a long bracket at `s[p]`
 * (i.e. `[` `=`* `[`), else -1. */
static long longbracket(const char *s, size_t n, size_t p) {
    if (p >= n || s[p] != '[') return -1;
    size_t q = p + 1;
    while (q < n && s[q] == '=') q++;
    if (q < n && s[q] == '[') return (long)(q - p - 1);
    return -1;
}

/* skip a long bracket already known to start at P->p; returns 0 on success */
static int skip_long(parser *P, long level) {
    const char *s = P->src;
    size_t n = strlen(s);
    P->p += level + 2;                       /* opening [=*+[ */
    if (P->p < n && s[P->p] == '\n') { P->line++; P->p++; }
    else if (P->p + 1 < n && s[P->p] == '\r' && s[P->p+1] == '\n') { P->line++; P->p += 2; }
    for (;;) {
        if (P->p >= n) { wb_lv_perr(P, "unfinished long string/comment"); return -1; }
        if (s[P->p] == ']') {
            size_t q = P->p + 1;
            long k = 0;
            while (q < n && s[q] == '=') { k++; q++; }
            if (k == level && q < n && s[q] == ']') { P->p = q + 1; return 0; }
        }
        if (s[P->p] == '\n') P->line++;
        P->p++;
    }
}

/* --------------------------------------------------------------- numbers */

static int isdig(char c) { return c >= '0' && c <= '9'; }
static int ishex(char c) { return isdig(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

/* Parse a numeral starting at P->p. Handles decimal and hex, with optional
 * fractional part and exponent, matching Lua 5.4. */
static void lex_number(parser *P) {
    const char *s = P->src;
    size_t n = strlen(s);
    size_t st = P->p;
    int isfloat = 0;

    if (s[P->p] == '0' && P->p + 1 < n && (s[P->p+1] == 'x' || s[P->p+1] == 'X')) {
        P->p += 2;
        /* hex mantissa */
        double acc = 0;
        int digits = 0;
        while (P->p < n && ishex(s[P->p])) { acc = acc * 16 + hexval(s[P->p]); digits++; P->p++; }
        if (P->p < n && s[P->p] == '.') {
            isfloat = 1; P->p++;
            double scale = 1.0 / 16.0;
            while (P->p < n && ishex(s[P->p])) { acc += hexval(s[P->p]) * scale; scale /= 16.0; digits++; P->p++; }
        }
        if (digits == 0) { wb_lv_perr(P, "malformed number"); return; }
        /* binary exponent */
        if (P->p < n && (s[P->p] == 'p' || s[P->p] == 'P')) {
            size_t save = P->p;
            P->p++;
            int sgn = 1;
            if (P->p < n && (s[P->p] == '+' || s[P->p] == '-')) { if (s[P->p] == '-') sgn = -1; P->p++; }
            if (P->p < n && isdig(s[P->p])) {
                isfloat = 1;
                double e = 0;
                while (P->p < n && isdig(s[P->p])) { e = e * 10 + (s[P->p] - '0'); P->p++; }
                acc *= pow(2.0, sgn * e);
            } else P->p = save;
        }
        if (isfloat) { P->cur.type = T_NUM; P->cur.num = acc; }
        else {
            /* hex without a fraction/exponent is an integer */
            if (acc >= -9.2233720368547758e18 && acc <= 9.2233720368547758e18) {
                P->cur.type = T_INT; P->cur.i = (int64_t)acc;
            } else { P->cur.type = T_NUM; P->cur.num = acc; }
        }
        return;
    }

    /* decimal: one accumulator, integer part + fraction, then exponent */
    int64_t ipart = 0;
    double frac = 0, val;
    while (P->p < n && isdig(s[P->p])) { ipart = ipart * 10 + (s[P->p] - '0'); P->p++; }
    if (P->p < n && s[P->p] == '.') {
        isfloat = 1; P->p++;
        double scale = 0.1;
        while (P->p < n && isdig(s[P->p])) { frac += (s[P->p] - '0') * scale; scale *= 0.1; P->p++; }
    }
    val = (double)ipart + frac;
    if (P->p < n && (s[P->p] == 'e' || s[P->p] == 'E')) {
        size_t save = P->p;
        P->p++;
        int sgn = 1;
        if (P->p < n && (s[P->p] == '+' || s[P->p] == '-')) { if (s[P->p] == '-') sgn = -1; P->p++; }
        if (P->p < n && isdig(s[P->p])) {
            isfloat = 1;
            double e = 0;
            while (P->p < n && isdig(s[P->p])) { e = e * 10 + (s[P->p] - '0'); P->p++; }
            val *= pow(10.0, sgn * e);
        } else P->p = save;
    }
    /* decimal: use strtod over the whole token so the parse is correctly
     * rounded like Lua's (lua_str2number = strtod). Manual fraction/exponent
     * accumulation is off by 1 ulp for many decimals (12.149 -> 12.1490..).
     * Keep the integer scan/overhead code above; only the float mapping uses
     * strtod. */
    if (isfloat) { P->cur.type = T_NUM; P->cur.num = strtod(s + st, NULL); }
    else {
        /* a decimal integer becomes a float only if it does not fit */
        if (ipart < INT64_MIN / 10 || ipart > INT64_MAX / 10) {
            P->cur.type = T_NUM; P->cur.num = strtod(s + st, NULL);
        } else { P->cur.type = T_INT; P->cur.i = ipart; }
    }
}

/* --------------------------------------------------------------- strings */

static int lex_quoted(parser *P, char q) {
    const char *s = P->src;
    size_t n = strlen(s);
    P->p++;                                   /* opening quote */
    sbuf b; b.p = NULL; b.n = 0; b.cap = 0;
    for (;;) {
        if (P->p >= n) { free(b.p); wb_lv_perr(P, "unfinished string"); return -1; }
        char c = s[P->p];
        if (c == q) { P->p++; break; }
        if (c == '\n') { free(b.p); wb_lv_perr(P, "unfinished string"); return -1; }
        if (c != '\\') { sb_put(&b, c); P->p++; continue; }
        /* escape */
        P->p++;
        if (P->p >= n) { free(b.p); wb_lv_perr(P, "unfinished string"); return -1; }
        char e = s[P->p++];
        switch (e) {
        case 'a': sb_put(&b, '\a'); break;
        case 'b': sb_put(&b, '\b'); break;
        case 'f': sb_put(&b, '\f'); break;
        case 'n': sb_put(&b, '\n'); break;
        case 'r': sb_put(&b, '\r'); break;
        case 't': sb_put(&b, '\t'); break;
        case 'v': sb_put(&b, '\v'); break;
        case '\\': sb_put(&b, '\\'); break;
        case '"': sb_put(&b, '"'); break;
        case '\'': sb_put(&b, '\''); break;
        case '\n': sb_put(&b, '\n'); P->line++; break;
        case '\r':
            sb_put(&b, '\n');
            if (P->p < n && s[P->p] == '\n') P->p++;
            P->line++;
            break;
        case 'x': {
            int v = 0, k = 0;
            while (k < 2 && P->p < n && ishex(s[P->p])) { v = v * 16 + hexval(s[P->p]); P->p++; k++; }
            if (k != 2) { free(b.p); wb_lv_perr(P, "hexadecimal digit expected"); return -1; }
            sb_put(&b, (char)v);
            break;
        }
        case 'z':
            while (P->p < n && (s[P->p] == ' ' || s[P->p] == '\t' || s[P->p] == '\n' || s[P->p] == '\r')) {
                if (s[P->p] == '\n') P->line++;
                P->p++;
            }
            break;
        case 'u': {
            if (P->p >= n || s[P->p] != '{') { free(b.p); wb_lv_perr(P, "missing '{' in \\u{xxxx}"); return -1; }
            P->p++;
            long cp = 0;
            int k = 0;
            while (P->p < n && ishex(s[P->p])) { cp = cp * 16 + hexval(s[P->p]); P->p++; k++; }
            if (k == 0 || P->p >= n || s[P->p] != '}') { free(b.p); wb_lv_perr(P, "missing '}' in \\u{xxxx}"); return -1; }
            P->p++;
            if (cp > 0x7FFFFFFF) { free(b.p); wb_lv_perr(P, "UTF-8 value too large"); return -1; }
            /* UTF-8 encode */
            if (cp < 0x80) sb_put(&b, (char)cp);
            else if (cp < 0x800) {
                sb_put(&b, (char)(0xC0 | (cp >> 6)));
                sb_put(&b, (char)(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                sb_put(&b, (char)(0xE0 | (cp >> 12)));
                sb_put(&b, (char)(0x80 | ((cp >> 6) & 0x3F)));
                sb_put(&b, (char)(0x80 | (cp & 0x3F)));
            } else {
                sb_put(&b, (char)(0xF0 | (cp >> 18)));
                sb_put(&b, (char)(0x80 | ((cp >> 12) & 0x3F)));
                sb_put(&b, (char)(0x80 | ((cp >> 6) & 0x3F)));
                sb_put(&b, (char)(0x80 | (cp & 0x3F)));
            }
            break;
        }
        default:
            if (isdig(e)) {
                int v = e - '0', k = 1;
                while (k < 3 && P->p < n && isdig(s[P->p])) { v = v * 10 + (s[P->p] - '0'); P->p++; k++; }
                if (v > 255) { free(b.p); wb_lv_perr(P, "decimal escape too large"); return -1; }
                sb_put(&b, (char)v);
            } else {
                free(b.p);
                wb_lv_perr(P, "invalid escape sequence");
                return -1;
            }
        }
    }
    size_t litsz = b.n;                    /* real bytes; sentinel next */
    sb_put(&b, 0);
    P->cur.type = T_STR;
    P->cur.str = wb_lv_intern(P->L, b.p ? b.p : "", litsz);
    P->cur.slen = litsz;
    free(b.p);
    return 0;
}

/* ----------------------------------------------------------------- names */

static int kwmatch(const char *w, size_t len) {
    switch (len) {
    case 2:
        if (!memcmp(w, "do", 2)) return T_DO;
        if (!memcmp(w, "if", 2)) return T_IF;
        if (!memcmp(w, "in", 2)) return T_IN;
        if (!memcmp(w, "or", 2)) return T_OR;
        break;
    case 3:
        if (!memcmp(w, "end", 3)) return T_END;
        if (!memcmp(w, "for", 3)) return T_FOR;
        if (!memcmp(w, "nil", 3)) return T_NIL;
        if (!memcmp(w, "not", 3)) return T_NOT;
        if (!memcmp(w, "and", 3)) return T_AND;
        break;
    case 4:
        if (!memcmp(w, "then", 4)) return T_THEN;
        if (!memcmp(w, "else", 4)) return T_ELSE;
        if (!memcmp(w, "goto", 4)) return T_GOTO;
        if (!memcmp(w, "true", 4)) return T_TRUE;
        break;
    case 5:
        if (!memcmp(w, "break", 5)) return T_BREAK;
        if (!memcmp(w, "false", 5)) return T_FALSE;
        if (!memcmp(w, "until", 5)) return T_UNTIL;
        if (!memcmp(w, "while", 5)) return T_WHILE;
        if (!memcmp(w, "local", 5)) return T_LOCAL;
        break;
    case 6:
        if (!memcmp(w, "elseif", 6)) return T_ELSEIF;
        if (!memcmp(w, "return", 6)) return T_RETURN;
        if (!memcmp(w, "repeat", 6)) return T_REPEAT;
        break;
    case 8:
        if (!memcmp(w, "function", 8)) return T_FUNCTION;
        break;
    }
    return 0;
}

/* ------------------------------------------------------------- the lexer */

void wb_lv_next_tok(parser *P) {
    const char *s = P->src;
    size_t n = strlen(s);
    for (;;) {
        while (P->p < n && (s[P->p] == ' ' || s[P->p] == '\t' || s[P->p] == '\r' || s[P->p] == '\v' || s[P->p] == '\f')) P->p++;
        if (P->p < n && s[P->p] == '\n') { P->line++; P->p++; continue; }
        if (P->p + 1 < n && s[P->p] == '-' && s[P->p+1] == '-') {
            P->p += 2;
            long lvl = -1;
            if (P->p < n && s[P->p] == '[') lvl = longbracket(s, n, P->p);
            if (lvl >= 0) { if (skip_long(P, lvl) != 0) { P->cur.type = T_EOF; return; } }
            else { while (P->p < n && s[P->p] != '\n') P->p++; }
            continue;
        }
        break;
    }
    tok t; memset(&t, 0, sizeof t);
    t.line = P->line;
    if (P->p >= n) { t.type = T_EOF; P->cur = t; return; }
    char c = s[P->p];

    /* number */
    if (isdig(c) || (c == '.' && P->p + 1 < n && isdig(s[P->p+1]))) {
        lex_number(P);
        if (P->cur.type == T_INT || P->cur.type == T_NUM) P->cur.line = t.line;
        return;
    }

    /* name / keyword */
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
        size_t st = P->p;
        while (P->p < n && ((s[P->p] >= 'a' && s[P->p] <= 'z') ||
                            (s[P->p] >= 'A' && s[P->p] <= 'Z') ||
                            s[P->p] == '_' || isdig(s[P->p]))) P->p++;
        size_t wl = P->p - st;
        char w[128];
        if (wl >= sizeof w) wl = sizeof w - 1;
        memcpy(w, s + st, wl); w[wl] = 0;
        int kw = kwmatch(w, wl);
        if (kw) t.type = kw;
        else { t.type = T_NAME; t.str = wb_lv_intern(P->L, w, wl); }
        t.line = P->line;
        P->cur = t;
        return;
    }

    /* string */
    if (c == '"' || c == '\'') {
        if (lex_quoted(P, c) != 0) { P->cur.type = T_EOF; return; }
        P->cur.line = t.line;
        return;
    }
    if (c == '[') {
        long lvl = longbracket(s, n, P->p);
        if (lvl >= 0) {
            /* long string */
            P->p += lvl + 2;
            if (P->p < n && s[P->p] == '\n') { P->line++; P->p++; }
            else if (P->p + 1 < n && s[P->p] == '\r' && s[P->p+1] == '\n') { P->line++; P->p += 2; }
            sbuf b; b.p = NULL; b.n = 0; b.cap = 0;
            for (;;) {
                if (P->p >= n) { free(b.p); wb_lv_perr(P, "unfinished long string"); P->cur.type = T_EOF; return; }
                if (s[P->p] == ']') {
                    size_t q = P->p + 1; long k = 0;
                    while (q < n && s[q] == '=') { k++; q++; }
                    if (k == lvl && q < n && s[q] == ']') { P->p = q + 1; break; }
                }
                if (s[P->p] == '\n') P->line++;
                sb_put(&b, s[P->p++]);
            }
            size_t litsz = b.n;                /* real bytes; sentinel next */
            sb_put(&b, 0);
            t.type = T_STR;
            t.str = wb_lv_intern(P->L, b.p ? b.p : "", litsz);
            t.slen = litsz;
            free(b.p);
            t.line = P->line;
            P->cur = t;
            return;
        }
    }

    /* punctuation */
    P->p++;
    switch (c) {
    case '=': if (P->p < n && s[P->p] == '=') { P->p++; t.type = T_EQ; } else t.type = '='; break;
    case '<':
        if (P->p < n && s[P->p] == '=') { P->p++; t.type = T_LE; }
        else if (P->p < n && s[P->p] == '<') { P->p++; t.type = T_SHL; }
        else t.type = '<';
        break;
    case '>':
        if (P->p < n && s[P->p] == '=') { P->p++; t.type = T_GE; }
        else if (P->p < n && s[P->p] == '>') { P->p++; t.type = T_SHR; }
        else t.type = '>';
        break;
    case '~': if (P->p < n && s[P->p] == '=') { P->p++; t.type = T_NE; } else t.type = '~'; break;
    case '/': if (P->p < n && s[P->p] == '/') { P->p++; t.type = T_IDIV; } else t.type = '/'; break;
    case ':':
        if (P->p < n && s[P->p] == ':') { P->p++; t.type = T_DBCOLON; }
        else t.type = ':';
        break;
    case '.':
        if (P->p < n && s[P->p] == '.') {
            P->p++;
            if (P->p < n && s[P->p] == '.') { P->p++; t.type = T_DOTS; }
            else t.type = T_CONCAT;
        } else t.type = '.';
        break;
    default: t.type = WB_LV_TKCHAR(c); break;
    }
    t.line = P->line;
    P->cur = t;
}
