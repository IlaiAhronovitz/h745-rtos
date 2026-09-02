/* modem.h — EC25 hardening layer: everything the field taught us, above the
 * raw USB host and the AT engine. Encodes the state machine and init policy
 * from 5g-h745-modem-kb/EC25-ISSUES.md (67 catalogued issues).
 *
 * Split of responsibility:
 *   usbh.c  — enumerate, configure, bulk pipes         (transport)
 *   atc.c   — line classification, URC dispatch, resync (protocol)
 *   modem.c — WHICH device, WHEN it is ready, WHAT init (this file: policy)
 */
#pragma once
#include <stdint.h>
#include "usbh.h"
#include "atc.h"

typedef enum {
    MODEM_ABSENT = 0,
    MODEM_EDL,          /* 05C6:9008 QDLoader — bricked/crashed, no AT ever  */
    MODEM_UNKNOWN_DEV,  /* enumerated, but not a Quectel modem               */
    MODEM_BOOTING,      /* our modem, AT not answering yet (< ~13-20 s)      */
    MODEM_READY,        /* AT answers, init normalized                       */
    MODEM_TERMINAL,     /* +CFUN: 5 (FTM/corrupt QCN) — stop retrying        */
} modem_state_t;

/* Flags a normalized modem may raise — surfaced, not fatal. */
#define MODEM_WARN_USBNET_NONZERO  (1u << 0)  /* usbnet != 0 (data mode)     */
#define MODEM_WARN_URCPORT_MOVED   (1u << 1)  /* URCs not on the AT port     */
#define MODEM_WARN_AT_PORT_FLAG    (1u << 2)  /* usbcfg at_port != 1         */

typedef struct {
    modem_state_t state;
    uint8_t  ep_at_in, ep_at_out;   /* the AT interface's bulk endpoints     */
    uint8_t  at_iface;              /* its bInterfaceNumber                   */
    char     ati[96];               /* AT+GMM / ATI identity, capped         */
    char     qgmr[48];              /* firmware string — the quirk key       */
    int8_t   usbnet;                /* AT+QCFG usbnet value, -1 if unread     */
    uint8_t  warns;                 /* MODEM_WARN_*                           */
    volatile uint8_t rebooted;      /* URC handler saw a boot burst          */
} modem_t;

/* Install the time hooks modem.c needs for the boot-window poll (same
 * now/sleep the app installs into atc). Call before modem_wait_at_ready. */
void modem_set_clock(uint32_t (*now)(void), void (*sleep)(uint32_t));

/* Classify the enumerated device by descriptor. No AT traffic. */
modem_state_t modem_classify(const usbh_device_t *dev);

/* Poll bare "AT" every 500 ms up to timeout_ms until OK (boot window: USB is
 * up at ~13 s but AT answers later; first commands are silently ignored).
 * Returns 0 when the modem answers, -1 on timeout. */
int modem_wait_at_ready(uint32_t timeout_ms);

/* Register the reset/terminal URC watcher on the atc engine. Call once, with
 * atc already initialized. */
void modem_watch(modem_t *m);

/* Normalize a booted modem into a known state and capture its identity:
 * ATE0/ATV1/ATQ0, CMEE=2, QSCLK=0, read+force QURCCFG urcport=usbat, read
 * usbnet (flag nonzero, never write 3/4), read usbcfg at_port, capture
 * ATI + QGMR. Fills m->state = MODEM_READY on success. Returns 0 / -1. */
int modem_normalize(modem_t *m);

/* True once a boot burst was seen since the last clear — the in-band
 * signature of a silent crash-reboot; the caller re-runs bring-up. */
int  modem_rebooted(modem_t *m);
void modem_clear_reboot(modem_t *m);

/* ---- data call (AT-driven RmNet, no host QMI stack) ---------------------
 * Quectel's documented path for MCU hosts: the module runs its own QMI
 * client (AT+QCRMTOOL) and we trigger the RmNet call with AT$QCRMCALL.
 * Quectel LTE&5G Linux USB Driver User Guide V2.0 s4.4: "if the developers'
 * MCU's USB Host Controller does not fully support USB interrupt type
 * endpoint, it has to use AT$QCRMCALL instead of QMI tools" -- that is us.
 * After this returns 0 the module holds an IP for ~2 MINUTES waiting for the
 * host to claim it by DHCP, then tears the call down. Move fast. */
int modem_data_call_up(void);

/* Stop the RmNet call and the in-module QMI client. */
void modem_data_call_down(void);
