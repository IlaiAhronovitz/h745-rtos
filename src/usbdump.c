/* usbdump.c — full USB descriptor tree dump.
 *
 * The composition baseline: every interface (INCLUDING alternate settings,
 * which the AT-port walker deliberately skips) and every endpoint, with the
 * class triple spelled out. This is the artifact to diff a composition
 * change against — e.g. before/after AT+QCFG="usbnet",<n> — so it prints the
 * raw bytes too, not just the decoded view.
 */
#include "usbdump.h"
#include "kprintf.h"

static const char *class_name(uint8_t c, uint8_t sub, uint8_t proto)
{
    if (c == 0x02 && sub == 0x06) return "CDC-ECM control";
    if (c == 0x02 && sub == 0x0E) return "CDC-MBIM control";
    if (c == 0x02)                return "CDC control";
    if (c == 0x0A)                return "CDC data";
    if (c == 0x08)                return "mass storage";
    if (c == 0x03)                return "HID";
    if (c == 0xFF && sub == 0xFF && proto == 0xFF) return "vendor (DIAG/QMI)";
    if (c == 0xFF && sub == 0x00 && proto == 0x00) return "vendor serial";
    if (c == 0xFF)                return "vendor";
    return "?";
}

static const char *ep_type(uint8_t attr)
{
    switch (attr & 3u) {
    case 0:  return "control";
    case 1:  return "iso";
    case 2:  return "bulk";
    default: return "interrupt";
    }
}

void usbdump_config(const usbh_device_t *d)
{
    const uint8_t *p   = d->cfg_desc;
    uint16_t       len = d->cfg_len;

    kprintf("\n===== USB COMPOSITION BASELINE =====\n");
    kprintf("device  %04x:%04x  class %02x/%02x/%02x  ep0 mps %u  %s speed\n",
            d->vid, d->pid, d->dev_desc[4], d->dev_desc[5], d->dev_desc[6],
            d->mps0, d->speed == 2u ? "low" : "full");
    kprintf("config  %u of %u bytes stored\n", d->cfg_len, d->cfg_total);

    for (uint16_t i = 0; i + 1u < len && p[i] >= 2u; i += p[i]) {
        const uint8_t *q = p + i;
        switch (q[1]) {
        case 2:
            kprintf("CONFIG  #%u  %u interfaces  %u mA  attr %02x\n",
                    q[5], q[4], q[8] * 2u, q[7]);
            break;
        case 4:
            kprintf("  IF %u alt %u  class %02x/%02x/%02x (%s)  %u endpoints\n",
                    q[2], q[3], q[5], q[6], q[7],
                    class_name(q[5], q[6], q[7]), q[4]);
            break;
        case 5:
            kprintf("      EP %02x  %s  mps %u  interval %u\n",
                    q[2], ep_type(q[3]),
                    (unsigned)q[4] | ((unsigned)q[5] << 8), (unsigned)q[6]);
            break;
        case 11:
            kprintf("  IAD if %u..%u  class %02x/%02x/%02x\n",
                    q[2], (unsigned)(q[2] + q[3] - 1u), q[4], q[5], q[6]);
            break;
        case 0x24:
            /* CDC functional descriptors — the ECM ones carry the MAC string
             * index and the max segment size, so print their subtype + bytes */
            kprintf("      CDC func subtype %02x:", q[2]);
            for (uint8_t k = 3; k < q[0]; k++) kprintf(" %02x", q[k]);
            kprintf("\n");
            break;
        default:
            kprintf("      desc type %02x len %u\n", q[1], q[0]);
            break;
        }
    }

    kprintf("--- raw config descriptor (%u bytes) ---\n", len);
    for (uint16_t i = 0; i < len; i++)
        kprintf("%02x%s", p[i], ((i & 15u) == 15u) ? "\n" : " ");
    if (len & 15u) kprintf("\n");
    kprintf("===== END BASELINE =====\n\n");
}
