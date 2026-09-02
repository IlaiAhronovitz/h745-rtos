/* sim_hw.c — behavioral model of the USB2 OTG core (buffer-DMA, host mode)
 * plus an emulated USB device, at register level.
 *
 * Mechanism: every register access from the driver goes through sim_reg32(),
 * which (1) diffs the register file against a shadow copy to detect the
 * driver's writes and applies hardware reactions, (2) advances simulated
 * time (DWT_CYCCNT), then (3) returns the storage cell. The driver's polling
 * loops all touch at least one REG32 per iteration, so the model always
 * makes progress — single-threaded and fully deterministic.
 *
 * DMA note: the model reads/writes host memory through the 32-bit HCDMA
 * value, which is lossless because the test binary links -no-pie (statics
 * live below 4 GB); sim_reset() asserts this.
 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "shim/stm32h745.h"      /* gpio_t + sim_reg32 + sim_gpio externs */
#include "sim_hw.h"

/* ---- register file ------------------------------------------------------- */
#define OTG_BASE  0x40080000u
#define RCC_BASE  0x58024400u
#define PWR_CR3_A 0x5802480Cu
#define DBG_CR_A  0x5C001004u
#define DWT_A     0xE0001004u

static uint32_t rcc[0x40];               /* RCC_BASE + 4*i, i < 0x40         */
static uint32_t otg[0x400];              /* OTG_BASE + 4*i, covers 0x1000    */
static uint32_t pwr_cr3, dbg_cr, dwt, demcr, dwt_ctrl, misc;

static uint32_t shadow_otg[0x400];
static uint32_t shadow_rcc[0x40];

gpio_t sim_gpioa, sim_gpiod;

#define CYC_PER_STEP 40000u              /* 0.625 ms of 64 MHz per access    */

/* register indices */
#define R(off)      rcc[(off) / 4]
#define O(off)      otg[(off) / 4]
#define SH_O(off)   shadow_otg[(off) / 4]

/* OTG offsets used by the model */
#define GOTGCTL 0x000
#define GAHBCFG 0x008
#define GUSBCFG 0x00C
#define GRSTCTL 0x010
#define GINTSTS 0x014
#define GCCFG   0x038
#define GHWCFG2 0x048
#define HPRT    0x440
#define HCCHAR(c)  (0x500 + 0x20 * (c))
#define HCINT(c)   (0x508 + 0x20 * (c))
#define HCTSIZ(c)  (0x510 + 0x20 * (c))
#define HCDMA(c)   (0x514 + 0x20 * (c))

/* ---- model state --------------------------------------------------------- */
static enum sim_core_fault core_fault;
static int  dev_present, dev_speed;      /* speed: 1 FS, 2 LS                */
static int  reset_pending;               /* PRST fell; PENA due              */
static int  hprt_violations;

static const uint8_t *dev_desc;
static const uint8_t *cfg_desc;
static int  cfg_len;
static int  dev_addr;                    /* address the device answers to    */

static sim_txn_t txns[128];
static int ntxn;
static int req_index;                    /* control requests seen            */
static int fault_at = -1;
static enum sim_dev_fault dev_fault_kind;

static uint8_t cur_setup[8];
static int in_data_pending;              /* bytes the device will send       */
static const uint8_t *in_data_src;

/* ---- configured state + per-endpoint data toggles + AT responder --------- */
static int configured;
static uint8_t ep_toggle[16][2];         /* [epnum][is_in]                   */
static int toggle_violations;

static uint8_t at_line[256];
static int at_len;
static int at_echo;
static uint8_t at_q[4096];               /* modem -> host byte queue         */
static int at_q_len;

/* EC25 hardening model state (test_m3) */
static int at_ignore_left;               /* boot-not-ready: drop N AT lines  */
static char urcport[16] = "usbat";
static int  usbnet_val;
static int  at_port_flag = 1;
static int  zlp_ok = 1;                  /* every exact-mult OUT got a ZLP    */
static int  pend_zlp;                    /* an OUT needs a following ZLP      */
static int  line_state_iface = -1;
static int  bulk_stall_left;             /* bulk transactions left to STALL  */
static int  bulk_halted;                 /* endpoint stays halted until clear */
static int  clear_feature_seen;          /* CLEAR_FEATURE(ENDPOINT_HALT) count*/
static int  bulk_txns;                   /* bulk transactions the device served */
static int  bulk_partial;                /* serve only N packets, then hang    */

void sim_at_ready_after(int n)   { at_ignore_left = n; }
void sim_set_urcport(const char *p)
{ strncpy(urcport, p, sizeof urcport - 1); urcport[sizeof urcport - 1] = 0; }
void sim_set_usbnet(int v)       { usbnet_val = v; }
void sim_set_at_port_flag(int on){ at_port_flag = on; }
int  sim_zlp_after_multiple(void){ return zlp_ok; }
int  sim_line_state_iface(void)  { return line_state_iface; }
void sim_bulk_stall(int n)       { bulk_stall_left = n; bulk_halted = 0; }
int  sim_clear_feature_count(void){ return clear_feature_seen; }
int  sim_bulk_txn_count(void)    { return bulk_txns; }
void sim_bulk_reset_count(void)  { bulk_txns = 0; }
void sim_bulk_partial(int packets) { bulk_partial = packets; }
void sim_bulk_drain(void)        { at_q_len = 0; }
void sim_bulk_push(const uint8_t *d, int n)
{ for (int i = 0; i < n && at_q_len < (int)sizeof at_q; i++) at_q[at_q_len++] = d[i]; }

static void at_push(const char *s)
{
    while (*s && at_q_len < (int)sizeof at_q) at_q[at_q_len++] = (uint8_t)*s++;
}

void sim_urc_push(const char *line)
{
    at_push("\r\n");
    at_push(line);
    at_push("\r\n");
}

int sim_toggle_violations(void) { return toggle_violations; }

static int at_prefix(const char *cmd, const char *p)
{
    while (*p) if (*cmd++ != *p++) return 0;
    return 1;
}

static void at_set_urcport_from(const char *cmd);   /* defined below */

/* The scripted EC25: enough of the AT surface for the bring-up + hardening
 * scripts, including the boot-not-ready window and the NV-config reads. */
static void at_respond(const char *cmd)
{
    /* Boot window (U2/A1): the first N commands are silently ignored, just
     * as the real modem drops AT before its baseband services are up. */
    if (at_ignore_left > 0) { at_ignore_left--; return; }

    if (at_echo) { at_push(cmd); at_push("\r"); }
    if (0 == strcmp(cmd, "AT")) {
        at_push("\r\nOK\r\n");
    } else if (0 == strcmp(cmd, "ATE0")) {
        at_echo = 0;
        at_push("\r\nOK\r\n");
    } else if (0 == strcmp(cmd, "ATV1") || 0 == strcmp(cmd, "ATQ0")
               || 0 == strcmp(cmd, "AT+CMEE=2") || 0 == strcmp(cmd, "AT+QSCLK=0")
               || 0 == strcmp(cmd, "AT+QCFG=\"apready\",0")) {
        at_push("\r\nOK\r\n");
    } else if (at_prefix(cmd, "AT+QURCCFG=\"urcport\",")) {
        at_set_urcport_from(cmd);        /* the write */
        at_push("\r\nOK\r\n");
    } else if (0 == strcmp(cmd, "AT+QURCCFG=\"urcport\"")) {
        at_push("\r\n+QURCCFG: \"urcport\",\"");
        at_push(urcport);
        at_push("\"\r\n\r\nOK\r\n");
    } else if (0 == strcmp(cmd, "AT+QCFG=\"usbnet\"")) {
        char b[40]; int n = 0, v = usbnet_val;
        const char *pre = "\r\n+QCFG: \"usbnet\",";
        at_push(pre);
        if (v == 0) b[n++] = '0';
        else { char t[8]; int m = 0; while (v) { t[m++] = (char)('0'+v%10); v/=10; }
               while (m) b[n++] = t[--m]; }
        b[n] = 0; at_push(b);
        at_push("\r\n\r\nOK\r\n");
    } else if (0 == strcmp(cmd, "AT+QCFG=\"usbcfg\"")) {
        at_push("\r\n+QCFG: \"usbcfg\",0x2C7C,0x0125,1,1,");
        at_push(at_port_flag ? "1" : "0");
        at_push(",1,1,0,0\r\n\r\nOK\r\n");
    } else if (0 == strcmp(cmd, "ATI")) {
        at_push("\r\nQuectel\r\nEC25\r\nRevision: EC25AUGAR08A16M1G\r\n\r\nOK\r\n");
    } else if (0 == strcmp(cmd, "AT+QGMR")) {
        at_push("\r\nEC25AUGAR08A16M1G\r\n\r\nOK\r\n");
    } else if (0 == strcmp(cmd, "AT+GSN")) {
        at_push("\r\nIMEI-TEST-0001\r\n\r\nOK\r\n");
    } else if (0 == strcmp(cmd, "AT+CPIN?")) {
        at_push("\r\n+CME ERROR: 13\r\n");
    } else if (0 == strcmp(cmd, "AT+QTEMP")) {
        at_push("\r\n+QTEMP: \"xo-therm-usr\",\"33\"\r\n\r\nOK\r\n");
    } else if (0 == strcmp(cmd, "AT+QCSQ?") || 0 == strcmp(cmd, "AT+QCSQ")) {
        at_push("\r\n+QCSQ: \"LTE\",-61,-89,13,-9\r\n\r\nOK\r\n");
    } else {
        at_push("\r\nERROR\r\n");
    }
}

/* Pull the quoted port out of AT+QURCCFG="urcport","<port>" */
static void at_set_urcport_from(const char *cmd)
{
    const char *c = cmd;
    int q = 0;
    for (; *c; c++) if (*c == '"' && ++q == 3) break;   /* 3rd quote */
    if (!*c) return;
    c++;
    int n = 0;
    while (*c && *c != '"' && n < (int)sizeof urcport - 1) urcport[n++] = *c++;
    urcport[n] = 0;
}

static void at_rx_byte(uint8_t b)
{
    if (b == '\r') {
        at_line[at_len] = 0;
        if (at_len) at_respond((const char *)at_line);
        at_len = 0;
        return;
    }
    if (b != '\n' && at_len < (int)sizeof at_line - 1)
        at_line[at_len++] = b;
}

/* ---- emulated devices ---------------------------------------------------- */
/* EC25 FS composite per the bring-up playbook: VID 2c7c PID 0125,
 * IF0 DIAG ff/ff/ff (2 bulk eps), IF1 NMEA / IF2 AT / IF3 Modem ff/00/00
 * (int IN + bulk pair each), IF4 QMI ff/ff/ff (3 eps). 152 bytes. */
static const uint8_t ec25_dev[18] = {
    18, 1, 0x00, 0x02, 0xEF, 0x02, 0x01, 64,
    0x7C, 0x2C, 0x25, 0x01, 0x18, 0x03, 1, 2, 3, 1,
};
static uint8_t ec25_cfg[9 + (9 + 2 * 7) + 3 * (9 + 3 * 7) + (9 + 3 * 7)];

static const uint8_t mouse_dev[18] = {
    18, 1, 0x00, 0x02, 0x00, 0x00, 0x00, 8,
    0x6D, 0x04, 0x4D, 0xC0, 0x00, 0x01, 1, 2, 0, 1,
};
static uint8_t mouse_cfg[9 + 9 + 7];
static uint8_t big_cfg[600];

/* Qualcomm QDLoader (EDL): a bricked/crashed modem. One vendor interface,
 * bulk pair, no AT — the driver must classify by VID:PID and stop. */
static const uint8_t edl_dev[18] = {
    18, 1, 0x00, 0x02, 0xFF, 0xFF, 0xFF, 64,
    0xC6, 0x05, 0x08, 0x90, 0x00, 0x00, 1, 2, 3, 1,
};
static uint8_t edl_cfg[9 + 9 + 2 * 7];

static void put_iface(uint8_t **pp, uint8_t num, uint8_t neps,
                      uint8_t sub, uint8_t proto)
{
    uint8_t itf[9] = { 9, 4, num, 0, neps, 0xFF, sub, proto, 0 };
    memcpy(*pp, itf, 9); *pp += 9;
}

static void put_ep(uint8_t **pp, uint8_t addr, uint8_t attr, uint8_t mps)
{
    uint8_t ep[7] = { 7, 5, addr, attr, mps, 0, (uint8_t)(attr == 3 ? 5 : 0) };
    memcpy(*pp, ep, 7); *pp += 7;
}

static void build_devices(void)
{
    uint8_t *p = ec25_cfg;
    int total = (int)sizeof ec25_cfg;
    uint8_t cfg9[9] = { 9, 2, (uint8_t)total, (uint8_t)(total >> 8),
                        5, 1, 0, 0xA0, 250 };
    memcpy(p, cfg9, 9); p += 9;
    put_iface(&p, 0, 2, 0xFF, 0xFF);              /* DIAG                   */
    put_ep(&p, 0x81, 2, 64); put_ep(&p, 0x01, 2, 64);
    put_iface(&p, 1, 3, 0x00, 0x00);              /* NMEA                   */
    put_ep(&p, 0x83, 3, 10); put_ep(&p, 0x82, 2, 64); put_ep(&p, 0x02, 2, 64);
    put_iface(&p, 2, 3, 0x00, 0x00);              /* AT                     */
    put_ep(&p, 0x85, 3, 10); put_ep(&p, 0x84, 2, 64); put_ep(&p, 0x03, 2, 64);
    put_iface(&p, 3, 3, 0x00, 0x00);              /* Modem/PPP              */
    put_ep(&p, 0x87, 3, 10); put_ep(&p, 0x86, 2, 64); put_ep(&p, 0x04, 2, 64);
    put_iface(&p, 4, 3, 0xFF, 0xFF);              /* QMI                    */
    put_ep(&p, 0x89, 3, 8); put_ep(&p, 0x88, 2, 64); put_ep(&p, 0x05, 2, 64);
    uint8_t m9[9]  = { 9, 2, (uint8_t)sizeof mouse_cfg, 0, 1, 1, 0, 0xA0, 50 };
    uint8_t mi[9]  = { 9, 4, 0, 0, 1, 3, 1, 2, 0 };
    uint8_t me[7]  = { 7, 5, 0x81, 3, 8, 0, 10 };
    memcpy(mouse_cfg, m9, 9);
    memcpy(mouse_cfg + 9, mi, 9);
    memcpy(mouse_cfg + 18, me, 7);
    {
        uint8_t *e = edl_cfg;
        uint8_t e9[9] = { 9, 2, (uint8_t)sizeof edl_cfg, 0, 1, 1, 0, 0xC0, 0 };
        memcpy(e, e9, 9); e += 9;
        put_iface(&e, 0, 2, 0xFF, 0xFF);
        put_ep(&e, 0x81, 2, 64); put_ep(&e, 0x01, 2, 64);
    }
    memset(big_cfg, 0xEE, sizeof big_cfg);
    uint8_t b9[9] = { 9, 2, (uint8_t)(sizeof big_cfg & 0xFF),
                      (uint8_t)(sizeof big_cfg >> 8), 1, 1, 0, 0xA0, 50 };
    memcpy(big_cfg, b9, 9);
    /* fill with valid-looking 7-byte descriptors so the walker stays sane */
    for (int i = 9; i + 7 <= (int)sizeof big_cfg; i += 7) {
        big_cfg[i] = 7; big_cfg[i + 1] = 5; big_cfg[i + 2] = 0x82;
        big_cfg[i + 3] = 2; big_cfg[i + 4] = 64; big_cfg[i + 5] = 0;
        big_cfg[i + 6] = 0;
    }
}

/* ---- public control ------------------------------------------------------ */
static void qmi_reset(void);          /* defined with the QMI model below */

void sim_reset(void)
{
    static int built;
    if (!built) { build_devices(); built = 1; }
    assert(sizeof(void *) == 8 && (uintptr_t)&ntxn < 0x100000000ull
           && "link with -no-pie so statics sit below 4 GB");
    memset(rcc, 0, sizeof rcc);
    memset(otg, 0, sizeof otg);
    memset(shadow_otg, 0, sizeof shadow_otg);
    memset(shadow_rcc, 0, sizeof shadow_rcc);
    memset(&sim_gpioa, 0, sizeof sim_gpioa);
    memset(&sim_gpiod, 0, sizeof sim_gpiod);
    pwr_cr3 = 0x44; dbg_cr = 0; dwt = 0; demcr = 0; dwt_ctrl = 0; misc = 0;
    core_fault = F_NONE;
    dev_present = 0; dev_speed = 1; reset_pending = 0;
    hprt_violations = 0; ntxn = 0; req_index = 0;
    fault_at = -1; dev_fault_kind = DF_NONE;
    dev_addr = 0; in_data_pending = 0;
    configured = 0; toggle_violations = 0;
    memset(ep_toggle, 0, sizeof ep_toggle);
    at_len = 0; at_q_len = 0; at_echo = 1;
    at_ignore_left = 0; usbnet_val = 0; at_port_flag = 1;
    zlp_ok = 1; pend_zlp = 0; line_state_iface = -1;
    bulk_stall_left = 0; bulk_halted = 0; clear_feature_seen = 0;
    bulk_txns = 0; bulk_partial = 0;
    strncpy(urcport, "usbat", sizeof urcport - 1);
    qmi_reset();
    /* silicon identity (real values from the bench) */
    O(0x03C) = 0x00002300;               /* CID     */
    O(0x040) = 0x4F54330A;               /* GSNPSID */
    O(0x044) = 0x00000000;
    O(GHWCFG2) = 0x229FE190;             /* arch=2: internal DMA             */
    O(0x04C) = 0x03B8D2E8;
    O(0x050) = 0xE3F00030;
    O(GRSTCTL) = 0x80000000;             /* AHBIDL idle                      */
    O(0x024) = 0x400;                    /* GRXFSIZ power-on                 */
    O(0x404) = 0xEA60;                   /* HFIR reset                       */
    memcpy(shadow_otg, otg, sizeof otg);
}

void sim_core_fault(enum sim_core_fault f) { core_fault = f; }

static void attach(const uint8_t *dd, const uint8_t *cd, int cl, int speed)
{
    dev_desc = dd; cfg_desc = cd; cfg_len = cl;
    dev_present = 1; dev_speed = speed; dev_addr = 0;
    O(HPRT) |= 1u | 2u;                  /* PCSTS + PCDET                    */
}

void sim_attach_fs(void)     { attach(ec25_dev, ec25_cfg, (int)sizeof ec25_cfg, 1); }
void sim_attach_ls(void)     { attach(mouse_dev, mouse_cfg, (int)sizeof mouse_cfg, 2); }
void sim_attach_bigcfg(void) { attach(ec25_dev, big_cfg, (int)sizeof big_cfg, 1); }
void sim_attach_edl(void)    { attach(edl_dev, edl_cfg, (int)sizeof edl_cfg, 1); }

void sim_detach(void)
{
    dev_present = 0;
    O(HPRT) &= ~1u;
    O(GINTSTS) |= (1u << 29);            /* DISCINT                          */
}

void sim_dev_fault(int nth, enum sim_dev_fault f)
{ fault_at = nth; dev_fault_kind = f; }

int  sim_txn_count(void)          { return ntxn; }
const sim_txn_t *sim_txn(int i)   { return &txns[i]; }
int  sim_hprt_w1c_violations(void){ return hprt_violations; }

/* ---- emulated QMI service -------------------------------------------------
 * A miniature Qualcomm modem behind EP0: it decodes the QMUX/TLV request that
 * SEND_ENCAPSULATED_COMMAND delivered and queues the reply that
 * GET_ENCAPSULATED_RESPONSE will hand back. Two behaviours matter as much as
 * the happy path, so both are modelled:
 *   - the reply is NOT instant: sim_qmi_delay(n) makes the first n polls
 *     return an empty frame, which is what a thinking module does;
 *   - unsolicited INDICATIONS share the same pipe, so sim_qmi_indication(1)
 *     pushes one in front of every reply. A client that does not match on
 *     message id + transaction id will swallow it as an answer.
 */
#define QMI_Q_MAX 4
static uint8_t qmi_q[QMI_Q_MAX][512];
static int     qmi_q_len[QMI_Q_MAX];
static int     qmi_q_head, qmi_q_tail;
static int     qmi_delay_left, qmi_delay_cfg, qmi_indicate, qmi_alias;
static int     qmi_link = 1, qmi_ul, qmi_dl;      /* 802.3 until told other */
static int     qmi_next_cid = 1;
static int     qmi_requests, qmi_net_up;
static int     bind_mux_id = -1, bind_client_type = -1, bind_ep_iface = -1;
static int     qmi_bad_frames;                    /* malformed host frames  */

static void qmi_push(const uint8_t *f, int len)
{
    if ((qmi_q_tail + 1) % QMI_Q_MAX == qmi_q_head) return;   /* full */
    memcpy(qmi_q[qmi_q_tail], f, (size_t)len);
    qmi_q_len[qmi_q_tail] = len;
    qmi_q_tail = (qmi_q_tail + 1) % QMI_Q_MAX;
}

static const uint8_t *qmi_pop(int *len)
{
    static const uint8_t empty[1] = { 0 };
    if (qmi_delay_left > 0) { qmi_delay_left--; *len = 0; return empty; }
    if (qmi_q_head == qmi_q_tail) { *len = 0; return empty; }
    const uint8_t *f = qmi_q[qmi_q_head];
    *len = qmi_q_len[qmi_q_head];
    qmi_q_head = (qmi_q_head + 1) % QMI_Q_MAX;
    return f;
}

/* Assemble one QMUX frame: marker, length, flags, service, client, then the
 * QMI header (1-byte txn for CTL, 2 for everything else) and the TLVs. */
static int qmi_frame(uint8_t *out, int ctl, uint8_t service, uint8_t cid,
                     uint16_t txn, uint16_t msg, int indication,
                     const uint8_t *tlvs, int tlv_len)
{
    int hdr = ctl ? 6 : 7;
    int total = 6 + hdr + tlv_len;
    out[0] = 0x01;
    out[1] = (uint8_t)((total - 1) & 0xFF);
    out[2] = (uint8_t)((total - 1) >> 8);
    out[3] = 0x80;                       /* device -> host */
    out[4] = service;
    out[5] = cid;
    uint8_t *h = out + 6;
    if (ctl) {
        h[0] = indication ? 0x02 : 0x01;
        h[1] = (uint8_t)txn;
        h[2] = (uint8_t)msg; h[3] = (uint8_t)(msg >> 8);
        h[4] = (uint8_t)tlv_len; h[5] = (uint8_t)(tlv_len >> 8);
    } else {
        h[0] = indication ? 0x04 : 0x02;
        h[1] = (uint8_t)txn; h[2] = (uint8_t)(txn >> 8);
        h[3] = (uint8_t)msg; h[4] = (uint8_t)(msg >> 8);
        h[5] = (uint8_t)tlv_len; h[6] = (uint8_t)(tlv_len >> 8);
    }
    memcpy(out + 6 + hdr, tlvs, (size_t)tlv_len);
    return total;
}

static int tlv_put(uint8_t *p, uint8_t type, const void *val, int len)
{
    p[0] = type; p[1] = (uint8_t)len; p[2] = (uint8_t)(len >> 8);
    memcpy(p + 3, val, (size_t)len);
    return 3 + len;
}

static int tlv_get(const uint8_t *tlvs, int len, uint8_t type,
                   const uint8_t **val)
{
    int o = 0;
    while (o + 3 <= len) {
        int l = tlvs[o + 1] | (tlvs[o + 2] << 8);
        if (o + 3 + l > len) break;
        if (tlvs[o] == type) { *val = tlvs + o + 3; return l; }
        o += 3 + l;
    }
    return -1;
}

static void qmi_serve(const uint8_t *req, uint16_t len)
{
    uint8_t f[512], tlv[256];
    int n = 0;

    qmi_requests++;
    if (len < 12 || req[0] != 0x01) { qmi_bad_frames++; return; }

    uint8_t  service = req[4], cid = req[5];
    int      ctl     = (service == 0x00);
    int      hdr     = ctl ? 6 : 7;
    const uint8_t *h = req + 6;
    uint16_t txn = ctl ? h[1] : (uint16_t)(h[1] | (h[2] << 8));
    uint16_t msg = ctl ? (uint16_t)(h[2] | (h[3] << 8))
                       : (uint16_t)(h[3] | (h[4] << 8));
    uint16_t tlen = ctl ? (uint16_t)(h[4] | (h[5] << 8))
                        : (uint16_t)(h[5] | (h[6] << 8));
    const uint8_t *tlvs = req + 6 + hdr;

    /* the host's own length field must agree with what the transfer moved */
    if ((int)(req[1] | (req[2] << 8)) + 1 != (int)len) qmi_bad_frames++;
    if (6 + hdr + tlen != len) qmi_bad_frames++;

    /* A decoy carrying OUR transaction id and OUR message id, but belonging
     * to a different service. Message ids are not unique across services --
     * CTL ALLOCATE_CID and WDA GET_DATA_FORMAT are both 0x0022 -- so a client
     * that matches on msg+txn alone will swallow this and return its TLVs as
     * if they were the answer. It also has the other service's header size,
     * so mis-parsing it shifts every field. */
    if (qmi_alias) {
        uint8_t junk[7];
        static const uint8_t ok4[4] = { 0, 0, 0, 0 };
        int jn = tlv_put(junk, 0x02, ok4, 4);
        int an = qmi_frame(f, !ctl, ctl ? 0x01 : 0x00, cid, txn, msg, 0,
                           junk, jn);
        qmi_push(f, an);
    }

    if (qmi_indicate) {                  /* noise ahead of the answer */
        uint8_t ind[8] = { 0x01, 0x02, 0x00, 0x00 };  /* status TLV body */
        int in = qmi_frame(f, 0, 0x01, cid, 0x9999, 0x0022, 1, ind, 4);
        qmi_push(f, in);
    }

    uint16_t status = 0, err = 0;
    const uint8_t *v = NULL;

    switch ((service << 16) | msg) {
    case (0x00 << 16) | 0x0022: {                  /* CTL ALLOCATE_CID */
        uint8_t body[2];
        if (tlv_get(tlvs, tlen, 0x01, &v) != 1) { status = 1; err = 0x0011; break; }
        body[0] = v[0];
        body[1] = (uint8_t)qmi_next_cid++;
        n += tlv_put(tlv + n, 0x01, body, 2);
        break;
    }
    case (0x1A << 16) | 0x0020:                    /* WDA SET_DATA_FORMAT */
        if (tlv_get(tlvs, tlen, 0x11, &v) == 4) qmi_link = v[0];
        if (tlv_get(tlvs, tlen, 0x12, &v) == 4) qmi_ul   = v[0];
        if (tlv_get(tlvs, tlen, 0x13, &v) == 4) qmi_dl   = v[0];
        break;
    case (0x1A << 16) | 0x0022: {                  /* WDA GET_DATA_FORMAT */
        uint32_t w;
        w = (uint32_t)qmi_link; n += tlv_put(tlv + n, 0x11, &w, 4);
        w = (uint32_t)qmi_ul;   n += tlv_put(tlv + n, 0x12, &w, 4);
        w = (uint32_t)qmi_dl;   n += tlv_put(tlv + n, 0x13, &w, 4);
        break;
    }
    case (0x01 << 16) | 0x0020: {                  /* WDS START_NETWORK */
        uint32_t handle = 0x12345678u;
        if (qmi_net_up) { status = 1; err = 0x001A; break; }   /* NO_EFFECT */
        if (tlv_get(tlvs, tlen, 0x14, &v) < 1) { status = 1; err = 0x0011; break; }
        qmi_net_up = 1;
        n += tlv_put(tlv + n, 0x01, &handle, 4);
        break;
    }
    case (0x01 << 16) | 0x00A2: {                  /* WDS BIND_MUX_DATA_PORT */
        /* Decode it properly so a test can assert the exact byte layout:
         * TLV 0x10 = {endpoint type u32, interface number u32}, TLV 0x11 =
         * mux id (one byte), TLV 0x13 = client type u32. Writing the mux id
         * at the wrong offset silently clobbers the 0x13 type byte, which is
         * a mistake the module itself will not complain about. */
        bind_mux_id = bind_client_type = bind_ep_iface = -1;
        if (tlv_get(tlvs, tlen, 0x10, &v) == 8) bind_ep_iface = v[4];
        if (tlv_get(tlvs, tlen, 0x11, &v) == 1) bind_mux_id = v[0];
        if (tlv_get(tlvs, tlen, 0x13, &v) == 4) bind_client_type = v[0];
        if (bind_mux_id < 0 || bind_client_type < 0) {
            status = 1; err = 0x0011;                  /* MISSING_ARG */
        }
        break;
    }
    case (0x01 << 16) | 0x0021:                    /* WDS STOP_NETWORK */
        if (!qmi_net_up) { status = 1; err = 0x001A; break; }
        qmi_net_up = 0;
        break;
    case (0x01 << 16) | 0x002D: {                  /* WDS GET_SETTINGS */
        uint32_t ip = 0x0A141EB0u, gw = 0x0A141EB1u;
        uint32_t mask = 0xFFFFFFFCu, dns = 0x0A000001u;
        uint32_t mtu = 1500;   /* real QMI reports MTU as a u32 */
        if (!qmi_net_up) { status = 1; err = 0x000F; break; }  /* OUT_OF_CALL */
        n += tlv_put(tlv + n, 0x1E, &ip,   4);
        n += tlv_put(tlv + n, 0x20, &gw,   4);
        n += tlv_put(tlv + n, 0x21, &mask, 4);
        n += tlv_put(tlv + n, 0x15, &dns,  4);
        n += tlv_put(tlv + n, 0x29, &mtu,  4);
        break;
    }
    default:
        status = 1; err = 0x0002;                  /* UNSUPPORTED */
        break;
    }

    /* the mandatory result TLV goes FIRST, as real firmware emits it */
    uint8_t out[256];
    uint8_t res[4] = { (uint8_t)status, (uint8_t)(status >> 8),
                       (uint8_t)err, (uint8_t)(err >> 8) };
    int on = tlv_put(out, 0x02, res, 4);
    if (!status) { memcpy(out + on, tlv, (size_t)n); on += n; }

    int flen = qmi_frame(f, ctl, service, cid, txn, msg, 0, out, on);
    qmi_push(f, flen);
    qmi_delay_left = qmi_delay_cfg;
}

static void qmi_reset(void)
{
    qmi_q_head = qmi_q_tail = 0;
    qmi_delay_left = qmi_delay_cfg = 0;
    qmi_indicate = 0; qmi_alias = 0;
    qmi_link = 1; qmi_ul = 0; qmi_dl = 0;
    qmi_next_cid = 1;
    qmi_requests = 0; qmi_net_up = 0; qmi_bad_frames = 0;
    bind_mux_id = bind_client_type = bind_ep_iface = -1;
}

void sim_qmi_delay(int polls)      { qmi_delay_cfg = polls; qmi_delay_left = 0; }
void sim_qmi_indication(int on)    { qmi_indicate = on; }
void sim_qmi_alias(int on)         { qmi_alias = on; }
int  sim_qmi_requests(void)        { return qmi_requests; }
int  sim_qmi_bad_frames(void)      { return qmi_bad_frames; }
int  sim_qmi_link_layer(void)      { return qmi_link; }
int  sim_qmi_net_up(void)          { return qmi_net_up; }
int  sim_qmi_bind_mux(void)        { return bind_mux_id; }
int  sim_qmi_bind_client(void)     { return bind_client_type; }
int  sim_qmi_bind_iface(void)      { return bind_ep_iface; }

/* ---- device-model transaction service ------------------------------------ */
static void log_txn(uint8_t kind, uint8_t dad, uint16_t len)
{
    if (ntxn < (int)(sizeof txns / sizeof txns[0])) {
        txns[ntxn].kind = kind; txns[ntxn].dad = dad;
        txns[ntxn].breq = cur_setup[1];
        txns[ntxn].wvalue = (uint16_t)(cur_setup[2] | (cur_setup[3] << 8));
        txns[ntxn].len = len;
        ntxn++;
    }
}

static int apply_dev_fault(int ch)
{
    /* fault_at == -2 means "every request" (persistent fault). */
    if (dev_fault_kind == DF_NONE) return 0;
    if (fault_at != -2 && req_index - 1 != fault_at) return 0;
    switch (dev_fault_kind) {
    case DF_STALL:  O(HCINT(ch)) |= (1u << 3) | (1u << 1); return 1;
    case DF_NAK:    O(HCINT(ch)) |= (1u << 4) | (1u << 1); return 1;
    case DF_TXERR:  O(HCINT(ch)) |= (1u << 7) | (1u << 1); return 1;
    case DF_DTERR:  O(HCINT(ch)) |= (1u << 10) | (1u << 1); return 1;
    case DF_BBERR:  O(HCINT(ch)) |= (1u << 8) | (1u << 1); return 1;
    case DF_TIMEOUT: return 1;                    /* never halt              */
    case DF_DISCONNECT: sim_detach(); return 1;
    case DF_SHORT_GARBAGE: {
        uint8_t *dst = (uint8_t *)(uintptr_t)O(HCDMA(ch));
        dst[0] = 0xAA; dst[1] = 0xBB;             /* 2 junk bytes            */
        uint32_t x = O(HCTSIZ(ch));
        O(HCTSIZ(ch)) = (x & ~0x7FFFFu) | ((x & 0x7FFFFu) - 2u);
        O(HCINT(ch)) |= 1u | (1u << 1);
        return 1;
    }
    default: return 0;
    }
}

/* Bulk endpoint service: the AT interface's pair (IN 0x84 / OUT 0x03) is
 * live; other INs never answer (driver sees a timeout, like real silicon
 * retrying NAKs); other OUTs accept-and-discard. Every bulk transaction's
 * DPID is checked against the endpoint's true toggle — a mismatch is a
 * DTERR plus a counted violation (the toggle-tracking proof). */
static void serve_bulk(int ch, uint32_t ts, uint32_t xfrsiz, uint8_t *mem)
{
    uint32_t hc = O(HCCHAR(ch));
    int epnum = (hc >> 11) & 0xF;
    int is_in = (hc >> 15) & 1;
    int pid   = (ts >> 29) & 3;
    uint16_t mps = hc & 0x7FF;

    bulk_txns++;          /* one channel program = one bulk transaction */

    /* A halted endpoint STALLs before it looks at anything else -- it does
     * not consume the data toggle, which is why the toggle check comes
     * after. The halt persists until CLEAR_FEATURE(ENDPOINT_HALT). */
    if (bulk_stall_left > 0) {
        bulk_stall_left--;
        bulk_halted = 1;
        O(HCINT(ch)) |= (1u << 3) | (1u << 1);        /* STALL|CHH           */
        return;
    }
    if (bulk_halted) {                                /* still halted        */
        O(HCINT(ch)) |= (1u << 3) | (1u << 1);
        return;
    }

    int exp = ep_toggle[epnum][is_in];
    if (pid != (exp ? 2 : 0)) {
        toggle_violations++;
        O(HCINT(ch)) |= (1u << 10) | (1u << 1);       /* DTERR|CHH           */
        return;
    }

    if (!is_in) {
        if (epnum == 3) {                             /* AT OUT              */
            /* U14 ZLP check: a data OUT that is an exact MPS multiple must
             * be followed by a zero-length OUT before any further data. */
            if (xfrsiz == 0) {
                pend_zlp = 0;                          /* the ZLP arrived    */
            } else {
                if (pend_zlp) zlp_ok = 0;              /* prev mult unclosed */
                for (uint32_t i = 0; i < xfrsiz; i++) at_rx_byte(mem[i]);
                pend_zlp = (xfrsiz % mps == 0u) ? 1 : 0;
            }
        }
        uint32_t npkt = xfrsiz ? (xfrsiz + mps - 1u) / mps : 1u;
        ep_toggle[epnum][0] ^= (uint8_t)(npkt & 1u);
        O(HCTSIZ(ch)) = (ts & ~((0x7FFFFu) | (3u << 29)))
                      | ((uint32_t)(ep_toggle[epnum][0] ? 2u : 0u) << 29);
        O(HCINT(ch)) |= 1u | (1u << 1);
        return;
    }

    if (epnum != 4 || at_q_len == 0) return;          /* no data: no halt    */
    uint32_t got = (uint32_t)at_q_len;
    if (got > xfrsiz) got = xfrsiz;

    /* Partial-transfer mode: hand over exactly N packets, advance the
     * device's toggle for each, and then NEVER raise CHH -- the host times
     * out mid-transfer. This is the shape that desyncs a multi-packet pipe
     * when the host assumes a timeout means nothing moved. */
    if (bulk_partial > 0) {
        uint32_t part = (uint32_t)bulk_partial * mps;
        if (part > got) part = got;
        memcpy(mem, at_q, part);
        memmove(at_q, at_q + part, (size_t)(at_q_len - (int)part));
        at_q_len -= (int)part;
        uint32_t pk = (part + mps - 1u) / mps;
        ep_toggle[epnum][1] ^= (uint8_t)(pk & 1u);
        O(HCTSIZ(ch)) = (ts & ~((0x7FFFFu) | (3u << 29)))
                      | ((xfrsiz - part) & 0x7FFFFu)
                      | ((uint32_t)(ep_toggle[epnum][1] ? 2u : 0u) << 29);
        bulk_partial = 0;
        return;                                       /* no CHH: host waits */
    }
    memcpy(mem, at_q, got);
    memmove(at_q, at_q + got, (size_t)(at_q_len - (int)got));
    at_q_len -= (int)got;
    uint32_t pkts = (got + mps - 1u) / mps;
    if (pkts == 0) pkts = 1;
    ep_toggle[epnum][1] ^= (uint8_t)(pkts & 1u);
    O(HCTSIZ(ch)) = (ts & ~((0x7FFFFu) | (3u << 29)))
                  | (xfrsiz - got)
                  | ((uint32_t)(ep_toggle[epnum][1] ? 2u : 0u) << 29);
    O(HCINT(ch)) |= 1u | (1u << 1);
}

static void serve_channel(int ch)
{
    uint32_t hc = O(HCCHAR(ch));
    uint32_t ts = O(HCTSIZ(ch));
    int is_in  = (hc >> 15) & 1;
    int dad    = (hc >> 22) & 0x7F;
    int pid    = (ts >> 29) & 3;
    uint32_t xfrsiz = ts & 0x7FFFFu;
    uint8_t *mem = (uint8_t *)(uintptr_t)O(HCDMA(ch));

    if (!dev_present) {                  /* nobody home: transaction errors  */
        O(HCINT(ch)) |= (1u << 7) | (1u << 1);
        return;
    }
    assert(dad == dev_addr && "host addressed a device address that is not ours");

    if (((hc >> 18) & 3u) == 2u) {       /* bulk endpoint */
        serve_bulk(ch, ts, xfrsiz, mem);
        return;
    }

    if (!is_in && pid == 3) {            /* SETUP */
        memcpy(cur_setup, mem, 8);
        req_index++;
        log_txn('S', (uint8_t)dad, 8);
        if (apply_dev_fault(ch)) return;
        /* SET_CONTROL_LINE_STATE (class 0x21 / bRequest 0x22): record it. */
        if (cur_setup[0] == 0x21 && cur_setup[1] == 0x22)
            line_state_iface = cur_setup[4];
        /* CLEAR_FEATURE(ENDPOINT_HALT): 0x02 / bRequest 1 / wValue 0. Clears
         * the halt AND resets that endpoint's data toggle to DATA0 (USB 2.0
         * §9.4.5) -- a host that forgets to follow will fail on DTERR, which
         * is exactly what sim_toggle_violations() counts. */
        if (cur_setup[0] == 0x02 && cur_setup[1] == 0x01
            && cur_setup[2] == 0x00 && cur_setup[3] == 0x00) {
            clear_feature_seen++;
            /* Clears the CURRENT halt only. bulk_stall_left is the test's
             * budget of transactions that will stall, and it deliberately
             * survives -- otherwise the model could not represent an
             * endpoint that halts again the moment it is cleared. */
            bulk_halted = 0;
            ep_toggle[cur_setup[4] & 0xFu][(cur_setup[4] >> 7) & 1u] = 0;
        }
        uint16_t wlen = (uint16_t)(cur_setup[6] | (cur_setup[7] << 8));
        in_data_pending = 0;
        if ((cur_setup[0] & 0x80) && wlen) {
            uint8_t type = cur_setup[3], breq = cur_setup[1];
            if (breq == 6 && type == 1) {
                in_data_src = dev_desc; in_data_pending = 18;
            } else if (breq == 6 && type == 2) {
                in_data_src = cfg_desc; in_data_pending = cfg_len;
            } else if (cur_setup[0] == 0xA1 && breq == 0x01) {
                /* GET_ENCAPSULATED_RESPONSE: hand over the head of the QMI
                 * reply queue, or a zero-length frame when it is empty --
                 * which is exactly what a real module does while it thinks. */
                in_data_src = qmi_pop(&in_data_pending);
            } else {
                in_data_pending = -1;    /* unknown request -> STALL later   */
            }
            if (in_data_pending > wlen) in_data_pending = wlen;
        }
        O(HCINT(ch)) |= 1u | (1u << 1) | (1u << 5);   /* XFRC|CHH|ACK       */
        return;
    }

    if (is_in) {                         /* IN data or IN status             */
        if (apply_dev_fault(ch)) return;
        if (xfrsiz == 0 || in_data_pending == 0) {    /* status ZLP          */
            log_txn(xfrsiz ? 'I' : 'i', (uint8_t)dad, 0);
            if (cur_setup[1] == 5) dev_addr = cur_setup[2]; /* SET_ADDRESS
                                                     * takes effect after
                                                     * the status stage      */
            if (cur_setup[1] == 9) {     /* SET_CONFIGURATION: Configured
                                          * state, all data toggles reset   */
                configured = cur_setup[2];
                memset(ep_toggle, 0, sizeof ep_toggle);
                at_len = 0; at_q_len = 0; at_echo = 1;
            }
            O(HCINT(ch)) |= 1u | (1u << 1);
            return;
        }
        if (in_data_pending < 0) {       /* unknown request                  */
            O(HCINT(ch)) |= (1u << 3) | (1u << 1);    /* STALL               */
            return;
        }
        uint32_t got = (uint32_t)in_data_pending;
        if (got > xfrsiz) got = xfrsiz;
        memcpy(mem, in_data_src, got);
        in_data_pending = 0;
        O(HCTSIZ(ch)) = (ts & ~0x7FFFFu) | (xfrsiz - got);
        log_txn('I', (uint8_t)dad, (uint16_t)got);
        O(HCINT(ch)) |= 1u | (1u << 1);
        return;
    }

    /* OUT data or OUT status */
    if (apply_dev_fault(ch)) return;
    /* SEND_ENCAPSULATED_COMMAND carries a QMI request in its data stage. */
    if (xfrsiz && cur_setup[0] == 0x21 && cur_setup[1] == 0x00)
        qmi_serve(mem, (uint16_t)xfrsiz);
    log_txn(xfrsiz ? 'O' : 'o', (uint8_t)dad, (uint16_t)xfrsiz);
    O(HCINT(ch)) |= 1u | (1u << 1);
}

/* ---- the step function: react to writes, advance time -------------------- */
static void step(void)
{
    /* rc_w1 semantics FIRST: a driver write of 1s to HCINT/GINTSTS clears
     * those bits (plain storage would SET them and hand the driver a fake
     * XFRC -- the first harness run proved exactly that). Detected via
     * shadow diff; the model's own OR-ins happen later in this step and
     * are captured by the trailing shadow sync. */
    for (int c = 0; c < 16; c++) {
        uint32_t v = O(HCINT(c)), sh = SH_O(HCINT(c));
        if (v != sh) O(HCINT(c)) = sh & ~v;
    }
    {
        uint32_t v = O(GINTSTS), sh = SH_O(GINTSTS);
        if (v != sh) O(GINTSTS) = sh & ~v;
    }

    /* clocks / power */
    if ((R(0x000) & (1u << 16)) && core_fault != F_HSE_DEAD)
        R(0x000) |= (1u << 17);
    if ((R(0x000) & (1u << 28)) && core_fault != F_PLL3_DEAD)
        R(0x000) |= (1u << 29);
    if ((pwr_cr3 & (1u << 24)) && core_fault != F_USB33_DEAD)
        pwr_cr3 |= (1u << 26);

    /* core resets / flushes self-clear */
    if (O(GRSTCTL) & 1u) {
        if (core_fault != F_CSRST_STUCK) O(GRSTCTL) &= ~1u;
    }
    if (O(GRSTCTL) & ((1u << 4) | (1u << 5))) {
        if (core_fault != F_FLUSH_STUCK)
            O(GRSTCTL) &= ~((1u << 4) | (1u << 5) | (0x1Fu << 6));
    }

    /* forced host mode -> CMOD */
    if ((O(GUSBCFG) & (1u << 29)) && core_fault != F_CMOD_NEVER)
        O(GINTSTS) |= 1u;
    if (core_fault == F_NO_DMA_ARCH)
        O(GHWCFG2) = 0x229FE190u & ~(3u << 3);

    /* HPRT: detect the driver's writes via shadow diff */
    uint32_t w = O(HPRT), sh = SH_O(HPRT);
    if (w != sh) {
        /* driver wrote HPRT; check the w1c discipline */
        if (w & (1u << 2)) hprt_violations++;         /* PENA written as 1   */
        if (w & (1u << 1)) { w &= ~(1u << 1); }       /* PCDET ack           */
        if (w & (1u << 3)) { w &= ~(1u << 3); }       /* PENCHNG ack         */
        if ((sh & (1u << 8)) && !(w & (1u << 8)))     /* PRST released       */
            reset_pending = dev_present ? 3 : 0;
        if (dev_present) w |= 1u;                     /* PCSTS sticks        */
        O(HPRT) = w;
    }
    if (reset_pending && core_fault != F_PENA_NEVER) {
        if (--reset_pending == 0) {
            O(HPRT) |= (1u << 2) | (1u << 3);         /* PENA + PENCHNG      */
            O(HPRT) = (O(HPRT) & ~(3u << 17))
                    | ((uint32_t)dev_speed << 17);
            dev_addr = 0;                             /* bus reset resets it */
        }
    }

    /* channels: CHENA rising edge -> serve; CHDIS+CHENA = halt request */
    for (int c = 0; c < 16; c++) {
        uint32_t hc = O(HCCHAR(c)), shc = SH_O(HCCHAR(c));
        if ((hc & (1u << 31)) && !(shc & (1u << 31))) {
            if (hc & (1u << 30)) {                    /* halt protocol       */
                O(HCCHAR(c)) &= ~((1u << 31) | (1u << 30));
                O(HCINT(c)) |= (1u << 1);             /* CHH                 */
            } else {
                O(HCCHAR(c)) &= ~(1u << 31);          /* core takes it       */
                serve_channel(c);
            }
        }
    }

    dwt += CYC_PER_STEP;
    memcpy(shadow_otg, otg, sizeof otg);
    memcpy(shadow_rcc, rcc, sizeof rcc);
}

/* ---- the access router --------------------------------------------------- */
volatile uint32_t *sim_reg32(uint32_t addr)
{
    step();
    if (addr == DWT_A)     return (volatile uint32_t *)&dwt;
    if (addr == 0xE0001000u) return (volatile uint32_t *)&dwt_ctrl;
    if (addr == 0xE000EDFCu) return (volatile uint32_t *)&demcr;
    if (addr == PWR_CR3_A) return (volatile uint32_t *)&pwr_cr3;
    if (addr == DBG_CR_A)  return (volatile uint32_t *)&dbg_cr;
    if (addr >= RCC_BASE && addr < RCC_BASE + 0x100)
        return (volatile uint32_t *)&R(addr - RCC_BASE);
    if (addr >= OTG_BASE && addr < OTG_BASE + 0x1000)
        return (volatile uint32_t *)&O(addr - OTG_BASE);
    misc = 0;
    return (volatile uint32_t *)&misc;   /* unmodeled: reads 0, writes void  */
}

uint32_t sim_read(uint32_t addr)
{
    if (addr >= OTG_BASE && addr < OTG_BASE + 0x1000)
        return O(addr - OTG_BASE);
    if (addr >= RCC_BASE && addr < RCC_BASE + 0x100)
        return R(addr - RCC_BASE);
    if (addr == PWR_CR3_A) return pwr_cr3;
    return 0;
}

/* ---- host kprintf -------------------------------------------------------- */
void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void kvprintf(const char *fmt, va_list ap) { vprintf(fmt, ap); }
