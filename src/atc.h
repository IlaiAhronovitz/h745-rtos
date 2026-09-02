/* atc.h — AT command engine: a C port of the cellmux ATSession semantics.
 *
 * The rules this engine enforces were proven on real modems by cellmux:
 * every RX line goes through classification,
 * never positional parsing; the solicited-response prefix is derived from
 * the in-flight command's name; a timeout marks the session out-of-sync and
 * the next command first resyncs with a bare "AT" + drain-until-quiet; one
 * command in flight, ever.
 *
 * Kernel-free like eth.c: the environment (transport write, time, sleep) is
 * injected via atc_ops_t; the one outbound event hook is atc_rx_notify.
 * atc_feed() is the only function legal outside the single owner task.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define ATC_ETIMEDOUT  1     /* returned negated by atc_cmd/atc_run          */
#define ATC_ECLOSED    2
#define ATC_EBUSY      3
#define ATC_EAT        4     /* atc_run only: the modem answered an error    */

typedef struct {
    int      (*write)(const uint8_t *data, uint16_t len);
    uint32_t (*now_ms)(void);
    void     (*sleep_ms)(uint32_t ms);
} atc_ops_t;

typedef enum { ATC_ERR_NONE = 0, ATC_ERR_CME, ATC_ERR_CMS, ATC_ERR_FINAL }
    atc_err_kind_t;

#define ATC_RESP_BUF   1024u
#define ATC_MAX_LINES  24u
#define ATC_FINAL_MAX  64u

typedef struct {
    char     buf[ATC_RESP_BUF];          /* payload lines, NUL-separated    */
    uint16_t line_off[ATC_MAX_LINES];
    uint8_t  n_lines;
    uint8_t  overflow;
    char     final[ATC_FINAL_MAX];       /* the final result line, raw      */
    atc_err_kind_t err_kind;             /* NONE = OK                       */
    int32_t  err_code;                   /* numeric CME/CMS, -1 if verbose  */
    uint32_t elapsed_ms;
} atc_resp_t;

typedef void (*atc_urc_cb_t)(const char *line, void *ctx);

int  atc_init(const atc_ops_t *ops);

/* Blocking single-writer command execution. Returns 0 when a final result
 * arrived (an error FINAL still returns 0 — inspect resp->err_kind), or
 * -ATC_ETIMEDOUT / -ATC_ECLOSED / -ATC_EBUSY. */
int  atc_cmd(const char *cmd, atc_resp_t *resp, uint32_t timeout_ms);
/* Like atc_cmd but also returns -ATC_EAT when the modem answered an error. */
int  atc_run(const char *cmd, atc_resp_t *resp, uint32_t timeout_ms);

void atc_on_urc(const char *prefix, atc_urc_cb_t cb, void *ctx);
void atc_urc_known(const char *prefix);

/* Feed raw modem->host bytes (from the USB bulk-IN completion path). */
void atc_feed(const uint8_t *data, uint16_t len);

/* Drain ring -> line assembler -> classifier. Call from the owner task's
 * idle loop to pump URCs; atc_cmd pumps it internally while waiting.
 * Returns the number of complete lines handled. */
int  atc_process(void);

/* The single payload line, or NULL if the response has != 1 lines. */
const char *atc_resp_single(const atc_resp_t *r);

extern void (*atc_rx_notify)(void);
extern volatile uint32_t atc_rx_dropped;
extern volatile uint32_t atc_line_overflow;
extern volatile uint32_t atc_resync_count;
