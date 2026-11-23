/* MezinXT - the NEC uPD765A floppy disk controller (upd765.h). Portable C11.
 * MezinXT's own code, written from the data sheet. */
#include "upd765.h"

#include <string.h>

enum { PH_IDLE, PH_CMD, PH_EXEC, PH_RESULT };
/* The stage of the command being executed: what the chip's deadline (t_cmd)
 * is for. */
enum {
    X_NONE,
    X_START,        /* the command has been accepted: check the drive, load the head */
    X_HEAD,         /* the head load time has passed */
    X_WAKE,         /* waiting for a turning disk (t_cmd: when it will turn, or never) */
    X_FIND,         /* the outcome of the ID search is due: a sector found (x.found) or ND */
    X_RT_IDX,       /* read track: the index pulse */
    X_BYTE,         /* a byte of the data field is due */
    X_END,          /* the end of the sector: commit a write, decide what comes next */
    X_FMT_IDX,      /* format: the index pulse */
    X_FMT_TICK,     /* format: a byte of a sector's ID is due */
    X_FMT_END       /* format: the end of the revolution: write the track */
};

#define CMD_LATENCY_US 24u      /* last command byte to first action */
#define RQM_DELAY_US   12u      /* the data register service interval */
#define RESET_INT_US   100u     /* RESET released to the first interrupt */
#define RECAL_PULSES   77u

static uint64_t now_of(const mx_upd765 *f) { return mx_dev_now(&f->dev); }

/* --- the pins ------------------------------------------------------------------------------ */

static void set_pin(mx_upd765 *f, int pin, uint8_t *cache, int level) {
    level = level != 0;
    if (*cache == level) return;
    *cache = (uint8_t)level;
    mx_dev_line(&f->dev, pin, level);
}
static void update_int(mx_upd765 *f) {
    set_pin(f, MX_UPD765_PIN_INT, &f->line_int, !f->in_reset && (f->int_cmd || f->int_status));
}
static void set_drq(mx_upd765 *f, int level) { set_pin(f, MX_UPD765_PIN_DRQ, &f->line_drq, level); }

static void rearm(mx_upd765 *f) {
    uint64_t m = f->t_cmd < f->t_rint ? f->t_cmd : f->t_rint;
    if (f->t_poll < m) m = f->t_poll;
    for (unsigned u = 0; u < 4; u++)
        if (f->t_seek[u] < m) m = f->t_seek[u];
    if (m != f->armed) {
        f->armed = m;
        mx_dev_schedule(&f->dev, m);
    }
}

/* --- times from Specify --------------------------------------------------------------------- */

static uint64_t step_time(const mx_upd765 *f) { return mx_fdd_us((16u - f->srt) * 2000u); }
static uint64_t hut_time(const mx_upd765 *f) { return mx_fdd_us((f->hut ? f->hut : 16u) * 32000u); }
static uint64_t hlt_time(const mx_upd765 *f) { return mx_fdd_us((f->hlt ? f->hlt : 128u) * 4000u); }

static int ready_input(const mx_upd765 *f, unsigned unit, uint64_t now) {
    if (f->ready_tied) return 1;
    return f->drive[unit] && mx_fdd_rotating(f->drive[unit], now);
}

/* --- reset ---------------------------------------------------------------------------------- */

static void assert_reset(mx_upd765 *f) {
    f->in_reset = 1;
    f->t_cmd = f->t_rint = f->t_poll = MX_NEVER;
    for (unsigned u = 0; u < 4; u++) f->t_seek[u] = MX_NEVER;
    f->phase = PH_IDLE;
    f->cmd_got = f->cmd_len = f->res_len = f->res_pos = 0;
    memset(&f->x, 0, sizeof f->x);
    f->int_cmd = f->int_status = 0;
    memset(f->pcn, 0, sizeof f->pcn);
    memset(f->seeking, 0, sizeof f->seeking);
    memset(f->se_pending, 0, sizeof f->se_pending);
    memset(f->rc_pending, 0, sizeof f->rc_pending);
    f->reset_n = f->reset_pos = 0;
    f->srt = f->hut = f->hlt = f->nd = 0;
    f->head_loaded = 0;
    f->rqm_at = 0;
    set_drq(f, 0);
    update_int(f);
    rearm(f);
}

static void release_reset(mx_upd765 *f) {
    if (!f->in_reset) return;
    const uint64_t now = now_of(f);
    f->in_reset = 0;
    f->phase = PH_IDLE;
    f->rqm_at = now;
    f->t_rint = now + mx_fdd_us(RESET_INT_US);
    rearm(f);
}

/* --- results -------------------------------------------------------------------------------- */

/* The reset results the program did not ask for are gone once anything else
 * interrupts: a BIOS that reads one (the Junior's) must see the seek end of
 * its next Recalibrate, not the second drive's ready change. */
static void drop_reset_results(mx_upd765 *f) { f->reset_pos = f->reset_n; }

static void enter_result(mx_upd765 *f, unsigned n, uint64_t t, int raise_int) {
    if (raise_int) drop_reset_results(f);
    f->phase = PH_RESULT;
    f->res_len = (uint8_t)n;
    f->res_pos = 0;
    f->rqm_at = t;
    f->x.stage = X_NONE;
    f->x.pending = 0;
    f->t_cmd = MX_NEVER;
    set_drq(f, 0);
    if (raise_int) f->int_cmd = 1;
    update_int(f);
}

/* The seven result bytes of a data command; ic 1 is abnormal termination. */
static void finish(mx_upd765 *f, uint64_t t, unsigned ic) {
    mx_upd765_exec *x = &f->x;
    f->res[0] = (uint8_t)(ic << 6 | x->st0 | x->head << 2 | x->unit);
    f->res[1] = x->st1;
    f->res[2] = x->st2;
    f->res[3] = x->c;
    f->res[4] = x->h;
    f->res[5] = x->r;
    f->res[6] = x->n;
    f->head_loaded = 1;
    f->head_until = t + hut_time(f);
    if (x->fdd) mx_fdd_notify(x->fdd, MX_DRIVE_HEAD_HOLD, t, (uint32_t)hut_time(f), 0);
    enter_result(f, 7, t, 1);
}
static void fail(mx_upd765 *f, uint64_t t, unsigned st1, unsigned st2) {
    f->x.st1 |= (uint8_t)st1;
    f->x.st2 |= (uint8_t)st2;
    finish(f, t, 1);
}

/* --- the data transfer ----------------------------------------------------------------------- */

/* A byte is waiting for the host (read) or wanted from it (write). */
static void request(mx_upd765 *f) {
    f->x.pending = 1;
    if (f->nd) {
        f->int_cmd = 1;
        update_int(f);
    } else set_drq(f, 1);
}
static void unrequest(mx_upd765 *f) {
    f->x.pending = 0;
    if (f->nd) {
        f->int_cmd = 0;
        update_int(f);
    } else set_drq(f, 0);
}

/* Which of the sectors' ID the controller sees next as the disk turns, and
 * when: sets the deadline of the outcome (a sector found, or the search
 * ended by the second index pulse). */
static void begin_find(mx_upd765 *f, uint64_t from) {
    mx_upd765_exec *x = &f->x;
    mx_fdd *d = x->fdd;
    const uint64_t rotating_at = d ? mx_fdd_rotating_at(d) : MX_NEVER;
    x->found = 0;
    if (rotating_at == MX_NEVER || from < rotating_at) {
        x->stage = X_WAKE;
        f->t_cmd = rotating_at;
        return;
    }
    const uint64_t deadline = mx_fdd_next_index(d, from) + MX_FDD_REV;
    uint64_t cursor = from;
    x->saw_id = 0;
    x->st2 &= (uint8_t)~MX_ST2_WC;
    for (;;) {
        uint64_t slot = 0;
        mx_fdd_id id = {0, 0, 0, 0};
        const int k = x->mfm ? mx_fdd_find_id(d, x->head, cursor, &slot, &id) : -1;
        if (k < 0) break;
        const uint64_t end = slot + mx_fdd_bytes(MX_FDD_ID_END);
        if (end >= deadline) break;
        x->saw_id = 1;
        if (x->op == MX_UPD765_READ_ID) {
            x->c = id.c; x->h = id.h; x->r = id.r; x->n = id.n;
        }
        if (x->op == MX_UPD765_READ_ID || (id.c == x->c && id.h == x->h && id.r == x->r && id.n == x->n)) {
            x->found = 1;
            x->slot_t = slot;
            x->stage = X_FIND;
            f->t_cmd = end;
            return;
        }
        if (id.c != x->c) x->st2 |= MX_ST2_WC;
        cursor = end;
    }
    x->fail_st1 = x->op == MX_UPD765_READ_ID ? MX_ST1_MA : (uint8_t)(MX_ST1_ND | (x->saw_id ? 0 : MX_ST1_MA));
    x->stage = X_FIND;
    f->t_cmd = deadline;
}

/* The wait for the next index pulse that format and read track start at. */
static void begin_index(mx_upd765 *f, uint64_t from, unsigned stage) {
    mx_upd765_exec *x = &f->x;
    mx_fdd *d = x->fdd;
    const uint64_t rotating_at = d ? mx_fdd_rotating_at(d) : MX_NEVER;
    if (rotating_at == MX_NEVER || from < rotating_at) {
        x->stage = X_WAKE;
        f->t_cmd = rotating_at;
        return;
    }
    x->stage = (uint8_t)stage;
    f->t_cmd = mx_fdd_next_index(d, from);
}

/* The sector in x.buf is read from the disk and its data field starts coming
 * (check_mark: a data mark of the wrong kind is a control mark). */
static void begin_read_transfer(mx_upd765 *f, uint64_t t, int check_mark) {
    mx_upd765_exec *x = &f->x;
    int deleted = 0;
    if (mx_fdd_read_sector(x->fdd, x->head, x->r, x->buf, &deleted) != MX_FDD_OK) {
        fail(f, t, MX_ST1_ND, 0);
        return;
    }
    x->skip = 0;
    if (check_mark && (deleted != 0) != (x->op == MX_UPD765_READ_DELETED)) {
        x->st2 |= MX_ST2_CM;
        if (x->sk) x->skip = 1;
        else x->stop = 1;
    }
    x->dir = 1;
    x->bytepos = 0;
    if (x->skip) {
        x->stage = X_END;
        f->t_cmd = x->slot_t + mx_fdd_bytes(MX_FDD_DATA_END);
        return;
    }
    x->data_t = x->slot_t + mx_fdd_bytes(MX_FDD_DATA);
    x->stage = X_BYTE;
    f->t_cmd = x->data_t + mx_fdd_bytes(1);
}

/* An ID matched (or, for Read ID, passed): `t` is the end of the ID field. */
static void sector_start(mx_upd765 *f, uint64_t t) {
    mx_upd765_exec *x = &f->x;
    switch (x->op) {
    case MX_UPD765_READ_ID:
        finish(f, t, 0);
        break;
    case MX_UPD765_WRITE_DATA:
    case MX_UPD765_WRITE_DELETED:
        x->dir = 0;
        x->bytepos = 0;
        x->data_t = x->slot_t + mx_fdd_bytes(MX_FDD_DATA);
        x->stage = X_BYTE;
        f->t_cmd = x->data_t;
        request(f);
        break;
    default:
        begin_read_transfer(f, t, 1);
        break;
    }
}

/* The ID registers after a sector, as the data sheet's table has them. */
static void advance(mx_upd765_exec *x) {
    if (x->r != x->eot) {
        x->r++;
        return;
    }
    x->r = 1;
    if (x->mt && x->head == 0) {
        x->head = 1;
        x->h = 1;
    } else {
        x->c++;
        if (x->mt) x->h = 0;
    }
}

static void rt_sector(mx_upd765 *f, uint64_t t) {
    mx_upd765_exec *x = &f->x;
    mx_fdd_id id = {0, 0, 0, 0};
    uint64_t slot = 0;
    const int k = x->mfm ? mx_fdd_find_id(x->fdd, x->head, x->fmt_t0 + mx_fdd_bytes(MX_FDD_PRE + x->rt_slot * MX_FDD_SLOT), &slot, &id) : -1;
    if (k < 0) {
        fail(f, t, MX_ST1_ND | MX_ST1_MA, 0);
        return;
    }
    if ((unsigned)k != x->rt_slot) {            /* the track has no more sectors */
        x->r++;
        finish(f, t, 0);
        return;
    }
    x->c = id.c; x->h = id.h; x->r = id.r; x->n = id.n;
    x->slot_t = slot;
    begin_read_transfer(f, t, 0);
}

/* The sector is over: what next. */
static void evaluate(mx_upd765 *f, uint64_t t) {
    mx_upd765_exec *x = &f->x;
    if (x->op == MX_UPD765_READ_TRACK) {
        x->rt_count++;
        if (x->tc || x->rt_count >= x->eot || x->rt_slot + 1 >= x->fdd->geom.sectors) {
            x->r++;
            finish(f, t, 0);
        } else {
            x->rt_slot++;
            rt_sector(f, t);
        }
        return;
    }
    if (x->tc || x->stop) {
        advance(x);
        finish(f, t, 0);
        return;
    }
    if (x->r == x->eot && !(x->mt && x->head == 0)) {
        advance(x);
        x->st1 |= MX_ST1_EN;
        finish(f, t, 1);
        return;
    }
    advance(x);
    begin_find(f, t);
}

static void on_byte(mx_upd765 *f, uint64_t t) {
    mx_upd765_exec *x = &f->x;
    if (x->dir) {                               /* controller to host */
        if (x->pending) { fail(f, t, MX_ST1_OR, 0); return; }
        if (x->tc) {                            /* the host is done: the rest of the sector passes */
            x->stage = X_END;
            f->t_cmd = x->data_t + mx_fdd_bytes(MX_FDD_DATA_END - MX_FDD_DATA);
            return;
        }
        x->dreg = x->buf[x->bytepos++];
        request(f);
        if (x->bytepos >= MX_FDD_SECTOR) {
            x->stage = X_END;
            f->t_cmd = x->data_t + mx_fdd_bytes(MX_FDD_DATA_END - MX_FDD_DATA);
        } else f->t_cmd = x->data_t + mx_fdd_bytes(x->bytepos + 1u);
        return;
    }
    /* host to controller: byte `bytepos` is consumed now */
    if (x->pending) { unrequest(f); fail(f, t, MX_ST1_OR, 0); return; }
    x->buf[x->bytepos++] = x->dreg;
    if (x->tc || x->bytepos >= MX_FDD_SECTOR) {
        memset(x->buf + x->bytepos, 0, MX_FDD_SECTOR - x->bytepos);
        x->stage = X_END;
        f->t_cmd = x->data_t + mx_fdd_bytes(MX_FDD_DATA_END - MX_FDD_DATA);
        return;
    }
    request(f);
    f->t_cmd = x->data_t + mx_fdd_bytes(x->bytepos);
}

static void on_end(mx_upd765 *f, uint64_t t) {
    mx_upd765_exec *x = &f->x;
    if (x->dir) {
        if (x->pending) { fail(f, t, MX_ST1_OR, 0); return; }
    } else {
        const int rc = mx_fdd_write_sector(x->fdd, x->head, x->r, x->buf, x->op == MX_UPD765_WRITE_DELETED);
        if (rc == MX_FDD_PROTECTED) { fail(f, t, MX_ST1_NW, 0); return; }
        if (rc != MX_FDD_OK) { fail(f, t, MX_ST1_ND, 0); return; }
    }
    evaluate(f, t);
}

/* --- format track ---------------------------------------------------------------------------- */

static uint64_t fmt_tick_time(const mx_upd765_exec *x, unsigned q) {
    const uint64_t pitch = 62u + (128u << (x->fmt_n & 7u)) + x->fmt_gpl;
    return x->fmt_t0 + mx_fdd_bytes(MX_FDD_PRE + (q / 4u) * pitch + 16u + q % 4u);
}

static void on_fmt_index(mx_upd765 *f, uint64_t t) {
    mx_upd765_exec *x = &f->x;
    x->fmt_t0 = t;
    x->fmt_q = 0;
    x->dir = 0;
    if (x->fmt_sc == 0) {
        x->stage = X_FMT_END;
        f->t_cmd = t + MX_FDD_REV;
        return;
    }
    request(f);
    x->stage = X_FMT_TICK;
    f->t_cmd = fmt_tick_time(x, 0);
}

static void on_fmt_tick(mx_upd765 *f, uint64_t t) {
    mx_upd765_exec *x = &f->x;
    if (x->pending) { unrequest(f); fail(f, t, MX_ST1_OR, 0); return; }
    mx_fdd_id *id = &x->fmt_ids[x->fmt_q / 4u];
    switch (x->fmt_q % 4u) {
    case 0: id->c = x->dreg; break;
    case 1: id->h = x->dreg; break;
    case 2: id->r = x->dreg; break;
    default: id->n = x->dreg; break;
    }
    x->fmt_q++;
    if (x->fmt_q == 4u * x->fmt_sc) {
        const uint64_t rev_end = x->fmt_t0 + MX_FDD_REV;
        x->stage = X_FMT_END;
        f->t_cmd = rev_end > t ? rev_end : t;
        return;
    }
    request(f);
    f->t_cmd = fmt_tick_time(x, x->fmt_q);
}

static void on_fmt_end(mx_upd765 *f, uint64_t t) {
    mx_upd765_exec *x = &f->x;
    if (!x->mfm || x->fmt_n != 2) { fail(f, t, MX_ST1_ND, 0); return; }
    const int rc = mx_fdd_format_track(x->fdd, x->head, x->fmt_ids, x->fmt_sc, x->fmt_d);
    if (rc == MX_FDD_PROTECTED) { fail(f, t, MX_ST1_NW, 0); return; }
    if (rc != MX_FDD_OK) { fail(f, t, MX_ST1_ND, 0); return; }
    if (x->fmt_sc) {
        const mx_fdd_id *last = &x->fmt_ids[x->fmt_sc - 1u];
        x->c = last->c; x->h = last->h; x->r = last->r;
    }
    x->n = x->fmt_n;
    finish(f, t, 0);
}

/* --- starting a data command ----------------------------------------------------------------- */

static void begin_op(mx_upd765 *f, uint64_t t) {
    mx_upd765_exec *x = &f->x;
    switch (x->op) {
    case MX_UPD765_FORMAT: begin_index(f, t, X_FMT_IDX); break;
    case MX_UPD765_READ_TRACK: begin_index(f, t, X_RT_IDX); break;
    default: begin_find(f, t); break;
    }
}

static void on_start(mx_upd765 *f, uint64_t t) {
    mx_upd765_exec *x = &f->x;
    if (!ready_input(f, x->unit, t)) {
        x->st0 |= MX_ST0_NR;
        finish(f, t, 1);
        return;
    }
    if ((x->op == MX_UPD765_WRITE_DATA || x->op == MX_UPD765_WRITE_DELETED || x->op == MX_UPD765_FORMAT) &&
        x->fdd && mx_fdd_wprot(x->fdd)) {
        fail(f, t, MX_ST1_NW, 0);
        return;
    }
    if (!f->head_loaded || t >= f->head_until) {
        x->stage = X_HEAD;
        f->t_cmd = t + hlt_time(f);
        if (x->fdd) mx_fdd_notify(x->fdd, MX_DRIVE_HEAD_LOAD, t, 0, 0);
        return;
    }
    begin_op(f, t);
}

static void on_cmd_timer(mx_upd765 *f, uint64_t t) {
    mx_upd765_exec *x = &f->x;
    f->t_cmd = MX_NEVER;
    switch (x->stage) {
    case X_START: on_start(f, t); break;
    case X_HEAD: f->head_loaded = 1; begin_op(f, t); break;
    case X_WAKE: begin_op(f, t); break;
    case X_FIND:
        if (x->found) sector_start(f, t);
        else fail(f, t, x->fail_st1, 0);
        break;
    case X_RT_IDX:
        x->fmt_t0 = t;
        x->rt_slot = x->rt_count = 0;
        rt_sector(f, t);
        break;
    case X_BYTE: on_byte(f, t); break;
    case X_END: on_end(f, t); break;
    case X_FMT_IDX: on_fmt_index(f, t); break;
    case X_FMT_TICK: on_fmt_tick(f, t); break;
    case X_FMT_END: on_fmt_end(f, t); break;
    default: break;
    }
}

/* --- seek and recalibrate -------------------------------------------------------------------- */

static void seek_done(mx_upd765 *f, unsigned u, unsigned st0) {
    f->seeking[u] = 0;
    f->t_seek[u] = MX_NEVER;
    drop_reset_results(f);
    f->se_pending[u] = 1;
    f->se_st0[u] = (uint8_t)(st0 | f->seek_hd[u] << 2 | u);
    f->int_status = 1;
    update_int(f);
}

static void seek_event(mx_upd765 *f, unsigned u, uint64_t t) {
    mx_fdd *d = f->seek_drive[u];
    if (!f->seek_recal[u]) {
        if (f->seek_rem[u] == 0) {
            seek_done(f, u, MX_ST0_SE);
            return;
        }
        if (d) mx_fdd_step(d, f->seek_dir[u], t);
        f->pcn[u] = (uint8_t)(f->pcn[u] + f->seek_dir[u]);
        f->seek_rem[u]--;
        f->t_seek[u] = t + step_time(f);
        return;
    }
    if (d && mx_fdd_track0(d)) {
        f->pcn[u] = 0;
        seek_done(f, u, MX_ST0_SE);
        return;
    }
    if (f->seek_pulses[u] >= RECAL_PULSES) {
        seek_done(f, u, MX_ST0_SE | MX_ST0_EC | MX_ST0_IC_ABNORMAL);
        return;
    }
    if (d) mx_fdd_step(d, -1, t);
    if (f->pcn[u]) f->pcn[u]--;
    f->seek_pulses[u]++;
    f->t_seek[u] = t + step_time(f);
}

/* --- the command phase ----------------------------------------------------------------------- */

static unsigned command_length(unsigned code) {
    switch (code) {
    case MX_UPD765_READ_TRACK: case MX_UPD765_WRITE_DATA: case MX_UPD765_READ_DATA:
    case MX_UPD765_WRITE_DELETED: case MX_UPD765_READ_DELETED: return 9;
    case MX_UPD765_FORMAT: return 6;
    case MX_UPD765_SPECIFY: case MX_UPD765_SEEK: return 3;
    case MX_UPD765_SENSE_DRIVE: case MX_UPD765_RECALIBRATE: case MX_UPD765_READ_ID: return 2;
    default: return 1;                          /* Sense Interrupt Status, and the invalid codes */
    }
}

static void start_exec(mx_upd765 *f, uint64_t now) {
    const uint8_t *c = f->cmd;
    mx_upd765_exec *x = &f->x;
    memset(x, 0, sizeof *x);
    x->op = c[0] & 0x1Fu;
    x->mt = (c[0] >> 7) & 1u;
    x->mfm = (c[0] >> 6) & 1u;
    x->sk = (c[0] >> 5) & 1u;
    x->unit = c[1] & 3u;
    x->head = (c[1] >> 2) & 1u;
    x->fdd = f->drive[x->unit];
    if (x->op == MX_UPD765_FORMAT) {
        x->fmt_n = c[2]; x->fmt_sc = c[3]; x->fmt_gpl = c[4]; x->fmt_d = c[5];
        x->n = c[2];
    } else if (x->op != MX_UPD765_READ_ID) {
        x->c = c[2]; x->h = c[3]; x->r = c[4]; x->n = c[5]; x->eot = c[6]; x->gpl = c[7]; x->dtl = c[8];
    }
    x->dir = x->op == MX_UPD765_READ_DATA || x->op == MX_UPD765_READ_DELETED || x->op == MX_UPD765_READ_TRACK;
    x->stage = X_START;
    f->phase = PH_EXEC;
    f->t_cmd = now + mx_fdd_us(CMD_LATENCY_US);
}

static void start_command(mx_upd765 *f, uint64_t now) {
    const uint8_t *c = f->cmd;
    const unsigned code = c[0] & 0x1Fu;
    const uint64_t ready = now + mx_fdd_us(RQM_DELAY_US);
    switch (code) {
    case MX_UPD765_SPECIFY:
        f->srt = c[1] >> 4;
        f->hut = c[1] & 0x0Fu;
        f->hlt = c[2] >> 1;
        f->nd = c[2] & 1u;
        f->phase = PH_IDLE;
        f->rqm_at = ready;
        break;
    case MX_UPD765_SENSE_DRIVE: {
        const unsigned u = c[1] & 3u;
        const mx_fdd *d = f->drive[u];
        unsigned st3 = u | (c[1] & 4u);
        if (d && d->installed) st3 |= MX_ST3_TS;
        if (d && mx_fdd_track0(d)) st3 |= MX_ST3_T0;
        if (d && mx_fdd_wprot(d)) st3 |= MX_ST3_WP;
        if (ready_input(f, u, now)) st3 |= MX_ST3_RY;
        f->res[0] = (uint8_t)st3;
        enter_result(f, 1, ready, 0);
        break;
    }
    case MX_UPD765_SENSE_INT: {
        unsigned n = 1, u;
        f->int_status = 0;
        if (f->reset_pos < f->reset_n) {
            u = f->reset_list[f->reset_pos++];
            f->res[0] = (uint8_t)(MX_ST0_IC_READY | u);
            f->res[1] = 0;
            n = 2;
        } else {
            for (u = 0; u < 4 && !f->rc_pending[u]; u++) {}
            if (u < 4) {
                f->rc_pending[u] = 0;
                f->res[0] = (uint8_t)(MX_ST0_IC_READY | u);
                f->res[1] = f->pcn[u];
                n = 2;
            } else {
                for (u = 0; u < 4 && !f->se_pending[u]; u++) {}
                if (u < 4) {
                    f->se_pending[u] = 0;
                    f->res[0] = f->se_st0[u];
                    f->res[1] = f->pcn[u];
                    n = 2;
                } else f->res[0] = MX_ST0_IC_INVALID;
            }
        }
        for (u = 0; u < 4; u++)
            if (f->rc_pending[u] || f->se_pending[u]) f->int_status = 1;
        update_int(f);
        enter_result(f, n, ready, 0);
        break;
    }
    case MX_UPD765_RECALIBRATE:
    case MX_UPD765_SEEK: {
        const unsigned u = c[1] & 3u;
        f->seeking[u] = 1;
        f->seek_recal[u] = code == MX_UPD765_RECALIBRATE;
        f->seek_hd[u] = (c[1] >> 2) & 1u;
        f->seek_drive[u] = f->drive[u];
        f->seek_pulses[u] = 0;
        if (code == MX_UPD765_SEEK) {
            f->seek_target[u] = c[2];
            f->seek_dir[u] = c[2] >= f->pcn[u] ? 1 : -1;
            f->seek_rem[u] = (uint8_t)(c[2] >= f->pcn[u] ? c[2] - f->pcn[u] : f->pcn[u] - c[2]);
        }
        f->t_seek[u] = now + mx_fdd_us(CMD_LATENCY_US);
        f->phase = PH_IDLE;
        f->rqm_at = ready;
        break;
    }
    case MX_UPD765_READ_TRACK: case MX_UPD765_WRITE_DATA: case MX_UPD765_READ_DATA:
    case MX_UPD765_WRITE_DELETED: case MX_UPD765_READ_ID: case MX_UPD765_READ_DELETED:
    case MX_UPD765_FORMAT:
        start_exec(f, now);
        break;
    default:                                    /* invalid, and the Scan commands */
        f->res[0] = MX_ST0_IC_INVALID;
        enter_result(f, 1, ready, 0);
        break;
    }
}

/* --- the READY input -------------------------------------------------------------------------- */

/* With READY following the drives: a change of a drive's ready state is an
 * interrupt (IC = 11); the next change that is due by itself is a spindle
 * reaching speed, polled at that time. */
static void poll_ready(mx_upd765 *f, uint64_t t) {
    uint64_t next = MX_NEVER;
    if (!f->ready_tied && !f->in_reset && f->t_rint == MX_NEVER) {
        for (unsigned u = 0; u < 4; u++) {
            const uint8_t r = (uint8_t)ready_input(f, u, t);
            if (r != f->ready_prev[u]) {
                f->ready_prev[u] = r;
                f->rc_pending[u] = 1;
                f->int_status = 1;
            }
            const uint64_t at = f->drive[u] ? mx_fdd_rotating_at(f->drive[u]) : MX_NEVER;
            if (at != MX_NEVER && at > t && at < next) next = at;
        }
        update_int(f);
    }
    f->t_poll = next;
}

/* --- the bus interface ----------------------------------------------------------------------- */

static uint8_t read_msr(const mx_upd765 *f, uint64_t now) {
    if (f->in_reset) return 0;
    unsigned v = 0;
    for (unsigned u = 0; u < 4; u++)
        if (f->seeking[u]) v |= 1u << u;
    switch (f->phase) {
    case PH_IDLE:
        if (now >= f->rqm_at) v |= MX_UPD765_RQM;
        break;
    case PH_CMD:
        v |= MX_UPD765_CB;
        if (now >= f->rqm_at) v |= MX_UPD765_RQM;
        break;
    case PH_EXEC:
        v |= MX_UPD765_CB;
        if (f->nd) {
            v |= MX_UPD765_EXM;
            if (f->x.pending) v |= MX_UPD765_RQM;
            if (f->x.dir) v |= MX_UPD765_DIO;
        }
        break;
    default:
        v |= MX_UPD765_CB | MX_UPD765_DIO;
        if (now >= f->rqm_at) v |= MX_UPD765_RQM;
        break;
    }
    return (uint8_t)v;
}

static uint8_t read_data(mx_upd765 *f, uint64_t now) {
    if (f->in_reset) return 0xFF;
    if (f->phase == PH_RESULT) {
        const uint8_t v = f->res[f->res_pos];
        if (f->res_pos == 0 && f->int_cmd) {
            f->int_cmd = 0;
            update_int(f);
        }
        f->res_pos++;
        if (f->res_pos >= f->res_len) f->phase = PH_IDLE;
        f->rqm_at = now + mx_fdd_us(RQM_DELAY_US);
        return v;
    }
    if (f->phase == PH_EXEC && f->nd && f->x.dir && f->x.pending) {
        const uint8_t v = f->x.dreg;
        unrequest(f);
        return v;
    }
    return 0xFF;
}

static void write_data(mx_upd765 *f, uint64_t now, uint8_t v) {
    if (f->in_reset) return;
    if (f->phase == PH_IDLE) {
        f->cmd[0] = v;
        f->cmd_len = (uint8_t)command_length(v & 0x1Fu);
        f->cmd_got = 1;
        f->phase = PH_CMD;
        f->rqm_at = now + mx_fdd_us(RQM_DELAY_US);
    } else if (f->phase == PH_CMD) {
        f->cmd[f->cmd_got++] = v;
        f->rqm_at = now + mx_fdd_us(RQM_DELAY_US);
    } else if (f->phase == PH_EXEC && f->nd && !f->x.dir && f->x.pending) {
        f->x.dreg = v;
        unrequest(f);
        return;
    } else return;
    if (f->cmd_got >= f->cmd_len) start_command(f, now);
}

static uint32_t chip_read(void *dev, unsigned space, uint32_t offset, unsigned size) {
    mx_upd765 *f = dev;
    const uint64_t now = now_of(f);
    (void)space; (void)size;
    const uint32_t v = (offset & 1u) ? read_data(f, now) : read_msr(f, now);
    rearm(f);
    return v;
}

static void chip_write(void *dev, unsigned space, uint32_t offset, unsigned size, uint32_t value) {
    mx_upd765 *f = dev;
    (void)space; (void)size;
    if (offset & 1u) write_data(f, now_of(f), (uint8_t)value);
    rearm(f);
}

static void chip_event(void *dev, uint64_t now) {
    mx_upd765 *f = dev;
    f->armed = MX_NEVER;                        /* the host has cleared the deadline that fired */
    for (;;) {
        uint64_t best = f->t_cmd;
        int which = 0;
        if (f->t_rint < best) { best = f->t_rint; which = 1; }
        if (f->t_poll < best) { best = f->t_poll; which = 2; }
        for (unsigned u = 0; u < 4; u++)
            if (f->t_seek[u] < best) { best = f->t_seek[u]; which = 3 + (int)u; }
        if (best == MX_NEVER || best > now) break;
        if (which == 0) on_cmd_timer(f, best);
        else if (which == 1) {
            f->t_rint = MX_NEVER;
            f->reset_n = f->reset_pos = 0;
            for (unsigned u = 0; u < 4; u++) {
                f->ready_prev[u] = (uint8_t)ready_input(f, u, best);
                if (f->ready_prev[u]) f->reset_list[f->reset_n++] = (uint8_t)u;
            }
            if (f->reset_n) {
                f->int_status = 1;
                update_int(f);
            }
            poll_ready(f, best);
        } else if (which == 2) poll_ready(f, best);
        else seek_event(f, (unsigned)(which - 3), best);
    }
    rearm(f);
}

static void chip_reset(void *dev) {
    mx_upd765 *f = dev;
    assert_reset(f);
    release_reset(f);
}

static void chip_input(void *dev, int pin, int state) {
    mx_upd765 *f = dev;
    if (pin == MX_UPD765_PIN_TC) {
        if (state && f->phase == PH_EXEC) f->x.tc = 1;
    } else if (pin == MX_UPD765_PIN_RESET) {
        if (state) assert_reset(f);
        else release_reset(f);
    }
}

static uint8_t chip_dack(void *dev, int channel, int to_device, uint8_t value, int tc) {
    mx_upd765 *f = dev;
    uint8_t r = 0xFF;
    (void)channel;
    if (f->phase == PH_EXEC && !f->nd) {
        mx_upd765_exec *x = &f->x;
        if (x->pending) {
            if (!to_device && x->dir) {
                r = x->dreg;
                unrequest(f);
            } else if (to_device && !x->dir) {
                x->dreg = value;
                unrequest(f);
            }
        }
        if (tc) x->tc = 1;
    }
    return r;
}

static const mx_dev_ops UPD765_OPS = {chip_read, chip_write, chip_event, chip_reset, chip_input, NULL, chip_dack};

void mx_upd765_init(mx_upd765 *f, const mx_hw_host *host, const char *name) {
    memset(f, 0, sizeof *f);
    f->dev.ops = &UPD765_OPS;
    f->dev.host = host;
    f->dev.name = name;
    f->ready_tied = 1;
    f->t_cmd = f->t_rint = f->t_poll = f->armed = MX_NEVER;
    for (unsigned u = 0; u < 4; u++) f->t_seek[u] = MX_NEVER;
}

void mx_upd765_attach(mx_upd765 *f, unsigned unit, mx_fdd *drive) {
    if (unit < 4) f->drive[unit] = drive;
}

void mx_upd765_ready_wiring(mx_upd765 *f, int tied_high) {
    const uint64_t now = now_of(f);
    f->ready_tied = tied_high != 0;
    for (unsigned u = 0; u < 4; u++) f->ready_prev[u] = (uint8_t)ready_input(f, u, now);
    poll_ready(f, now);
    rearm(f);
}

void mx_upd765_drive_changed(mx_upd765 *f) {
    const uint64_t now = now_of(f);
    poll_ready(f, now);
    if (f->phase == PH_EXEC) {
        mx_upd765_exec *x = &f->x;
        const int idle = !(x->fdd && mx_fdd_rotating(x->fdd, now));
        if (x->stage == X_WAKE || (idle && (x->stage == X_FIND || x->stage == X_RT_IDX || x->stage == X_FMT_IDX))) begin_op(f, now);
    }
    rearm(f);
}
