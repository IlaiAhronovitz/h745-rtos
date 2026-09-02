/* sim_hw.h — register-level simulation of the H745's USB2 OTG core + an
 * emulated USB device, for host-side testing of src/usbh.c (unmodified).
 */
#pragma once
#include <stdint.h>

/* Faults injectable into the CORE model. */
enum sim_core_fault {
    F_NONE = 0,
    F_HSE_DEAD,        /* HSERDY never sets                                  */
    F_PLL3_DEAD,       /* PLL3RDY never sets                                 */
    F_USB33_DEAD,      /* USB33RDY never sets                                */
    F_CSRST_STUCK,     /* core soft reset never completes                    */
    F_FLUSH_STUCK,     /* FIFO flush bits never self-clear                   */
    F_CMOD_NEVER,      /* forced host mode never takes                       */
    F_NO_DMA_ARCH,     /* GHWCFG2 reports slave-only silicon                 */
    F_PENA_NEVER,      /* port reset never produces a port enable            */
};

/* Faults injectable into the DEVICE model, armed for the Nth control
 * request the device sees (0-based). */
enum sim_dev_fault {
    DF_NONE = 0,
    DF_STALL, DF_NAK, DF_TXERR, DF_DTERR, DF_BBERR,
    DF_TIMEOUT,        /* channel never halts                                */
    DF_DISCONNECT,     /* device drops off the bus mid-request              */
    DF_SHORT_GARBAGE,  /* answers with fewer bytes than a valid header      */
};

/* One entry per channel transaction the model served (for assertions). */
typedef struct {
    uint8_t  kind;      /* 'S' setup, 'I' in-data, 'O' out-data, 'i'/'o' status */
    uint8_t  dad;       /* device address field the host used                */
    uint8_t  breq;      /* bRequest of the governing setup                   */
    uint16_t wvalue;
    uint16_t len;       /* bytes moved                                       */
} sim_txn_t;

void sim_reset(void);                    /* fresh model, no device attached  */
void sim_core_fault(enum sim_core_fault f);
void sim_attach_fs(void);                /* EC25-like FS composite           */
void sim_attach_ls(void);                /* low-speed mouse (mps0 = 8)       */
void sim_attach_bigcfg(void);            /* FS device, config > 256 bytes    */
void sim_attach_edl(void);              /* 05C6:9008 QDLoader, no AT         */
void sim_detach(void);
void sim_dev_fault(int nth_request, enum sim_dev_fault f);

/* EC25 hardening model controls (test_m3). */
void sim_at_ready_after(int n_ignored);  /* first n AT lines silently dropped */
void sim_set_urcport(const char *port);  /* what QURCCFG urcport reports      */
void sim_set_usbnet(int val);            /* what QCFG usbnet reports          */
void sim_set_at_port_flag(int on);       /* usbcfg at_port field (1 = present)*/
int  sim_zlp_after_multiple(void);       /* 1 if every exact-mult OUT got ZLP */
int  sim_line_state_iface(void);         /* iface DTR/RTS was set on, -1 none */
void sim_bulk_stall(int n);              /* STALL the next n bulk transactions */
int  sim_clear_feature_count(void);      /* CLEAR_FEATURE(ENDPOINT_HALT) count */
int  sim_bulk_txn_count(void);           /* bulk transactions served           */
void sim_bulk_reset_count(void);
void sim_bulk_partial(int packets);      /* serve N packets then hang the channel */
void sim_bulk_drain(void);               /* discard anything left queued        */
void sim_bulk_push(const uint8_t *d, int n);  /* queue raw bytes on the IN ep  */

int              sim_txn_count(void);
const sim_txn_t *sim_txn(int i);
int              sim_hprt_w1c_violations(void);  /* writes with PENA=1 etc.  */
uint32_t         sim_read(uint32_t addr);        /* peek without stepping    */

/* QMI device model (test_m4): the emulated modem behind EP0. */
void sim_qmi_delay(int polls);        /* empty GET replies before the answer */
void sim_qmi_indication(int on);      /* push an indication ahead of each reply */
void sim_qmi_alias(int on);           /* decoy: same msg+txn, WRONG service      */
int  sim_qmi_requests(void);          /* QMI requests the model decoded      */
int  sim_qmi_bad_frames(void);        /* host frames that did not parse      */
int  sim_qmi_link_layer(void);        /* 1 = 802.3, 2 = raw IP               */
int  sim_qmi_net_up(void);            /* data call state in the model        */
int  sim_qmi_bind_mux(void);          /* mux id from BIND_MUX, -1 if absent   */
int  sim_qmi_bind_client(void);       /* client type from BIND_MUX            */
int  sim_qmi_bind_iface(void);        /* interface number from BIND_MUX       */

/* milestone-2 additions */
int  sim_toggle_violations(void);        /* bulk DPID vs true toggle         */
void sim_urc_push(const char *line);     /* inject an unsolicited AT line    */
