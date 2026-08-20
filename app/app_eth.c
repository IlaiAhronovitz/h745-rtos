/* app_eth.c — Ethernet demo.  Build and run with:  make APP=eth flash
 *
 * Brings the link up, reports what the PHY negotiated, then runs three things
 * concurrently on a 1 ms poll loop:
 *
 *   - receives every frame on the wire (the MAC is in promiscuous mode) and
 *     prints a one-line summary of the first few;
 *   - answers ARP requests and pings for 192.168.137.50;
 *   - transmits one broadcast frame per second as a liveness check.
 *
 * Deliberately a bare main() rather than a kernel task: when something
 * misbehaves here, it is the Ethernet peripheral misbehaving and not the
 * scheduler. Moving this onto the RTOS is a later exercise.
 *
 * Expected at boot (LAN8742A, DS00002165 section 3.9):
 *   ID1 (reg 2) = 0x0007
 *   ID2 (reg 3) = 0xC13x   -- model 0x13, low nibble is the silicon revision
 */
#include <stdint.h>
#include "stm32h745.h"
#include "uart.h"
#include "board.h"
#include "kprintf.h"
#include "eth.h"
#include "net.h"

/* A minimal frame to transmit once a second.
 *
 * Broadcast destination, and a source address with bit 1 of the first byte set
 * (0x02) marking it locally administered rather than globally assigned.
 * EtherType 0x88B5 is reserved by IEEE for local experimental use, so nothing
 * on the network will try to interpret the payload.
 *
 * The MAC appends the CRC and pads this to the 60-byte minimum by itself, so
 * only the real content appears here.
 */
static const uint8_t tx_frame[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,        /* dst: broadcast          */
    0x02, 0x00, 0x00, 0x74, 0x50, 0x01,        /* src: locally administered */
    0x88, 0xB5,                                 /* EtherType               */
    'h','7','4','5','-','r','t','o','s',' ','h','e','l','l','o'
};

/* ---- Frame summaries -----------------------------------------------------
 * Every Ethernet frame begins with the same 14 bytes, whatever the payload:
 *
 *   bytes  0..5   destination MAC
 *   bytes  6..11  source MAC
 *   bytes 12..13  EtherType
 *
 * That is the same header a packet analyser shows on its summary line.
 */
static void print_mac(const uint8_t *m)
{
    kprintf("%02x:%02x:%02x:%02x:%02x:%02x",
            m[0], m[1], m[2], m[3], m[4], m[5]);
}

static const char *ethertype_name(uint16_t t)
{
    switch (t) {
    case 0x0800u: return "IPv4";
    case 0x0806u: return "ARP";
    case 0x86DDu: return "IPv6";
    case 0x88B5u: return "ours";
    default:      return "?";
    }
}

static void print_frame(const uint8_t *f, uint16_t len)
{
    /* The EtherType is big-endian on the wire and this core is little-endian,
     * so it is assembled by hand rather than read as a uint16_t. */
    uint16_t type = (uint16_t)((f[12] << 8) | f[13]);

    print_mac(f);
    kprintf(" <- ");
    print_mac(f + 6);
    kprintf("  type=%04x %s  %u B\n", type, ethertype_name(type), len);
}

/* Busy-wait milliseconds using the DWT cycle counter, which system_init()
 * already switched on. No kernel here, so no k_sleep_ms(). */
static void delay_ms(uint32_t ms)
{
    uint32_t start = DWT_CYCCNT;
    uint32_t ticks = ms * (SYS_CLK_HZ / 1000u);

    while ((DWT_CYCCNT - start) < ticks) {
        /* spin */
    }
}

int main(void)
{
    board_init();
    uart_init(115200);

    kprintf("\n=== h745-rtos: bare-metal ethernet ===\n");
    kprintf("HCLK %lu MHz, PHY addr %u\n",
            (unsigned long)(SYS_CLK_HZ / 1000000u), ETH_PHY_ADDR);

    /* Order matters: see the header comment in eth.c. */
    eth_mem_init();
    eth_rings_init();
    eth_pins_init();
    eth_mdio_init();

    uint16_t id1 = eth_mdio_read(ETH_PHY_ADDR, PHY_REG_ID1);
    uint16_t id2 = eth_mdio_read(ETH_PHY_ADDR, PHY_REG_ID2);
    uint16_t bsr = eth_mdio_read(ETH_PHY_ADDR, PHY_REG_BSR);

    kprintf("ID1=%04x ID2=%04x BSR=%04x\n", id1, id2, bsr);

    if (id1 == 0x0007u && (id2 & 0xFFF0u) == 0xC130u) {
        kprintf("PASS: LAN8742A responding (rev %u)\n", id2 & 0xFu);
        board_led(LED_GREEN, 1);
    } else if (id1 == 0xFFFFu || id1 == 0x0000u) {
        /* Less a failure than a finding: 0xFFFF is a floating bus with nobody
         * answering, 0x0000 is a bus held low. Those point at different
         * faults -- clock, divider or pins versus address or reset. */
        kprintf("NO ANSWER: bus reads %04x -- check MDC divider, AF11 pins,"
                " MAC clock, PHY address\n", id1);
        board_led(LED_RED, 1);
    } else {
        kprintf("UNEXPECTED ID -- something answered, but not a LAN8742A\n");
        board_led(LED_YELLOW, 1);
    }

    uart_flush();

    /* The main loop polls every 1 ms, because the RX ring is only four deep
     * and a busy wire fills that quickly. MDIO is comparatively slow, so the
     * link is only re-checked every 200th pass. */
    int      link_prev = -1;
    int      started   = 0;
    uint32_t frames    = 0;
    uint32_t total     = 0;
    uint32_t sent      = 0;
    unsigned tick      = 0;

    for (;;) {
        if ((tick++ % 200u) == 0u) {
            uint16_t bsr_now  = eth_mdio_read(ETH_PHY_ADDR, PHY_REG_BSR);
            int      link_now = (bsr_now & PHY_BSR_LINK) ? 1 : 0;

            /* link_prev starts at -1, a value the link bit can never hold, so
             * the first reading always counts as a change and reports once. */
            if (link_now != link_prev) {
                kprintf("Link %s\n", link_now ? "up" : "down");
                link_prev = link_now;
                board_led(LED_GREEN, link_now);
            }

            /* The MAC's speed and duplex must match what the PHY negotiated,
             * so it cannot be started until the link is up. */
            if (link_now && !started) {
                uint16_t scsr = eth_mdio_read(ETH_PHY_ADDR, PHY_REG_SCSR);
                unsigned hcd  = (scsr >> PHY_SCSR_HCD_SHIFT) & PHY_SCSR_HCD_MASK;
                unsigned s100 = (hcd & PHY_HCD_100)  ? 1u : 0u;
                unsigned fd   = (hcd & PHY_HCD_FULL) ? 1u : 0u;

                kprintf("negotiated %s Mbps, %s duplex -- starting MAC\n",
                        s100 ? "100" : "10", fd ? "full" : "half");
                eth_start(s100, fd);
                kprintf("MAC started (swr_timeout=%u)\n", eth_swr_timed_out);
                started = 1;
            }
        }

        if (started && (tick % 1000u) == 0u) {
            int rc = eth_tx_send(tx_frame, (uint16_t)sizeof(tx_frame));

            if (rc == 0) {
                sent++;
            }
            kprintf("tx %lu sent (last rc=%d) | rx %lu frames, %lu bytes\n",
                    (unsigned long)sent, rc,
                    (unsigned long)frames, (unsigned long)total);
        }

        if (started) {
            static uint8_t rx_frame[ETH_BUF_SIZE];
            uint16_t       len = eth_rx_poll(rx_frame, sizeof(rx_frame));

            if (len != 0u) {
                frames++;
                total += len;

                /* Only the first few: at 100 Mbit the console cannot keep up. */
                if (frames <= 12u) {
                    print_frame(rx_frame, len);
                }

                net_handle_frame(rx_frame, len);
            }
        }

        delay_ms(1);
    }
}
