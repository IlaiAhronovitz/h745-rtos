/* net.c — ARP replies and ICMP echo replies. See net.h for the wire layouts. */
#include "stm32h745.h"
#include "libmin.h"
#include "kprintf.h"
#include "eth.h"
#include "net.h"

const uint8_t NET_MAC[6] = { 0x02, 0x00, 0x00, 0x74, 0x50, 0x01 };
static const uint8_t NET_IP[4] = { NET_IP0, NET_IP1, NET_IP2, NET_IP3 };

/* Scratch buffer for the reply we are building. Plain RAM is fine --
 * eth_tx_send() copies into the DMA-visible buffer for us. */
static uint8_t txbuf[128];

/* Is this 4-byte address ours? */
static int ip_is_ours(const uint8_t *a)
{
    return a[0] == NET_IP[0] && a[1] == NET_IP[1]
        && a[2] == NET_IP[2] && a[3] == NET_IP[3];
}

/* Read a big-endian 16-bit field. Same shift-and-combine as print_frame(). */
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

/* Write a big-endian 16-bit field -- the mirror operation. */
static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

/* ==========================================================================
 * STEP P1 -- the internet checksum (RFC 1071).
 *
 * Both IPv4 and ICMP use the same algorithm:
 *
 *   1. add up the data as a sequence of BIG-ENDIAN 16-bit words
 *   2. every time the running total overflows 16 bits, add the carry back
 *      into the bottom ("end-around carry")
 *   3. the checksum is the one's complement of the result -- i.e. ~sum
 *
 * If len is odd, the final lone byte is treated as the HIGH byte of a word
 * whose low byte is zero.
 * ========================================================================== */
uint16_t net_checksum16(const uint8_t *data, uint16_t len)
{
    /* ---- YOUR CODE ------------------------------------------------------
     * About 10 lines. Suggested shape:
     *
     *   uint32_t sum = 0;          <-- 32-bit on purpose, so it can overflow
     *                                  past 16 bits and let you fold it back
     *
     *   loop i from 0 while (i + 1) < len, stepping by 2:
     *       sum += rd16(data + i);
     *
     *   if (len & 1u)              <-- odd length: one byte left over
     *       sum += (uint32_t)data[len - 1u] << 8;
     *
     *   fold the carries:
     *       while (sum >> 16)
     *           sum = (sum & 0xFFFFu) + (sum >> 16);
     *
     *   return (uint16_t)~sum;
     *
     * The fold loop is the part people get wrong. `sum >> 16` is everything
     * that overflowed out of the low 16 bits; adding it back in is the
     * "end-around carry" the RFC describes. It can itself carry, hence while.
     */
    uint32_t sum = 0u;
    for (uint16_t i = 0u; (i + 1u) < len; i += 2u) {
        sum += rd16(data + i);
    }
    if (len & 1u) {
        sum += (uint32_t)data[len - 1u] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    (void)data; (void)len;
    return (uint16_t)~sum;
}

/* ==========================================================================
 * STEP P2 -- answer "who has 192.168.137.50?"
 * I wrote this one: it is pure field-copying, and P3 is the interesting half.
 * ========================================================================== */
static void arp_reply(const uint8_t *f)
{
    const uint8_t *arp = f + ETH_HDR_LEN;
    uint8_t       *out = txbuf;
    uint8_t       *oarp = out + ETH_HDR_LEN;

    memset(out, 0, ETH_HDR_LEN + ARP_LEN);

    /* Ethernet header: back to whoever asked, from us. */
    memcpy(out + ETH_OFF_DST, arp + ARP_OFF_SHA, 6);
    memcpy(out + ETH_OFF_SRC, NET_MAC, 6);
    wr16(out + ETH_OFF_TYPE, ETHERTYPE_ARP);

    /* ARP body: same shape as the request, roles swapped, oper = reply. */
    wr16(oarp + ARP_OFF_HTYPE, 1u);
    wr16(oarp + ARP_OFF_PTYPE, ETHERTYPE_IPV4);
    oarp[ARP_OFF_HLEN] = 6u;
    oarp[ARP_OFF_PLEN] = 4u;
    wr16(oarp + ARP_OFF_OPER, ARP_OPER_REPLY);
    memcpy(oarp + ARP_OFF_SHA, NET_MAC, 6);          /* us   */
    memcpy(oarp + ARP_OFF_SPA, NET_IP, 4);
    memcpy(oarp + ARP_OFF_THA, arp + ARP_OFF_SHA, 6); /* them */
    memcpy(oarp + ARP_OFF_TPA, arp + ARP_OFF_SPA, 4);

    eth_tx_send(txbuf, ETH_HDR_LEN + ARP_LEN);
    kprintf("ARP: replied to %u.%u.%u.%u\n",
            arp[ARP_OFF_SPA + 0], arp[ARP_OFF_SPA + 1],
            arp[ARP_OFF_SPA + 2], arp[ARP_OFF_SPA + 3]);
}

/* ==========================================================================
 * STEP P3 -- answer a ping.  YOUR CODE.
 * ========================================================================== */
static void icmp_reply(const uint8_t *f, uint16_t len)
{
    /* ---- YOUR CODE ------------------------------------------------------
     * The trick: an echo REPLY is almost byte-identical to the REQUEST. So
     * copy the whole frame and change only what must change.
     *
     *   1. if (len > sizeof(txbuf)) return;          -- refuse oversize
     *      memcpy(txbuf, f, len);
     *
     *   2. Ethernet: send it back where it came from.
     *      memcpy(txbuf + ETH_OFF_DST, f + ETH_OFF_SRC, 6);
     *      memcpy(txbuf + ETH_OFF_SRC, NET_MAC, 6);
     *
     *   3. IP: swap source and destination.
     *      uint8_t *ip = txbuf + ETH_HDR_LEN;
     *      memcpy(ip + IP_OFF_SRC, NET_IP, 4);
     *      memcpy(ip + IP_OFF_DST, f + ETH_HDR_LEN + IP_OFF_SRC, 4);
     *
     *      The IP checksum does NOT need recomputing. Swapping two fields
     *      that are both already inside the same sum leaves the sum
     *      unchanged. Worth pausing on -- it is why this is cheap.
     *
     *   4. ICMP: type 8 (request) becomes type 0 (reply), and THAT does
     *      change the ICMP checksum, so it must be recomputed:
     *
     *      uint8_t *icmp = ip + IP_HDR_LEN;
     *      uint16_t icmp_len = len - ETH_HDR_LEN - IP_HDR_LEN;
     *
     *      icmp[ICMP_OFF_TYPE] = ICMP_ECHO_REPLY;
     *      wr16(icmp + ICMP_OFF_CSUM, 0u);        <-- zero it FIRST
     *      wr16(icmp + ICMP_OFF_CSUM, net_checksum16(icmp, icmp_len));
     *
     *      Zeroing first is mandatory: the checksum is computed over a
     *      message whose checksum field reads zero. Leave the old value in
     *      and you are summing it into itself.
     *
     *   5. eth_tx_send(txbuf, len);
     */

     if (len > sizeof(txbuf)) {
         return;  /* refuse oversize */
     }
     memcpy(txbuf, f, len);
     memcpy(txbuf + ETH_OFF_DST, f + ETH_OFF_SRC, 6);
     memcpy(txbuf + ETH_OFF_SRC, NET_MAC, 6);
     uint8_t *ip = txbuf + ETH_HDR_LEN;
     memcpy(ip + IP_OFF_SRC, NET_IP, 4);
        memcpy(ip + IP_OFF_DST, f + ETH_HDR_LEN + IP_OFF_SRC, 4);
        uint8_t *icmp = ip + IP_HDR_LEN;
        uint16_t icmp_len = len - ETH_HDR_LEN - IP_HDR_LEN;
        icmp[ICMP_OFF_TYPE] = ICMP_ECHO_REPLY;
        wr16(icmp + ICMP_OFF_CSUM, 0u);        /* zero it FIRST */
        wr16(icmp + ICMP_OFF_CSUM, net_checksum16(icmp, icmp_len));
        int rc = eth_tx_send(txbuf, len);
        kprintf("ICMP: echo reply %u B rc=%d\n", len, rc);
    }

void net_handle_frame(const uint8_t *f, uint16_t len)
{
    if (len < ETH_HDR_LEN) {
        return;
    }

    uint16_t type = rd16(f + ETH_OFF_TYPE);

    if (type == ETHERTYPE_ARP && len >= ETH_HDR_LEN + ARP_LEN) {
        const uint8_t *arp = f + ETH_HDR_LEN;

        if (rd16(arp + ARP_OFF_OPER) == ARP_OPER_REQUEST
            && ip_is_ours(arp + ARP_OFF_TPA)) {
            arp_reply(f);
        }
        return;
    }

    if (type == ETHERTYPE_IPV4 && len >= ETH_HDR_LEN + IP_HDR_LEN) {
        const uint8_t *ip = f + ETH_HDR_LEN;

        if (ip_is_ours(ip + IP_OFF_DST)) {
            kprintf("IPv4 for us: proto=%u len=%u icmp_type=%u\n",
                    ip[IP_OFF_PROTO], len, ip[IP_HDR_LEN + ICMP_OFF_TYPE]);
        }

        if (ip[IP_OFF_PROTO] == IP_PROTO_ICMP
            && ip_is_ours(ip + IP_OFF_DST)) {
            const uint8_t *icmp = ip + IP_HDR_LEN;

            if (icmp[ICMP_OFF_TYPE] == ICMP_ECHO_REQUEST) {
                icmp_reply(f, len);
            }
        }
    }
}
