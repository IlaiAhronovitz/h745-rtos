/* net.c — just enough of ARP and ICMP to answer a ping.
 *
 * Not a TCP/IP stack. This answers exactly two questions:
 *
 *     "who has 192.168.137.50?"          -> an ARP reply
 *     "echo request to 192.168.137.50"   -> an ICMP echo reply
 *
 * Everything is addressed by byte offset rather than through packed structs,
 * because these formats are defined as byte layouts on the wire and every
 * multi-byte field is big-endian. Reading them by hand keeps that visible and
 * avoids depending on how the compiler chooses to lay out a struct.
 *
 * See net.h for the offsets, and RFC 826 (ARP), RFC 791 (IPv4), RFC 792
 * (ICMP), RFC 1071 (checksum).
 */
#include "stm32h745.h"
#include "libmin.h"
#include "kprintf.h"
#include "eth.h"
#include "net.h"

const uint8_t NET_MAC[6] = { 0x02, 0x00, 0x00, 0x74, 0x50, 0x01 };
static const uint8_t NET_IP[4] = { NET_IP0, NET_IP1, NET_IP2, NET_IP3 };

/* Scratch space for the reply being built. Ordinary RAM is fine here --
 * eth_tx_send() copies into the DMA-visible buffer itself. */
static uint8_t txbuf[128];

static int ip_is_ours(const uint8_t *a)
{
    return a[0] == NET_IP[0] && a[1] == NET_IP[1]
        && a[2] == NET_IP[2] && a[3] == NET_IP[3];
}

/* Network byte order is big-endian and the Cortex-M7 is little-endian, so
 * multi-byte fields cannot simply be cast -- they are assembled a byte at a
 * time in both directions. */
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

/* ==========================================================================
 * The internet checksum (RFC 1071)
 *
 * Shared by IPv4 and ICMP. Sum the data as big-endian 16-bit words, fold any
 * overflow back into the low 16 bits, and return the one's complement.
 *
 * The accumulator is 32-bit deliberately: the overflow past 16 bits has to
 * remain visible so it can be added back in. That end-around carry is what
 * makes this one's-complement arithmetic rather than ordinary truncation, and
 * it is the step most implementations get wrong.
 *
 * An odd trailing byte is treated as the high half of a final word.
 *
 * Validated against the worked example in RFC 1071 section 3, which this
 * reproduces exactly (0x220D).
 * ========================================================================== */
uint16_t net_checksum16(const uint8_t *data, uint16_t len)
{
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

    return (uint16_t)~sum;
}

/* ==========================================================================
 * ARP: answer "who has this IP?"
 *
 * Machines address each other by IP, but the wire only understands MAC
 * addresses. ARP is the lookup between the two, and without it nothing on the
 * network can address a packet to this board at all.
 * ========================================================================== */
static void arp_reply(const uint8_t *f)
{
    const uint8_t *arp  = f + ETH_HDR_LEN;
    uint8_t       *out  = txbuf;
    uint8_t       *oarp = out + ETH_HDR_LEN;

    memset(out, 0, ETH_HDR_LEN + ARP_LEN);

    /* Ethernet header: back to whoever asked, from us. */
    memcpy(out + ETH_OFF_DST, arp + ARP_OFF_SHA, 6);
    memcpy(out + ETH_OFF_SRC, NET_MAC, 6);
    wr16(out + ETH_OFF_TYPE, ETHERTYPE_ARP);

    /* ARP body: the same shape as the request with the roles swapped. */
    wr16(oarp + ARP_OFF_HTYPE, 1u);              /* Ethernet */
    wr16(oarp + ARP_OFF_PTYPE, ETHERTYPE_IPV4);
    oarp[ARP_OFF_HLEN] = 6u;
    oarp[ARP_OFF_PLEN] = 4u;
    wr16(oarp + ARP_OFF_OPER, ARP_OPER_REPLY);
    memcpy(oarp + ARP_OFF_SHA, NET_MAC, 6);           /* sender: us   */
    memcpy(oarp + ARP_OFF_SPA, NET_IP, 4);
    memcpy(oarp + ARP_OFF_THA, arp + ARP_OFF_SHA, 6); /* target: them */
    memcpy(oarp + ARP_OFF_TPA, arp + ARP_OFF_SPA, 4);

    eth_tx_send(txbuf, ETH_HDR_LEN + ARP_LEN);
}

/* ==========================================================================
 * ICMP: answer a ping
 *
 * An echo reply is almost byte-identical to the request, so the whole frame is
 * copied and only what must differ is changed: the two sets of addresses, and
 * one byte of type.
 * ========================================================================== */
static void icmp_reply(const uint8_t *f, uint16_t len)
{
    if (len > sizeof(txbuf)) {
        return;
    }

    memcpy(txbuf, f, len);

    /* Ethernet: back where it came from. */
    memcpy(txbuf + ETH_OFF_DST, f + ETH_OFF_SRC, 6);
    memcpy(txbuf + ETH_OFF_SRC, NET_MAC, 6);

    /* IP: swap source and destination. The destination is read from f, the
     * original, because the line above has already overwritten txbuf's source
     * with ours.
     *
     * The IP header checksum needs no recomputing. Swapping two fields that
     * are both already inside the same sum leaves the sum unchanged. */
    uint8_t *ip = txbuf + ETH_HDR_LEN;
    memcpy(ip + IP_OFF_SRC, NET_IP, 4);
    memcpy(ip + IP_OFF_DST, f + ETH_HDR_LEN + IP_OFF_SRC, 4);

    /* ICMP: request becomes reply. That single byte does change the ICMP
     * checksum, so it is recomputed -- and the field must be zeroed first,
     * because the checksum is defined over a message whose checksum field
     * reads zero. Leaving the old value in would sum it into itself. */
    uint8_t *icmp     = ip + IP_HDR_LEN;
    uint16_t icmp_len = (uint16_t)(len - ETH_HDR_LEN - IP_HDR_LEN);

    icmp[ICMP_OFF_TYPE] = ICMP_ECHO_REPLY;
    wr16(icmp + ICMP_OFF_CSUM, 0u);
    wr16(icmp + ICMP_OFF_CSUM, net_checksum16(icmp, icmp_len));

    eth_tx_send(txbuf, len);
}

/* Dispatch one received frame. Anything not recognised is ignored. */
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

        if (ip[IP_OFF_PROTO] == IP_PROTO_ICMP
            && ip_is_ours(ip + IP_OFF_DST)) {
            const uint8_t *icmp = ip + IP_HDR_LEN;

            if (icmp[ICMP_OFF_TYPE] == ICMP_ECHO_REQUEST) {
                icmp_reply(f, len);
            }
        }
    }
}
