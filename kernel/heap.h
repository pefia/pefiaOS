/* kernel/heap.h
 * -----------------------------------------------------------------------------
 * First-fit free-list kernel heap, living in the RAM just past _kernel_end.
 * -----------------------------------------------------------------------------
 */
#ifndef PEFIA_HEAP_H
#define PEFIA_HEAP_H

#include <stdint.h>
#include <stddef.h>

struct multiboot_info;

void   heap_init(uintptr_t start, size_t size);
void   heap_init_from_multiboot(struct multiboot_info *mb);
void  *kmalloc(size_t n);
void   kfree(void *p);
size_t heap_free_bytes(void);   /* for the memtest command */
size_t heap_total_bytes(void);

#endif /* PEFIA_HEAP_H */