/* usbh.h — bare-metal USB Full-Speed HOST on the H745's USB2 OTG instance.
 *
 * Scope (milestone 1): bring the core up in forced-host buffer-DMA mode,
 * power VBUS on CN13, detect a connection, reset the port, and enumerate
 * the device over EP0 control transfers — device descriptor, address,
 * configuration descriptor. Polled, no interrupts yet: enumeration has no
 * latency requirement and polling keeps the first bring-up observable.
 *
 * Like eth.c, this file includes no kernel header: the driver stays usable
 * from a bare main() or any RTOS.
 *
 * References: RM0399 ch.60 (OTG), docs/refs/USB-OTG-NOTES.md (page-cited
 * register map used to write this driver), UM2408 §7.11 + MB1363 sheet 7
 * (board wiring: CN13, PD10 VBUS switch, PG7 overcurrent).
 */
#pragma once
#include <stdint.h>

/* Error codes: 0 ok, negatives are failures. */
#define USBH_OK          0
#define USBH_ERR_INIT   -1   /* clock / PHY power / core reset failed        */
#define USBH_ERR_TIMEOUT -2  /* bounded wait expired                         */
#define USBH_ERR_STALL  -3   /* device STALLed the request                   */
#define USBH_ERR_XFER   -4   /* TXERR/BBERR/DTERR/FRMOR on the channel       */
#define USBH_ERR_NAK    -5   /* core gave up on a NAKed transfer             */
#define USBH_ERR_DISC   -6   /* device disconnected mid-operation            */

typedef struct {
    uint8_t  speed;          /* HPRT.PSPD: 1 = full, 2 = low                 */
    uint8_t  addr;           /* assigned USB address (1)                     */
    uint8_t  mps0;           /* EP0 max packet size from bMaxPacketSize0     */
    uint16_t vid, pid;
    uint8_t  dev_desc[18];   /* full device descriptor                       */
    uint8_t  cfg_desc[512];  /* configuration descriptor tree (truncated to
                              * fit; cfg_len holds the stored length)        */
    uint16_t cfg_total;      /* wTotalLength as reported by the device       */
    uint16_t cfg_len;        /* bytes actually stored in cfg_desc            */
} usbh_device_t;

/* ---- milestone 2: configured state, interfaces, bulk pipes --------------- */

typedef struct {
    uint8_t  ep_addr;        /* bEndpointAddress incl. bit7 direction        */
    uint8_t  channel;        /* host channel 2..15 (0/1 belong to EP0)       */
    uint8_t  toggle;         /* 0 = next PID DATA0, 1 = DATA1                */
    uint16_t mps;            /* wMaxPacketSize & 0x7FF                       */
} usbh_pipe_t;

typedef struct {
    const uint8_t *intf;     /* 9-byte interface descriptor                  */
    const uint8_t *ep_in;    /* bulk IN endpoint descriptor, or NULL         */
    const uint8_t *ep_out;   /* bulk OUT endpoint descriptor, or NULL        */
    const uint8_t *ep_int;   /* interrupt IN endpoint descriptor, or NULL    */
    uint8_t        alt;      /* bAlternateSetting this match came from       */
} usbh_iface_t;

/* SET_CONFIGURATION with the device's own bConfigurationValue. Endpoint
 * data toggles reset to DATA0 on entering Configured (USB 2.0 §9.1.1.5),
 * so open pipes only AFTER this succeeds. */
int usbh_set_configuration(usbh_device_t *dev);

/* Find the index-th interface matching a class triple, collecting its bulk
 * endpoint descriptors. Match by class triple, never by interface number:
 * Quectel modems renumber interfaces when the usbnet mode changes. */
int usbh_find_interface(const usbh_device_t *dev, uint8_t cls, uint8_t sub,
                        uint8_t proto, uint8_t index, usbh_iface_t *out);

/* Like usbh_find_interface but matches a specific bAlternateSetting. CDC-ECM
 * hides its bulk endpoints in alt setting 1 (alt 0 has none by spec), so the
 * data interface must be searched with alt=1 and then activated with
 * usbh_set_interface(). */
int usbh_find_interface_alt(const usbh_device_t *dev, uint8_t cls, uint8_t sub,
                            uint8_t proto, uint8_t index, uint8_t alt,
                            usbh_iface_t *out);

/* SET_INTERFACE: activate an alternate setting on an interface. Required
 * before an ECM data interface's endpoints become usable (USB 2.0 §9.4.10). */
int usbh_set_interface(usbh_device_t *dev, uint8_t iface, uint8_t alt);

/* Bind a bulk endpoint to a free host channel. */
int usbh_pipe_open(usbh_device_t *dev, const uint8_t *ep_desc, usbh_pipe_t *p);

/* Halt every bulk channel and recycle the allocator — used when probing
 * candidate interfaces (open pipes, try "AT", not it, reset, next). */
void usbh_pipes_reset(void);

/* CLEAR_FEATURE(ENDPOINT_HALT) on a bulk pipe, and reset the host's data
 * toggle to DATA0 to match what the device does. usbh_bulk() calls this
 * itself on a STALL; without it a stalled bulk endpoint never recovers. */
int  usbh_clear_halt(usbh_device_t *dev, usbh_pipe_t *p);

/* One bulk transfer; direction comes from the pipe's endpoint address.
 * Returns bytes moved, or USBH_ERR_*. On TIMEOUT the channel is halted and
 * the data toggle is untouched — for an idle IN endpoint this is the normal
 * "no data yet" outcome; just submit again. Submit IN reads of exactly one
 * MPS: it keeps every outcome toggle-atomic. A bulk OUT whose length is an
 * exact multiple of the endpoint's MPS is followed by a zero-length packet. */
int usbh_bulk(usbh_device_t *dev, usbh_pipe_t *p, void *buf, uint16_t len,
              uint32_t timeout_ms);

/* Read one complete transfer (= one Ethernet frame on an ECM pipe): keeps
 * reading max-size packets until a SHORT packet marks the end. Returns total
 * bytes, 0 if the pipe was idle, or USBH_ERR_*. */
int usbh_bulk_frame(usbh_device_t *dev, usbh_pipe_t *p, void *buf,
                    uint16_t cap, uint32_t timeout_ms);

/* Assert DTR/RTS on a vendor-serial interface via SET_CONTROL_LINE_STATE.
 * Tolerant of an unsupported (STALLed) request; only USBH_ERR_DISC bubbles. */
int usbh_set_line_state(usbh_device_t *dev, uint8_t iface, int dtr, int rts);

/* Bring-up through "port powered". Prints the core's ID registers (CID,
 * GSNPSID, GHWCFG1..4) — ground truth about the silicon's DMA capability. */
int  usbh_init(void);

/* 1 while a device is physically connected (HPRT.PCSTS). */
int  usbh_connected(void);

/* Block (bounded) until a connection is detected. 0 = connected. */
int  usbh_wait_connect(uint32_t timeout_ms);

/* Debounce, reset the port, and enumerate the attached device. */
int  usbh_enumerate(usbh_device_t *dev);

/* One control transfer to the enumerated device. setup = the 8-byte packet;
 * data may be NULL when wLength is 0. IN data lands in data[]; returns the
 * number of bytes transferred, or a negative USBH_ERR_*. */
int  usbh_control(usbh_device_t *dev, const uint8_t setup[8], void *data);
