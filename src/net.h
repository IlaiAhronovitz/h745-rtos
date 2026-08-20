/* net.h — just enough of ARP and ICMP to answer a ping.
 *
 * This is NOT a TCP/IP stack. It handles exactly two questions:
 *   "who has 192.168.137.50?"   -> an ARP reply
 *   "echo request to 192.168.137.50" -> an ICMP echo reply
 *
 * Everything is done with byte offsets rather than packed structs, because
 * these formats are defined as byte layouts on the wire and every multi-byte
 * field is BIG-endian. Reading them by hand keeps that visible.
 *
 * References: RFC 826 (ARP), RFC 791 (IPv4), RFC 792 (ICMP).
 */
#pragma once
#include <stdint.h>

/* Our identity on the wire. The MAC matches the one app_eth.c transmits with:
 * bit 1 of the first byte set = locally administered, bit 0 clear = unicast. */
extern const uint8_t NET_MAC[6];
#define NET_IP0 192u
#define NET_IP1 168u
#define NET_IP2 137u
#define NET_IP3  50u

/* ---- Ethernet header, 14 bytes ------------------------------------------ */
#define ETH_OFF_DST    0u
#define ETH_OFF_SRC    6u
#define ETH_OFF_TYPE  12u
#define ETH_HDR_LEN   14u

#define ETHERTYPE_IPV4 0x0800u
#define ETHERTYPE_ARP  0x0806u

/* ---- ARP packet, 28 bytes, starting at ETH_HDR_LEN (RFC 826) ------------- */
#define ARP_OFF_HTYPE  0u   /* 2 B, 1 = Ethernet          */
#define ARP_OFF_PTYPE  2u   /* 2 B, 0x0800 = IPv4         */
#define ARP_OFF_HLEN   4u   /* 1 B, 6                     */
#define ARP_OFF_PLEN   5u   /* 1 B, 4                     */
#define ARP_OFF_OPER   6u   /* 2 B, 1 = request, 2 = reply*/
#define ARP_OFF_SHA    8u   /* 6 B, sender MAC            */
#define ARP_OFF_SPA   14u   /* 4 B, sender IP             */
#define ARP_OFF_THA   18u   /* 6 B, target MAC            */
#define ARP_OFF_TPA   24u   /* 4 B, target IP             */
#define ARP_LEN       28u

#define ARP_OPER_REQUEST 1u
#define ARP_OPER_REPLY   2u

/* ---- IPv4 header, 20 bytes without options (RFC 791) --------------------- */
#define IP_OFF_VER_IHL  0u
#define IP_OFF_TOTLEN   2u  /* 2 B */
#define IP_OFF_TTL      8u
#define IP_OFF_PROTO    9u
#define IP_OFF_CSUM    10u  /* 2 B */
#define IP_OFF_SRC     12u  /* 4 B */
#define IP_OFF_DST     16u  /* 4 B */
#define IP_HDR_LEN     20u

#define IP_PROTO_ICMP   1u

/* ---- ICMP echo (RFC 792) ------------------------------------------------- */
#define ICMP_OFF_TYPE   0u
#define ICMP_OFF_CODE   1u
#define ICMP_OFF_CSUM   2u  /* 2 B */

#define ICMP_ECHO_REQUEST 8u
#define ICMP_ECHO_REPLY   0u

/* One's-complement checksum used by both IPv4 and ICMP (RFC 1071). */
uint16_t net_checksum16(const uint8_t *data, uint16_t len);

/* Hand every received frame here. Replies are sent via eth_tx_send(). */
void net_handle_frame(const uint8_t *f, uint16_t len);
