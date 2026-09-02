/* test_m2.c — milestone-2 simulation: SET_CONFIGURATION, interface finding,
 * bulk pipes with data-toggle tracking, and the atc AT engine — unit-tested
 * against scripted byte streams, then end-to-end against the emulated EC25
 * (real usbh.c + real atc.c, both unmodified).
 *
 * Sections:
 *   G  configured state + usbh_find_interface + pipes + raw bulk AT probe
 *   H  atc engine unit tests (no USB): classification, prefix derivation,
 *      echo swallow, the +CEREG in-flight-vs-URC rule, finals (numeric and
 *      verbose CME), timeout -> lost_sync -> resync drain
 *   I  full chain: enumerate -> configure -> AT port -> identity script ->
 *      URC injection, with zero toggle violations across everything
 */
#include <stdio.h>
#include <string.h>
#include "shim/stm32h745.h"
#include "usbh.h"
#include "atc.h"
#include "sim_hw.h"

static int n_pass, n_fail;

#define CHECK(cond, name) do { \
    if (cond) { n_pass++; } \
    else { n_fail++; printf("  [FAIL] %s (line %d)\n", name, __LINE__); } \
} while (0)

#define CHECK_EQ(got, want, name) do { \
    long ceq_g = (long)(got), ceq_w = (long)(want); \
    if (ceq_g == ceq_w) { n_pass++; } \
    else { n_fail++; \
        printf("  [FAIL] %s: got %ld (0x%lx), want %ld (0x%lx) (line %d)\n", \
               name, ceq_g, ceq_g, ceq_w, ceq_w, __LINE__); } \
} while (0)

#define CHECK_STR(got, want, name) do { \
    const char *cs_g = (got); \
    if (cs_g && 0 == strcmp(cs_g, (want))) { n_pass++; } \
    else { n_fail++; printf("  [FAIL] %s: got \"%s\" (line %d)\n", \
                            name, cs_g ? cs_g : "(null)", __LINE__); } \
} while (0)

static usbh_device_t dev;
static usbh_pipe_t at_in, at_out;

/* ---- G: configured state, interface walk, raw bulk ------------------------ */
static void bring_up(void)
{
    sim_reset();
    usbh_init();
    sim_attach_fs();
    usbh_wait_connect(1000);
    usbh_enumerate(&dev);
}

static void test_config_and_ifaces(void)
{
    printf("G1 SET_CONFIGURATION + interface finding\n");
    bring_up();
    CHECK_EQ(usbh_set_configuration(&dev), USBH_OK, "set_configuration");
    const sim_txn_t *last_setup = NULL;
    for (int i = 0; i < sim_txn_count(); i++)
        if (sim_txn(i)->kind == 'S') last_setup = sim_txn(i);
    CHECK(last_setup && last_setup->breq == 9, "SET_CONFIGURATION on the wire");
    CHECK_EQ(last_setup->wvalue, 1, "bConfigurationValue = 1");

    usbh_iface_t f;
    CHECK_EQ(usbh_find_interface(&dev, 0xFF, 0x00, 0x00, 1, &f), USBH_OK,
             "ff/00/00 #1 found");
    CHECK_EQ(f.intf[2], 2, "... is interface 2 (AT)");
    CHECK_EQ(f.ep_in[2], 0x84, "... bulk IN 0x84");
    CHECK_EQ(f.ep_out[2], 0x03, "... bulk OUT 0x03");

    usbh_iface_t d;
    CHECK_EQ(usbh_find_interface(&dev, 0xFF, 0xFF, 0xFF, 0, &d), USBH_OK,
             "ff/ff/ff #0 (DIAG) found");
    CHECK_EQ(d.intf[2], 0, "... is interface 0");
    CHECK_EQ(d.ep_in[2], 0x81, "... bulk IN 0x81");
    CHECK(usbh_find_interface(&dev, 0xFF, 0x00, 0x00, 9, &f) != USBH_OK,
          "index out of range rejected");
    CHECK(usbh_find_interface(&dev, 0x08, 0x06, 0x50, 0, &f) != USBH_OK,
          "absent class rejected");
}

static int open_at_pipes(void)
{
    usbh_iface_t f;
    if (usbh_find_interface(&dev, 0xFF, 0x00, 0x00, 1, &f) != USBH_OK) return -1;
    usbh_pipes_reset();
    if (usbh_pipe_open(&dev, f.ep_in, &at_in) != USBH_OK) return -1;
    if (usbh_pipe_open(&dev, f.ep_out, &at_out) != USBH_OK) return -1;
    return 0;
}

static int raw_at_ok(void)
{
    uint8_t ping[3] = { 'A', 'T', '\r' };
    if (usbh_bulk(&dev, &at_out, ping, 3, 1000) != 3) return 0;
    uint8_t acc[128]; uint16_t n = 0;
    for (int tries = 0; tries < 50 && n < sizeof acc - 1u; tries++) {
        uint8_t b[64];
        int r = usbh_bulk(&dev, &at_in, b, at_in.mps, 5);
        if (r > 0) { memcpy(acc + n, b, (size_t)r); n = (uint16_t)(n + r); }
        if (r == 0 || r == USBH_ERR_TIMEOUT) {
            if (n >= 2) break;
        }
    }
    acc[n] = 0;
    return strstr((char *)acc, "OK") != NULL;
}

static void test_raw_bulk(void)
{
    printf("G2 raw bulk AT probe + toggle continuity\n");
    bring_up();
    usbh_set_configuration(&dev);
    CHECK_EQ(open_at_pipes(), 0, "pipes open (channels 2/3)");
    CHECK_EQ(at_in.channel, 2, "IN on channel 2");
    CHECK_EQ(at_out.channel, 3, "OUT on channel 3");
    CHECK_EQ(at_in.mps, 64, "IN mps 64");
    for (int i = 0; i < 6; i++)
        CHECK(raw_at_ok(), "AT -> OK roundtrip");
    CHECK_EQ(sim_toggle_violations(), 0, "zero DPID/toggle violations");
}

/* ---- G3: a bulk STALL must be recoverable --------------------------------
 * Without CLEAR_FEATURE(ENDPOINT_HALT) a stalled bulk endpoint answers every
 * later token with STALL and the pipe is dead until the whole device is
 * re-enumerated. Clearing the halt also resets the endpoint's data toggle to
 * DATA0 on the device (USB 2.0 §9.4.5), so the host has to follow -- if it
 * does not, the very next transfer fails on DTERR, which the model counts. */
static void test_bulk_stall_recovery(void)
{
    printf("G3 bulk STALL recovery via CLEAR_FEATURE(ENDPOINT_HALT)\n");
    bring_up();
    usbh_set_configuration(&dev);
    CHECK_EQ(open_at_pipes(), 0, "pipes open");
    CHECK(raw_at_ok(), "a healthy roundtrip first");

    int clears_before = sim_clear_feature_count();
    sim_bulk_stall(1);                    /* the next transaction STALLs */

    uint8_t ping[3] = { 'A', 'T', '\r' };
    int w = usbh_bulk(&dev, &at_out, ping, 3, 1000);
    CHECK_EQ(w, 3, "the stalled OUT is retried and succeeds");
    CHECK_EQ(sim_clear_feature_count(), clears_before + 1,
             "exactly one CLEAR_FEATURE(ENDPOINT_HALT) was sent");
    CHECK_EQ(at_out.toggle, 1, "host toggle followed the device back to DATA0");
    CHECK_EQ(sim_toggle_violations(), 0, "and no DTERR followed the clear");

    /* the pipe must still be usable afterwards */
    CHECK(raw_at_ok(), "the pipe still works after recovery");

    /* An endpoint that keeps stalling is reported, not retried forever. */
    sim_bulk_stall(99);
    int r = usbh_bulk(&dev, &at_out, ping, 3, 200);
    CHECK_EQ(r, USBH_ERR_STALL, "a persistently halted endpoint reports STALL");
    sim_bulk_stall(0);
}

/* ---- G4: usbh_bulk_frame -- framing AND transaction economy ---------------
 * This function had no coverage at all while it was the whole receive path.
 * It must do two things at once: end a frame on the device's short packet,
 * and get the frame in as few USB transactions as possible. Reading one
 * 64-byte packet per transaction is correct but costs 23 channel programs,
 * spin-waits and memcpys for a full-size frame. */
static void test_bulk_frame(void)
{
    printf("G4 usbh_bulk_frame framing and transaction count\n");
    bring_up();
    usbh_set_configuration(&dev);
    CHECK_EQ(open_at_pipes(), 0, "pipes open");

    static uint8_t src[1428], got[1600];

    /* 1. a short frame: one packet, one transaction */
    for (int i = 0; i < 40; i++) src[i] = (uint8_t)(i ^ 0x5A);
    sim_bulk_push(src, 40);
    sim_bulk_reset_count();
    int n = usbh_bulk_frame(&dev, &at_in, got, sizeof got, 500);
    CHECK_EQ(n, 40, "40-byte frame arrives whole");
    CHECK_EQ(memcmp(got, src, 40), 0, "and byte for byte");
    CHECK_EQ(sim_bulk_txn_count(), 1, "in a single transaction");

    /* 2. a full-size frame: still ONE transaction, not 23. This is the
     *    optimisation -- if it regresses, the count is what catches it. */
    for (int i = 0; i < (int)sizeof src; i++) src[i] = (uint8_t)(i * 7 + 3);
    sim_bulk_push(src, sizeof src);
    sim_bulk_reset_count();
    n = usbh_bulk_frame(&dev, &at_in, got, sizeof got, 500);
    CHECK_EQ(n, (int)sizeof src, "1428-byte frame arrives whole");
    CHECK_EQ(memcmp(got, src, sizeof src), 0, "and byte for byte");
    CHECK(sim_bulk_txn_count() <= 2,
          "in at most two transactions (was 23 when clamped to MPS)");

    /* 3. nothing queued: a clean "no frame", not an error */
    sim_bulk_reset_count();
    CHECK_EQ(usbh_bulk_frame(&dev, &at_in, got, sizeof got, 5), 0,
             "an idle endpoint reports no frame");

    /* 4. an exact multiple of MPS has no short packet to end it, so the
     *    frame is closed by the mid-frame timeout rather than lost */
    for (int i = 0; i < 128; i++) src[i] = (uint8_t)(0xA5 ^ i);
    sim_bulk_push(src, 128);
    n = usbh_bulk_frame(&dev, &at_in, got, sizeof got, 500);
    CHECK_EQ(n, 128, "an exact-multiple frame is still delivered whole");
    CHECK_EQ(memcmp(got, src, 128), 0, "and intact");

    CHECK_EQ(sim_toggle_violations(), 0, "no toggle violations throughout");

    /* 5. THE ONE THAT MULTI-PACKET READS INTRODUCED. A transfer spanning
     *    many packets can stall part-way: the device has advanced its toggle
     *    once per delivered packet, so after an ODD number the two ends
     *    disagree. Treating a timeout as "nothing moved" was safe only while
     *    every transfer was one packet; now it kills the pipe for good. */
    int viol_before = sim_toggle_violations();
    sim_bulk_push(src, 1428);
    sim_bulk_partial(7);                  /* 7 packets, then hang the channel */
    (void)usbh_bulk_frame(&dev, &at_in, got, sizeof got, 60);
    CHECK_EQ(sim_toggle_violations(), viol_before,
             "an aborted multi-packet read does not desync the toggle");

    /* the pipe has to still work afterwards -- that is the whole point */
    sim_bulk_drain();
    for (int i = 0; i < 24; i++) src[i] = (uint8_t)(i + 1);
    sim_bulk_push(src, 24);
    n = usbh_bulk_frame(&dev, &at_in, got, sizeof got, 500);
    CHECK_EQ(n, 24, "the pipe still delivers after the aborted transfer");
    CHECK_EQ(memcmp(got, src, 24), 0, "and the data is right");
    CHECK_EQ(sim_toggle_violations(), viol_before, "still no DTERR");
}

/* ---- H: atc engine unit tests (no USB) ------------------------------------ */
static uint32_t fk_ms;
static char     fk_writes[512];
static struct { const char *data; int at_sleep_n; } fk_feed[8];
static int fk_nfeed, fk_sleeps;

static int fk_write(const uint8_t *d, uint16_t n)
{
    size_t o = strlen(fk_writes);
    for (uint16_t i = 0; i < n && o < sizeof fk_writes - 2u; i++)
        fk_writes[o++] = (d[i] == '\r') ? '|' : (char)d[i];
    fk_writes[o] = 0;
    return 0;
}

static uint32_t fk_now(void) { return fk_ms; }

static void fk_sleep(uint32_t ms)
{
    fk_ms += ms;
    fk_sleeps++;
    for (int i = 0; i < fk_nfeed; i++)
        if (fk_feed[i].data && fk_feed[i].at_sleep_n == fk_sleeps) {
            atc_feed((const uint8_t *)fk_feed[i].data,
                     (uint16_t)strlen(fk_feed[i].data));
            fk_feed[i].data = NULL;
        }
}

static const atc_ops_t fk_ops = { fk_write, fk_now, fk_sleep };

static void fk_reset(void)
{
    fk_ms = 1000; fk_writes[0] = 0; fk_nfeed = 0; fk_sleeps = 0;
    atc_init(&fk_ops);
}

static void feed_on_sleep(int n, const char *data)
{
    fk_feed[fk_nfeed].data = data;
    fk_feed[fk_nfeed].at_sleep_n = n;
    fk_nfeed++;
}

static int cereg_hits;
static void on_cereg(const char *line, void *ctx)
{ (void)line; (void)ctx; cereg_hits++; }

static void test_atc_units(void)
{
    printf("H1 prefix derivation + solicited collection\n");
    fk_reset();
    atc_resp_t r;
    feed_on_sleep(1, "+QCSQ: \"LTE\",-61,-89,13,-9\r\nOK\r\n");
    CHECK_EQ(atc_cmd("AT+QCSQ?", &r, 1000), 0, "AT+QCSQ? completes");
    CHECK_EQ(r.err_kind, ATC_ERR_NONE, "OK final");
    CHECK_STR(atc_resp_single(&r), "+QCSQ: \"LTE\",-61,-89,13,-9",
              "prefixed line collected");

    printf("H2 echo swallow (ATE1 mode)\n");
    fk_reset();
    feed_on_sleep(1, "AT+GSN\r\nIMEI-TEST-0001\r\nOK\r\n");
    CHECK_EQ(atc_cmd("AT+GSN", &r, 1000), 0, "completes");
    CHECK_STR(atc_resp_single(&r), "IMEI-TEST-0001", "echo not in payload");

    printf("H3 the +CEREG ambiguity: solicited beats URC in flight\n");
    fk_reset();
    cereg_hits = 0;
    atc_on_urc("+CEREG:", on_cereg, NULL);
    feed_on_sleep(1, "+CEREG: 0,1\r\nOK\r\n");
    CHECK_EQ(atc_cmd("AT+CEREG?", &r, 1000), 0, "completes");
    CHECK_STR(atc_resp_single(&r), "+CEREG: 0,1", "line went to the response");
    CHECK_EQ(cereg_hits, 0, "URC callback NOT fired in flight");
    atc_feed((const uint8_t *)"+CEREG: 5\r\n", 11);
    atc_process();
    CHECK_EQ(cereg_hits, 1, "same prefix as URC when idle fires callback");

    printf("H4 finals: numeric CME, verbose CME, literals\n");
    fk_reset();
    feed_on_sleep(1, "+CME ERROR: 13\r\n");
    CHECK_EQ(atc_run("AT+CPIN?", &r, 1000), -ATC_EAT, "error final -> -EAT");
    CHECK_EQ(r.err_kind, ATC_ERR_CME, "kind CME");
    CHECK_EQ(r.err_code, 13, "numeric code 13");
    fk_reset();
    feed_on_sleep(1, "+CME ERROR: SIM not inserted\r\n");
    atc_run("AT+CPIN?", &r, 1000);
    CHECK_EQ(r.err_kind, ATC_ERR_CME, "verbose kind CME");
    CHECK_EQ(r.err_code, -1, "verbose code -1");
    fk_reset();
    feed_on_sleep(1, "ERROR\r\n");
    CHECK_EQ(atc_run("AT+NOPE", &r, 1000), -ATC_EAT, "bare ERROR");
    CHECK_EQ(r.err_kind, ATC_ERR_FINAL, "kind FINAL");
    fk_reset();
    feed_on_sleep(1, "Quectel\r\nEC25\r\nRevision: X\r\nOK\r\n");
    CHECK_EQ(atc_cmd("ATI", &r, 1000), 0, "ATI (no prefix)");
    CHECK_EQ(r.n_lines, 3, "3 unprefixed body lines");

    printf("H5 timeout -> lost_sync -> resync drain\n");
    fk_reset();
    CHECK_EQ(atc_cmd("AT+SLOW", &r, 100), -ATC_ETIMEDOUT, "times out");
    /* next command triggers a resync: the ping's own OK plus the stale
     * final from the dead command arrive during the drain, get swallowed,
     * then the real command runs clean */
    fk_writes[0] = 0;
    fk_sleeps = 0;                            /* schedule relative to now   */
    feed_on_sleep(1, "OK\r\nOK\r\n");         /* stale + ping's OK          */
    feed_on_sleep(7, "EC25AUGAR08A16M1G\r\nOK\r\n");
    CHECK_EQ(atc_cmd("AT+QGMR", &r, 2000), 0, "post-resync command clean");
    CHECK_EQ(atc_resync_count, 1, "exactly one resync");
    CHECK(strstr(fk_writes, "AT|AT+QGMR|") != NULL,
          "ping written before the real command");
    CHECK_STR(atc_resp_single(&r), "EC25AUGAR08A16M1G",
              "stale OK did not complete the new command");
}

/* ---- I: full chain against the emulated EC25 ------------------------------ */
static uint32_t u_ms;
static int u_write(const uint8_t *d, uint16_t n)
{ return usbh_bulk(&dev, &at_out, (void *)d, n, 1000) == (int)n ? 0 : -1; }
static uint32_t u_now(void) { return u_ms; }
static void u_sleep(uint32_t ms)
{
    u_ms += ms;
    uint8_t b[64];
    int r = usbh_bulk(&dev, &at_in, b, at_in.mps, 5);
    if (r > 0) atc_feed(b, (uint16_t)r);
}
static const atc_ops_t u_ops = { u_write, u_now, u_sleep };

static void test_full_chain(void)
{
    printf("I  full chain: usbh + atc against the emulated EC25\n");
    bring_up();
    CHECK_EQ(usbh_set_configuration(&dev), USBH_OK, "configure");
    CHECK_EQ(open_at_pipes(), 0, "AT pipes");
    u_ms = 5000;
    atc_init(&u_ops);
    cereg_hits = 0;
    atc_on_urc("+CEREG:", on_cereg, NULL);

    atc_resp_t r;
    CHECK_EQ(atc_run("ATE0", &r, 3000), 0, "ATE0");
    CHECK_EQ(atc_run("AT+CMEE=2", &r, 3000), 0, "AT+CMEE=2");
    CHECK_EQ(atc_run("ATI", &r, 3000), 0, "ATI");
    CHECK_EQ(r.n_lines, 3, "ATI: 3 lines");
    CHECK_STR(r.buf + r.line_off[2], "Revision: EC25AUGAR08A16M1G",
              "ATI revision line");
    CHECK_EQ(atc_run("AT+QGMR", &r, 3000), 0, "AT+QGMR");
    CHECK_STR(atc_resp_single(&r), "EC25AUGAR08A16M1G", "firmware id");
    CHECK_EQ(atc_run("AT+GSN", &r, 3000), 0, "AT+GSN");
    CHECK_STR(atc_resp_single(&r), "IMEI-TEST-0001", "IMEI");
    CHECK_EQ(atc_run("AT+CPIN?", &r, 3000), -ATC_EAT, "no SIM -> CME");
    CHECK_EQ(r.err_code, 13, "CME 13");
    CHECK_EQ(atc_run("AT+QTEMP", &r, 3000), 0, "AT+QTEMP");
    CHECK(atc_resp_single(&r)
          && 0 == strncmp(atc_resp_single(&r), "+QTEMP:", 7), "QTEMP line");

    sim_urc_push("+CEREG: 1");
    for (int i = 0; i < 5; i++) u_sleep(10);
    atc_process();
    CHECK_EQ(cereg_hits, 1, "injected URC dispatched");

    CHECK_EQ(sim_toggle_violations(), 0, "zero toggle violations end-to-end");
    CHECK_EQ(sim_hprt_w1c_violations(), 0, "zero HPRT w1c violations");
}

int main(void)
{
    test_config_and_ifaces();
    test_raw_bulk();
    test_bulk_stall_recovery();
    test_bulk_frame();
    test_atc_units();
    test_full_chain();
    printf("\n==== %d passed, %d failed ====\n", n_pass, n_fail);
    return n_fail ? 1 : 0;
}
