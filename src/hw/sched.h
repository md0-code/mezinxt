/* MezinXT - the board's event scheduler.
 *
 * Each chip has at most one deadline (hw.h: mx_hw_host.schedule arms it,
 * MX_NEVER cancels, re-arming replaces it), in OSC periods. The board asks
 * the scheduler to run every deadline up to a time; each falls due in time
 * order (ties in the order the chips first scheduled) and calls the chip's
 * ops->event with the deadline as `now`. While an event runs, mx_sched_now
 * gives that deadline, so whatever the chip does then (an edge it drives, a
 * new deadline it arms) is stamped with its own time, not the CPU's.
 *
 * Nothing runs per clock: a run with no deadline due costs one comparison.
 * A deadline armed in the past runs at once, at the time of the event that
 * armed it (time never goes backwards inside a run).
 *
 * Portable C11. MezinXT's own code. */
#ifndef MX_SCHED_H
#define MX_SCHED_H

#include "hw.h"

#define MX_SCHED_MAX 32

typedef struct mx_sched {
    mx_dev *dev[MX_SCHED_MAX];
    uint64_t when[MX_SCHED_MAX];
    unsigned count;
    uint64_t next;              /* the earliest deadline (MX_NEVER: none) */
    uint64_t event_time;        /* the deadline of the event running */
    int in_event;               /* an event is running */
} mx_sched;

void mx_sched_init(mx_sched *s);
/* Arms dev's deadline (MX_NEVER cancels). 0 when the table is full. */
int mx_sched_set(mx_sched *s, mx_dev *dev, uint64_t when);
/* dev's deadline, MX_NEVER when none. */
uint64_t mx_sched_get(const mx_sched *s, const mx_dev *dev);
/* Runs every deadline at or before `until`, in time order. */
void mx_sched_run(mx_sched *s, uint64_t until);
/* Inline check for the hot paths: anything due at `t`? */
static inline int mx_sched_due(const mx_sched *s, uint64_t t) { return s->next <= t; }

#endif /* MX_SCHED_H */
