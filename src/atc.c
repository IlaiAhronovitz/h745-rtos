/* atc.c — AT command engine, ported from cellmux's ATSession.
 *
 * Faithful to session.py/parser.py/urc.py: the classification order, the
 * prefix derivation, the resync numbers (200 ms quiet / 2000 ms cap) and
 * the "solicited beats URC while in flight" rule are all verbatim; the
 * deviations are the bounded buffers and the poll-based waits, both forced
 * by C / the kernel's semaphore surface. See docs and cellmux for history.
 */
#include "atc.h"
#include "libmin.h"

/* ---- tunables ------------------------------------------------------------ */
#define ATC_RX_RING          2048u      /* power of two                       */
#define ATC_LINE_MAX         512u
#define ATC_CMD_MAX          128u
#define ATC_PREFIX_MAX       24u
#define ATC_MAX_KNOWN        32u
#define ATC_MAX_SUBS         16u
#define ATC_RESYNC_QUIET_MS  200u
#define ATC_RESYNC_MAX_MS    2000u
#define ATC_RESYNC_POLL_MS   50u
#define ATC_CMD_POLL_MS      5u

void (*atc_rx_notify)(void);
volatile uint32_t atc_rx_dropped;
volatile uint32_t atc_line_overflow;
volatile uint32_t atc_resync_count;

static atc_ops_t ops;

/* ---- SPSC byte ring: producer = atc_feed, consumer = atc_process --------- */
static uint8_t rx_ring[ATC_RX_RING];
static volatile uint32_t rx_head, rx_tail;

static void dmb(void)
{
#if defined(__arm__)
    __asm volatile("dmb 0xF" ::: "memory");
#else
    __asm volatile("" ::: "memory");
#endif
}

void atc_feed(const uint8_t *data, uint16_t len)
{
    uint32_t head = rx_head;
    uint32_t free = ATC_RX_RING - (head - rx_tail);
    uint16_t n = len;
    if (n > free) {
        atc_rx_dropped += (uint32_t)(n - free);
        n = (uint16_t)free;
    }
    for (uint16_t i = 0; i < n; i++)
        rx_ring[(head + i) & (ATC_RX_RING - 1u)] = data[i];
    dmb();                               /* data before index publish        */
    rx_head = head + n;
    if (atc_rx_notify) atc_rx_notify();
}

/* ---- line assembler ------------------------------------------------------ */
static struct {
    uint8_t  buf[ATC_LINE_MAX];
    uint16_t len;
    uint8_t  truncating;
} lasm;

/* ---- in-flight command --------------------------------------------------- */
static struct {
    char     cmd[ATC_CMD_MAX];
    char     prefix[ATC_PREFIX_MAX];     /* one auto-derived prefix (or "")  */
    uint8_t  echo_pending;
    uint8_t  done;
    uint32_t started_ms;
    atc_resp_t *resp;
} pending;
static uint8_t pending_active;
static uint8_t draining;
static uint8_t lost_sync;
static uint32_t last_rx_ms;

/* ---- URC tables ---------------------------------------------------------- */
static const char *known[ATC_MAX_KNOWN];
static uint8_t n_known;
static struct { const char *prefix; atc_urc_cb_t cb; void *ctx; }
    subs[ATC_MAX_SUBS];
static uint8_t n_subs;

/* Default known set, verbatim from cellmux urc.py DEFAULT_URC_PREFIXES. */
static const char *const default_known[] = {
    "+CREG:", "+CGREG:", "+CEREG:", "+C5GREG:", "+CGEV:", "+CPIN:",
    "+CFUN:", "+CMTI:", "+CMT:", "+CDSI:", "+CDS:", "+QIND:", "+QIURC:",
    "+QUSIM:", "+QSIMSTAT:", "RDY", "POWERED DOWN", "NORMAL POWER DOWN",
};

static int starts_with(const char *s, const char *p)
{
    while (*p) if (*s++ != *p++) return 0;
    return 1;
}

static int is_known_urc(const char *line)
{
    for (uint8_t i = 0; i < n_known; i++)
        if (starts_with(line, known[i])) return 1;
    return 0;
}

static void dispatch_urc(const char *line, int known_urc)
{
    for (uint8_t i = 0; i < n_subs; i++) {
        if (subs[i].prefix == NULL) {            /* wildcard sees everything */
            subs[i].cb(line, subs[i].ctx);
        } else if (known_urc && starts_with(line, subs[i].prefix)) {
            subs[i].cb(line, subs[i].ctx);
        }
    }
}

void atc_urc_known(const char *prefix)
{
    for (uint8_t i = 0; i < n_known; i++)
        if (known[i] == prefix || 0 == strcmp(known[i], prefix)) return;
    if (n_known < ATC_MAX_KNOWN) known[n_known++] = prefix;
}

void atc_on_urc(const char *prefix, atc_urc_cb_t cb, void *ctx)
{
    if (n_subs < ATC_MAX_SUBS) {
        subs[n_subs].prefix = prefix;
        subs[n_subs].cb = cb;
        subs[n_subs].ctx = ctx;
        n_subs++;
    }
    if (prefix) atc_urc_known(prefix);   /* subscribing also registers known */
}

/* ---- prefix derivation (verbatim port of session.py RESP_PREFIX_RE) ------ */
/* AT[+^]NAME... -> "+NAME:"; basic commands (ATI, ATE0, AT&V) -> none.      */
static void derive_prefix(const char *cmd, char *out)
{
    out[0] = 0;
    if (!((cmd[0] == 'A' || cmd[0] == 'a') && (cmd[1] == 'T' || cmd[1] == 't')))
        return;
    char lead = cmd[2];
    if (lead != '+' && lead != '^') return;
    unsigned o = 0;
    out[o++] = lead;
    for (unsigned i = 3; cmd[i] && o < ATC_PREFIX_MAX - 2u; i++) {
        char c = cmd[i];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '%') {
            out[o++] = c;
        } else if (c >= 'a' && c <= 'z') {
            out[o++] = (char)(c - 'a' + 'A');
        } else {
            break;
        }
    }
    if (o < 2) { out[0] = 0; return; }   /* need at least one body char      */
    out[o++] = ':';
    out[o] = 0;
}

/* ---- final result classification (verbatim port of parse_final) ---------- */
static const char *const final_error_literals[] = {
    "ERROR", "NO CARRIER", "BUSY", "NO ANSWER", "NO DIALTONE", "ABORTED",
    "COMMAND NOT SUPPORT",               /* Quectel-specific spelling        */
};

/* Returns 1 and fills kind/code if `line` is a final result. */
static int parse_final(const char *line, atc_err_kind_t *kind, int32_t *code)
{
    if (0 == strcmp(line, "OK")) { *kind = ATC_ERR_NONE; *code = 0; return 1; }

    const char *detail = NULL;
    if (starts_with(line, "+CME ERROR:")) { *kind = ATC_ERR_CME; detail = line + 11; }
    else if (starts_with(line, "+CMS ERROR:")) { *kind = ATC_ERR_CMS; detail = line + 11; }
    if (detail) {
        while (*detail == ' ') detail++;
        int32_t v = 0; int digits = 0, all = 1;
        for (const char *q = detail; *q; q++) {
            if (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); digits = 1; }
            else all = 0;
        }
        *code = (digits && all) ? v : -1;
        return 1;
    }
    for (unsigned i = 0;
         i < sizeof final_error_literals / sizeof final_error_literals[0]; i++)
        if (0 == strcmp(line, final_error_literals[i])) {
            *kind = ATC_ERR_FINAL; *code = -1; return 1;
        }
    return 0;
}

/* ---- response assembly --------------------------------------------------- */
static void resp_add_line(atc_resp_t *r, const char *line)
{
    if (!r) return;
    size_t n = strlen(line) + 1u;
    uint16_t off = r->n_lines ? (uint16_t)(r->line_off[r->n_lines - 1]
                     + strlen(r->buf + r->line_off[r->n_lines - 1]) + 1u) : 0u;
    if (r->n_lines >= ATC_MAX_LINES || off + n > ATC_RESP_BUF) {
        r->overflow = 1;
        return;
    }
    memcpy(r->buf + off, line, n);
    r->line_off[r->n_lines++] = off;
}

const char *atc_resp_single(const atc_resp_t *r)
{
    return (r && r->n_lines == 1) ? r->buf + r->line_off[0] : NULL;
}

/* ---- per-line dispatch (verbatim order from session._handle_line) -------- */
static void handle_line(const char *line)
{
    /* 1: during a drain, swallow stale traffic but never lose URCs */
    if (draining) {
        if (is_known_urc(line)) dispatch_urc(line, 1);
        return;
    }

    /* 2: lines after the final belong to nobody */
    int have = pending_active && !pending.done;

    /* 3: no command in flight -> URC path */
    if (!have) {
        dispatch_urc(line, is_known_urc(line));
        return;
    }

    /* 4: command echo (ATE1 mode) is swallowed once */
    if (pending.echo_pending && 0 == strcmp(line, pending.cmd)) {
        pending.echo_pending = 0;
        return;
    }

    /* 5: final result closes the command */
    atc_err_kind_t kind; int32_t code;
    if (parse_final(line, &kind, &code)) {
        if (pending.resp) {
            atc_resp_t *r = pending.resp;
            strncpy(r->final, line, ATC_FINAL_MAX - 1u);
            r->final[ATC_FINAL_MAX - 1u] = 0;
            r->err_kind = kind;
            r->err_code = code;
            r->elapsed_ms = ops.now_ms() - pending.started_ms;
        }
        pending.done = 1;
        return;
    }

    /* 6: solicited info response wins over URC (the +CEREG ambiguity) */
    if (pending.prefix[0] && starts_with(line, pending.prefix)) {
        resp_add_line(pending.resp, line);
        return;
    }

    /* 7: known URC interleaved with a command */
    if (is_known_urc(line)) {
        dispatch_urc(line, 1);
        return;
    }

    /* 8: unprefixed body (ATI, AT+GSN answer lines, ...) */
    resp_add_line(pending.resp, line);
}

/* ---- ring -> lines ------------------------------------------------------- */
static void emit_line(void)
{
    /* strip CR + whitespace both ends; drop empties (parser.py behavior) */
    uint16_t a = 0, b = lasm.len;
    while (a < b && (lasm.buf[a] == '\r' || lasm.buf[a] == ' '
                     || lasm.buf[a] == '\t')) a++;
    while (b > a && (lasm.buf[b - 1] == '\r' || lasm.buf[b - 1] == ' '
                     || lasm.buf[b - 1] == '\t')) b--;
    lasm.buf[b] = 0;
    if (b > a) handle_line((const char *)lasm.buf + a);
    lasm.len = 0;
}

int atc_process(void)
{
    int lines = 0;
    while (rx_tail != rx_head) {
        uint8_t c = rx_ring[rx_tail & (ATC_RX_RING - 1u)];
        dmb();
        rx_tail++;
        last_rx_ms = ops.now_ms();
        if (c == '\n') {
            if (lasm.truncating) { lasm.truncating = 0; lasm.len = 0; }
            else { emit_line(); lines++; }
            continue;
        }
        if (lasm.truncating) continue;
        if (lasm.len >= ATC_LINE_MAX - 1u) {
            atc_line_overflow++;
            lasm.truncating = 1;         /* keep nothing; wait for '\n'      */
            emit_line();                 /* classify the truncated head      */
            lines++;
            lasm.truncating = 1;
            continue;
        }
        lasm.buf[lasm.len++] = c;
    }
    return lines;
}

/* ---- resync (verbatim numbers from session._resync_locked) --------------- */
static void resync(void)
{
    draining = 1;
    uint32_t t0 = ops.now_ms();
    last_rx_ms = t0;
    uint8_t ping[3] = { 'A', 'T', '\r' };
    (void)ops.write(ping, 3);
    for (;;) {
        atc_process();
        uint32_t now = ops.now_ms();
        if (now - last_rx_ms >= ATC_RESYNC_QUIET_MS) break;
        if (now - t0 >= ATC_RESYNC_MAX_MS) break;
        ops.sleep_ms(ATC_RESYNC_POLL_MS);
    }
    draining = 0;
    lost_sync = 0;
    atc_resync_count++;
}

/* ---- command execution ---------------------------------------------------- */
int atc_cmd(const char *cmd, atc_resp_t *resp, uint32_t timeout_ms)
{
    if (pending_active) return -ATC_EBUSY;      /* single-writer violation   */
    if (lost_sync) resync();

    /* arm */
    memset(&pending, 0, sizeof pending);
    unsigned n = 0;
    while (cmd[n] && n < ATC_CMD_MAX - 1u) { pending.cmd[n] = cmd[n]; n++; }
    pending.cmd[n] = 0;
    derive_prefix(pending.cmd, pending.prefix);
    pending.echo_pending = 1;
    pending.started_ms = ops.now_ms();
    if (resp) memset(resp, 0, sizeof *resp);
    pending.resp = resp;
    pending_active = 1;

    uint8_t out[ATC_CMD_MAX + 1];
    memcpy(out, pending.cmd, n);
    out[n] = '\r';
    if (ops.write(out, (uint16_t)(n + 1u)) != 0) {
        pending_active = 0;
        lost_sync = 1;                   /* mid-command write failure        */
        return -ATC_ECLOSED;
    }

    for (;;) {
        atc_process();
        if (pending.done) {
            pending_active = 0;
            return 0;
        }
        if (ops.now_ms() - pending.started_ms >= timeout_ms) {
            pending_active = 0;
            lost_sync = 1;               /* a stale OK must never complete
                                          * the NEXT command                 */
            return -ATC_ETIMEDOUT;
        }
        ops.sleep_ms(ATC_CMD_POLL_MS);
    }
}

int atc_run(const char *cmd, atc_resp_t *resp, uint32_t timeout_ms)
{
    atc_resp_t local;
    atc_resp_t *r = resp ? resp : &local;
    int rc = atc_cmd(cmd, r, timeout_ms);
    if (rc != 0) return rc;
    return (r->err_kind == ATC_ERR_NONE) ? 0 : -ATC_EAT;
}

int atc_init(const atc_ops_t *o)
{
    ops = *o;
    rx_head = rx_tail = 0;
    memset(&lasm, 0, sizeof lasm);
    memset(&pending, 0, sizeof pending);
    pending_active = 0; draining = 0; lost_sync = 0;
    n_known = 0; n_subs = 0;
    atc_rx_dropped = atc_line_overflow = atc_resync_count = 0;
    for (unsigned i = 0;
         i < sizeof default_known / sizeof default_known[0]; i++)
        known[n_known++] = default_known[i];
    return 0;
}
