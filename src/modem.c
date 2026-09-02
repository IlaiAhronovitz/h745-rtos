/* modem.c — EC25 hardening policy. See modem.h and EC25-ISSUES.md.
 *
 * Every branch here traces to a catalogued field failure:
 *   classify()      U6  — a bricked modem enumerates as 05C6:9008, no AT ever
 *   wait_at_ready() U2/A1 — enumeration != AT readiness; retry bare AT
 *   normalize()     A3/A16/P6/U10/U11/F1 — NV state persists maliciously
 *   reset watcher   A5  — a boot-URC burst mid-session means it crash-rebooted
 *   CFUN:5 watcher  R4  — FTM/corrupt-QCN state is terminal
 */
#include "modem.h"
#include "libmin.h"
#include "kprintf.h"

/* Injected time: the AT engine already owns now/sleep via atc_ops, but the
 * boot-window poll runs BEFORE a command is in flight, so modem.c takes the
 * same two hooks directly. Set by modem_set_clock() from the app. */
static uint32_t (*now_ms)(void);
static void     (*sleep_ms)(uint32_t);

void modem_set_clock(uint32_t (*now)(void), void (*slp)(uint32_t))
{
    now_ms = now;
    sleep_ms = slp;
}

modem_state_t modem_classify(const usbh_device_t *dev)
{
    if (dev->vid == 0x05C6u && dev->pid == 0x9008u) return MODEM_EDL;
    if (dev->vid == 0x2C7Cu)                        return MODEM_BOOTING;
    return MODEM_UNKNOWN_DEV;
}

int modem_wait_at_ready(uint32_t timeout_ms)
{
    uint32_t t0 = now_ms();
    for (;;) {
        atc_resp_t r;
        /* A1: bare "AT", CR-only (atc terminates with a single CR), short
         * timeout — the boot-state modem simply won't answer yet. */
        if (atc_cmd("AT", &r, 300) == 0 && r.err_kind == ATC_ERR_NONE)
            return 0;
        if (now_ms() - t0 >= timeout_ms) return -1;
        sleep_ms(500);                   /* the QNavigator cadence */
    }
}

/* ---- reset / terminal URC watcher (A5, R4) ------------------------------- */
static modem_t *watched;

static int has(const char *line, const char *needle)
{
    for (const char *s = line; *s; s++) {
        const char *a = s, *b = needle;
        while (*a && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

static void on_urc(const char *line, void *ctx)
{
    (void)ctx;
    if (!watched) return;
    /* A5: the boot burst — RDY, or +CPIN: READY, or +QUSIM: appearing at any
     * time — is how a silent crash-reboot shows in-band. */
    if (0 == strcmp(line, "RDY")
        || has(line, "+CPIN: READY")
        || has(line, "+QUSIM:")) {
        watched->rebooted = 1;
    }
    /* R4: FTM / corrupted QCN. RF is dead and CFUN=1 is refused — terminal. */
    if (has(line, "+CFUN: 5")) {
        watched->state = MODEM_TERMINAL;
    }
}

void modem_watch(modem_t *m)
{
    watched = m;
    atc_on_urc(0, on_urc, 0);            /* wildcard: sees every URC line */
}

int  modem_rebooted(modem_t *m)     { return m->rebooted; }
void modem_clear_reboot(modem_t *m) { m->rebooted = 0; }

/* ---- init normalization -------------------------------------------------- */
/* Read a "+QCFG: "name",<val>" style response's first integer after the name,
 * or -1 if the line is missing/odd. */
static int qcfg_int(atc_resp_t *r)
{
    const char *line = atc_resp_single(r);
    if (!line) { if (r->n_lines) line = r->buf + r->line_off[0]; else return -1; }
    const char *c = line;
    while (*c && *c != ',') c++;          /* skip to first comma */
    if (!*c) return -1;
    c++;
    while (*c == ' ' || *c == '"') c++;
    int v = 0, any = 0;
    while (*c >= '0' && *c <= '9') { v = v * 10 + (*c - '0'); c++; any = 1; }
    return any ? v : -1;
}

int modem_normalize(modem_t *m)
{
    atc_resp_t r;

    /* A16: force a known terminal profile regardless of a stored AT&W. */
    atc_run("ATE0",      0, 1000);       /* echo off  */
    atc_run("ATV1",      0, 1000);       /* verbose result codes (OK/ERROR) */
    atc_run("ATQ0",      0, 1000);       /* result codes NOT suppressed */
    atc_run("AT+CMEE=2", 0, 1000);       /* verbose +CME ERROR text */

    /* P6: QSCLK=0 — never let the module sleep under a host that never
     * suspends the bus (and closes the corpus's persistence conflict). */
    atc_run("AT+QSCLK=0", 0, 1000);

    /* A3: URCs go to exactly ONE port; a prior owner may have moved them off
     * the AT port. Force usbat. NV-saved, so this heals a stuck unit. */
    atc_run("AT+QURCCFG=\"urcport\",\"usbat\"", 0, 1000);
    if (atc_run("AT+QURCCFG=\"urcport\"", &r, 1000) == 0) {
        if (!(atc_resp_single(&r) && has(atc_resp_single(&r), "usbat")))
            m->warns |= MODEM_WARN_URCPORT_MOVED;
    }

    /* A3: apready URC caching drops URCs when AP_READY is unwired. Off. */
    atc_run("AT+QCFG=\"apready\",0", 0, 1000);

    /* U10: usbnet != 0 means a data composition; 3/4 is an AT-port brick.
     * READ ONLY — never write it here, and never write 3/4 anywhere. */
    m->usbnet = -1;
    if (atc_run("AT+QCFG=\"usbnet\"", &r, 1000) == 0) {
        m->usbnet = (int8_t)qcfg_int(&r);
        if (m->usbnet > 0) m->warns |= MODEM_WARN_USBNET_NONZERO;
    }

    /* U11: usbcfg can delete the AT interface. The at_port flag is field 5
     * (vid,pid,diag,nmea,at_port,...). If it ever reads 0 the AT port is
     * gone — surface it. */
    if (atc_run("AT+QCFG=\"usbcfg\"", &r, 1000) == 0) {
        const char *line = atc_resp_single(&r);
        if (line) {
            int commas = 0;
            const char *c = line;
            for (; *c; c++) {
                if (*c == ',') {
                    if (++commas == 5) {
                        while (c[1] == ' ') c++;
                        if (c[1] == '0') m->warns |= MODEM_WARN_AT_PORT_FLAG;
                        break;
                    }
                }
            }
        }
    }

    /* F1: capture the identity — every quirk record keys to the firmware. */
    if (atc_run("ATI", &r, 1000) == 0) {
        m->ati[0] = 0;
        for (uint8_t i = 0; i < r.n_lines && strlen(m->ati) < sizeof m->ati - 40u; i++) {
            const char *ln = r.buf + r.line_off[i];
            size_t o = strlen(m->ati);
            if (o) m->ati[o++] = ' ';
            strncpy(m->ati + o, ln, sizeof m->ati - o - 1u);
            m->ati[sizeof m->ati - 1u] = 0;
        }
    }
    if (atc_run("AT+QGMR", &r, 1000) == 0) {
        const char *ln = atc_resp_single(&r);
        if (ln) { strncpy(m->qgmr, ln, sizeof m->qgmr - 1u);
                  m->qgmr[sizeof m->qgmr - 1u] = 0; }
    }

    if (m->state != MODEM_TERMINAL) m->state = MODEM_READY;
    return (m->state == MODEM_READY) ? 0 : -1;
}

/* ---- data call: AT-driven RmNet (see modem.h) ---------------------------- */
int modem_data_call_up(void)
{
    atc_resp_t r;

    /* 1. start the in-module QMI client. It answers OK immediately, then
     *    reports four init stages by URC: 1=service, 2=parameters,
     *    3=port binding, 4=normal operation; <code> 0 means that stage is
     *    healthy, negative means it failed and the service stopped. */
    /* 0. Stage 2 ("parameter setting") fails with code -1 unless the
     *    aggregation parameters are ones this host can honour. On Linux the
     *    qmi_wwan_q driver supplies them; we have no such driver, so set
     *    them explicitly and small enough for our 1600 B pipe buffers.
     *    These are not saved, so they must be re-applied every boot. */
    static const char *const setup[] = {
        "AT+QCRMTOOL=\"CTL\",0",           /* clean slate                  */
        "AT+QCRMTOOL=\"IP_TYPE\",4",       /* IPv4 only                    */
        "AT+QCRMTOOL=\"DLAGG_SIZE\",4096",
        "AT+QCRMTOOL=\"ULAGG_GRAMS\",8",
        "AT+QCRMTOOL=\"ULAGG_SIZE\",1024",
        "AT+QCRMTOOL=\"MUX_ID\",129",
    };
    for (unsigned i = 0; i < sizeof setup / sizeof setup[0]; i++) {
        int s = atc_run(setup[i], &r, 3000);
        kprintf("modem: %s rc=%d %s\n", setup[i], s, r.final);
        sleep_ms(200);                     /* let the module settle        */
    }

    int rc = atc_run("AT+QCRMTOOL=\"CTL\",1", &r, 5000);
    kprintf("modem: QCRMTOOL start rc=%d final=\"%s\" lines=%u\n",
            rc, r.final, r.n_lines);

    /* 2. let the URCs land. The engine dispatches them; we just wait. */
    for (int i = 0; i < 60; i++) sleep_ms(100);

    /* Search ALL payload lines, not just a lone one: the reply can arrive
     * with URCs interleaved, and atc_resp_single() only answers when the
     * response holds exactly one line (that cost a bench run). */
    int running = 0;
    if (atc_run("AT+QCRMTOOL=\"CTL\"", &r, 3000) == 0) {
        for (uint8_t i = 0; i < r.n_lines; i++) {
            const char *ln = r.buf + r.line_off[i];
            kprintf("modem:   %s\n", ln);
            if (has(ln, "\"CTL\",1")) running = 1;
        }
    }
    if (!running) {
        kprintf("modem: QCRMTOOL is not running (%u reply lines)\n",
                r.n_lines);
        return -1;
    }

    /* 3. enable the readiness URC, then start the call. NOTE: $QCRMCALL
     *    answers "NO CARRIER" on the AT port -- that is the RmNet call
     *    moving off the command channel, NOT a failure. The truth is in
     *    AT+QNETDEVSTATUS?. */
    atc_run("AT+QNETDEVSTATUS=1", &r, 3000);
    atc_run("AT$QCRMCALL=1,1", &r, 20000);

    /* 4. poll for state 1 = "call ready, MCU can get IP by DHCP". */
    for (int i = 0; i < 30; i++) {
        if (atc_run("AT+QNETDEVSTATUS?", &r, 3000) == 0) {
            const char *ln = atc_resp_single(&r);
            /* +QNETDEVSTATUS: <on_off>,<state>,<IP_type>,<instance> */
            if (ln && (has(ln, ",1,4,") || has(ln, ",2,4,"))) {
                kprintf("modem: RmNet call UP -- %s\n", ln);
                return 0;
            }
        }
        sleep_ms(500);
    }
    kprintf("modem: RmNet call never reached ready state\n");
    return -1;
}

void modem_data_call_down(void)
{
    atc_resp_t r;
    atc_run("AT$QCRMCALL=0,1", &r, 10000);
    atc_run("AT+QCRMTOOL=\"CTL\",0", &r, 5000);
}
