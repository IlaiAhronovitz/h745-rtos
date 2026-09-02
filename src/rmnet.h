/* rmnet.h — the RmNet/QMAP data pipe on the modem's QMI interface.
 *
 * After modem_data_call_up() the module carries IP traffic on the QMI
 * interface's bulk pair (RM500Q composition: IF4 ff/ff/ff, bulk IN 0x8e /
 * OUT 0x0f). This layer is the frame adapter between that pipe and ipnet.c.
 *
 * QMAP: the RG50xQ/RM5xxQ modules enable QMAP aggregation by default
 * (qmap_mode=1 per the Data Call Application Note), which prefixes each
 * packet with a 4-byte header:
 *
 *      byte 0  bit7 = CD (1 = command, 0 = data), bits 5:0 = pad length
 *      byte 1  MUX id     (129 / 0x81 by default)
 *      bytes 2-3  payload length, BIG endian, excluding this header
 *
 * The payload is a bare IP packet -- no Ethernet header, because RmNet is a
 * point-to-point link. ipnet.c speaks Ethernet, so rmnet_rx/tx translate:
 * on RX we synthesise an Ethernet header in front of the IP packet, on TX we
 * strip it. That keeps one IP stack usable over both the RMII driver and
 * this modem link.
 */
#pragma once
#include <stdint.h>
#include "usbh.h"

#define RMNET_MUX_ID_DEFAULT 0x81u   /* 129, the module default */

typedef struct {
    usbh_device_t *dev;
    usbh_pipe_t    in;
    usbh_pipe_t    out;
    uint8_t        mux_id;
    uint8_t        qmap;             /* 1 = add/expect QMAP headers */
    uint8_t        debug;            /* 1 = hex-dump every frame both ways */
    uint8_t        host_mac[6];      /* synthetic, for the Ethernet shim */
    uint8_t        peer_mac[6];      /* synthetic "gateway" MAC          */
} rmnet_t;

/* Open the data pipes on the QMI interface (class ff/ff/ff, index 0 among
 * ff/ff/ff interfaces with 3 endpoints). Returns 0 on success. */
int rmnet_open(rmnet_t *r, usbh_device_t *dev, int qmap);

/* Send one Ethernet frame: strips the Ethernet header, adds QMAP if enabled,
 * writes the IP packet to the modem. Returns 0 on success. */
int rmnet_tx_eth(rmnet_t *r, const uint8_t *frame, uint16_t len);

/* Receive one packet and present it as an Ethernet frame in buf.
 * Returns the frame length, 0 if nothing arrived, or negative on error. */
int rmnet_rx_eth(rmnet_t *r, uint8_t *buf, uint16_t cap);
