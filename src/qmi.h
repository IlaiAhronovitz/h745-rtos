/* qmi.h — Qualcomm MSM Interface client for the bare-metal H745 USB host.
 *
 * This is what gives the MCU FULL control of the modem: the AT port commands
 * the radio, QMI commands the data path.
 *
 * Transport (the fact that shapes everything): QMI control messages do NOT
 * travel on the QMI interface's bulk pipes. They ride EP0 as CDC-style class
 * control transfers, even though the interface is vendor-class ff/ff/ff:
 *
 *   send:    bmRequestType 0x21, bRequest 0x00 SEND_ENCAPSULATED_COMMAND
 *   receive: bmRequestType 0xA1, bRequest 0x01 GET_ENCAPSULATED_RESPONSE
 *   wIndex = the QMI interface number (4 on the RM500Q composition)
 *
 * The bulk pair (0x8e/0x0f) carries only data-plane packets, which rmnet.c
 * handles. The interrupt endpoint (0x88, 8 bytes) signals RESPONSE_AVAILABLE;
 * a host without interrupt-pipe support polls GET_ENCAPSULATED_RESPONSE
 * instead, which is what this driver does.
 *
 * DTR first: the kernel marks 2c7c:0800 with QMI_WWAN_QUIRK_DTR — the module
 * ignores QMI requests until SET_CONTROL_LINE_STATE asserts DTR.
 *
 * Wire format (all little-endian):
 *   QMUX header   : 01 | len16 | flags8 | service8 | client8
 *   QMI header    : flags8 | txn(1 byte for CTL, 2 for others) | msg16 | len16
 *   TLVs          : type8 | len16 | value...   (result TLV type 0x02)
 *
 * Matching matters: the module also pushes unsolicited INDICATIONS down the
 * same pipe (packet-service-status, signal info). qmi_request() therefore
 * accepts a frame only when its message id AND transaction id match what it
 * sent; anything else is discarded and the poll continues.
 */
#pragma once
#include <stdint.h>
#include "usbh.h"

/* services */
#define QMI_SVC_CTL   0x00
#define QMI_SVC_WDS   0x01
#define QMI_SVC_WDA   0x1A

/* messages we use */
#define QMI_CTL_ALLOCATE_CID         0x0022
#define QMI_CTL_RELEASE_CID          0x0023
#define QMI_WDS_START_NETWORK        0x0020
#define QMI_WDS_STOP_NETWORK         0x0021
#define QMI_WDS_GET_CURRENT_SETTINGS 0x002D
#define QMI_WDS_BIND_MUX_DATA_PORT   0x00A2
#define QMI_WDA_SET_DATA_FORMAT      0x0020
#define QMI_WDA_GET_DATA_FORMAT      0x0022

#define QMI_RESULT_TLV  0x02

typedef struct {
    usbh_device_t *dev;
    uint8_t  iface;          /* QMI interface number (bInterfaceNumber)     */
    uint8_t  cid_wds;        /* client id allocated for WDS                 */
    uint8_t  cid_wda;
    uint16_t txn;            /* rolling transaction id, service messages     */
    uint8_t  txn_ctl;        /* CTL's own counter: its txn field is ONE byte  */
    uint8_t  ip[4];          /* from WDS GET_CURRENT_SETTINGS               */
    uint8_t  gw[4];
    uint8_t  mask[4];
    uint8_t  dns[4];
    uint16_t mtu;
    uint32_t pkt_handle;     /* handle returned by START_NETWORK            */
    uint16_t last_err;       /* QMI error code of the last failed request   */
    uint8_t  link_layer;     /* as READ BACK: 1 = 802.3, 2 = raw IP         */
    uint8_t  ul_agg;         /* aggregation protocol in force, 0 = none     */
    uint8_t  dl_agg;
    uint8_t  format_verified;/* 1 = read back from the module, 0 = assumed  */
    uint8_t  debug;          /* 1 = hex-dump failed request/response frames  */
    uint8_t  mux_id;         /* mux the data port was bound to, 0xFF = none  */
    uint8_t  profile;        /* 3GPP profile index to pin the call to, 0 = any */
} qmi_t;

/* Time hooks, same pattern as modem.c — the driver never busy-spins. */
void qmi_set_clock(uint32_t (*now)(void), void (*slp)(uint32_t));

/* Find the QMI interface, assert DTR, and get ready to talk. 0 = ok. */
int qmi_open(qmi_t *q, usbh_device_t *dev);

/* One request/response round trip. req_tlvs may be NULL when there are none.
 * Returns the response's TLV byte count, or negative on error. The response
 * TLVs (excluding headers) land in resp, up to resp_cap. */
int qmi_request(qmi_t *q, uint8_t service, uint8_t cid, uint16_t msg_id,
                const uint8_t *req_tlvs, uint16_t req_len,
                uint8_t *resp, uint16_t resp_cap, uint32_t timeout_ms);

/* Allocate a client id for a service (CTL 0x0022). 0 = ok, cid in *out. */
int qmi_alloc_cid(qmi_t *q, uint8_t service, uint8_t *out);

/* Hand a client id back (CTL 0x0023). Client ids are MODULE-side state that
 * outlives a host reset -- leaking them eventually wedges the CTL service. */
int qmi_release_cid(qmi_t *q, uint8_t service, uint8_t cid);

/* Release every client id this session allocated. Call on every exit path. */
void qmi_close(qmi_t *q);

/* Ask for raw-IP link protocol (WDA 0x0020). Must precede the data call:
 * without it the modem frames in 802.3 and our raw-IP path never matches. */
int qmi_set_data_format_raw_ip(qmi_t *q);

/* Same, but qmap != 0 asks for raw IP carried inside QMAP framing with one
 * datagram per USB transfer. */
int qmi_set_data_format(qmi_t *q, int qmap);

/* WDS 0x00A2: associate this WDS client's call with THIS USB interface and
 * mux id. Without it the module holds a bearer that is not attached to the
 * data pipe, and every uplink packet earns an ICMP host-unreachable from
 * inside the operator network. Call it before qmi_start_network(). */
int qmi_bind_mux(qmi_t *q, uint8_t mux_id, int qmap);

/* Read the format back (WDA 0x0022) into link_layer/ul_agg/dl_agg. The
 * data path is framed from THIS, never from what we asked for. */
int qmi_get_data_format(qmi_t *q);

/* Start the data call on the given APN (WDS 0x0020), IPv4. 0 = ok. */
int qmi_start_network(qmi_t *q, const char *apn);

/* Tear the call down again — always call this before leaving the app. */
int qmi_stop_network(qmi_t *q);

/* Read the assigned IP/gateway/DNS/MTU (WDS 0x002D) into the struct. */
int qmi_get_settings(qmi_t *q);

/* Find a TLV of the given type inside a response buffer. Returns its length
 * and sets *val, or -1 if absent. */
int qmi_find_tlv(const uint8_t *resp, uint16_t len, uint8_t type,
                 const uint8_t **val);
