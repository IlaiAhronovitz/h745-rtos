/* test_m3.c — simulation of the EC25 HARDENING layer (modem.c) + the two
 * usbh additions (ZLP, SET_CONTROL_LINE_STATE). Real usbh.c + atc.c +
 * modem.c against the emulated EC25, exercising the field-failure states
 * catalogued in EC25-ISSUES.md.
 *
 *   J  usbh additions: ZLP after exact-multiple bulk OUT; DTR/RTS line state
 *   K  modem_classify: EC25 vs EDL vs unknown device
 *   L  boot window: AT ignored until ready, modem_wait_at_ready recovers
 *   M  normalize: reads/forces urcport, flags usbnet!=0 and at_port=0,
 *      captures ATI + QGMR
 *   N  reset detection: an injected RDY burst flips modem_rebooted;
 *      +CFUN: 5 flips the state to TERMINAL
 */
#include <stdio.h>
#include <string.h>
#include "shim/stm32h745.h"
#include "usbh.h"
#include "atc.h"
#include "modem.h"
#include "sim_hw.h"

static int n_pass, n_fail;
#define CHECK(cond, name) do { \
    if (cond) { n_pass++; } \
    else { n_fail++; printf("  [FAIL] %s (line %d)\n", name, __LINE__); } \
} while (0)
#define CHECK_EQ(got, want, name) do { \
    long g = (long)(got), w = (long)(want); \
    if (g == w) { n_pass++; } \
    else { n_fail++; printf("  [FAIL] %s: got %ld, want %ld (line %d)\n", \
                            name, g, w, __LINE__); } \
} while (0)
#define CHECK_STR(got, want, name) do { \
    const char *cs = (got); \
    if (cs && 0 == strcmp(cs, (want))) { n_pass++; } \
    else { n_fail++; printf("  [FAIL] %s: got \"%s\" (line %d)\n", \
                            name, cs ? cs : "(null)", __LINE__); } \
} while (0)

static usbh_device_t dev;
static usbh_pipe_t at_in, at_out;
static modem_t m;

/* time + atc glue driving the real bulk pipes */
static uint32_t u_ms;
static int u_write(const uint8_t *d, uint16_t n)
{ return usbh_bulk(&dev, &at_out, (void *)d, n, 1000) == (int)n ? 0 : -1; }
static uint32_t u_now(void) { return u_ms; }
static void u_sleep(uint32_t ms)
{
    u_ms += ms ? ms : 1;
    uint8_t b[64];
    int r = usbh_bulk(&dev, &at_in, b, at_in.mps, 5);
    if (r > 0) atc_feed(b, (uint16_t)r);
}
static const atc_ops_t u_ops = { u_write, u_now, u_sleep };

static int open_at(void)
{
    usbh_iface_t f;
    if (usbh_find_interface(&dev, 0xFF, 0x00, 0x00, 1, &f) != USBH_OK) return -1;
    usbh_pipes_reset();
    if (usbh_pipe_open(&dev, f.ep_in, &at_in) != USBH_OK) return -1;
    if (usbh_pipe_open(&dev, f.ep_out, &at_out) != USBH_OK) return -1;
    return f.intf[2];
}

static void bring_up_to_at(void)
{
    sim_reset();
    usbh_init();
    sim_attach_fs();
    usbh_wait_connect(1000);
    usbh_enumerate(&dev);
    usbh_set_configuration(&dev);
    open_at();
    u_ms = 5000;
    memset(&m, 0, sizeof m);
    atc_init(&u_ops);
    modem_set_clock(u_now, u_sleep);
    modem_watch(&m);
}

/* ---- J: usbh additions ---------------------------------------------------- */
static void test_zlp_and_dtr(void)
{
    printf("J  ZLP after exact-multiple OUT + DTR/RTS line state\n");
    bring_up_to_at();

    /* a 64-byte bulk OUT (== MPS) must be followed by a zero-length packet */
    uint8_t big[64];
    memset(big, 'A', sizeof big);
    CHECK_EQ(usbh_bulk(&dev, &at_out, big, 64, 1000), 64, "64-byte OUT");
    CHECK(sim_zlp_after_multiple(), "exact-multiple OUT was ZLP-terminated");

    /* a 63-byte OUT must NOT get a spurious ZLP (still clean) */
    CHECK_EQ(usbh_bulk(&dev, &at_out, big, 63, 1000), 63, "63-byte OUT");
    CHECK(sim_zlp_after_multiple(), "non-multiple OUT left ZLP state clean");

    /* SET_CONTROL_LINE_STATE lands on the AT interface with DTR|RTS */
    CHECK_EQ(usbh_set_line_state(&dev, 2, 1, 1), USBH_OK, "set line state");
    CHECK_EQ(sim_line_state_iface(), 2, "DTR/RTS set on interface 2");
}

/* ---- K: device classification --------------------------------------------- */
static void test_classify(void)
{
    printf("K  first-contact classifier (EC25 / EDL / unknown)\n");
    sim_reset(); usbh_init(); sim_attach_fs();
    usbh_wait_connect(1000); usbh_enumerate(&dev);
    CHECK_EQ(modem_classify(&dev), MODEM_BOOTING, "EC25 -> BOOTING");

    sim_reset(); usbh_init(); sim_attach_edl();
    usbh_wait_connect(1000);
    CHECK_EQ(usbh_enumerate(&dev), USBH_OK, "EDL enumerates");
    CHECK_EQ(dev.vid, 0x05C6, "EDL VID");
    CHECK_EQ(dev.pid, 0x9008, "EDL PID");
    CHECK_EQ(modem_classify(&dev), MODEM_EDL, "05C6:9008 -> EDL");

    sim_reset(); usbh_init(); sim_attach_ls();
    usbh_wait_connect(1000); usbh_enumerate(&dev);
    CHECK_EQ(modem_classify(&dev), MODEM_UNKNOWN_DEV, "mouse -> UNKNOWN_DEV");
}

/* ---- L: boot window ------------------------------------------------------- */
static void test_boot_window(void)
{
    printf("L  boot-not-ready: AT ignored, then modem_wait_at_ready recovers\n");
    bring_up_to_at();
    sim_at_ready_after(4);               /* first 4 AT lines silently dropped */

    atc_resp_t r;
    CHECK(atc_cmd("AT", &r, 200) != 0, "AT ignored while booting");
    CHECK_EQ(modem_wait_at_ready(15000), 0, "wait_at_ready eventually gets OK");

    /* the readiness poll must not have taken forever */
    CHECK(u_now() - 5000 < 6000, "readiness reached within a few seconds");
}

/* ---- M: normalization ----------------------------------------------------- */
static void test_normalize(void)
{
    printf("M1 normalize on a clean modem\n");
    bring_up_to_at();
    modem_wait_at_ready(15000);
    CHECK_EQ(modem_normalize(&m), 0, "normalize -> READY");
    CHECK_EQ(m.state, MODEM_READY, "state READY");
    CHECK_EQ(m.usbnet, 0, "usbnet read as 0");
    CHECK_EQ(m.warns, 0, "no warnings on a clean modem");
    CHECK_STR(m.qgmr, "EC25AUGAR08A16M1G", "firmware captured");
    CHECK(strstr(m.ati, "EC25") != NULL, "ATI captured");

    printf("M2 urcport moved off the AT port -> forced back + no warn\n");
    bring_up_to_at();
    sim_set_urcport("uart1");            /* a prior owner diverted URCs */
    modem_wait_at_ready(15000);
    modem_normalize(&m);
    /* modem.c writes usbat then reads back; the model now reports usbat */
    CHECK(!(m.warns & MODEM_WARN_URCPORT_MOVED), "urcport forced to usbat");

    printf("M3 usbnet != 0 is flagged, never rewritten\n");
    bring_up_to_at();
    sim_set_usbnet(1);                   /* ECM composition */
    modem_wait_at_ready(15000);
    modem_normalize(&m);
    CHECK_EQ(m.usbnet, 1, "usbnet read as 1");
    CHECK(m.warns & MODEM_WARN_USBNET_NONZERO, "usbnet warn raised");

    printf("M4 usbcfg at_port flag = 0 is flagged\n");
    bring_up_to_at();
    sim_set_at_port_flag(0);
    modem_wait_at_ready(15000);
    modem_normalize(&m);
    CHECK(m.warns & MODEM_WARN_AT_PORT_FLAG, "at_port=0 warn raised");
}

/* ---- N: reset / terminal detection ---------------------------------------- */
static void test_reset_detect(void)
{
    printf("N1 injected boot burst mid-session -> modem_rebooted\n");
    bring_up_to_at();
    modem_wait_at_ready(15000);
    modem_normalize(&m);
    CHECK(!modem_rebooted(&m), "not flagged before the burst");

    sim_urc_push("RDY");
    sim_urc_push("+CFUN: 1");
    sim_urc_push("+CPIN: READY");
    for (int i = 0; i < 6; i++) u_sleep(10);
    atc_process();
    CHECK(modem_rebooted(&m), "boot burst set the reboot flag");
    modem_clear_reboot(&m);
    CHECK(!modem_rebooted(&m), "flag clears");

    printf("N2 +CFUN: 5 -> terminal state\n");
    bring_up_to_at();
    modem_wait_at_ready(15000);
    modem_normalize(&m);
    CHECK(m.state != MODEM_TERMINAL, "not terminal before");
    sim_urc_push("+CFUN: 5");
    for (int i = 0; i < 4; i++) u_sleep(10);
    atc_process();
    CHECK_EQ(m.state, MODEM_TERMINAL, "+CFUN: 5 -> TERMINAL");
}

int main(void)
{
    test_zlp_and_dtr();
    test_classify();
    test_boot_window();
    test_normalize();
    test_reset_detect();
    printf("\n==== %d passed, %d failed ====\n", n_pass, n_fail);
    return n_fail ? 1 : 0;
}
