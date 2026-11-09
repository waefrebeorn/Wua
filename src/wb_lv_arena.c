/*
 * wb_lv_arena.c -- the interpreter's non-intrusive arena. See wb_lv_internal.h.
 */
#include "wb_lv_internal.h"

#include <stdlib.h>

void *wb_lv_arena_alloc(wb_lv *L, size_t n) {
    anode *h = (anode *)calloc(1, sizeof(anode) + n);
    if (!h) return NULL;
    h->next = L->root;
    L->root = h;
    L->arena_bytes += sizeof(anode) + n;
    return (void *)(h + 1);
}
