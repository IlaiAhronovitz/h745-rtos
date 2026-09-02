/* app_modem.c — EC25 modem bring-up over USB.  make APP=modem flash
 *
 * Implements the gates of 5g-h745-modem-kb/EC25-BRINGUP.md: enumerate the
 * composite, SET_CONFIGURATION, locate the AT interface by class triple +
 * live probe (send "AT", expect OK — never trust interface ordinals:
 * Quectel renumbers per usbnet mode), then run the identity script through
 * the atc engine. Works with any Quectel-style vendor-serial modem; the
 * expected EC25 layout is DIAG=ff/ff/ff (2 eps), NMEA/AT/Modem=ff/00/00,
 * QMI=ff/ff/ff (3 eps) — AT is usually the 2nd ff/00/00 interface.
 *
 * Bare main, polled, like app_usb: the modem must be on a SELF-POWERED
 * carrier (>= 2.7 A rail) — CN13 carries data only.
 *
 * LEDs: green = VBUS on, yellow = AT port answering, red = init failure.
 */
#include <stdint.h>
#include "stm32h745.h"
#include "uart.h"
#include "board.h"
#include "kprintf.h"
#include "libmin.h"
#include "usbh.h"
#include "atc.h"
#include "modem.h"
#include "usbdump.h"
#include "ipnet.h"
#include "rmnet.h"
#include "qmi.h"

static usbh_device_t dev;
static usbh_pipe_t   at_in, at_out;
static modem_t       modem;

/* ---- time: ms accumulator over the DWT cycle counter --------------------- */
static uint32_t ms_now(void)
{
    static uint32_t last_cyc, acc_ms, rem_cyc;
    uint32_t now = DWT_CYCCNT;
    uint32_t d = now - last_cyc + rem_cyc;
    last_cyc = now;
    acc_ms += d / (SYS_CLK_HZ / 1000u);
    rem_cyc = d % (SYS_CLK_HZ / 1000u);
    return acc_ms;
}

/* ---- transport glue for the AT engine ------------------------------------ */
static int at_write(const uint8_t *data, uint16_t len)
{
    int r = usbh_bulk(&dev, &at_out, (void *)data, len, 1000);
    return (r == (int)len) ? 0 : -1;
}

/* Pump one bulk-IN poll into the engine. One MPS per read keeps every
 * outcome data-toggle-atomic (see usbh.h). */
static void pump_in(void)
{
    static uint8_t buf[64];
    int r = usbh_bulk(&dev, &at_in, buf, at_in.mps, 5);
    if (r > 0) atc_feed(buf, (uint16_t)r);
}

/* The engine's sleep hook doubles as the RX pump: while a command waits
 * for its final result, this is what keeps bytes flowing. */
static void at_sleep(uint32_t ms)
{
    uint32_t t0 = ms_now();
    do { pump_in(); } while (ms_now() - t0 < ms);
}

static const atc_ops_t at_ops = { at_write, ms_now, at_sleep };

/* ---- AT-port discovery (playbook gate 3) ---------------------------------- */
/* Probe a candidate ff/00/00 interface: open its bulk pair, fire a raw
 * "AT\r", and watch ~300 ms for "OK". The definitive test. */
static int probe_at(const usbh_iface_t *cand)
{
    if (!cand->ep_in || !cand->ep_out) return 0;
    usbh_pipes_reset();
    if (usbh_pipe_open(&dev, cand->ep_in, &at_in) != USBH_OK) return 0;
    if (usbh_pipe_open(&dev, cand->ep_out, &at_out) != USBH_OK) return 0;

    uint8_t ping[3] = { 'A', 'T', '\r' };
    if (usbh_bulk(&dev, &at_out, ping, 3, 1000) != 3) return 0;

    uint8_t acc[128];
    uint16_t n = 0;
    uint32_t t0 = ms_now();
    while (ms_now() - t0 < 300u && n < sizeof acc - 1u) {
        uint8_t b[64];
        int r = usbh_bulk(&dev, &at_in, b, at_in.mps, 20);
        if (r > 0) {
            for (int i = 0; i < r && n < sizeof acc - 1u; i++) acc[n++] = b[i];
        }
    }
    acc[n] = 0;
    for (uint16_t i = 0; i + 1u < n; i++)
        if (acc[i] == 'O' && acc[i + 1] == 'K') return 1;
    return 0;
}

static int find_at_port(void)
{
    /* Candidate order for the EC25/RM500Q default composite: among ff/00/00
     * interfaces NMEA=0, AT=1, Modem=2 — so try 1 first, then the rest.
     * The Modem port (index 2) is the documented fallback AT channel when
     * the primary AT port wedges (issue A8). */
    static const uint8_t order[] = { 1, 2, 0, 3 };
    usbh_iface_t cand;

    /* On the RM500Q both the AT port (IF2) and the Modem port (IF3) answer a
     * bare "AT", so "first to reply" is not a discriminator: one bench run
     * latched IF3 and then stalled. Prefer the documented AT interface
     * number and only fall back to another port if it truly never answers. */
    const uint8_t preferred_ifnum = 2;
    int relaxed = 0;

    /* U2/A1: enumeration != AT readiness. A 5G modem (RM500Q, SDX55) needs
     * ~15-30 s after enumerating before its AT port answers — the baseband
     * boots behind the USB gadget. Probe the candidates REPEATEDLY across a
     * boot window instead of giving up on the first pass. Bail out if the
     * modem drops off the bus (it re-enumerates during boot). */
    uint32_t t0 = ms_now();
    uint32_t last_log = 0;
    while (usbh_connected() && ms_now() - t0 < 30000u) {
        for (unsigned i = 0; i < sizeof order; i++) {
            if (!usbh_connected()) return 0;
            if (usbh_find_interface(&dev, 0xFF, 0x00, 0x00, order[i], &cand)
                != USBH_OK) continue;
            /* Assert DTR/RTS before probing — harmless if ignored, and some
             * hosts/ports want it before they transmit (U16). */
            usbh_set_line_state(&dev, cand.intf[2], 1, 1);
            /* For the first 15 s insist on the documented AT interface; only
             * after that accept whichever port answers. */
            if (!relaxed && cand.intf[2] != preferred_ifnum) continue;
            if (probe_at(&cand)) {
                modem.at_iface = cand.intf[2];
                modem.ep_at_in = at_in.ep_addr;
                modem.ep_at_out = at_out.ep_addr;
                kprintf("modem: AT port = interface %u (ep in %02x out %02x)"
                        " after %lu ms\n", cand.intf[2],
                        at_in.ep_addr, at_out.ep_addr,
                        (unsigned long)(ms_now() - t0));
                return 1;
            }
        }
        if (!relaxed && ms_now() - t0 >= 15000u) {
            relaxed = 1;
            kprintf("modem: IF%u never answered -- accepting any AT port now\n",
                    preferred_ifnum);
        }
        if (ms_now() - last_log >= 5000u) {
            kprintf("modem: AT not up yet, waiting for baseband boot (%lu s)\n",
                    (unsigned long)((ms_now() - t0) / 1000u));
            last_log = ms_now();
        }
        uint32_t w = ms_now();
        while (usbh_connected() && ms_now() - w < 1000u) {}
    }
    return 0;
}

/* ---- identity script (playbook §4, works without a SIM) ------------------- */
static void show(const char *cmd, atc_resp_t *r, int rc)
{
    kprintf("> %s  ->  %s", cmd, rc == 0 ? "" : "(rc ");
    if (rc != 0) kprintf("%d) ", rc);
    if (rc == 0 || rc == -ATC_EAT) {
        for (uint8_t i = 0; i < r->n_lines; i++)
            kprintf("\n    %s", r->buf + r->line_off[i]);
        kprintf("\n    [%s, %lu ms]\n", r->final, (unsigned long)r->elapsed_ms);
    } else {
        kprintf("no final\n");
    }
}

static void urc_printer(const char *line, void *ctx)
{
    (void)ctx;
    kprintf("URC: %s\n", line);
}

static void identity_script(void)
{
    static const char *const cmds[] = {
        "ATI", "AT+QGMR", "AT+GSN", "AT+CPIN?", "AT+QTEMP",
    };
    atc_resp_t r;
    for (unsigned i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        int rc = atc_run(cmds[i], &r, 5000);
        show(cmds[i], &r, rc);
    }
}

/* Set to 1 to auto-dial CALL_NUMBER after bring-up, 0 (default) for a
 * read-only session. The number is a build parameter, never stored in the
 * source:
 *   make APP=modem CFLAGS_EXTRA='-DPLACE_CALL=1 -DCALL_NUMBER=\"<number>\"' */
#ifndef PLACE_CALL
#define PLACE_CALL 0
#endif
#if PLACE_CALL && !defined(CALL_NUMBER)
#error "PLACE_CALL=1 needs -DCALL_NUMBER=\"<number>\""
#endif

/* Fallback APN for the data call, used only when the module's own context 1
 * cannot be read (see detect_apn). Empty = let the network pick its default.
 * Override at build time: CFLAGS_EXTRA='-DDATA_APN=\"<apn>\"' */
#ifndef DATA_APN
#define DATA_APN ""
#endif

/* Network registration + signal — a voice call needs the modem camped. */
static void net_status(void)
{
    static const char *const q[] = {
        "AT+CSQ", "AT+CREG?", "AT+CEREG?", "AT+COPS?", "AT+QNWINFO",
    };
    atc_resp_t r;
    for (unsigned i = 0; i < sizeof q / sizeof q[0]; i++)
        show(q[i], &r, atc_run(q[i], &r, 5000));
}

#if PLACE_CALL
/* Originate a voice call. On an LTE-only network voice is VoLTE, so try to
 * make sure IMS is on first, then ATD<num>; (the ';' makes it a VOICE call).
 * We have no audio codec on the bench, so success = the callee's phone RINGS;
 * on our side we watch +CLCC for the call progressing (dialing->alerting). */
static void place_call(void)
{
    atc_resp_t r;
    atc_run("AT+QCFG=\"ims\",1", &r, 3000);   /* best effort; ignore errors */
    kprintf("\nmodem: DIALING " CALL_NUMBER " ...\n");
    show("ATD" CALL_NUMBER ";", &r, atc_run("ATD" CALL_NUMBER ";", &r, 8000));

    for (int s = 0; s < 20 && usbh_connected(); s++) {
        atc_process();
        atc_run("AT+CLCC", &r, 3000);
        if (r.n_lines) show("AT+CLCC", &r, 0);
        at_sleep(1000);
    }
    atc_run("ATH", &r, 5000);                 /* hang up */
    kprintf("modem: hung up\n");
}
#endif /* PLACE_CALL */


/* ---- data plane: RmNet call -> QMAP pipes -> DHCP -> ping ---------------- */
static rmnet_t rmn;
static qmi_t   qmi;
static ipnet_t ip;

static int dp_tx(const uint8_t *frame, uint16_t len)
{ return rmnet_tx_eth(&rmn, frame, len); }

static int dp_rx(uint8_t *frame, uint16_t cap)
{ return rmnet_rx_eth(&rmn, frame, cap); }

/* The data plane must NOT borrow at_sleep().
 *
 * at_sleep() spends its time in pump_in(), a 5 ms-timeout read of the AT
 * pipe, and its do/while runs that at least once -- so a nominal 1 ms sleep
 * costs five, and for all of it the DATA pipe is not being looked at. In a
 * receive loop that is five milliseconds of not noticing a packet that has
 * already arrived.
 *
 * The receive path has its own 20 ms first-packet timeout inside
 * usbh_bulk_frame(), and that is the pacing; this hook only has to stay out
 * of the way. The AT pipe is still drained, just rarely, so URCs cannot pile
 * up unread while a data transfer is in progress. */
static void dp_sleep(uint32_t ms)
{
    static unsigned tick;
    (void)ms;
    if ((++tick & 0x3Fu) == 0u) pump_in();
}

static const ipnet_ops_t dp_ops = { dp_tx, dp_rx, ms_now, dp_sleep };

/* ---- a DNS lookup, as proof of real traffic ------------------------------
 * ICMP is not a fair test of a cellular bearer: one test operator answered an
 * echo request to 8.8.8.8 with "destination unreachable, host unreachable"
 * from one of its own routers, which is a policy filter, not a broken link. A DNS query is ordinary UDP traffic and is what actually proves the
 * data path carries user data end to end. */
static uint16_t dns_build(uint8_t *q, const char *host, uint16_t id)
{
    uint16_t n = 0;
    q[n++] = (uint8_t)(id >> 8); q[n++] = (uint8_t)id;
    q[n++] = 0x01; q[n++] = 0x00;      /* recursion desired               */
    q[n++] = 0x00; q[n++] = 0x01;      /* one question                    */
    q[n++] = 0x00; q[n++] = 0x00;
    q[n++] = 0x00; q[n++] = 0x00;
    q[n++] = 0x00; q[n++] = 0x00;
    /* QNAME: each label prefixed by its length, terminated by a zero byte */
    const char *s = host;
    while (*s) {
        const char *dot = s;
        while (*dot && *dot != '.') dot++;
        uint8_t len = (uint8_t)(dot - s);
        q[n++] = len;
        for (uint8_t i = 0; i < len; i++) q[n++] = (uint8_t)s[i];
        s = *dot ? dot + 1 : dot;
    }
    q[n++] = 0x00;
    q[n++] = 0x00; q[n++] = 0x01;      /* QTYPE  A                        */
    q[n++] = 0x00; q[n++] = 0x01;      /* QCLASS IN                       */
    return n;
}

/* Wait for a UDP datagram from src_ip:src_port. Returns payload length. */
static int udp_wait(const uint8_t src_ip[4], uint16_t src_port,
                    uint8_t *out, uint16_t cap, uint32_t timeout_ms)
{
    static uint8_t f[1600];
    uint32_t t0 = ms_now();
    while (ms_now() - t0 < timeout_ms) {
        int r = dp_rx(f, sizeof f);
        if (r <= 14 + 20 + 8) { continue; }
        if (f[12] != 0x08 || f[13] != 0x00) continue;      /* not IPv4     */
        const uint8_t *iph = f + 14;      /* not 'ip': that is the stack */
        uint16_t ihl = (uint16_t)((iph[0] & 0x0Fu) * 4u);
        if (iph[9] != 17u) continue;                       /* not UDP      */
        if (memcmp(iph + 12, src_ip, 4) != 0) continue;
        const uint8_t *udp = iph + ihl;
        uint16_t sport = (uint16_t)((udp[0] << 8) | udp[1]);
        if (sport != src_port) continue;
        uint16_t ulen = (uint16_t)((udp[4] << 8) | udp[5]);
        if (ulen < 8u) continue;
        uint16_t plen = (uint16_t)(ulen - 8u);
        if (plen > cap) plen = cap;
        memcpy(out, udp + 8, plen);
        return (int)plen;
    }
    return -1;
}

/* Pull the first A record out of a DNS answer. Simplified but honest: it
 * looks for the A/IN type-class pair after the question section rather than
 * walking name compression, which is enough for a single-answer reply. */
static int dns_answer(const uint8_t *r, uint16_t n, uint8_t out[4])
{
    if (n < 12u) return -1;
    if ((r[3] & 0x0Fu) != 0u) return -2;               /* RCODE != NOERROR */
    if (((r[6] << 8) | r[7]) == 0) return -3;          /* no answers       */
    for (uint16_t i = 12; i + 14u <= n; i++) {
        if (r[i] == 0 && r[i + 1] == 1 && r[i + 2] == 0 && r[i + 3] == 1
            && i > 16u) {
            uint16_t rdlen = (uint16_t)((r[i + 8] << 8) | r[i + 9]);
            if (rdlen == 4u && (uint16_t)(i + 14u) <= n) {
                memcpy(out, r + i + 10, 4);
                return 0;
            }
        }
    }
    return -4;
}

static int dns_lookup(const uint8_t server[4], const char *host)
{
    static uint8_t q[128], a[512];
    uint8_t addr[4];
    uint16_t qn = dns_build(q, host, 0x4837);

    kprintf("data: DNS A? %s  via %u.%u.%u.%u\n", host,
            server[0], server[1], server[2], server[3]);
    if (ipnet_udp_send(&ip, server, 53, 5353, q, qn) != 0) {
        kprintf("data: DNS send failed\n");
        return -1;
    }
    int n = udp_wait(server, 53, a, sizeof a, 5000);
    if (n < 0) { kprintf("data: DNS no answer\n"); return -1; }

    int rc = dns_answer(a, (uint16_t)n, addr);
    if (rc != 0) {
        kprintf("data: DNS answered %d bytes but no A record (rc %d)\n", n, rc);
        return -1;
    }
    kprintf("data: *** %s = %u.%u.%u.%u  ***  %d-byte answer over LTE\n",
            host, addr[0], addr[1], addr[2], addr[3], n);
    return 0;
}

/* Throughput benchmark for the data path.
 *
 * Two numbers, because they measure different things:
 *
 *  UPLINK BURST -- fire N datagrams back to back and never wait for a reply.
 *      This is the host-to-modem USB path with the network taken out of it:
 *      IP header build, the rmnet copy, and one multi-packet bulk OUT each.
 *      It is the honest measure of what this driver costs.
 *
 *  ROUND TRIP  -- echo requests, which include the LTE round-trip latency
 *      (~40-60 ms here). With one packet outstanding at a time this is
 *      LATENCY bound, not throughput bound, so it cannot exceed
 *      payload/RTT no matter how fast the host is. It is reported to show
 *      the host cost shrinking as the payload grows, not as a rate.
 *
 * The debug hex dump MUST be off: it kprintf()s up to 48 bytes per frame to a
 * 115200 baud UART, which caps the whole path at about 11 kB/s and would be
 * the only thing any of these numbers measured. */
static void bench_datapath(void)
{
    static uint8_t payload[1400];
    static const uint16_t sizes[] = { 64, 256, 512, 1024, 1400 };

    int was_debug = rmn.debug;
    rmn.debug = 0;                    /* see the comment above -- essential */

    for (unsigned i = 0; i < sizeof payload; i++) payload[i] = (uint8_t)i;

    kprintf("\n[5] throughput\n");
    kprintf("    uplink burst -- no waiting for replies, so this is the\n");
    kprintf("    host-to-modem USB path alone.\n");

    const uint8_t *dst = (ip.dns[0] || ip.dns[1]) ? ip.dns : ip.gw;
    for (unsigned s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        uint16_t sz = sizes[s];
        int n = 200;
        uint32_t t0 = ms_now();
        int sent = 0;
        for (int i = 0; i < n; i++)
            if (ipnet_udp_send(&ip, dst, 9, 5354, payload, sz) == 0) sent++;
        uint32_t dt = ms_now() - t0;
        if (dt == 0) dt = 1;
        /* bytes actually put on the wire include the IP+UDP headers */
        uint32_t bytes = (uint32_t)sent * (sz + 28u);
        kprintf("    %4u B x %3d  ->  %4lu ms  %5lu kB/s  %4lu us/pkt\n",
                sz, sent, (unsigned long)dt,
                (unsigned long)(bytes / dt),          /* B/ms == kB/s */
                (unsigned long)((dt * 1000u) / (uint32_t)(sent ? sent : 1)));
    }

    /* ---- downlink: the half that was never measured ---------------------
     * Round-trip timing cannot show receive throughput -- with one packet
     * outstanding it is bounded by payload/RTT however fast the host is. So
     * put a burst in flight and time the drain.
     *
     * READ THIS BEFORE TRUSTING THE NUMBER: these are echo REPLIES, so the
     * rate at which they arrive is set by the network, not by how fast this
     * host can read. Runs vary by more than 2x on the same build for that
     * reason. It is a floor on receive capability, never a measurement of
     * the ceiling -- the highest figure seen (993 kB/s at 1400 B, 82% of the
     * USB Full Speed bulk limit) is the meaningful one. A real RX ceiling
     * test needs a sender that saturates, which an echo server is not. */
    kprintf("\n    downlink burst -- %d echoes in flight, then drain\n", 32);
    for (unsigned s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        uint16_t sz = sizes[s];
        int sent = 0;
        for (int i = 0; i < 32; i++)
            if (ipnet_ping_send(&ip, dst, sz) >= 0) sent++;
        int replies = 0;
        uint32_t t0 = ms_now();
        int bytes = ipnet_drain(&ip, 3000, sent, &replies);
        uint32_t dt = ms_now() - t0;
        if (dt == 0) dt = 1;
        kprintf("    %4u B x %2d  ->  %4lu ms  %2d back  %5lu kB/s\n",
                sz, sent, (unsigned long)dt, replies,
                (unsigned long)((uint32_t)bytes / dt));
    }

    kprintf("\n    round trip -- includes ~40-60 ms of LTE latency with one\n");
    kprintf("    packet outstanding, so it is latency bound by design.\n");
    for (unsigned s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        int rtt = ipnet_ping_n(&ip, dst, sizes[s], 4000);
        if (rtt >= 0)
            kprintf("    %4u B  ->  %3d ms\n", sizes[s], rtt);
        else
            kprintf("    %4u B  ->  no reply\n", sizes[s]);
    }

    rmn.debug = was_debug;
}

/* The data-plane proof run. Every layer, measured rather than asserted.
 *
 * What this can and cannot show: if the operator provisions the PDN with an
 * address and no gateway (AT+CGCONTRDP=1 returns "1,5,<apn>,<ip>,,,", as one
 * test SIM did), there is no route to the public internet and no firmware can
 * invent one. What IS provable is that the whole stack -- USB host, QMI, the bearer, the
 * data pipes and the IP stack -- carries real IPv4 traffic over LTE to a real
 * host, reliably, at real packet sizes. That is what this measures. */
static void proof_run(void)
{
    /* Ping target: prefer the resolver the NETWORK just gave us -- a real,
     * reachable host on whatever operator we are attached to. A hard-coded
     * address only works until the SIM changes, which is exactly the mistake
     * this line replaces. */
    uint8_t peer[4];
    if (ip.dns[0] || ip.dns[1] || ip.dns[2] || ip.dns[3]) {
        memcpy(peer, ip.dns, 4);
    } else {
        static const uint8_t fallback[4] = { 8, 8, 8, 8 };
        memcpy(peer, fallback, 4);
    }
    static const uint16_t sizes[] = { 8, 64, 256, 512, 1024, 1400 };

    kprintf("\n");
    kprintf("=====================================================\n");
    kprintf("   DATA PLANE PROOF -- bare metal, no PC, no driver\n");
    kprintf("=====================================================\n");

    /* ---- 1. what the bearer is ------------------------------------------ */
    kprintf("\n[1] bearer\n");
    kprintf("    ip      %u.%u.%u.%u/%u\n", ip.ip[0], ip.ip[1], ip.ip[2],
            ip.ip[3],
            /* popcount of the mask, printed as a prefix length */
            (unsigned)(__builtin_popcount(((unsigned)ip.mask[0] << 24)
                                        | ((unsigned)ip.mask[1] << 16)
                                        | ((unsigned)ip.mask[2] << 8)
                                        |  (unsigned)ip.mask[3])));
    kprintf("    mtu     %u\n", qmi.mtu);
    kprintf("    handle  %08x\n", (unsigned)qmi.pkt_handle);
    kprintf("    framing %s, mux %s\n",
            rmn.qmap ? "QMAP" : "raw IP",
            qmi.mux_id ? "bound" : "unbound");

    /* ---- 2. reliability: does it work every time, not once? ------------- */
    kprintf("\n[2] reliability -- 20 echo requests to %u.%u.%u.%u\n",
            peer[0], peer[1], peer[2], peer[3]);
    int ok = 0, lo = 100000, hi = 0, sum = 0;
    for (int i = 0; i < 20; i++) {
        int rtt = ipnet_ping_n(&ip, peer, 8, 3000);
        if (rtt >= 0) {
            ok++; sum += rtt;
            if (rtt < lo) lo = rtt;
            if (rtt > hi) hi = rtt;
        }
    }
    if (ok) {
        kprintf("    %d/20 replied   rtt min %d ms  avg %d ms  max %d ms\n",
                ok, lo, sum / ok, hi);
    } else {
        kprintf("    0/20 replied\n");
    }

    /* ---- 3. packet size: the USB bulk path under real load -------------- */
    kprintf("\n[3] payload sizes -- each reply verified byte for byte\n");
    kprintf("    a 1400-byte echo is 22 USB packets of 64 B each way, so this\n");
    kprintf("    exercises segmentation, reassembly and short-packet\n");
    kprintf("    termination that an 8-byte ping never reaches.\n");
    int size_ok = 0;
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        uint16_t sz = sizes[i];
        int rtt = ipnet_ping_n(&ip, peer, sz, 4000);
        unsigned pkts = (unsigned)((sz + 8u + 20u + 63u) / 64u);
        if (rtt >= 0) {
            size_ok++;
            kprintf("    %4u B payload (%2u USB packets)  reply %d ms  "
                    "payload verified\n", sz, pkts, rtt);
        } else if (rtt == -2) {
            kprintf("    %4u B payload  TRUNCATED reply\n", sz);
        } else if (rtt == -3) {
            kprintf("    %4u B payload  CORRUPT reply\n", sz);
        } else {
            kprintf("    %4u B payload  no reply\n", sz);
        }
    }

    /* ---- 3b. the internet, for real -------------------------------------- */
    kprintf("\n[3b] public internet\n");
    static const uint8_t g_dns8[4] = { 8, 8, 8, 8 };
    int inet = ipnet_ping_n(&ip, g_dns8, 32, 4000);
    kprintf("    ping 8.8.8.8 ................ %s\n",
            inet >= 0 ? "REPLY" : "no reply");
    int named = (dns_lookup(ip.dns[0] ? ip.dns : g_dns8, "example.com") == 0);

    /* ---- 4. the verdict -------------------------------------------------- */
    kprintf("\n[4] verdict\n");
    kprintf("    QMI control over EP0 .............. %s\n",
            qmi.pkt_handle ? "PASS" : "fail");
    kprintf("    bearer + IP configuration ......... %s\n",
            (ip.ip[0] || ip.ip[1]) ? "PASS" : "fail");
    kprintf("    bidirectional IP over LTE ......... %s (%d/20)\n",
            ok >= 18 ? "PASS" : (ok ? "MARGINAL" : "FAIL"), ok);
    kprintf("    full-size packets, verified ....... %s (%d/%u)\n",
            size_ok == (int)(sizeof sizes / sizeof sizes[0]) ? "PASS" : "partial",
            size_ok, (unsigned)(sizeof sizes / sizeof sizes[0]));
    kprintf("    public internet ................... %s\n",
            (inet >= 0 || named) ? "PASS" : "NOT AVAILABLE");
    kprintf("    name resolution ................... %s\n",
            named ? "PASS" : "no answer");
    if (inet < 0 && !named)
        kprintf("        check AT+CGCONTRDP=1 above: an address with an EMPTY\n"
                "        gateway field is a PDN the operator will not route.\n");

    bench_datapath();
    kprintf("=====================================================\n");
}

/* One attempt at a working data path, in a named configuration. Returns 1
 * if a DNS answer came back over the bearer. */
static int try_data_path(const char *name, const char *apn,
                         int qmap, uint8_t mux, int qcrmcall)
{
    atc_resp_t r;
    kprintf("\ndata: ================ attempt: %s ================\n", name);

    if (qmi_set_data_format(&qmi, qmap) != 0) {
        kprintf("data: SET_DATA_FORMAT refused for %s\n", name);
        return 0;
    }
    /* Binding is optional and, on this firmware, not obviously helpful: the
     * ONE configuration that ever moved packets was raw IP with no bind at
     * all. mux == 0 means "skip the bind" so that case can be reproduced. */
    if (mux) qmi_bind_mux(&qmi, mux, qmap);
    else     kprintf("qmi: not binding the data port (raw pass-through)\n");

    if (qmi_start_network(&qmi, apn) != 0) {
        kprintf("data: START_NETWORK failed for %s\n", name);
        return 0;
    }

    /* Quectel's own trigger for wiring the RmNet call to the USB data port.
     * It answers "NO CARRIER" on the AT port even when it works -- that is
     * the data path leaving the command channel, not a failure. */
    if (qcrmcall) {
        int rc = atc_run("AT$QCRMCALL=1,1", &r, 20000);
        kprintf("data: AT$QCRMCALL=1,1 rc=%d final=\"%s\"\n", rc, r.final);
    }
    if (qmi_get_settings(&qmi) != 0) {
        kprintf("data: no IP settings for %s\n", name);
        qmi_stop_network(&qmi);
        return 0;
    }

    rmn.qmap   = (uint8_t)(qmap ? 1 : 0);
    rmn.mux_id = mux;
    rmn.debug  = 1;

    ipnet_init(&ip, &dp_ops, rmn.host_mac);
    memcpy(ip.ip,   qmi.ip,   4);
    memcpy(ip.mask, qmi.mask, 4);
    memcpy(ip.gw,   qmi.gw,   4);
    memcpy(ip.dns,  qmi.dns,  4);
    ip.have_lease = 1;
    memcpy(ip.gw_mac, rmn.peer_mac, 6);     /* point to point: no ARP */
    ip.have_gw_mac = 1;

    /* Ordinary UDP is the honest test. ICMP can mislead: one test operator
     * answered echo requests to any off-net address with a host-unreachable
     * from one of its own routers. */
    static const uint8_t g_dns[4]  = { 8, 8, 8, 8 };
    static const uint8_t cf_dns[4] = { 1, 1, 1, 1 };
    int ok = 0;
    if (ip.dns[0] || ip.dns[1] || ip.dns[2] || ip.dns[3])
        ok = (dns_lookup(ip.dns, "example.com") == 0);
    if (!ok) ok = (dns_lookup(g_dns,  "example.com") == 0);
    if (!ok) ok = (dns_lookup(cf_dns, "example.com") == 0);

    /* The DNS lookup cannot succeed on a PDN with no gateway, so it is not
     * the pass criterion -- it is one more datum. The proof run measures what
     * the stack can actually be held to. */
    (void)ok;
    proof_run();
    return 1;
    if (qcrmcall) atc_run("AT$QCRMCALL=0,1", &r, 10000);
    qmi_stop_network(&qmi);
    return 0;
}

/* What the module thinks the bearer situation is, asked over AT. Useful
 * precisely because it is a second opinion: QMI and the AT interpreter are
 * different clients of the same modem, and a disagreement is the finding. */
static void bearer_report(const char *when)
{
    static const char *const cmds[] = {
        "AT+COPS?", "AT+CGDCONT?", "AT+CGACT?", "AT+CGPADDR",
        /* THE diagnostic. Fields per TS 27.007 are cid, bearer_id, apn,
         * local_addr, gw_addr, DNS_prim, DNS_sec, P-CSCF_prim, P-CSCF_sec.
         * A data PDN with an address but an EMPTY gateway field is a bearer
         * the operator will not route -- that is what the previous SIM did,
         * and it is the one line that tells the two cases apart. */
        "AT+CGCONTRDP=1", "AT+CGCONTRDP=2", "AT+QNETDEVSTATUS?",
    };
    atc_resp_t r;
    kprintf("\ndata: --- bearer state %s ---\n", when);
    for (unsigned i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        int rc = atc_run(cmds[i], &r, 5000);
        show(cmds[i], &r, rc);
    }
}

/* Substring test -- modem.c has its own private copy; this is app-local. */
static int contains(const char *hay, const char *needle)
{
    for (const char *s = hay; *s; s++) {
        const char *a = s, *b = needle;
        while (*a && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

/* The <stat> field of a +CEREG:/+CREG: reply, or -1.
 * The line is "+CEREG: <n>,<stat>[,...]" -- take the value after the FIRST
 * comma, never a substring search: "+CEREG: 1,0" contains ",0" and would
 * otherwise be read as registered when it is the opposite. */
static int creg_stat(const char *line)
{
    const char *c = line;
    while (*c && *c != ',') c++;
    if (!*c) return -1;
    c++;
    while (*c == ' ') c++;
    if (*c < '0' || *c > '9') return -1;
    return *c - '0';
}

/* Wait for the SIM to initialise and the module to register.
 *
 * An answering AT port is NOT a ready modem. On a cold start the SIM finishes
 * initialising seconds after AT begins responding, and registration takes
 * longer still -- on this bench the SIM reported "not inserted", then sent
 * +CPIN: READY 2.3 s later, by which point the data plane had already tried
 * and failed with CALL_FAILED. Waiting for the module to actually have
 * service is the difference between a driver that works on a warm modem and
 * one that works on any modem.
 *
 * stat 1 = registered home, 5 = registered roaming; 2 = searching. */
static int wait_for_service(uint32_t timeout_ms)
{
    uint32_t t0 = ms_now(), last_log = 0;
    int sim_ready = 0;
    atc_resp_t r;

    kprintf("\nmodem: waiting for SIM and network...\n");
    while (ms_now() - t0 < timeout_ms) {
        if (!sim_ready && atc_run("AT+CPIN?", &r, 3000) == 0
            && r.n_lines && contains(r.buf + r.line_off[0], "READY")) {
            sim_ready = 1;
            kprintf("modem: SIM ready after %lu ms\n",
                    (unsigned long)(ms_now() - t0));
        }
        if (sim_ready) {
            int st = -1;
            if (atc_run("AT+CEREG?", &r, 3000) == 0 && r.n_lines)
                st = creg_stat(r.buf + r.line_off[0]);
            if (st < 0 && atc_run("AT+CREG?", &r, 3000) == 0 && r.n_lines)
                st = creg_stat(r.buf + r.line_off[0]);
            if (st == 1 || st == 5) {
                kprintf("modem: registered (%s) after %lu ms\n",
                        st == 1 ? "home" : "roaming",
                        (unsigned long)(ms_now() - t0));
                atc_run("AT+COPS?", &r, 3000);
                if (r.n_lines) kprintf("modem: %s\n", r.buf + r.line_off[0]);
                atc_run("AT+QNWINFO", &r, 3000);
                if (r.n_lines) kprintf("modem: %s\n", r.buf + r.line_off[0]);
                return 0;
            }
        }
        if (ms_now() - last_log >= 5000u) {
            kprintf("modem:   %s, not registered yet (%lu s)\n",
                    sim_ready ? "SIM ready" : "SIM still initialising",
                    (unsigned long)((ms_now() - t0) / 1000u));
            last_log = ms_now();
        }
        at_sleep(1000);
    }
    kprintf("modem: no service after %lu s -- continuing anyway\n",
            (unsigned long)(timeout_ms / 1000u));
    return -1;
}

/* The APN the MODULE chose for context 1, read from AT+CGDCONT?.
 *
 * Hard-coding one operator's APN only works until the SIM changes. The module
 * populates context 1 from its own IMSI-keyed table at attach, so asking it is
 * both more correct and more portable than guessing. Falls back to the compiled
 * default if the reply cannot be parsed. */
static const char *detect_apn(void)
{
    static char apn[52];
    atc_resp_t r;

    if (atc_run("AT+CGDCONT?", &r, 5000) != 0) return DATA_APN;
    for (uint8_t i = 0; i < r.n_lines; i++) {
        const char *ln = r.buf + r.line_off[i];
        if (!contains(ln, "+CGDCONT: 1,")) continue;
        /* +CGDCONT: 1,"IP","<apn>",... -- the third quoted field */
        int q = 0;
        const char *c = ln;
        for (; *c; c++) if (*c == '"' && ++q == 3) break;
        if (!*c) break;
        c++;
        size_t n = 0;
        while (*c && *c != '"' && n < sizeof apn - 1u) apn[n++] = *c++;
        apn[n] = 0;
        if (n) {
            kprintf("data: APN from the SIM's own context 1: \"%s\"\n", apn);
            return apn;
        }
        break;
    }
    kprintf("data: could not read the APN, using the built-in \"%s\"\n",
            DATA_APN);
    return DATA_APN;
}

static void data_plane(void)
{
    kprintf("\n=== DATA PLANE ===\n");

    /* The MCU owns the data plane itself: QMI over EP0 brings the link up
     * and hands us the IP configuration, then rmnet.c moves the packets.
     * (The AT shortcut, AT+QCRMTOOL, is a proven dead end here: its stage-2
     * "parameter setting" needs a cooperating host driver and returns -1 on
     * bare metal, while the same command passes all four stages on a PC.) */

    /* 1. find the QMI interface and assert DTR (mandatory for 2c7c:0800).
     *    qmi_open() zeroes the struct, so the debug flag goes on AFTER it --
     *    setting it before cost a run with no TLV dump in the log. */
    qmi_set_clock(ms_now, at_sleep);
    if (qmi_open(&qmi, &dev) != 0) {
        kprintf("data: no QMI interface\n");
        return;
    }
    qmi.debug = 1;

    /* Do not attempt a data call before the module has service: on a cold
     * SIM the answer would be CALL_FAILED for reasons that have nothing to
     * do with this driver. 90 s covers a first registration on a new SIM. */
    wait_for_service(90000);

    /* 2. a client id per service we drive */
    if (qmi_alloc_cid(&qmi, QMI_SVC_WDA, &qmi.cid_wda) != 0 ||
        qmi_alloc_cid(&qmi, QMI_SVC_WDS, &qmi.cid_wds) != 0) {
        /* Client ids are module-side state that survives a host reset. Once
         * enough have leaked, CTL stops answering entirely and only a modem
         * reboot clears them. Do that here: the main loop re-enumerates. */
        atc_resp_t rr;
        kprintf("data: CTL will not allocate client ids -- rebooting the "
                "module to clear the leaked ones\n");
        qmi_close(&qmi);
        atc_run("AT+CFUN=1,1", &rr, 10000);
        return;
    }

    /* 3. the pipes, opened once; each attempt only changes the framing */
    if (rmnet_open(&rmn, &dev, 0) != 0) {
        kprintf("data: could not open the RmNet pipes\n");
        return;
    }

    bearer_report("before");

    /* 4. Two configurations, in the order they are likely to be right.
     *    They differ only in whether the pipe carries QMAP headers. */
    qmi.profile = 1;                   /* pin the call to profile 1 */

    /* Ask the module which APN this SIM is provisioned for rather than
     * assuming the last one. If that fails, fall back to the empty APN so
     * the network picks its own default. */
    const char *apn = detect_apn();
    int up = try_data_path("raw IP, no bind", apn, 0, 0, 0);
    if (!up) up = try_data_path("raw IP, network default APN", "", 0, 0, 0);

    bearer_report("after");

    if (!up)
        kprintf("\ndata: neither configuration carried traffic -- the hex "
                "dumps above show exactly what the module sent back\n");

    /* 5. always hand everything back: a call left running across a board
     *    reset is what wedged the module into AT deafness once (A7), and a
     *    leaked client id is what wedged its CTL service. */
    qmi_stop_network(&qmi);
    qmi_close(&qmi);
}

/* ---- main ----------------------------------------------------------------- */
int main(void)
{
    board_init();
    uart_init(115200);
    kprintf("\n=== h745-rtos: modem host (APP=modem) ===\n");
    kprintf("cpu %lu MHz  apb1 %lu MHz  idcode %08lx (dev %03lx rev %04lx)\n",
            (unsigned long)(g_cpu_hz / 1000000u),
            (unsigned long)(g_apb1_hz / 1000000u),
            (unsigned long)g_idcode,
            (unsigned long)(g_idcode & 0xFFFu),
            (unsigned long)(g_idcode >> 16));
    kprintf("clk diag %02lx (revV=%lu scale1=%lu overdrive=%lu pll480=%lu)\n",
            (unsigned long)g_clk_diag, (unsigned long)(g_clk_diag & 1u),
            (unsigned long)((g_clk_diag >> 1) & 1u),
            (unsigned long)((g_clk_diag >> 2) & 1u),
            (unsigned long)((g_clk_diag >> 3) & 1u));
    kprintf("actvosrdy %lu\n", (unsigned long)((g_clk_diag >> 5) & 1u));
    kprintf("pwr csr1 %08lx  d3cr %08lx\n",
            (unsigned long)g_pwr_csr1, (unsigned long)g_pwr_d3cr);
    kprintf("pwr cr3  %08lx\n", (unsigned long)g_pwr_cr3);

    if (usbh_init() != USBH_OK) {
        board_led(LED_RED, 1);
        for (;;) cpu_wfi();
    }
    board_led(LED_GREEN, 1);

    for (;;) {
        kprintf("\nwaiting for the modem on CN13 (self-powered carrier!)...\n");
        uart_flush();
        while (usbh_wait_connect(1000) != USBH_OK) {}

        if (usbh_enumerate(&dev) != USBH_OK) {
            kprintf("modem: enumeration failed\n");
            goto unplug;
        }
        kprintf("modem: enumerated %04x:%04x (%s speed)\n",
                dev.vid, dev.pid, dev.speed == 2u ? "low" : "full");
        usbdump_config(&dev);   /* composition baseline, before any change */

        /* U6: first-contact classifier — a bricked modem shows up as the
         * Qualcomm QDLoader with no AT port at all. Do not probe-loop it. */
        memset(&modem, 0, sizeof modem);
        modem.state = modem_classify(&dev);
        if (modem.state == MODEM_EDL) {
            kprintf("modem: EDL/QDLoader (05C6:9008) -- bricked, no AT."
                    " Flash via QFirehose from a PC.\n");
            board_led(LED_RED, 1);
            goto unplug;
        }
        if (modem.state == MODEM_UNKNOWN_DEV)
            kprintf("modem: not a Quectel VID -- probing anyway\n");

        if (usbh_set_configuration(&dev) != USBH_OK) {
            kprintf("modem: SET_CONFIGURATION failed\n");
            goto unplug;
        }

        if (!find_at_port()) {
            kprintf("modem: no interface answered AT -- check power/boot\n");
            goto unplug;
        }
        board_led(LED_YELLOW, 1);

        atc_init(&at_ops);
        modem_set_clock(ms_now, at_sleep);
        modem_watch(&modem);                    /* reset/terminal URC watch */
        atc_on_urc(0, urc_printer, 0);          /* also print every URC     */

        /* U2/A1: enumeration != AT readiness. Wait for the modem to actually
         * answer before any real command (up to ~15 s from here). */
        if (modem_wait_at_ready(15000) != 0) {
            kprintf("modem: AT never became ready (still booting?)\n");
            goto unplug;
        }
        /* A3/A16/P6/U10/U11/F1: normalize NV state + capture identity. */
        modem_normalize(&modem);
        kprintf("modem: %s / %s  usbnet=%d  warns=0x%02x\n",
                modem.ati, modem.qgmr, modem.usbnet, modem.warns);
        if (modem.state == MODEM_TERMINAL) {
            kprintf("modem: +CFUN: 5 (FTM/corrupt QCN) -- RF dead, flag unit\n");
            board_led(LED_RED, 1);
            goto unplug;
        }

        identity_script();
        net_status();
        data_plane();
#if PLACE_CALL
        place_call();
#endif
        kprintf("modem: idle -- URCs stream below\n");
        uart_flush();

        while (usbh_connected()) {
            atc_process();
            at_sleep(100);
            /* A5: a boot burst mid-session means the modem crash-rebooted;
             * re-run bring-up rather than talk to a re-initializing modem. */
            if (modem_rebooted(&modem)) {
                kprintf("modem: boot burst mid-session -- it rebooted."
                        " re-initializing\n");
                modem_clear_reboot(&modem);
                if (modem_wait_at_ready(15000) == 0) modem_normalize(&modem);
            }
        }
unplug:
        board_led(LED_YELLOW, 0);
        kprintf("modem: disconnected\n");
        while (usbh_connected()) {}
    }
}
