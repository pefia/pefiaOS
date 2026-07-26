#include "sched.h"
#include "heap.h"
#include "util.h"

#define MAX_THREADS 16
#define STACK_SIZE  8192

typedef enum { TH_UNUSED = 0, TH_RUNNABLE, TH_RUNNING, TH_DEAD } TState;

typedef struct {
    uint32_t  esp;
    uint32_t *stack;      /* base of the allocated stack (NULL for thread 0) */
    thread_fn fn;
    TState    state;
    int       tid;
    uint32_t  beats;      /* how many times this thread has been scheduled */
    char      name[16];
} Thread;

static Thread threads[MAX_THREADS];
static int    thread_count;   /* high-water mark of slots ever used - bounds the
                               * introspection walk for `ps`; NOT a live count,
                               * so it must never gate spawning. */
static int    running;    /* index of the currently executing thread */
static int    next_tid;

/* old_esp: address of the uint32_t slot to save the outgoing esp into.
 * new_esp: the incoming thread's saved esp value (loaded directly). */
extern void sched_switch(uint32_t *old_esp, uint32_t new_esp);

static void copy_name(char *dst, const char *src)
{
    int i = 0;
    while (src[i] && i < 15) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

void sched_init(void)
{
    for (int i = 0; i < MAX_THREADS; i++) {
        threads[i].state = TH_UNUSED;
        threads[i].stack = 0;
    }
    thread_count = 1;
    running      = 0;
    next_tid     = 1;

    Thread *t0 = &threads[0];
    t0->stack = 0;
    t0->fn    = 0;
    t0->state = TH_RUNNING;
    t0->tid   = 0;
    t0->beats = 1;
    copy_name(t0->name, "kmain/wm");
}

/* A new thread starts life here: run its function, then mark itself dead and
 * yield forever so the scheduler routes around it. */
static void trampoline(void)
{
    threads[running].fn();
    threads[running].state = TH_DEAD;
    for (;;) sched_yield();
}

int sched_spawn(const char *name, thread_fn fn)
{
    int idx = -1;
    for (int i = 0; i < MAX_THREADS; i++)
        if (threads[i].state == TH_UNUSED || threads[i].state == TH_DEAD) { idx = i; break; }
    if (idx < 0) return -1;   /* every slot holds a live thread */

    /* A TH_DEAD slot still owns its stack, and all thread stacks are STACK_SIZE,
     * so reuse it instead of leaking it. Safe: the dead thread marked itself
     * TH_DEAD in the trampoline and then switched away, sched_yield never picks
     * TH_DEAD again, and threading is cooperative - so if we are here, nothing
     * is executing on that stack. (This is also why the trampoline itself must
     * not free the stack: it is still running on it.) */
    uint32_t *stack = threads[idx].stack;
    if (!stack) stack = (uint32_t *)kmalloc(STACK_SIZE);
    if (!stack) return -1;

    Thread *t = &threads[idx];
    t->stack = stack;
    t->fn    = fn;
    t->state = TH_RUNNABLE;
    t->tid   = next_tid++;
    t->beats = 0;
    copy_name(t->name, name);

    /* Prime the stack so the first switch's pop/ret sequence lands in the
     * trampoline. Order matches the pops in sched_switch: ebp,edi,esi,ebx,
     * eflags, return address (highest). */
    uint32_t *sp = (uint32_t *)((uint8_t *)stack + STACK_SIZE);
    *--sp = (uint32_t)trampoline;
    *--sp = 0x202;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    t->esp = (uint32_t)sp;

    if (idx >= thread_count) thread_count = idx + 1;
    return t->tid;
}

void sched_yield(void)
{
    int start = running;
    int next  = start;
    for (int step = 0; step < MAX_THREADS; step++) {
        next = (next + 1) % MAX_THREADS;
        if (threads[next].state == TH_RUNNABLE) break;
        if (next == start) return;
    }
    if (next == start || threads[next].state != TH_RUNNABLE) return;

    int prev = running;
    if (threads[prev].state == TH_RUNNING) threads[prev].state = TH_RUNNABLE;
    threads[next].state = TH_RUNNING;
    threads[next].beats++;
    running = next;

    sched_switch(&threads[prev].esp, threads[next].esp);
}

void sched_tick(void)
{
    /* Accounting hook for the timer IRQ. Intentionally does not switch - see
     * the header note on why preemption is gated. */
    if (running >= 0 && running < MAX_THREADS) {  }
}

int sched_count(void) { return thread_count; }

int sched_tid(int i)
{
    return (i >= 0 && i < thread_count) ? threads[i].tid : -1;
}

const char *sched_name(int i)
{
    return (i >= 0 && i < thread_count) ? threads[i].name : "";
}

const char *sched_state(int i)
{
    if (i < 0 || i >= thread_count) return "?       ";
    switch (threads[i].state) {
    case TH_RUNNING:  return "RUNNING ";
    case TH_RUNNABLE: return "RUNNABLE";
    case TH_DEAD:     return "DEAD    ";
    default:          return "UNUSED  ";
    }
}
