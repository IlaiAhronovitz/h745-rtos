/* test_m1.c â€” exhaustive host-side simulation of every milestone-1 function
 * in src/usbh.c, against the register-level OTG model + emulated devices.
 *
 * Sections:
 *   A  usbh_init: happy path (end state diffed against the REAL bench
 *      register audit of 2026-08-24) + every injectable init failure
 *   B  connect/detach and port reset, FS + LS
 *   C  enumeration: happy paths, boundary configs, transaction-order and
 *      device-addressing proofs
 *   D  control-transfer input space: lengths 0/1/mps/mps+1/large, OUT data
 *   E  fault matrix: persistent faults must propagate the right error;
 *      single-shot faults must be healed by the retry loop; disconnect
 *      must abort immediately
 *   F  global invariant: not one HPRT write ever carried a w1c bit
 */
#include <stdio.h>
#include <string.h>
#include "shim/stm32h745.h"      /* sim-backed GPIO structs for assertions */
#include "usbh.h"
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

static usbh_device_t dev;

/* ---- A: init ------------------------------------------------------------- */
static void test_init_happy(void)
{
    printf("A1 init happy path (vs bench register audit)\n");
    sim_reset();
    CHECK_EQ(usbh_init(), USBH_OK, "usbh_init");
    /* end state must equal what the real silicon showed over GDB */
    CHECK_EQ(sim_read(0x40080024), 0x238, "GRXFSIZ = 568w");
    CHECK_EQ(sim_read(0x40080028), 0x00800238, "HNPTXFSIZ = 128w @ 568");
    CHECK_EQ(sim_read(0x40080100), 0x010002B8, "HPTXFSIZ = 256w @ 696");
    CHECK_EQ(sim_read(0x40080038), 0x00010000, "GCCFG = PWRDWN only");
    CHECK_EQ(sim_read(0x40080008) & 0x2F, 0x2E, "GAHBCFG: INCR16+DMAEN, GINT off");
    unsigned g = sim_read(0x4008000C);
    CHECK((g >> 29) & 1, "GUSBCFG.FHMOD");
    CHECK((g >> 6) & 1, "GUSBCFG.PHYSEL");
    CHECK_EQ((g >> 10) & 0xF, 9, "GUSBCFG.TRDT = 9");
    CHECK_EQ(g & 7, 7, "GUSBCFG.TOCAL = 7");
    CHECK(sim_read(0x40080440) & (1u << 12), "HPRT.PPWR on");
    CHECK_EQ(sim_read(0x58024440), 0x00040095, "PLL3DIVR: N=150, Q=/5");
    CHECK_EQ(sim_read(0x58024454) & (3u << 20), 2u << 20, "USBSEL = pll3_q");
    CHECK(sim_read(0x5802480C) & (1u << 24), "PWR USB33DEN");
    CHECK_EQ((sim_gpioa.AFR[1] >> 12) & 0xF, 10, "PA11 = AF10");
    CHECK_EQ((sim_gpioa.AFR[1] >> 16) & 0xF, 10, "PA12 = AF10");
    CHECK_EQ(sim_gpiod.BSRR, 1u << 26, "PD10 driven LOW (VBUS on)");
}

static void test_init_faults(void)
{
    printf("A2 init failure paths\n");
    static const struct { enum sim_core_fault f; const char *n; } cases[] = {
        { F_HSE_DEAD, "HSE dead" }, { F_PLL3_DEAD, "PLL3 dead" },
        { F_USB33_DEAD, "USB33 dead" }, { F_CSRST_STUCK, "CSRST stuck" },
        { F_FLUSH_STUCK, "FIFO flush stuck" }, { F_CMOD_NEVER, "host mode never" },
        { F_NO_DMA_ARCH, "slave-only silicon" },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        sim_reset();
        sim_core_fault(cases[i].f);
        CHECK(usbh_init() == USBH_ERR_INIT, cases[i].n);
    }
}

/* ---- B: connect / port ---------------------------------------------------- */
static void test_connect(void)
{
    printf("B  connect, absence, LS port config\n");
    sim_reset();
    usbh_init();
    CHECK_EQ(usbh_connected(), 0, "no device -> not connected");
    CHECK_EQ(usbh_wait_connect(60), USBH_ERR_TIMEOUT, "wait times out");
    sim_attach_fs();
    CHECK_EQ(usbh_wait_connect(1000), USBH_OK, "connect detected");
    CHECK_EQ(usbh_connected(), 1, "connected reads 1");
    sim_detach();
    CHECK_EQ(usbh_connected(), 0, "detach reads 0");
}

/* ---- C: enumeration ------------------------------------------------------- */
static void test_enum_fs(void)
{
    printf("C1 FS composite enumeration (EC25-like)\n");
    sim_reset();
    usbh_init();
    sim_attach_fs();
    usbh_wait_connect(1000);
    CHECK_EQ(usbh_enumerate(&dev), USBH_OK, "enumerate");
    CHECK_EQ(dev.vid, 0x2C7C, "VID Quectel");
    CHECK_EQ(dev.pid, 0x0125, "PID EC25");
    CHECK_EQ(dev.speed, 1, "full speed");
    CHECK_EQ(dev.mps0, 64, "mps0 = 64");
    CHECK_EQ(dev.addr, 1, "address 1");
    CHECK_EQ(dev.cfg_total, 152, "wTotalLength");
    CHECK_EQ(dev.cfg_len, 152, "stored whole config");
    CHECK_EQ(dev.cfg_desc[9 + 5], 0xFF, "iface 0 class = vendor");
    /* port config after FS enable */
    CHECK_EQ(sim_read(0x40080400) & 3, 1, "HCFG.FSLSPCS = 48 MHz");
    CHECK_EQ(sim_read(0x40080404) & 0xFFFF, 47999, "HFIR = FS frame");
    /* transaction order + addressing discipline */
    int setups = 0;
    int addr_of_last_setup = -1;
    for (int i = 0; i < sim_txn_count(); i++)
        if (sim_txn(i)->kind == 'S') {
            setups++;
            addr_of_last_setup = sim_txn(i)->dad;
        }
    CHECK_EQ(setups, 5, "5 control requests total");
    CHECK_EQ(sim_txn(0)->breq, 6, "1st = GET_DESCRIPTOR");
    CHECK_EQ(sim_txn(0)->dad, 0, "... at address 0");
    CHECK_EQ(addr_of_last_setup, 1, "last request at address 1");
}

static void test_enum_ls(void)
{
    printf("C2 low-speed device (mouse)\n");
    sim_reset();
    usbh_init();
    sim_attach_ls();
    usbh_wait_connect(1000);
    CHECK_EQ(usbh_enumerate(&dev), USBH_OK, "enumerate LS");
    CHECK_EQ(dev.speed, 2, "low speed");
    CHECK_EQ(dev.mps0, 8, "mps0 = 8");
    CHECK_EQ(dev.vid, 0x046D, "mouse VID");
    CHECK_EQ(sim_read(0x40080400) & 3, 2, "HCFG.FSLSPCS = 6 MHz");
    CHECK_EQ(sim_read(0x40080404) & 0xFFFF, 5999, "HFIR = LS frame");
}

static void test_enum_bigcfg(void)
{
    printf("C3 config descriptor larger than the buffer\n");
    sim_reset();
    usbh_init();
    sim_attach_bigcfg();
    usbh_wait_connect(1000);
    CHECK_EQ(usbh_enumerate(&dev), USBH_OK, "enumerate");
    CHECK_EQ(dev.cfg_total, 600, "device reports 600");
    CHECK_EQ(dev.cfg_len, 512, "driver truncates to buffer");
}

/* ---- D: control-transfer input space -------------------------------------- */
static int get_cfg(uint16_t wlen, uint8_t *buf)
{
    uint8_t s[8] = { 0x80, 6, 0, 2, 0, 0,
                     (uint8_t)wlen, (uint8_t)(wlen >> 8) };
    return usbh_control(&dev, s, buf);
}

static void test_lengths(void)
{
    printf("D  control-transfer length boundaries\n");
    static uint8_t buf[512];
    sim_reset();
    usbh_init();
    sim_attach_fs();
    usbh_wait_connect(1000);
    usbh_enumerate(&dev);

    CHECK_EQ(get_cfg(0, NULL), 0, "wLength 0 (no data stage)");
    CHECK_EQ(get_cfg(1, buf), 1, "wLength 1");
    CHECK_EQ(buf[0], 9, "... first byte correct");
    CHECK_EQ(get_cfg(64, buf), 64, "wLength = exactly one MPS");
    CHECK_EQ(get_cfg(65, buf), 65, "wLength = MPS + 1");
    CHECK_EQ(get_cfg(152, buf), 152, "wLength = full config");
    CHECK_EQ(get_cfg(255, buf), 152, "wLength > available: short OK");
    /* OUT data stage (host-to-device with payload) */
    uint8_t out_setup[8] = { 0x40, 0x33, 0, 0, 0, 0, 16, 0 };
    uint8_t payload[16] = { 0 };
    CHECK_EQ(usbh_control(&dev, out_setup, payload), 16, "OUT data stage");
}

/* ---- E: fault matrix ------------------------------------------------------- */
static void test_faults(void)
{
    printf("E  device-fault matrix\n");
    static const struct { enum sim_dev_fault f; int want; const char *n; } hard[] = {
        { DF_STALL, USBH_ERR_STALL, "persistent STALL -> ERR_STALL" },
        { DF_NAK, USBH_ERR_NAK, "persistent NAK -> ERR_NAK" },
        { DF_TXERR, USBH_ERR_XFER, "persistent TXERR -> ERR_XFER" },
        { DF_DTERR, USBH_ERR_XFER, "persistent DTERR -> ERR_XFER" },
        { DF_BBERR, USBH_ERR_XFER, "persistent BBERR -> ERR_XFER" },
    };
    for (unsigned i = 0; i < sizeof hard / sizeof hard[0]; i++) {
        sim_reset();
        usbh_init();
        sim_attach_fs();
        usbh_wait_connect(1000);
        sim_dev_fault(-2, hard[i].f);
        CHECK_EQ(usbh_enumerate(&dev), hard[i].want, hard[i].n);
    }

    printf("E2 single-shot faults are healed by the retry loop\n");
    static const enum sim_dev_fault soft[] = { DF_TXERR, DF_NAK, DF_STALL };
    for (unsigned i = 0; i < sizeof soft / sizeof soft[0]; i++) {
        sim_reset();
        usbh_init();
        sim_attach_fs();
        usbh_wait_connect(1000);
        sim_dev_fault(0, soft[i]);       /* poison only the first request   */
        CHECK_EQ(usbh_enumerate(&dev), USBH_OK, "retry recovers");
        CHECK_EQ(dev.vid, 0x2C7C, "... and data is intact");
    }

    printf("E3 disconnect aborts immediately, no retry storm\n");
    sim_reset();
    usbh_init();
    sim_attach_fs();
    usbh_wait_connect(1000);
    sim_dev_fault(2, DF_DISCONNECT);     /* die on the 3rd request           */
    CHECK_EQ(usbh_enumerate(&dev), USBH_ERR_DISC, "ERR_DISC");
    CHECK(sim_txn_count() <= 10, "no post-disconnect traffic");

    printf("E4 garbage answer shorter than a descriptor header\n");
    sim_reset();
    usbh_init();
    sim_attach_fs();
    usbh_wait_connect(1000);
    sim_dev_fault(-2, DF_SHORT_GARBAGE);
    CHECK(usbh_enumerate(&dev) != USBH_OK, "short garbage rejected");

    printf("E5 dead port: reset with device that never enables\n");
    sim_reset();
    usbh_init();
    sim_core_fault(F_PENA_NEVER);
    sim_attach_fs();
    usbh_wait_connect(1000);
    CHECK_EQ(usbh_enumerate(&dev), USBH_ERR_TIMEOUT, "PENA timeout");
}

int main(void)
{
    test_init_happy();
    test_init_faults();
    test_connect();
    test_enum_fs();
    test_enum_ls();
    test_enum_bigcfg();
    test_lengths();
    test_faults();

    printf("F  global HPRT w1c invariant\n");
    CHECK_EQ(sim_hprt_w1c_violations(), 0,
             "no HPRT write ever carried a w1c bit");

    printf("\n==== %d passed, %d failed ====\n", n_pass, n_fail);
    return n_fail ? 1 : 0;
}
