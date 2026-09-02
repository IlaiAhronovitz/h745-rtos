/* ipnet.c — minimal IPv4 client stack (DHCP + ARP + UDP + ping) for the ECM
 * data path. See ipnet.h.
 *
 * Byte-offset addressing throughout, exactly like net.c: these are wire
 * formats and every multi-byte field is big-endian, so headers are assembled
 * a field at a time rather than through packed structs.
 */
#include "ipnet.h"
#include "libmin.h"
#include "kprintf.h"

#define ETH_HDR      14u
#define ET_IPV4      0x0800u
#define ET_ARP       0x0806u
#define IP_HDR       20u
#define PROTO_ICMP   1u
#define PROTO_UDP    17u
#define FRAME_MAX    1514u

static ipnet_ops_t ops;
static uint8_t     txf[FRAME_MAX];
static uint8_t     rxf[FRAME_MAX];
static uint8_t     scratch[FRAME_MAX];

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

/* RFC 1071 one's-complement checksum — the routine net.c already validated. */
static uint16_t csum16(const uint8_t *d, uint16_t len)
{
    uint32_t s = 0;
    for (uint16_t i = 0; (i + 1u) < len; i += 2u) s += rd16(d + i);
    if (len & 1u) s += (uint32_t)d[len - 1u] << 8;
    while (s >> 16) s = (s & 0xFFFFu) + (s >> 16);
    return (uint16_t)~s;
}

void ipnet_init(ipnet_t *n, const ipnet_ops_t *o, const uint8_t mac[6])
{
    memset(n, 0, sizeof *n);
    ops = *o;
    memcpy(n->mac, mac, 6);
}

/* ---- frame builders ------------------------------------------------------ */
static uint16_t eth_hdr(uint8_t *f, const uint8_t dst[6], const uint8_t src[6],
                        uint16_t type)
{
    memcpy(f, dst, 6);
    memcpy(f + 6, src, 6);
    wr16(f + 12, type);
    return ETH_HDR;
}

/* IPv4 header. Returns the offset where the payload starts. */
static uint16_t ip_hdr(uint8_t *f, const uint8_t src[4], const uint8_t dst[4],
                       uint8_t proto, uint16_t payload_len)
{
    uint8_t *ip = f + ETH_HDR;
    memset(ip, 0, IP_HDR);
    ip[0] = 0x45;                                  /* IPv4, 5-word header  */
    wr16(ip + 2, (uint16_t)(IP_HDR + payload_len));
    ip[6] = 0x40;                                  /* don't fragment       */
    ip[8] = 64;                                    /* TTL                  */
    ip[9] = proto;
    memcpy(ip + 12, src, 4);
    memcpy(ip + 16, dst, 4);
    wr16(ip + 10, csum16(ip, IP_HDR));
    return ETH_HDR + IP_HDR;
}

/* UDP datagram including the pseudo-header checksum. Returns frame length. */
static uint16_t udp_build(uint8_t *f, const uint8_t src_ip[4],
                          const uint8_t dst_ip[4], uint16_t sport,
                          uint16_t dport, const void *data, uint16_t len)
{
    uint16_t off = ip_hdr(f, src_ip, dst_ip, PROTO_UDP, (uint16_t)(8u + len));
    uint8_t *u = f + off;
    wr16(u + 0, sport);
    wr16(u + 2, dport);
    wr16(u + 4, (uint16_t)(8u + len));
    wr16(u + 6, 0);
    if (len) memcpy(u + 8, data, len);

    /* Checksum covers a pseudo-header (src, dst, proto, length) plus the
     * datagram — assembled in scratch because it is not contiguous. */
    uint16_t n = 0;
    memcpy(scratch + n, src_ip, 4); n = (uint16_t)(n + 4);
    memcpy(scratch + n, dst_ip, 4); n = (uint16_t)(n + 4);
    scratch[n++] = 0;
    scratch[n++] = PROTO_UDP;
    wr16(scratch + n, (uint16_t)(8u + len)); n = (uint16_t)(n + 2);
    memcpy(scratch + n, u, (uint16_t)(8u + len));
    n = (uint16_t)(n + 8u + len);

    uint16_t c = csum16(scratch, n);
    wr16(u + 6, c ? c : 0xFFFFu);        /* 0 is reserved for "no checksum" */
    return (uint16_t)(off + 8u + len);
}

/* ---- DHCP ---------------------------------------------------------------- */
/* BOOTP fixed part is 236 bytes, then the magic cookie and TLV options. */
#define DHCP_MAGIC 0x63825363u

static uint32_t dhcp_xid;

static uint16_t dhcp_build(ipnet_t *n, uint8_t msg_type, const uint8_t *req_ip,
                           const uint8_t *server_ip)
{
    static const uint8_t bcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    static const uint8_t any_ip[4]    = { 0, 0, 0, 0 };
    static const uint8_t bcast_ip[4]  = { 255, 255, 255, 255 };
    uint8_t body[300];

    memset(body, 0, sizeof body);
    body[0] = 1;                        /* op: BOOTREQUEST                 */
    body[1] = 1;                        /* htype: Ethernet                 */
    body[2] = 6;                        /* hlen                            */
    wr32(body + 4, dhcp_xid);           /* xid: ties replies to this run   */
    wr16(body + 10, 0x8000);            /* flags: broadcast the reply      */
    memcpy(body + 28, n->mac, 6);       /* chaddr                          */
    wr32(body + 236, DHCP_MAGIC);

    uint16_t o = 240;
    body[o++] = 53; body[o++] = 1; body[o++] = msg_type;
    body[o++] = 55; body[o++] = 4;      /* parameter request list          */
    body[o++] = 1;  body[o++] = 3;  body[o++] = 6;  body[o++] = 51;
    if (req_ip) {
        body[o++] = 50; body[o++] = 4;
        memcpy(body + o, req_ip, 4); o = (uint16_t)(o + 4);
    }
    if (server_ip) {
        body[o++] = 54; body[o++] = 4;
        memcpy(body + o, server_ip, 4); o = (uint16_t)(o + 4);
    }
    body[o++] = 255;                    /* end of options                  */

    eth_hdr(txf, bcast_mac, n->mac, ET_IPV4);
    return udp_build(txf, any_ip, bcast_ip, 68, 67, body, o);
}

/* Returns the DHCP message type of a reply addressed to us, else 0. */
static uint8_t dhcp_parse(ipnet_t *n, const uint8_t *f, uint16_t len,
                          uint8_t *server_ip, uint8_t *offered)
{
    if (len < ETH_HDR + IP_HDR + 8u + 240u) return 0;
    if (rd16(f + 12) != ET_IPV4) return 0;

    const uint8_t *ip = f + ETH_HDR;
    if (ip[9] != PROTO_UDP) return 0;
    const uint8_t *u = ip + ((ip[0] & 0xFu) * 4u);
    if (rd16(u + 2) != 68) return 0;              /* not the DHCP client port */

    const uint8_t *b = u + 8;
    if (b[0] != 2) return 0;                      /* not a BOOTREPLY          */
    memcpy(offered, b + 16, 4);                   /* yiaddr = our address     */

    uint8_t type = 0;
    uint16_t blen = (uint16_t)(len - (uint16_t)(b - f));
    uint16_t o = 240;
    while (o + 1u < blen && b[o] != 255u) {
        uint8_t code = b[o], l = b[o + 1];
        const uint8_t *v = b + o + 2;
        if      (code == 53 && l >= 1) type = v[0];
        else if (code == 1  && l >= 4) memcpy(n->mask, v, 4);
        else if (code == 3  && l >= 4) memcpy(n->gw,   v, 4);
        else if (code == 6  && l >= 4) memcpy(n->dns,  v, 4);
        else if (code == 54 && l >= 4) memcpy(server_ip, v, 4);
        o = (uint16_t)(o + 2u + l);
    }
    return type;
}

int ipnet_dhcp(ipnet_t *n, uint32_t timeout_ms)
{
    uint8_t server_ip[4] = { 0, 0, 0, 0 };
    uint8_t offered[4]   = { 0, 0, 0, 0 };
    int got_offer = 0;

    dhcp_xid = 0x48373435u ^ ops.now_ms();        /* "H745" ^ time            */

    uint16_t len = dhcp_build(n, 1, 0, 0);        /* DISCOVER                 */
    if (ops.tx(txf, len) != 0) return -1;

    uint32_t t0 = ops.now_ms();
    while (ops.now_ms() - t0 < timeout_ms) {
        int r = ops.rx(rxf, sizeof rxf);
        if (r > 0) {
            uint8_t t = dhcp_parse(n, rxf, (uint16_t)r, server_ip, offered);
            if (t == 2 && !got_offer) {           /* OFFER -> send REQUEST    */
                got_offer = 1;
                kprintf("dhcp: offer %u.%u.%u.%u from %u.%u.%u.%u\n",
                        offered[0], offered[1], offered[2], offered[3],
                        server_ip[0], server_ip[1], server_ip[2], server_ip[3]);
                len = dhcp_build(n, 3, offered, server_ip);
                ops.tx(txf, len);
            } else if (t == 5) {                  /* ACK -> lease is ours     */
                memcpy(n->ip, offered, 4);
                n->have_lease = 1;
                return 0;
            }
        }
        ops.sleep_ms(10);
    }
    return -1;
}

/* ---- ARP ----------------------------------------------------------------- */
int ipnet_arp_gw(ipnet_t *n, uint32_t timeout_ms)
{
    static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    uint8_t *a = txf + ETH_HDR;

    eth_hdr(txf, bcast, n->mac, ET_ARP);
    memset(a, 0, 28);
    wr16(a + 0, 1);                     /* htype: Ethernet                 */
    wr16(a + 2, ET_IPV4);               /* ptype                           */
    a[4] = 6; a[5] = 4;
    wr16(a + 6, 1);                     /* oper: request                   */
    memcpy(a + 8,  n->mac, 6);
    memcpy(a + 14, n->ip,  4);
    memcpy(a + 24, n->gw,  4);
    if (ops.tx(txf, ETH_HDR + 28u) != 0) return -1;

    uint32_t t0 = ops.now_ms();
    while (ops.now_ms() - t0 < timeout_ms) {
        int r = ops.rx(rxf, sizeof rxf);
        if (r >= (int)(ETH_HDR + 28u) && rd16(rxf + 12) == ET_ARP) {
            const uint8_t *p = rxf + ETH_HDR;
            if (rd16(p + 6) == 2 && 0 == memcmp(p + 14, n->gw, 4)) {
                memcpy(n->gw_mac, p + 8, 6);
                n->have_gw_mac = 1;
                return 0;
            }
        }
        ops.sleep_ms(10);
    }
    return -1;
}

/* ---- UDP / ping ---------------------------------------------------------- */
int ipnet_udp_send(ipnet_t *n, const uint8_t dst_ip[4], uint16_t dport,
                   uint16_t sport, const void *data, uint16_t len)
{
    if (!n->have_gw_mac) return -1;
    eth_hdr(txf, n->gw_mac, n->mac, ET_IPV4);
    uint16_t flen = udp_build(txf, n->ip, dst_ip, sport, dport, data, len);
    return ops.tx(txf, flen);
}

/* Echo request with a caller-chosen payload size, verifying that what comes
 * back is byte-for-byte what went out.
 *
 * Size is the point: a 16-byte ping fits in one 64-byte USB packet and proves
 * almost nothing about the bulk path. A 1400-byte one is 22 packets that must
 * be segmented, reassembled and terminated by a short packet in both
 * directions, so it exercises the parts of usbh.c that a small ping never
 * touches. Checking the echoed payload catches truncation and mis-assembly
 * that a "reply arrived" test would pass. */
/* Send an echo request and do NOT wait for it.
 *
 * Round-trip timing measures the network; with one packet outstanding it can
 * never exceed payload/RTT no matter how fast the host is. To measure the
 * RECEIVE path you have to get many replies in flight at once, which means
 * separating the send from the wait. Returns the sequence number used. */
int ipnet_ping_send(ipnet_t *n, const uint8_t dst_ip[4], uint16_t payload)
{
    static uint16_t burst_seq = 0x8000u;   /* a range ipnet_ping_n never uses */

    if (!n->have_gw_mac) return -1;
    if (payload > FRAME_MAX - ETH_HDR - IP_HDR - 8u) return -1;

    uint16_t icmp_len = (uint16_t)(8u + payload);
    eth_hdr(txf, n->gw_mac, n->mac, ET_IPV4);
    uint16_t off = ip_hdr(txf, n->ip, dst_ip, PROTO_ICMP, icmp_len);

    uint8_t *ic = txf + off;
    memset(ic, 0, icmp_len);
    ic[0] = 8;
    wr16(ic + 4, 0x4837);
    wr16(ic + 6, ++burst_seq);
    for (uint16_t i = 0; i < payload; i++) ic[8 + i] = (uint8_t)(i ^ 0x5A);
    wr16(ic + 2, csum16(ic, icmp_len));

    if (ops.tx(txf, (uint16_t)(off + icmp_len)) != 0) return -1;
    return (int)burst_seq;
}

/* Pull inbound frames for up to timeout_ms, counting ICMP echo replies and
 * the IP bytes they carried. Stops early once `want` replies have arrived. */
int ipnet_drain(ipnet_t *n, uint32_t timeout_ms, int want, int *replies)
{
    uint32_t t0 = ops.now_ms();
    int got = 0;
    uint32_t bytes = 0;
    (void)n;

    while (ops.now_ms() - t0 < timeout_ms && (want <= 0 || got < want)) {
        int r = ops.rx(rxf, sizeof rxf);
        if (r <= (int)(ETH_HDR + IP_HDR)) continue;
        if (rd16(rxf + 12) != ET_IPV4) continue;
        const uint8_t *ip = rxf + ETH_HDR;
        bytes += (uint32_t)(r - ETH_HDR);
        if (ip[9] == PROTO_ICMP) {
            const uint8_t *re = ip + ((ip[0] & 0xFu) * 4u);
            if (re[0] == 0) got++;
        }
    }
    if (replies) *replies = got;
    return (int)bytes;
}

int ipnet_ping_n(ipnet_t *n, const uint8_t dst_ip[4], uint16_t payload,
                 uint32_t timeout_ms)
{
    static uint16_t seq;

    if (!n->have_gw_mac) return -1;
    if (payload > FRAME_MAX - ETH_HDR - IP_HDR - 8u) return -1;

    uint16_t icmp_len = (uint16_t)(8u + payload);
    eth_hdr(txf, n->gw_mac, n->mac, ET_IPV4);
    uint16_t off = ip_hdr(txf, n->ip, dst_ip, PROTO_ICMP, icmp_len);

    uint8_t *ic = txf + off;
    memset(ic, 0, icmp_len);
    ic[0] = 8;                          /* echo request                    */
    wr16(ic + 4, 0x4837);               /* id                              */
    wr16(ic + 6, ++seq);
    /* a walking pattern, so a mis-assembled reply cannot look correct */
    for (uint16_t i = 0; i < payload; i++) ic[8 + i] = (uint8_t)(i ^ 0x5A);
    wr16(ic + 2, csum16(ic, icmp_len));

    uint32_t t0 = ops.now_ms();
    if (ops.tx(txf, (uint16_t)(off + icmp_len)) != 0) return -1;

    while (ops.now_ms() - t0 < timeout_ms) {
        int r = ops.rx(rxf, sizeof rxf);
        if (r > (int)(ETH_HDR + IP_HDR) && rd16(rxf + 12) == ET_IPV4) {
            const uint8_t *ip = rxf + ETH_HDR;
            if (ip[9] == PROTO_ICMP) {
                const uint8_t *re = ip + ((ip[0] & 0xFu) * 4u);
                if (re[0] == 0 && rd16(re + 6) == seq) {
                    /* the reply must carry our payload back unaltered */
                    int have = r - (int)(re + 8 - rxf);
                    if (have < (int)payload) return -2;      /* truncated  */
                    for (uint16_t i = 0; i < payload; i++)
                        if (re[8 + i] != (uint8_t)(i ^ 0x5A)) return -3;
                    return (int)(ops.now_ms() - t0);
                }
            }
        }
        ops.sleep_ms(1);
    }
    return -1;
}

int ipnet_ping(ipnet_t *n, const uint8_t dst_ip[4], uint32_t timeout_ms)
{
    return ipnet_ping_n(n, dst_ip, 8, timeout_ms);
}

int ipnet_poll(ipnet_t *n)
{
    (void)n;
    return ops.rx(rxf, sizeof rxf) > 0 ? 1 : 0;
}
