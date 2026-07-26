#ifndef PEFIA_SCHED_H
#define PEFIA_SCHED_H

typedef void (*thread_fn)(void);

void        sched_init(void);              /* registers the boot context as thread 0 */
int         sched_spawn(const char *name, thread_fn fn);
void        sched_yield(void);             /* cooperatively switch to the next runnable thread */
void        sched_tick(void);              /* called from the timer IRQ (accounting only) */

int         sched_count(void);
int         sched_tid(int i);
const char *sched_name(int i);
const char *sched_state(int i);

#endif
