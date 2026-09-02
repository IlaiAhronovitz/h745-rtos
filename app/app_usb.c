/* app_usb.c — USB host bring-up + descriptor ground truth.  make APP=usb flash
 *
 * Bare main(), no kernel: milestone-1 bring-up wants the smallest possible
 * system between the register writes and the UART, the same way the Ethernet
 * driver started. The kernel joins in a later milestone.
 *
 * What it does: bring the host core up, wait for anything to be plugged into
 * CN13 (Micro-AB — needs an OTG adapter for a USB-A device), enumerate it,
 * and dump the full descriptor tree. That dump is the point: the same
 * firmware becomes the descriptor ground-truth tool for the 5G modem work,
 * where interface layout per AT+QCFG="usbnet" mode decides the driver design.
 *
 * LEDs: green = VBUS on, yellow = device enumerated, red = overcurrent/fault.
 */
#include <stdint.h>
#include "stm32h745.h"
#include "uart.h"
#include "board.h"
#include "kprintf.h"
#include "usbh.h"

static usbh_device_t dev;

/* ---- descriptor walk ----------------------------------------------------- */

static const char *class_name(uint8_t c)
{
    switch (c) {
    case 0x00: return "per-interface";
    case 0x02: return "CDC control";
    case 0x03: return "HID";
    case 0x08: return "mass storage";
    case 0x09: return "hub";
    case 0x0A: return "CDC data";
    case 0xE0: return "wireless";
    case 0xEF: return "misc/IAD";
    case 0xFF: return "vendor-specific";
    default:   return "?";
    }
}

static void dump_hex(const uint8_t *p, uint16_t n)
{
    for (uint16_t i = 0; i < n; i++) {
        kprintf("%02x%s", p[i], ((i & 15u) == 15u) ? "\n" : " ");
    }
    if (n & 15u) kprintf("\n");
}

static void dump_device(const usbh_device_t *d)
{
    const uint8_t *p = d->dev_desc;

    kprintf("\ndevice descriptor:\n");
    dump_hex(p, 18);
    kprintf("  USB %x.%02x  class %02x/%02x/%02x (%s)  ep0 mps %u\n",
            p[3], p[2], p[4], p[5], p[6], class_name(p[4]), p[7]);
    kprintf("  VID:PID %04x:%04x  bcdDevice %02x%02x  configs %u\n",
            d->vid, d->pid, p[13], p[12], p[17]);
}

static void dump_config(const usbh_device_t *d)
{
    const uint8_t *p   = d->cfg_desc;
    uint16_t       len = d->cfg_len;

    kprintf("\nconfig descriptor (%u of %u bytes):\n", len, d->cfg_total);
    dump_hex(p, len);

    for (uint16_t i = 0; i + 1u < len; i += p[i]) {
        if (p[i] < 2u) break;                    /* corrupt length guard */
        const uint8_t *q = p + i;

        switch (q[1]) {
        case 2:
            kprintf("  config %u: %u interfaces, %s%s, %u mA\n",
                    q[5], q[4],
                    (q[7] & 0x40u) ? "self-powered" : "bus-powered",
                    (q[7] & 0x20u) ? "+remote-wake" : "",
                    q[8] * 2u);
            break;
        case 4:
            kprintf("  if %u alt %u: class %02x/%02x/%02x (%s), %u eps\n",
                    q[2], q[3], q[5], q[6], q[7], class_name(q[5]), q[4]);
            break;
        case 5:
            kprintf("    ep %02x %s mps %u interval %u\n",
                    q[2],
                    (const char *[]){"ctrl", "iso", "bulk", "intr"}[q[3] & 3u],
                    (unsigned)(q[4] | (q[5] << 8)), q[6]);
            break;
        case 11:
            kprintf("  IAD: if %u..%u class %02x/%02x/%02x\n",
                    q[2], (unsigned)(q[2] + q[3] - 1u), q[4], q[5], q[6]);
            break;
        default:
            kprintf("  desc type %02x len %u\n", q[1], q[0]);
            break;
        }
    }
}

/* ---- main ---------------------------------------------------------------- */

int main(void)
{
    board_init();
    uart_init(115200);

    kprintf("\n=== h745-rtos: usb host (APP=usb) ===\n");
    kprintf("HCLK %lu MHz, USB2 OTG_FS on CN13, 48 MHz from HSE/PLL3\n",
            (unsigned long)(SYS_CLK_HZ / 1000000u));

    if (usbh_init() != USBH_OK) {
        board_led(LED_RED, 1);
        kprintf("usbh_init FAILED -- halting\n");
        uart_flush();
        for (;;) cpu_wfi();
    }
    board_led(LED_GREEN, 1);            /* VBUS on */

    for (;;) {
        kprintf("\nwaiting for a device on CN13 (OTG adapter + anything USB)...\n");
        uart_flush();
        while (usbh_wait_connect(1000) != USBH_OK) {}

        kprintf("connect detected -- enumerating\n");
        int r = usbh_enumerate(&dev);
        if (r == USBH_OK) {
            board_led(LED_YELLOW, 1);
            kprintf("ENUMERATED: addr %u, %s speed, VID:PID %04x:%04x\n",
                    dev.addr, (dev.speed == 2u) ? "low" : "full",
                    dev.vid, dev.pid);
            dump_device(&dev);
            dump_config(&dev);
        } else {
            kprintf("enumeration FAILED (%d)\n", r);
        }
        uart_flush();

        while (usbh_connected()) {}     /* wait for unplug */
        board_led(LED_YELLOW, 0);
        kprintf("disconnected\n");
    }
}
