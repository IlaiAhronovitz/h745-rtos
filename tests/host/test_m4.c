/* test_m4.c — simulation of the QMI DATA-PLANE control layer (qmi.c) against
 * the emulated Qualcomm modem behind EP0. Real usbh.c + qmi.c, no stubs.
 *
 * Why this exists: QMI is where the MCU takes full control of the modem, and
 * every byte of it is a hand-built wire format. A single wrong offset — the
 * one-byte CTL transaction id, the length that excludes the marker — makes
 * every reply parse as garbage, and on the bench that looks like "the modem
 * is broken". These tests pin the format down before a single flash.
 *
 *   P  framing: CTL vs service headers, ALLOCATE_CID round trip
 *   Q  data format: raw IP set, read back, and the 802.3 rejection
 *   R  the call: START_NETWORK, GET_CURRENT_SETTINGS byte order, STOP
 *   S  adversity: slow replies, interleaved indications, error results,
 *      unsupported messages, an oversize request
 */
#include <stdio.h>
#include <string.h>
#include "shim/stm32h745.h"
#include "usbh.h"
#include "qmi.h"
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

static usbh_device_t dev;
static qmi_t q;

/* time hooks: simulated milliseconds, no real waiting anywhere */
static uint32_t u_ms;
static uint32_t u_now(void) { return u_ms; }
static void u_sleep(uint32_t ms) { u_ms += ms ? ms : 1; }

/* Bring the emulated EC25 up to "configured", which is all QMI needs: its
 * control channel is EP0, not a bulk pipe. */
static void bring_up(void)
{
    sim_reset();
    sim_attach_fs();
    u_ms = 0;
    qmi_set_clock(u_now, u_sleep);
    usbh_init();
    usbh_wait_connect(1000);
    usbh_enumerate(&dev);
    usbh_set_configuration(&dev);
}

/* ---- P: framing + client ids --------------------------------------------- */
static void test_framing(void)
{
    printf("P: QMUX framing and client ids\n");
    bring_up();

    CHECK_EQ(qmi_open(&q, &dev), 0, "qmi_open finds the QMI interface");
    CHECK_EQ(q.iface, 4, "QMI is interface 4, not the 2-endpoint DIAG");
    CHECK_EQ(sim_line_state_iface(), 4, "DTR asserted on the QMI interface");

    /* CTL uses a ONE-byte transaction id: if qmi.c used two, the model would
     * see a message id shifted by a byte and reject the frame. */
    CHECK_EQ(qmi_alloc_cid(&q, QMI_SVC_WDA, &q.cid_wda), 0, "WDA cid");
    CHECK_EQ(qmi_alloc_cid(&q, QMI_SVC_WDS, &q.cid_wds), 0, "WDS cid");
    CHECK(q.cid_wda != 0 && q.cid_wds != 0, "cids are non-zero");
    CHECK(q.cid_wda != q.cid_wds, "each service gets its own cid");
    CHECK_EQ(sim_qmi_bad_frames(), 0, "no malformed frame reached the modem");
    CHECK_EQ(sim_qmi_requests(), 2, "exactly two requests were sent");

    /* Transaction ids must advance and never repeat -- and CTL keeps its own
     * counter, because its field is one byte wide while a service's is two.
     * A single shared counter rewound to fit CTL would eventually hand a
     * service request a transaction id it had just used. */
    uint8_t  ctl_before = q.txn_ctl;
    uint16_t svc_before = q.txn;
    uint8_t spare;
    qmi_alloc_cid(&q, QMI_SVC_WDS, &spare);              /* a CTL message */
    CHECK(q.txn_ctl != ctl_before, "CTL transaction id advances");
    CHECK_EQ(q.txn, svc_before, "and leaves the service counter alone");

    /* a real service message must move the other counter, and only it */
    ctl_before = q.txn_ctl;
    uint8_t resp[64];
    qmi_request(&q, QMI_SVC_WDA, q.cid_wda, QMI_WDA_GET_DATA_FORMAT,
                0, 0, resp, sizeof resp, 1000);
    CHECK(q.txn != svc_before, "service transaction id advances");
    CHECK_EQ(q.txn_ctl, ctl_before, "and leaves the CTL counter alone");
}

/* ---- Q: link-layer format ------------------------------------------------- */
static void test_data_format(void)
{
    printf("Q: data format negotiation\n");
    bring_up();
    qmi_open(&q, &dev);
    qmi_alloc_cid(&q, QMI_SVC_WDA, &q.cid_wda);

    CHECK_EQ(sim_qmi_link_layer(), 1, "the modem starts in 802.3 mode");
    CHECK_EQ(qmi_set_data_format_raw_ip(&q), 0, "set raw IP succeeds");
    CHECK_EQ(sim_qmi_link_layer(), 2, "the modem really switched to raw IP");
    CHECK_EQ(q.link_layer, 2, "the readback agrees");
    CHECK_EQ(q.ul_agg, 0, "uplink aggregation is off");
    CHECK_EQ(q.dl_agg, 0, "downlink aggregation is off -- no QMAP header");
    CHECK_EQ(sim_qmi_bad_frames(), 0, "no malformed frames");
}

/* ---- R: the data call ----------------------------------------------------- */
static void test_call(void)
{
    printf("R: start, settings, stop\n");
    bring_up();
    qmi_open(&q, &dev);
    qmi_alloc_cid(&q, QMI_SVC_WDA, &q.cid_wda);
    qmi_alloc_cid(&q, QMI_SVC_WDS, &q.cid_wds);
    qmi_set_data_format_raw_ip(&q);

    /* settings before the call must fail, not hand back a stale address */
    CHECK(qmi_get_settings(&q) != 0, "no settings before the call is up");

    CHECK_EQ(qmi_start_network(&q, "internet"), 0, "START_NETWORK");
    CHECK_EQ(sim_qmi_net_up(), 1, "the model brought the bearer up");
    CHECK_EQ(q.pkt_handle, 0x12345678u, "packet handle came back intact");

    CHECK_EQ(qmi_get_settings(&q), 0, "GET_CURRENT_SETTINGS");
    /* The model returns 0x0A141EB0 as a host-order u32, so the wire carries
     * B0 1E 14 0A and the address is 10.20.30.176 (a placeholder for the
     * bench address). Byte order is the classic QMI trap. */
    CHECK_EQ(q.ip[0], 10,  "ip[0]");
    CHECK_EQ(q.ip[1], 20,  "ip[1]");
    CHECK_EQ(q.ip[2], 30,  "ip[2]");
    CHECK_EQ(q.ip[3], 176, "ip[3] -- little-endian u32, not big");
    CHECK_EQ(q.gw[3], 177, "gateway is the next address");
    CHECK_EQ(q.mask[0], 255, "netmask");
    CHECK_EQ(q.dns[0], 10, "dns");
    CHECK_EQ(q.mtu, 1500, "mtu");

    /* a second START on an already-up call reports NO_EFFECT, which is not
     * an error for us: the bearer we wanted is there */
    CHECK_EQ(qmi_start_network(&q, "internet"), 0, "second START is benign");

    CHECK_EQ(qmi_stop_network(&q), 0, "STOP_NETWORK");
    CHECK_EQ(sim_qmi_net_up(), 0, "the bearer is really down");
    CHECK_EQ(q.pkt_handle, 0u, "the handle was forgotten");
    CHECK_EQ(qmi_stop_network(&q), 0, "stopping twice is a no-op");
}

/* ---- R2: BIND_MUX_DATA_PORT byte layout ----------------------------------
 * The mux id is the VALUE of TLV 0x11, at offset 14 of the request. Writing
 * it one byte later lands on the TYPE byte of the following client-type TLV:
 * the mux id then silently stays 0, the client-type TLV becomes a TLV of
 * some other type, and the module accepts the whole thing without complaint.
 * That is exactly the bug this pins -- it cost two bench runs whose result
 * ("binding breaks traffic") was measuring a malformed message. */
static void test_bind_mux_layout(void)
{
    printf("R2: BIND_MUX_DATA_PORT byte layout\n");
    bring_up();
    qmi_open(&q, &dev);
    qmi_alloc_cid(&q, QMI_SVC_WDS, &q.cid_wds);

    CHECK_EQ(qmi_bind_mux(&q, 0x81, 1), 0, "bind with QMAP mux 0x81");
    CHECK_EQ(sim_qmi_bind_mux(), 0x81, "the module received mux id 0x81");
    CHECK_EQ(sim_qmi_bind_client(), 1, "client type survived as 1 = tethered");
    CHECK_EQ(sim_qmi_bind_iface(), 4, "endpoint info carried interface 4");

    CHECK_EQ(qmi_bind_mux(&q, 0x81, 0), 0, "bind unbound (no QMAP)");
    CHECK_EQ(sim_qmi_bind_mux(), 0x00,
             "unbound is mux 0x00, libqmi's convention -- not 0xFF");
    CHECK_EQ(sim_qmi_bind_client(), 1, "client type still intact");
    CHECK_EQ(sim_qmi_bad_frames(), 0, "no malformed frame reached the modem");
}

/* ---- S: adversity --------------------------------------------------------- */
static void test_adversity(void)
{
    printf("S: slow replies, indications, errors\n");

    /* 1. the modem takes its time: 12 polls return an empty frame first */
    bring_up();
    qmi_open(&q, &dev);
    sim_qmi_delay(12);
    CHECK_EQ(qmi_alloc_cid(&q, QMI_SVC_WDA, &q.cid_wda), 0,
             "a slow reply is still collected");
    CHECK(u_ms > 0, "the poll loop slept instead of spinning");

    /* 1b. THE aliasing trap: a frame carrying our own transaction id and our
     *     own message id, but belonging to a different service. Message ids
     *     are not unique across services -- CTL ALLOCATE_CID and WDA
     *     GET_DATA_FORMAT are both 0x0022 -- and this driver issues both. A
     *     client matching on msg+txn alone eats the decoy and reports its
     *     TLVs as the answer; it also has the other service's header size,
     *     so every field is read one byte out. */
    bring_up();
    qmi_open(&q, &dev);
    sim_qmi_alias(1);
    CHECK_EQ(qmi_alloc_cid(&q, QMI_SVC_WDA, &q.cid_wda), 0,
             "a same-msg different-service decoy does not steal the reply");
    CHECK(q.cid_wda != 0, "and the real client id still came through");
    sim_qmi_alias(0);

    /* 2. an unsolicited indication arrives ahead of every answer. Matching
     *    on message id + transaction id is what keeps it from being eaten. */
    bring_up();
    qmi_open(&q, &dev);
    sim_qmi_indication(1);
    CHECK_EQ(qmi_alloc_cid(&q, QMI_SVC_WDS, &q.cid_wds), 0,
             "an interleaved indication does not steal the reply");
    CHECK_EQ(qmi_alloc_cid(&q, QMI_SVC_WDA, &q.cid_wda), 0,
             "and the next request still works");
    sim_qmi_indication(0);

    /* 3. a real QMI error result: START without a bearer profile the model
     *    accepts. An empty APN means the mandatory TLV is missing. */
    bring_up();
    qmi_open(&q, &dev);
    qmi_alloc_cid(&q, QMI_SVC_WDS, &q.cid_wds);
    CHECK(qmi_start_network(&q, "") != 0, "a missing APN fails, loudly");
    CHECK_EQ(q.last_err, 0x0011, "the QMI error code is surfaced");
    CHECK_EQ(sim_qmi_net_up(), 0, "no bearer was left half-up");

    /* 4. an unsupported message must fail rather than hang or half-parse */
    uint8_t resp[16];
    CHECK(qmi_request(&q, QMI_SVC_WDS, q.cid_wds, 0x7FFF, 0, 0,
                      resp, sizeof resp, 1000) < 0, "unsupported message");
    CHECK_EQ(q.last_err, 0x0002, "reported as UNSUPPORTED");

    /* 5. a request larger than the EP0 bounce buffer is refused up front,
     *    not truncated onto the wire */
    static uint8_t huge[600];
    CHECK(qmi_request(&q, QMI_SVC_WDS, q.cid_wds, 0x0020, huge, sizeof huge,
                      resp, sizeof resp, 1000) < 0, "oversize request refused");

    /* 6. and nothing malformed ever reached the modem in any of the above */
    CHECK_EQ(sim_qmi_bad_frames(), 0, "no malformed frames across adversity");

    /* 7. a timeout is a timeout: with the queue permanently starved the
     *    client gives up instead of blocking forever */
    bring_up();
    qmi_open(&q, &dev);
    sim_qmi_delay(100000);
    uint32_t t0 = u_ms;
    CHECK(qmi_alloc_cid(&q, QMI_SVC_WDA, &q.cid_wda) < 0, "starved -> error");
    CHECK(u_ms - t0 >= 3000u, "it waited the full timeout first");
}

int main(void)
{
    test_framing();
    test_data_format();
    test_call();
    test_bind_mux_layout();
    test_adversity();
    printf("\n==== %d passed, %d failed ====\n", n_pass, n_fail);
    return n_fail ? 1 : 0;
}
