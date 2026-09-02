/* rmnet.c — RmNet/QMAP data pipe adapter. See rmnet.h. */
#include "rmnet.h"
#include "libmin.h"
#include "kprintf.h"

#define ETH_HDR   14u
#define ET_IPV4   0x0800u
#define QMAP_HDR  4u
#define BUF_MAX   1600u

static uint8_t pkt[BUF_MAX];        /* staging for one QMAP-framed packet */

/* Hex dump of what really crosses the bulk pipes. The whole raw-IP vs QMAP
 * question is decided by four bytes at the head of a downlink transfer, and
 * no amount of reasoning settles it as fast as looking. */
static void dump(const char *tag, const uint8_t *p, uint16_t n)
{
    uint16_t show = n > 48u ? 48u : n;
    kprintf("rmnet: %s [%u]", tag, n);
    for (uint16_t i = 0; i < show; i++) {
        if ((i & 15u) == 0u) kprintf("\n       ");
        kprintf("%02x ", p[i]);
    }
    kprintf("\n");
}

int rmnet_open(rmnet_t *r, usbh_device_t *dev, int qmap)
{
    usbh_iface_t qmi;

    memset(r, 0, sizeof *r);
    r->dev    = dev;
    r->qmap   = (uint8_t)(qmap ? 1 : 0);
    r->mux_id = RMNET_MUX_ID_DEFAULT;

    /* The QMI interface is ff/ff/ff. DIAG is also ff/ff/ff but has only two
     * endpoints (no interrupt IN), so index 1 among the ff/ff/ff matches is
     * QMI on the default composition; if that fails, fall back to index 0. */
    if (usbh_find_interface(dev, 0xFF, 0xFF, 0xFF, 1, &qmi) != USBH_OK ||
        !qmi.ep_in || !qmi.ep_out) {
        if (usbh_find_interface(dev, 0xFF, 0xFF, 0xFF, 0, &qmi) != USBH_OK ||
            !qmi.ep_in || !qmi.ep_out) {
            kprintf("rmnet: no ff/ff/ff interface with a bulk pair\n");
            return -1;
        }
    }

    if (usbh_pipe_open(dev, qmi.ep_in, &r->in) != USBH_OK) return -1;
    if (usbh_pipe_open(dev, qmi.ep_out, &r->out) != USBH_OK) return -1;

    /* Synthetic MACs for the Ethernet shim. The link is point-to-point, so
     * these never leave the host -- they only satisfy ipnet.c's framing.
     * Locally administered (bit 1 of the first byte), like net.c uses. */
    static const uint8_t host[6] = { 0x02, 0x00, 0x00, 0x74, 0x50, 0x02 };
    static const uint8_t peer[6] = { 0x02, 0x00, 0x00, 0x74, 0x50, 0xFE };
    memcpy(r->host_mac, host, 6);
    memcpy(r->peer_mac, peer, 6);

    kprintf("rmnet: data pipes open (if %u, ep in %02x out %02x, qmap %u)\n",
            qmi.intf[2], r->in.ep_addr, r->out.ep_addr, r->qmap);
    return 0;
}

int rmnet_tx_eth(rmnet_t *r, const uint8_t *frame, uint16_t len)
{
    if (len <= ETH_HDR) return -1;

    const uint8_t *ip  = frame + ETH_HDR;       /* strip the Ethernet header */
    uint16_t       iplen = (uint16_t)(len - ETH_HDR);
    uint16_t       n = 0;

    if (r->qmap) {
        if (iplen + QMAP_HDR > BUF_MAX) return -1;
        pkt[0] = 0;                              /* data packet, no padding  */
        pkt[1] = r->mux_id;
        pkt[2] = (uint8_t)(iplen >> 8);          /* length is big endian     */
        pkt[3] = (uint8_t)iplen;
        memcpy(pkt + QMAP_HDR, ip, iplen);
        n = (uint16_t)(QMAP_HDR + iplen);
    } else {
        if (iplen > BUF_MAX) return -1;
        memcpy(pkt, ip, iplen);
        n = iplen;
    }

    if (r->debug) dump("tx", pkt, n);
    int w = usbh_bulk(r->dev, &r->out, pkt, n, 1000);
    if (r->debug && w != (int)n)
        kprintf("rmnet: tx SHORT (%d of %u)\n", w, n);
    return (w == (int)n) ? 0 : -1;
}

int rmnet_rx_eth(rmnet_t *r, uint8_t *buf, uint16_t cap)
{
    int got = usbh_bulk_frame(r->dev, &r->in, pkt, BUF_MAX, 20);
    if (got <= 0) return got;                    /* 0 = idle, <0 = error     */
    if (r->debug) dump("rx", pkt, (uint16_t)got);

    const uint8_t *ip = pkt;
    uint16_t       iplen = (uint16_t)got;

    if (r->qmap) {
        if (got < (int)QMAP_HDR) return 0;
        if (pkt[0] & 0x80u) return 0;            /* QMAP command, not data   */
        uint16_t plen = (uint16_t)((pkt[2] << 8) | pkt[3]);
        uint8_t  pad  = pkt[0] & 0x3Fu;
        if (plen > (uint16_t)(got - QMAP_HDR)) plen = (uint16_t)(got - QMAP_HDR);
        if (pad < plen) plen = (uint16_t)(plen - pad);
        ip = pkt + QMAP_HDR;
        iplen = plen;
    }
    if (iplen == 0 || (uint16_t)(iplen + ETH_HDR) > cap) return 0;

    /* Wrap the bare IP packet in an Ethernet header so ipnet.c can parse it */
    memcpy(buf, r->host_mac, 6);
    memcpy(buf + 6, r->peer_mac, 6);
    buf[12] = (uint8_t)(ET_IPV4 >> 8);
    buf[13] = (uint8_t)ET_IPV4;
    memcpy(buf + ETH_HDR, ip, iplen);
    return (int)(ETH_HDR + iplen);
}
