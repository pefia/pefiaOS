/* kernel/heap.c
 * -----------------------------------------------------------------------------
 * First-fit free-list allocator. Every block carries a header (size + free
 * flag) and blocks sit back-to-back, so we can walk the whole heap by address.
 * kmalloc finds the first free block big enough and splits it; kfree flips the
 * flag and merges neighbouring free blocks so the heap doesn't fragment.
 * -----------------------------------------------------------------------------
 */
#include "heap.h"
#include "multiboot.h"
#include "util.h"

#define HEAP_ALIGN 8

extern uint8_t _kernel_end;     /* from linker.ld */

typedef struct block {
    size_t        size;   /* payload bytes, not counting this header */
    struct block *next;   /* next block by address (NULL at the tail) */
    int           free;   /* 1 = available, 0 = handed out */
} block_t;

/* Header size rounded up so every returned payload is 8-byte aligned. */
#define HDR (((sizeof(block_t) + HEAP_ALIGN - 1) / HEAP_ALIGN) * HEAP_ALIGN)

static block_t *g_head;
static size_t   g_total;

static size_t align_up(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

void heap_init(uintptr_t start, size_t size)
{
    uintptr_t base = align_up(start, HEAP_ALIGN);
    size -= (base - start);

    g_head = (block_t *)base;
    g_head->size = size - HDR;
    g_head->next = NULL;
    g_head->free = 1;
    g_total = size;
}

void heap_init_from_multiboot(struct multiboot_info *mb)
{
    uintptr_t kend = (uintptr_t)&_kernel_end;
    uintptr_t best_start = 0;
    size_t    best_size  = 0;

    if (mb->flags & MULTIBOOT_INFO_MMAP) {
        uint8_t *p   = (uint8_t *)mb->mmap_addr;
        uint8_t *end = p + mb->mmap_length;
        while (p < end) {
            struct multiboot_mmap_entry *e = (struct multiboot_mmap_entry *)p;
            if (e->type == MULTIBOOT_MEMORY_AVAILABLE && e->addr < 0x100000000ULL) {
                uintptr_t rbase  = (uintptr_t)e->addr;
                uint64_t  rend64 = e->addr + e->len;
                if (rend64 > 0x100000000ULL) rend64 = 0x100000000ULL;
                uintptr_t rend  = (uintptr_t)rend64;
                uintptr_t start = (rbase > kend) ? rbase : kend;  /* don't hit kernel */
                if (rend > start && (rend - start) > best_size) {
                    best_size  = rend - start;
                    best_start = start;
                }
            }
            p += e->size + 4;   /* size excludes its own 4-byte field */
        }
    }

    /* Fallback: no map, trust mem_upper (KB above 1 MiB). */
    if (best_size == 0 && (mb->flags & MULTIBOOT_INFO_MEMORY)) {
        uintptr_t rend = 0x100000 + (uintptr_t)mb->mem_upper * 1024;
        if (rend > kend) { best_start = kend; best_size = rend - kend; }
    }

    if (best_size > HDR + HEAP_ALIGN)
        heap_init(best_start, best_size);
}

void *kmalloc(size_t n)
{
    if (n == 0) return NULL;
    n = align_up(n, HEAP_ALIGN);

    for (block_t *b = g_head; b; b = b->next) {
        if (!b->free || b->size < n) continue;

        /* Split if there's room for a new header plus a usable remainder. */
        if (b->size >= n + HDR + HEAP_ALIGN) {
            block_t *nb = (block_t *)((uint8_t *)b + HDR + n);
            nb->size = b->size - n - HDR;
            nb->free = 1;
            nb->next = b->next;
            b->next  = nb;
            b->size  = n;
        }
        b->free = 0;
        return (uint8_t *)b + HDR;
    }
    return NULL;   /* out of memory */
}

void kfree(void *p)
{
    if (!p) return;
    block_t *blk = (block_t *)((uint8_t *)p - HDR);
    blk->free = 1;

    /* Sweep the heap merging any run of adjacent free blocks. */
    for (block_t *b = g_head; b && b->next; ) {
        if (b->free && b->next->free) {
            b->size += HDR + b->next->size;
            b->next  = b->next->next;
        } else {
            b = b->next;
        }
    }
}

size_t heap_free_bytes(void)
{
    size_t total = 0;
    for (block_t *b = g_head; b; b = b->next)
        if (b->free) total += b->size;
    return total;
}

size_t heap_total_bytes(void) { return g_total; }
