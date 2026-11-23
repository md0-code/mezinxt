/* MezinXT - the board's event scheduler (sched.h). MezinXT's own code. */
#include "sched.h"

#include <string.h>

static void recompute(mx_sched *s) {
    uint64_t best = MX_NEVER;
    for (unsigned i = 0; i < s->count; ++i)
        if (s->when[i] < best) best = s->when[i];
    s->next = best;
}

void mx_sched_init(mx_sched *s) {
    memset(s, 0, sizeof *s);
    s->next = MX_NEVER;
}

int mx_sched_set(mx_sched *s, mx_dev *dev, uint64_t when) {
    unsigned i = 0;
    while (i < s->count && s->dev[i] != dev) ++i;
    if (i == s->count) {
        if (when == MX_NEVER) return 1;
        if (s->count == MX_SCHED_MAX) return 0;
        s->dev[s->count++] = dev;
    }
    /* Time never goes backwards inside a run: a deadline already past is
     * due at the time of the event that arms it. */
    if (s->in_event && when < s->event_time) when = s->event_time;
    const uint64_t old = s->when[i];
    s->when[i] = when;
    if (when <= s->next) s->next = when;
    else if (old == s->next) recompute(s);
    return 1;
}

uint64_t mx_sched_get(const mx_sched *s, const mx_dev *dev) {
    for (unsigned i = 0; i < s->count; ++i)
        if (s->dev[i] == dev) return s->when[i];
    return MX_NEVER;
}

void mx_sched_run(mx_sched *s, uint64_t until) {
    if (s->in_event) return;              /* an event's own catch-up: the run in progress continues */
    while (s->next <= until) {
        unsigned first = 0;
        for (unsigned i = 1; i < s->count; ++i)
            if (s->when[i] < s->when[first]) first = i;
        const uint64_t when = s->when[first];
        mx_dev *dev = s->dev[first];
        s->when[first] = MX_NEVER;
        recompute(s);
        s->in_event = 1;
        s->event_time = when;
        dev->ops->event(dev, when);
        s->in_event = 0;
    }
}
