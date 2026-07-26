#include "heap.h"
#include "multiboot.h"
#include "util.h"

#define HEAP_ALIGN 8

extern uint8_t _kernel_end;

typedef struct block {
    size_t        size;   /* payload bytes, not counting this header */
    struct block *next;   /* next block by address (NULL at the tail) */
    int           free;
    uint32_t      magic;  /* HEAP_MAGIC; lets kfree reject pointers we never handed out */
} block_t;

#define HEAP_MAGIC 0x48454150u

/* Header size rounded up so every returned payload is 8-byte aligned. */
#define HDR (((sizeof(block_t) + HEAP_ALIGN - 1) / HEAP_ALIGN) * HEAP_ALIGN)

static block_t *heap_head;
static size_t   heap_size_bytes;

static size_t round_up(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

void heap_init(uintptr_t start, size_t size)
{
    uintptr_t base = round_up(start, HEAP_ALIGN);
    size -= (base - start);

    heap_head = (block_t *)base;
    heap_head->size = size - HDR;
    heap_head->next = NULL;
    heap_head->free = 1;
    heap_head->magic = HEAP_MAGIC;
    heap_size_bytes = size;
}

/* Multiboot hands us a memory map (when the bootloader bothered to build
 * one) listing every RAM region and whether it's usable. We want the
 * biggest usable region above the kernel image and hand the whole thing
 * to heap_init. Some bootloaders skip the map and just give us mem_upper
 * instead, so that's the fallback. */
void heap_init_from_multiboot(struct multiboot_info *mb)
{
    uintptr_t kernel_top = (uintptr_t)&_kernel_end;
    uintptr_t region_start = 0;
    size_t    region_size  = 0;

    if (mb->flags & MULTIBOOT_INFO_MMAP) {
        uint8_t *entry = (uint8_t *)mb->mmap_addr;
        uint8_t *mmap_end = entry + mb->mmap_length;
        while (entry < mmap_end) {
            struct multiboot_mmap_entry *e = (struct multiboot_mmap_entry *)entry;
            if (e->type == MULTIBOOT_MEMORY_AVAILABLE && e->addr < 0x100000000ULL) {
                uintptr_t base  = (uintptr_t)e->addr;
                uint64_t  end64 = e->addr + e->len;
                if (end64 > 0x100000000ULL) end64 = 0x100000000ULL;
                uintptr_t end   = (uintptr_t)end64;
                uintptr_t start = (base > kernel_top) ? base : kernel_top;
                if (end > start && (end - start) > region_size) {
                    region_size  = end - start;
                    region_start = start;
                }
            }
            entry += e->size + 4;   /* the size field doesn't count itself */
        }
    }

    if (region_size == 0 && (mb->flags & MULTIBOOT_INFO_MEMORY)) {
        uintptr_t end = 0x100000 + (uintptr_t)mb->mem_upper * 1024;
        if (end > kernel_top) { region_start = kernel_top; region_size = end - kernel_top; }
    }

    if (region_size > HDR + HEAP_ALIGN)
        heap_init(region_start, region_size);
}

void *kmalloc(size_t n)
{
    if (n == 0) return NULL;
    n = round_up(n, HEAP_ALIGN);

    for (block_t *b = heap_head; b; b = b->next) {
        if (!b->free || b->size < n) continue;

        /* Only split off a new block if the leftover is actually big
         * enough to be useful once it has its own header. Otherwise we'd
         * just be handing out a few unusable bytes as their own "free"
         * block forever. */
        if (b->size >= n + HDR + HEAP_ALIGN) {
            block_t *rest = (block_t *)((uint8_t *)b + HDR + n);
            rest->size = b->size - n - HDR;
            rest->free = 1;
            rest->magic = HEAP_MAGIC;
            rest->next = b->next;
            b->next  = rest;
            b->size  = n;
        }
        b->free = 0;
        return (uint8_t *)b + HDR;
    }
    return NULL;
}

void kfree(void *p)
{
    if (!p) return;

    /* A bogus or double-freed pointer must not reach the coalescing pass
     * below - it rewrites headers across the whole heap, so one bad free
     * would surface as an unrelated crash much later. Bounds first (so we
     * never read a header outside the heap), then the magic/free check. */
    uint8_t *heap_lo = (uint8_t *)heap_head;
    if (!heap_head ||
        (uint8_t *)p < heap_lo + HDR ||
        (uint8_t *)p >= heap_lo + heap_size_bytes)
        return;
    block_t *blk = (block_t *)((uint8_t *)p - HDR);
    if (blk->magic != HEAP_MAGIC || blk->free)
        return;
    blk->free = 1;

    /* Coalesce every run of adjacent free blocks in one pass. This is
     * O(n) over the whole heap on every free, which is fine at the scale
     * this kernel operates at - if that ever changes, tracking free-block
     * neighbours directly would be the fix. */
    for (block_t *b = heap_head; b && b->next; ) {
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
    for (block_t *b = heap_head; b; b = b->next)
        if (b->free) total += b->size;
    return total;
}

size_t heap_total_bytes(void) { return heap_size_bytes; }
