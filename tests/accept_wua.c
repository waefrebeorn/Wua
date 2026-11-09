/* accept_wua.c -- parse-only conformance probe for Wua (the analogue of
 * `luac -p`): exit 0 if the file PARSES cleanly, 1 if it is a syntax error.
 * Crucially it does NOT execute the source, so a mutant that would loop or
 * throw a runtime error is still judged purely on parse acceptance.
 *
 *   gcc -std=c11 -O1 -Isrc accept_wua.c src/wb_lv_*.c -lm -o accept_wua.exe
 */
#include "wb_lv_internal.h"
#include "wb_lv.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return 2; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = 0;

    wb_lv *L = wb_lv_new();
    if (!L) { free(buf); return 2; }
    L->has_err = 0; L->err[0] = 0; L->has_err_obj = 0;
    L->chunkname = argv[1]; L->curline = 1;
    if (!L->pool) {
        L->pool = WB_LV_ALLOC(pool, L);
        if (!L->pool) { wb_lv_free(L); free(buf); return 2; }
    }
    ex *root = NULL;
    const char *err = wb_lv_parse_driver(L, buf, L->pool, &root);
    wb_lv_free(L);
    free(buf);
    return err ? 1 : 0;               /* 0 = parses, 1 = syntax error */
}
