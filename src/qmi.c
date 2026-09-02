/* qmi.c — QMI client over EP0 encapsulated control transfers. See qmi.h. */
#include "qmi.h"
#include "libmin.h"
#include "kprintf.h"

#define QMUX_MARKER   0x01u
#define QMI_BUF_MAX   512u          /* == the driver's EP0 bounce buffer */
#define QMI_POLL_MS   20u

static uint8_t txbuf[QMI_BUF_MAX];
static uint8_t rxbuf[QMI_BUF_MAX];

static uint32_t (*now_ms)(void);
static void     (*sleep_ms)(uint32_t);

void qmi_set_clock(uint32_t (*now)(void), void (*slp)(uint32_t))
{
    now_ms = now;
    sleep_ms = slp;
}

static void wr16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t rd16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* QMI hands out IPv4 addresses as a u32 in host order; on the wire that is
 * little-endian, so a.b.c.d arrives as d,c,b,a. */
static void rd_ipv4(const uint8_t *v, uint8_t out[4])
{
    out[0] = v[3]; out[1] = v[2]; out[2] = v[1]; out[3] = v[0];
}

/* The QMI protocol error codes worth naming on a serial log. Everything else
 * prints as a number -- see libqmi's qmi-errors.h for the full list. */
static const char *qmi_err_name(uint16_t e)
{
    switch (e) {
    case 0x0001: return "MALFORMED_MSG";
    case 0x0002: return "NO_MEMORY";
    case 0x0003: return "INTERNAL";
    case 0x0005: return "CLIENT_IDS_EXHAUSTED";
    case 0x0007: return "INVALID_CLIENT_ID";
    case 0x0009: return "INVALID_HANDLE";
    case 0x000A: return "INVALID_PROFILE";
    case 0x000D: return "NO_NETWORK_FOUND";
    case 0x000E: return "CALL_FAILED";
    case 0x000F: return "OUT_OF_CALL";
    case 0x0010: return "NOT_PROVISIONED";
    case 0x0011: return "MISSING_ARG";
    case 0x0013: return "ARG_TOO_LONG";
    case 0x0016: return "INVALID_TX_ID";
    case 0x0019: return "OP_DEVICE_UNSUPPORTED";
    case 0x001A: return "NO_EFFECT";
    case 0x003D: return "INVALID_IP_FAMILY";
    default:     return "?";
    }
}

/* Dump a frame the way a protocol analyser would. Costs a few hundred bytes
 * of flash and settles arguments that otherwise cost a bench cycle each. */
static void qmi_dump(const char *tag, const uint8_t *p, uint16_t n)
{
    if (n > 64u) n = 64u;
    kprintf("qmi: %s [%u]", tag, n);
    for (uint16_t i = 0; i < n; i++) {
        if ((i & 15u) == 0u) kprintf("\n     ");
        kprintf("%02x ", p[i]);
    }
    kprintf("\n");
}

int qmi_find_tlv(const uint8_t *resp, uint16_t len, uint8_t type,
                 const uint8_t **val)
{
    uint16_t o = 0;
    while ((uint16_t)(o + 3u) <= len) {
        uint8_t  t = resp[o];
        uint16_t l = rd16le(resp + o + 1);
        if ((uint16_t)(o + 3u + l) > len) break;
        if (t == type) {
            if (val) *val = resp + o + 3;
            return (int)l;
        }
        o = (uint16_t)(o + 3u + l);
    }
    return -1;
}

int qmi_open(qmi_t *q, usbh_device_t *dev)
{
    usbh_iface_t qi;

    memset(q, 0, sizeof *q);
    q->dev = dev;

    /* The QMI interface is ff/ff/ff with 3 endpoints. On the RM500Q the DIAG
     * interface is ff/ff/30, so QMI is the only ff/ff/ff and sorts at index 0;
     * on compositions where DIAG is also ff/ff/ff, QMI is the one that owns
     * an interrupt endpoint. Try the 3-endpoint match, then fall back. */
    if (usbh_find_interface(dev, 0xFF, 0xFF, 0xFF, 1, &qi) != USBH_OK
        || !qi.ep_int || !qi.ep_in || !qi.ep_out) {
        if (usbh_find_interface(dev, 0xFF, 0xFF, 0xFF, 0, &qi) != USBH_OK) {
            kprintf("qmi: no ff/ff/ff interface found\n");
            return -1;
        }
    }
    q->iface = qi.intf[2];

    /* 2c7c:0800 carries the kernel's QMI_WWAN_QUIRK_DTR: it ignores QMI
     * requests until DTR is asserted on the interface. */
    usbh_set_line_state(dev, q->iface, 1, 1);

    /* endpoint descriptors: byte 2 is bEndpointAddress */
    kprintf("qmi: interface %u (ctrl EP0, data %02x/%02x, int %02x)\n",
            q->iface,
            qi.ep_in  ? qi.ep_in[2]  : 0,
            qi.ep_out ? qi.ep_out[2] : 0,
            qi.ep_int ? qi.ep_int[2] : 0);
    return 0;
}

int qmi_request(qmi_t *q, uint8_t service, uint8_t cid, uint16_t msg_id,
                const uint8_t *req_tlvs, uint16_t req_len,
                uint8_t *resp, uint16_t resp_cap, uint32_t timeout_ms)
{
    /* ---- build the frame ------------------------------------------------
     * CTL uses a 1-byte transaction id and 6-byte QMI header; every other
     * service uses 2 bytes and 7. Getting this wrong shifts the message id
     * by one byte and every reply parses as garbage. */
    int      ctl     = (service == QMI_SVC_CTL);
    uint16_t qmi_hdr = ctl ? 6u : 7u;
    uint16_t total   = (uint16_t)(6u + qmi_hdr + req_len);

    q->last_err = 0;
    if (total > QMI_BUF_MAX) return -1;

    /* CTL's transaction id is one byte, a service's is two. Rewinding a
     * single shared counter to stay inside CTL's range would hand a service
     * request a transaction id it had just used, so each keeps its own. */
    uint16_t txn;
    if (ctl) { if (++q->txn_ctl == 0) q->txn_ctl = 1; txn = q->txn_ctl; }
    else     { if (++q->txn == 0)     q->txn = 1;     txn = q->txn; }

    uint8_t *p = txbuf;
    p[0] = QMUX_MARKER;
    wr16le(p + 1, (uint16_t)(total - 1u));    /* length excludes the marker */
    p[3] = 0x00;                              /* flags: host -> device      */
    p[4] = service;
    p[5] = cid;

    uint8_t *h = p + 6;
    h[0] = 0x00;                              /* request                    */
    if (ctl) {
        h[1] = (uint8_t)txn;
        wr16le(h + 2, msg_id);
        wr16le(h + 4, req_len);
    } else {
        wr16le(h + 1, txn);
        wr16le(h + 3, msg_id);
        wr16le(h + 5, req_len);
    }
    if (req_len) memcpy(p + 6 + qmi_hdr, req_tlvs, req_len);

    /* ---- send: SEND_ENCAPSULATED_COMMAND (class OUT to the interface) --- */
    uint8_t setup[8] = { 0x21, 0x00, 0, 0, q->iface, 0,
                         (uint8_t)total, (uint8_t)(total >> 8) };
    int r = usbh_control(q->dev, setup, txbuf);
    if (r < 0) {
        kprintf("qmi: send svc %02x msg %04x failed (%d)\n",
                service, msg_id, r);
        return r;
    }

    /* ---- receive: poll GET_ENCAPSULATED_RESPONSE ------------------------
     * The interrupt endpoint would tell us when a response is ready; polling
     * this control request needs no interrupt pipe, and the module answers
     * with a zero-length frame while it has nothing to say.
     *
     * Indications share this pipe, so accept a frame ONLY when its message
     * id and transaction id are the ones we just sent. */
    uint32_t t0 = now_ms();
    do {
        uint8_t g[8] = { 0xA1, 0x01, 0, 0, q->iface, 0,
                         (uint8_t)QMI_BUF_MAX, (uint8_t)(QMI_BUF_MAX >> 8) };
        memset(rxbuf, 0, 16);
        r = usbh_control(q->dev, g, rxbuf);

        if (r >= (int)(6u + 6u) && rxbuf[0] == QMUX_MARKER) {
            /* Parse using the header size implied by the RECEIVED service,
             * never the one we happen to be waiting on: a CTL frame read
             * with the 7-byte service layout puts the message id one byte
             * out and can alias anything.
             *
             * Message ids are NOT unique across services -- CTL
             * ALLOCATE_CID and WDA GET_DATA_FORMAT are both 0x0022, and this
             * driver issues both -- so msg+txn alone is not an identity.
             * Demand the full envelope: direction, service, client id and
             * the response (not indication) flag. */
            uint8_t  rsvc     = rxbuf[4];
            uint8_t  rcid     = rxbuf[5];
            int      rctl     = (rsvc == QMI_SVC_CTL);
            uint16_t rhdr     = rctl ? 6u : 7u;

            if (r < (int)(6u + rhdr)) { sleep_ms(QMI_POLL_MS); continue; }

            const uint8_t *rh   = rxbuf + 6;
            uint8_t  rflags     = rh[0];
            uint16_t rtxn       = rctl ? rh[1] : rd16le(rh + 1);
            uint16_t rmsg       = rctl ? rd16le(rh + 2) : rd16le(rh + 3);
            uint16_t tlv_len    = rctl ? rd16le(rh + 4) : rd16le(rh + 5);
            const uint8_t *tlvs = rxbuf + 6 + rhdr;

            if ((uint32_t)(6u + rhdr + tlv_len) > (uint32_t)r)
                tlv_len = (uint16_t)((uint32_t)r - 6u - rhdr);

            /* response flags: 0x01 for CTL, 0x02 for a service. The
             * indication values (0x02 / 0x04) must never match here. */
            uint8_t want_flags = rctl ? 0x01u : 0x02u;

            if (rxbuf[3] != 0x80u || rsvc != service || rcid != cid
                || rflags != want_flags || rmsg != msg_id || rtxn != txn) {
                /* an indication, another client's reply, or a stale one */
                sleep_ms(QMI_POLL_MS);
                continue;
            }

            /* The result TLV is mandatory in a response. Treating its
             * absence as success would let a malformed frame through as an
             * empty, "successful" answer. */
            const uint8_t *res = 0;
            if (qmi_find_tlv(tlvs, tlv_len, QMI_RESULT_TLV, &res) != 4) {
                kprintf("qmi: svc %02x msg %04x reply has no result TLV\n",
                        service, msg_id);
                if (q->debug) qmi_dump("got ", rxbuf, (uint16_t)r);
                return -1;
            }
            {
                uint16_t status = rd16le(res);
                q->last_err     = rd16le(res + 2);
                if (status != 0) {
                    kprintf("qmi: svc %02x msg %04x -> error %u (%s)\n",
                            service, msg_id, q->last_err,
                            qmi_err_name(q->last_err));
                    if (q->debug) {
                        qmi_dump("sent", txbuf, total);
                        qmi_dump("got ", rxbuf, (uint16_t)r);
                    }
                    return -1;
                }
            }
            uint16_t n = tlv_len;
            if (n > resp_cap) n = resp_cap;
            if (resp && n) memcpy(resp, tlvs, n);
            return (int)n;
        }
        sleep_ms(QMI_POLL_MS);
    } while (now_ms() - t0 < timeout_ms);

    kprintf("qmi: no response to svc %02x msg %04x (%lu ms)\n",
            service, msg_id, (unsigned long)timeout_ms);
    return -1;
}

int qmi_alloc_cid(qmi_t *q, uint8_t service, uint8_t *out)
{
    /* CTL 0x0022 ALLOCATE_CID, TLV 0x01 = the service to allocate for */
    uint8_t req[4] = { 0x01, 0x01, 0x00, service };
    uint8_t resp[64];

    int n = qmi_request(q, QMI_SVC_CTL, 0x00, QMI_CTL_ALLOCATE_CID,
                        req, sizeof req, resp, sizeof resp, 3000);
    if (n < 0) return -1;

    /* the reply's TLV 0x01 is {service, cid} */
    const uint8_t *v = 0;
    if (qmi_find_tlv(resp, (uint16_t)n, 0x01, &v) >= 2) {
        *out = v[1];
        kprintf("qmi: service %02x -> client id %u\n", service, *out);
        return 0;
    }
    return -1;
}

int qmi_release_cid(qmi_t *q, uint8_t service, uint8_t cid)
{
    /* CTL 0x0023 RELEASE_CID, TLV 0x01 = {service, cid}.
     *
     * Not optional. Client ids are module-side state that survives a HOST
     * reset: every flash-and-run of this app leaked two, and after a handful
     * of runs the module's CTL service stopped answering ALLOCATE_CID
     * altogether -- no error, no response. The only recovery then is a modem
     * reboot. Release is the fix, and it belongs on every exit path. */
    if (!cid) return 0;
    uint8_t req[5] = { 0x01, 0x02, 0x00, service, cid };
    uint8_t resp[32];
    int n = qmi_request(q, QMI_SVC_CTL, 0x00, QMI_CTL_RELEASE_CID,
                        req, sizeof req, resp, sizeof resp, 3000);
    kprintf("qmi: released service %02x client %u (%s)\n", service, cid,
            n < 0 ? "refused" : "ok");
    return n < 0 ? -1 : 0;
}

void qmi_close(qmi_t *q)
{
    if (q->cid_wds) { qmi_release_cid(q, QMI_SVC_WDS, q->cid_wds); q->cid_wds = 0; }
    if (q->cid_wda) { qmi_release_cid(q, QMI_SVC_WDA, q->cid_wda); q->cid_wda = 0; }
}

int qmi_set_data_format(qmi_t *q, int qmap)
{
    /* WDA 0x0020 SET_DATA_FORMAT, QMAP variant.
     *   0x10 QoS header      : 0 = absent
     *   0x11 link layer      : 2 = raw IP
     *   0x12 uplink   agg    : 5 = QMAP
     *   0x13 downlink agg    : 5 = QMAP
     *   0x15 DL max datagrams: 1  -- QMAP framing WITHOUT real aggregation,
     *   0x16 DL max size     : 1520  so one transfer is still one packet
     *                                and a 1600-byte buffer always suffices
     * Keeping the header but refusing multi-datagram batches is what makes
     * QMAP safe for a host with fixed 1600-byte pipe buffers. */
    if (!qmap) return qmi_set_data_format_raw_ip(q);

    uint8_t req[] = {
        0x10, 0x01, 0x00, 0x00,
        0x11, 0x04, 0x00, 0x02, 0x00, 0x00, 0x00,
        0x12, 0x04, 0x00, 0x05, 0x00, 0x00, 0x00,
        0x13, 0x04, 0x00, 0x05, 0x00, 0x00, 0x00,
        0x15, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x16, 0x04, 0x00, 0xF0, 0x05, 0x00, 0x00,
    };
    uint8_t resp[64];
    int n = qmi_request(q, QMI_SVC_WDA, q->cid_wda, QMI_WDA_SET_DATA_FORMAT,
                        req, sizeof req, resp, sizeof resp, 5000);
    if (n < 0) {
        kprintf("qmi: QMAP data format refused\n");
        return -1;
    }
    q->link_layer = 2u;
    q->ul_agg = q->dl_agg = 5u;
    q->format_verified = 0;
    kprintf("qmi: data format = raw IP inside QMAP (1 datagram per transfer)\n");
    return 0;
}

int qmi_set_data_format_raw_ip(qmi_t *q)
{
    /* WDA 0x0020 SET_DATA_FORMAT.
     *   TLV 0x10 QoS header present  : 0 = no
     *   TLV 0x11 link layer protocol : 1 = 802.3, 2 = raw IP
     *   TLV 0x12 uplink aggregation  : 0 = disabled ... 5 = QMAP
     *   TLV 0x13 downlink aggregation: same enum
     *
     * Raw IP with aggregation OFF is the simplest thing that can work: each
     * bulk transfer is then exactly one bare IP packet, no QMAP header to
     * parse and no multi-datagram splitting. Some firmware rejects the
     * aggregation TLVs, so fall back to setting only the link layer. */
    static const uint8_t full[] = {
        0x10, 0x01, 0x00, 0x00,                    /* QoS header: absent   */
        0x11, 0x04, 0x00, 0x02, 0x00, 0x00, 0x00,  /* link layer: raw IP   */
        0x12, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,  /* UL aggregation: off  */
        0x13, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,  /* DL aggregation: off  */
    };
    static const uint8_t minimal[] = {
        0x10, 0x01, 0x00, 0x00,
        0x11, 0x04, 0x00, 0x02, 0x00, 0x00, 0x00,
    };
    uint8_t resp[64];

    int n = qmi_request(q, QMI_SVC_WDA, q->cid_wda, QMI_WDA_SET_DATA_FORMAT,
                        full, sizeof full, resp, sizeof resp, 5000);
    if (n < 0) {
        kprintf("qmi: aggregation TLVs refused, setting link layer only\n");
        n = qmi_request(q, QMI_SVC_WDA, q->cid_wda, QMI_WDA_SET_DATA_FORMAT,
                        minimal, sizeof minimal, resp, sizeof resp, 5000);
        if (n < 0) return -1;
    }
    return qmi_get_data_format(q);
}

int qmi_get_data_format(qmi_t *q)
{
    /* WDA 0x0022 GET_DATA_FORMAT — ask the module what it settled on rather
     * than assuming the SET took. Two shapes, because this RM500Q firmware
     * answers INTERNAL (error 3) to the bare form that libqmi sends:
     *   a) with the endpoint-info TLV, which a multi-endpoint SDX55 needs.
     *      Its id is 0x11 in the GET message and 0x17 in the SET -- the SET
     *      needs a higher id only because 0x11-0x16 are taken by link_prot,
     *      ul/dl aggregation, ndp_signature and the DL limits. Sending the
     *      SET's 0x17 here is what earned MISSING_ARG on the bench: the
     *      module ignored the unknown TLV, then faulted on the absent ep_id;
     *   b) bare, no TLVs, the generic form.
     *
     * This is a READBACK, not a control action: if the module refuses to
     * answer, that is not a reason to abandon a data call it already accepted
     * the settings for. Failing here once cost a whole flash cycle. */
    uint8_t ep[11] = {
        0x11, 0x08, 0x00,                /* GET's ep_id is 0x11, not 0x17: */
        0x02, 0x00, 0x00, 0x00,          /* endpoint type 2 = HSUSB        */
        0x00, 0x00, 0x00, 0x00,          /* interface number, filled below */
    };
    ep[7] = q->iface;

    uint8_t resp[128];
    int n = qmi_request(q, QMI_SVC_WDA, q->cid_wda, QMI_WDA_GET_DATA_FORMAT,
                        ep, sizeof ep, resp, sizeof resp, 5000);
    if (n < 0)
        n = qmi_request(q, QMI_SVC_WDA, q->cid_wda, QMI_WDA_GET_DATA_FORMAT,
                        0, 0, resp, sizeof resp, 5000);
    if (n < 0) {
        /* No readback available. Trust the SET that already returned success,
         * and say so plainly so the log never implies we verified it. */
        q->format_verified = 0;
        q->link_layer = 2u;
        q->ul_agg = q->dl_agg = 0u;
        kprintf("qmi: GET_DATA_FORMAT refused -- assuming the SET took "
                "(raw IP, no aggregation), UNVERIFIED\n");
        return 0;
    }

    const uint8_t *v = 0;
    if (qmi_find_tlv(resp, (uint16_t)n, 0x11, &v) == 4) q->link_layer = v[0];
    if (qmi_find_tlv(resp, (uint16_t)n, 0x12, &v) == 4) q->ul_agg = v[0];
    if (qmi_find_tlv(resp, (uint16_t)n, 0x13, &v) == 4) q->dl_agg = v[0];
    q->format_verified = 1;

    kprintf("qmi: data format link=%s ul_agg=%u dl_agg=%u (verified)\n",
            q->link_layer == 2u ? "raw-ip"
                                : (q->link_layer == 1u ? "802.3" : "?"),
            q->ul_agg, q->dl_agg);

    if (q->link_layer != 2u) {
        kprintf("qmi: link layer is not raw IP -- the data path will not work\n");
        return -1;
    }
    return 0;
}

int qmi_bind_mux(qmi_t *q, uint8_t mux_id, int qmap)
{
    /* WDS 0x00A2 BIND_MUX_DATA_PORT.
     *   TLV 0x10 endpoint info {endpoint_type u32, interface_number u32}
     *   TLV 0x11 mux id (u8)
     *   TLV 0x13 client type (u32, 1 = tethered)
     *
     * This is the step whose absence produced the bench's defining symptom:
     * the bearer came up with a real address, uplink packets were accepted
     * by the module, and an operator-side router answered every destination
     * with "destination unreachable / host unreachable". Without a mux bind
     * the WDS call is not associated with this USB endpoint, so the module
     * has a call and a data pipe that know nothing about each other. */
    uint8_t req[] = {
    /*  [0]   [1]   [2]                                                    */
        0x10, 0x08, 0x00,            /* TLV 0x10, len 8: endpoint info     */
    /*  [3]   [4]   [5]   [6]                                              */
        0x02, 0x00, 0x00, 0x00,      /*   endpoint type u32, 2 = HSUSB     */
    /*  [7]   [8]   [9]   [10]                                             */
        0x00, 0x00, 0x00, 0x00,      /*   interface number u32, set below  */
    /*  [11]  [12]  [13]  [14]                                             */
        0x11, 0x01, 0x00, 0x00,      /* TLV 0x11, len 1: mux id, set below */
    /*  [15]  [16]  [17]                                                   */
        0x13, 0x04, 0x00,            /* TLV 0x13, len 4: client type       */
    /*  [18]  [19]  [20]  [21]                                             */
        0x01, 0x00, 0x00, 0x00,      /*   1 = tethered                     */
    };
    req[7]  = q->iface;
    /* The mux id is the TLV's VALUE at [14], not [15] -- [15] is the next
     * TLV's type byte. Writing it there sent mux 0x00 every time while
     * turning the client-type TLV into garbage, so no valid bind was ever
     * transmitted and the "binding breaks traffic" result was measuring a
     * malformed message. The offsets are spelled out above for that reason. */
    req[14] = qmap ? mux_id : 0x00u; /* 0x00 = unbound (libqmi's convention,
                                      * valid mux ids are 0x01..0xFE)      */

    uint8_t resp[64];
    int n = qmi_request(q, QMI_SVC_WDS, q->cid_wds, QMI_WDS_BIND_MUX_DATA_PORT,
                        req, sizeof req, resp, sizeof resp, 5000);
    if (n < 0) {
        kprintf("qmi: BIND_MUX_DATA_PORT refused (iface %u mux %02x)\n",
                q->iface, req[14]);
        return -1;
    }
    q->mux_id = req[14];
    kprintf("qmi: data port bound (iface %u, mux %02x, tethered)\n",
            q->iface, req[15]);
    return 0;
}

int qmi_start_network(qmi_t *q, const char *apn)
{
    /* WDS 0x0020 START_NETWORK_INTERFACE.
     *   TLV 0x14 APN name (no NUL)
     *   TLV 0x19 IP family preference: 4 = IPv4
     * Naming the APN explicitly beats relying on a stored profile: the
     * profile the module boots with is not always the one we want, and a
     * mismatch surfaces as a bare "call failed" with no detail. */
    uint8_t  req[80];
    uint16_t n = 0;
    uint16_t alen = (uint16_t)strlen(apn);

    if (alen > 48u) return -1;
    if (alen) {
        req[n++] = 0x14;
        wr16le(req + n, alen); n = (uint16_t)(n + 2u);
        memcpy(req + n, apn, alen); n = (uint16_t)(n + alen);
    }
    req[n++] = 0x19;
    wr16le(req + n, 1); n = (uint16_t)(n + 2u);
    req[n++] = 0x04;                               /* IPv4 */

    /* TLV 0x31, the 3GPP profile index. Naming the APN alone leaves the
     * module free to attach the data port to whichever PDN it likes -- and
     * this SIM has an "ims" context active alongside the internet APN. Traffic
     * placed on the IMS PDN reaches the operator core and is then refused
     * with host-unreachable for every internet destination, which is exactly
     * what the bench saw. Pinning the profile removes the choice. */
    if (q->profile) {
        req[n++] = 0x31;
        wr16le(req + n, 1); n = (uint16_t)(n + 2u);
        req[n++] = q->profile;
    }

    uint8_t resp[128];
    /* attach + bearer setup is a network round trip: give it real time */
    int r = qmi_request(q, QMI_SVC_WDS, q->cid_wds, QMI_WDS_START_NETWORK,
                        req, n, resp, sizeof resp, 60000);
    if (r < 0) {
        /* Do NOT decide from the error code alone which failures are benign:
         * "the call is already up" and "the call failed" are neighbouring
         * numbers, and firmwares disagree about which they send. Ask the
         * module for the bearer's address instead — an answer with a real
         * IPv4 address is proof the bearer exists, whatever the code said. */
        uint16_t why = q->last_err;      /* the probe below overwrites it */
        kprintf("qmi: START_NETWORK reported error %u (%s) -- checking "
                "whether a bearer exists anyway\n", why, qmi_err_name(why));
        if (qmi_get_settings(q) == 0) {
            q->last_err = why;
            kprintf("qmi: a bearer is already up; continuing\n");
            return 0;
        }
        q->last_err = why;               /* report why START failed, not why
                                          * the probe did */
        return -1;
    }

    const uint8_t *v = 0;
    if (qmi_find_tlv(resp, (uint16_t)r, 0x01, &v) == 4) {
        q->pkt_handle = (uint32_t)v[0] | ((uint32_t)v[1] << 8)
                      | ((uint32_t)v[2] << 16) | ((uint32_t)v[3] << 24);
    }
    kprintf("qmi: network started on \"%s\", packet handle %08x\n",
            apn, (unsigned)q->pkt_handle);
    return 0;
}

int qmi_stop_network(qmi_t *q)
{
    if (!q->pkt_handle) return 0;
    uint8_t req[7] = { 0x01, 0x04, 0x00,
                       (uint8_t)q->pkt_handle,
                       (uint8_t)(q->pkt_handle >> 8),
                       (uint8_t)(q->pkt_handle >> 16),
                       (uint8_t)(q->pkt_handle >> 24) };
    int r = qmi_request(q, QMI_SVC_WDS, q->cid_wds, QMI_WDS_STOP_NETWORK,
                        req, sizeof req, 0, 0, 10000);
    q->pkt_handle = 0;
    kprintf("qmi: network stopped (rc %d)\n", r);
    return r < 0 ? -1 : 0;
}

int qmi_get_settings(qmi_t *q)
{
    /* WDS 0x002D GET_CURRENT_SETTINGS.
     * TLV 0x10 is a bitmask of which settings to return; asking for all of
     * them and taking whatever comes back is more robust than guessing the
     * exact mask this firmware honours. */
    uint8_t req[7] = { 0x10, 0x04, 0x00, 0xFF, 0xFF, 0xFF, 0xFF };
    uint8_t resp[256];

    int n = qmi_request(q, QMI_SVC_WDS, q->cid_wds,
                        QMI_WDS_GET_CURRENT_SETTINGS,
                        req, sizeof req, resp, sizeof resp, 10000);
    if (n < 0) return -1;

    /* The TLV ids for DNS in particular vary between QMI revisions, and
     * guessing one costs a bench cycle. Walk the whole response once so the
     * log names every id the module actually sent. */
    if (q->debug) {
        uint16_t o = 0;
        while ((uint16_t)(o + 3u) <= (uint16_t)n) {
            uint8_t  t = resp[o];
            uint16_t l = (uint16_t)(resp[o + 1] | (resp[o + 2] << 8));
            if ((uint16_t)(o + 3u + l) > (uint16_t)n) break;
            kprintf("qmi:   tlv %02x len %u :", t, l);
            for (uint16_t i = 0; i < l && i < 8u; i++)
                kprintf(" %02x", resp[o + 3 + i]);
            kprintf("\n");
            o = (uint16_t)(o + 3u + l);
        }
    }

    const uint8_t *v = 0;
    if (qmi_find_tlv(resp, (uint16_t)n, 0x1E, &v) == 4) rd_ipv4(v, q->ip);
    if (qmi_find_tlv(resp, (uint16_t)n, 0x20, &v) == 4) rd_ipv4(v, q->gw);
    if (qmi_find_tlv(resp, (uint16_t)n, 0x21, &v) == 4) rd_ipv4(v, q->mask);
    if (qmi_find_tlv(resp, (uint16_t)n, 0x15, &v) == 4) rd_ipv4(v, q->dns);
    if (qmi_find_tlv(resp, (uint16_t)n, 0x29, &v) == 4)
        q->mtu = (uint16_t)(v[0] | (v[1] << 8));

    kprintf("qmi: ip %u.%u.%u.%u  gw %u.%u.%u.%u  mask %u.%u.%u.%u"
            "  dns %u.%u.%u.%u  mtu %u\n",
            q->ip[0], q->ip[1], q->ip[2], q->ip[3],
            q->gw[0], q->gw[1], q->gw[2], q->gw[3],
            q->mask[0], q->mask[1], q->mask[2], q->mask[3],
            q->dns[0], q->dns[1], q->dns[2], q->dns[3], q->mtu);

    if (!q->ip[0] && !q->ip[1] && !q->ip[2] && !q->ip[3]) {
        kprintf("qmi: no IPv4 address in the settings\n");
        return -1;
    }
    return 0;
}
