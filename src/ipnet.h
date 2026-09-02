/* ipnet.h — minimal IPv4 client stack for the CDC-ECM data path.
 *
 * Separate from net.c on purpose: net.c is the Ethernet driver's SERVER side
 * (fixed IP, answers ARP/ping). Over ECM the modem is a router that hands US
 * an address, so this side is a CLIENT: DHCP to get a lease, ARP to resolve
 * the gateway, then UDP out / ICMP echo.
 *
 * Transport-agnostic: it never touches USB. The app supplies two callbacks
 * (send one Ethernet frame / poll one Ethernet frame) exactly like eth.c's
 * notify seam, so the same stack could run over the RMII driver.
 */
#pragma once
#include <stdint.h>

typedef struct {
    int  (*tx)(const uint8_t *frame, uint16_t len);          /* 0 = sent   */
    int  (*rx)(uint8_t *frame, uint16_t cap);                /* >0 = bytes */
    uint32_t (*now_ms)(void);
    void (*sleep_ms)(uint32_t ms);
} ipnet_ops_t;

typedef struct {
    uint8_t  mac[6];         /* our MAC (from the ECM iMACAddress or made up) */
    uint8_t  ip[4];          /* leased address                                */
    uint8_t  mask[4];
    uint8_t  gw[4];
    uint8_t  dns[4];
    uint8_t  gw_mac[6];      /* resolved by ARP                               */
    uint8_t  have_lease;
    uint8_t  have_gw_mac;
} ipnet_t;

/* Install transport + our MAC. */
void ipnet_init(ipnet_t *n, const ipnet_ops_t *ops, const uint8_t mac[6]);

/* Full DHCP DISCOVER/OFFER/REQUEST/ACK exchange. 0 = got a lease. */
int  ipnet_dhcp(ipnet_t *n, uint32_t timeout_ms);

/* ARP for the gateway's MAC (needed before any off-link packet). 0 = ok. */
int  ipnet_arp_gw(ipnet_t *n, uint32_t timeout_ms);

/* Send one UDP datagram to dst_ip:dst_port. 0 = handed to the transport. */
int  ipnet_udp_send(ipnet_t *n, const uint8_t dst_ip[4], uint16_t dst_port,
                    uint16_t src_port, const void *data, uint16_t len);

/* Ping: send an ICMP echo request and wait for the reply.
 * Returns round-trip ms, or negative on timeout. */
int  ipnet_ping(ipnet_t *n, const uint8_t dst_ip[4], uint32_t timeout_ms);

/* Echo request with a chosen payload size, verifying the echoed bytes.
 * Returns round-trip ms, -1 timeout, -2 truncated reply, -3 corrupt reply. */
int  ipnet_ping_n(ipnet_t *n, const uint8_t dst_ip[4], uint16_t payload,
                  uint32_t timeout_ms);

/* Send an echo request WITHOUT waiting; returns its sequence number.
 * Round-trip timing with one packet outstanding is latency bound and says
 * nothing about receive throughput -- these two let many replies be in
 * flight at once so the RX path can actually be measured. */
int  ipnet_ping_send(ipnet_t *n, const uint8_t dst_ip[4], uint16_t payload);

/* Drain inbound frames for up to timeout_ms, or until `want` echo replies
 * have arrived (want <= 0 means "until the timeout"). Returns IP bytes
 * received; *replies gets the echo-reply count. */
int  ipnet_drain(ipnet_t *n, uint32_t timeout_ms, int want, int *replies);

/* Pump: read one frame and handle it (ARP replies, ICMP echo replies).
 * Returns 1 if a frame was processed. */
int  ipnet_poll(ipnet_t *n);
