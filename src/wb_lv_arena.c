/*
 * wb_lv_arena.c -- the interpreter's arena, a bump-allocating slab pool.
 *
 * Every interpreter allocation goes through one arena so lifetimes are simple
 * (everything lives until wb_lv_free) and there is no per-object free. Memory
 * is carved out of a chain of slabs (one calloc per slab), and each request is
 * a zeroed pointer bump -- NOT one malloc/calloc per block. See wb_lv_internal.h.
 */
#include "wb_lv_internal.h"

#include <stdlib.h>
#include <string.h>

#define WB_LV_SLAB  65536            /* default slab payload, bytes */
#define WB_LV_ALIGN 16

void *wb_lv_arena_alloc(wb_lv *L, size_t n) {
    if (L->prof_on) L->prof.alloc++;
    size_t need = (n + WB_LV_ALIGN - 1) & ~(size_t)(WB_LV_ALIGN - 1);
    if (need == 0) need = WB_LV_ALIGN;
    anode *slab = L->arena_slab;
    if (!slab || slab->used + need > slab->cap) {
        /* full or no slab yet: spawn a new one. Oversized requests (<anode> +
         * need > SLAB) get a dedicated slab so a big string/array does not
         * waste a whole 64K block. */
        size_t sz = need > WB_LV_SLAB ? need : WB_LV_SLAB;
        slab = (anode *)calloc(1, sizeof(anode) + sz);
        if (!slab) return NULL;
        slab->cap = sz; slab->used = 0;
        slab->next = L->root; L->root = slab;
        L->arena_slab = slab;
        L->arena_bytes += sizeof(anode) + sz;
    }
    /* bump, then zero exactly the region handed out (calloc parity) */
    void *p = (char *)(slab + 1) + slab->used;
    L->arena_used += need;
    memset(p, 0, n);
    slab->used += need;
    return p;
}
