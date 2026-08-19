/* Mission 1 harness — build and run with:  make APP=eth flash
 *
 * No kernel here on purpose: bare main(), so that when something misbehaves you
 * are debugging the ETH peripheral and not the scheduler. Tasks come later.
 *
 * Expected once eth_mdio_read() works (LAN8742A, DS00002165 §3.9):
 *   ID1 (reg 2) = 0x0007
 *   ID2 (reg 3) = 0xC1_x_   -- model 0x13, low nibble is the silicon revision
 */
#include <stdint.h>
#include "stm32h745.h"
#include "uart.h"
#include "board.h"
#include "kprintf.h"
#include "eth.h"
#include "net.h"

/* Step G proof: this array is tagged into the .eth_ram section, so the
 * linker must place it in D2 SRAM3 (0x3004xxxx) and NOT in DTCM
 * (0x2000xxxx). Printing its address is how you check the linker script
 * really did what it claims. */

/* A minimal Ethernet frame to transmit: broadcast destination, a
 * locally-administered source address (the 0x02 in the first byte means
 * "not globally assigned"), and EtherType 0x88B5, which IEEE reserves for
 * local experimental use -- so nothing on the network will misinterpret it.
 * The MAC appends the CRC and pads to 60 bytes by itself. */
static const uint8_t tx_frame[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,       /* dst: broadcast    */
    0x02, 0x00, 0x00, 0x74, 0x50, 0x01,       /* src: locally admin */
    0x88, 0xB5,                                /* ethertype         */
    'h','7','4','5','-','r','t','o','s',' ','h','e','l','l','o'
};

/* ---- Frame parsing --------------------------------------------------------
 * Every Ethernet frame starts with the same 14 bytes, and they are always in
 * this order regardless of what the payload turns out to be:
 *
 *   bytes  0..5   destination MAC
 *   bytes  6..11  source MAC
 *   bytes 12..13  EtherType
 *
 * That is the header Wireshark shows on its summary line. Decode it and you
 * have the same information.
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
    /* ---- YOUR CODE ------------------------------------------------------
     * Print one line per frame, like:
     *
     *   ff:ff:ff:ff:ff:ff <- 02:00:00:74:50:01  type=88b5 ours  60 B
     *
     * Three steps:
     *
     *  1. print_mac(f);        -- the destination is the first 6 bytes, so the
     *                             array name f IS its address (d05).
     *  2. print_mac(f + 6);    -- pointer arithmetic: 6 bytes further along.
     *
     *  3. the EtherType is bytes 12 and 13, but you cannot just read a
     *     uint16_t from there. Ethernet is BIG-ENDIAN ("network byte order")
     *     and the Cortex-M7 is little-endian, so the bytes would come out
     *     swapped. Build it by hand instead:
     *
     *         uint16_t type = (uint16_t)((f[12] << 8) | f[13]);
     *
     *     That is shift-and-combine -- the mirror of the mask-and-shift you
     *     did in d01. f[12] is the HIGH byte, so it moves up 8 places.
     *
     *  4. kprintf the lot, using ethertype_name(type) for the label and len
     *     for the size.
     */
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

    eth_mem_init();      /* step G -- MPU first, before SRAM3 is used */
    eth_rings_init();    /* step H -- descriptors, after the MPU */
    eth_pins_init();     /* step E -- must run before the MAC clock comes on */
    eth_mdio_init();

    uint16_t id1 = eth_mdio_read(ETH_PHY_ADDR, PHY_REG_ID1);
    uint16_t id2 = eth_mdio_read(ETH_PHY_ADDR, PHY_REG_ID2);
    uint16_t bsr = eth_mdio_read(ETH_PHY_ADDR, PHY_REG_BSR);

    kprintf("ID1=%04x ID2=%04x BSR=%04x\n", id1, id2, bsr);

    if (id1 == 0x0007u && (id2 & 0xFFF0u) == 0xC130u) {
        kprintf("PASS: LAN8742A responding (rev %u)\n", id2 & 0xFu);
        board_led(LED_GREEN, 1);
    } else if (id1 == 0xFFFFu || id1 == 0x0000u) {
        /* Not a failure so much as a finding: 0xFFFF is a floating bus (no
         * answer at all), 0x0000 is a bus held low. They point at different
         * faults -- clock/divider/pins versus address or reset. */
        kprintf("NO ANSWER: bus reads %04x -- check MDC divider, AF11 pins,"
                " MAC clock, PHY address\n", id1);
        board_led(LED_RED, 1);
    } else {
        kprintf("UNEXPECTED ID -- something answered, but not a LAN8742A\n");
        board_led(LED_YELLOW, 1);
    }

    uart_flush();

    /* ---- STEP F: watch the link come and go ------------------------------
     * Plug an Ethernet cable into the board and this loop should notice.
     *
     * `link_prev` starts at -1 -- a value the link bit can never have -- so the
     * very first reading always counts as a change and prints once.
     *
     * Three lines are yours:
     *
     *   F1. read PHY_REG_BSR from ETH_PHY_ADDR, using your eth_mdio_read().
     *
     *   F2. turn the BSR into a 0 or a 1 in `link_now`. The mask is
     *       PHY_BSR_LINK. Careful: (bsr & PHY_BSR_LINK) gives 0 or 4, not
     *       0 or 1 -- so compare it against 0 rather than assigning it raw.
     *
     *   F3. if link_now != link_prev, kprintf the new state and store it
     *       back into link_prev. Light LED_GREEN when up if you like.
     */
    /* ---- STEP F + I: watch the link, then receive ------------------------
     * Your step F loop, with two changes:
     *   - it polls every 1 ms instead of 200 ms, because the RX ring only
     *     holds 4 frames and a busy wire fills that fast;
     *   - MDIO is slow, so the link is only re-checked every 200th pass.
     */
    int      link_prev = -1;
    int      started   = 0;
    uint32_t frames    = 0;
    uint32_t total     = 0;
    uint32_t sent      = 0;
    unsigned tick      = 0;

    for (;;) {
        if ((tick++ % 200u) == 0u) {
            uint16_t bsr      = eth_mdio_read(ETH_PHY_ADDR, PHY_REG_BSR);
            int      link_now = (bsr & PHY_BSR_LINK) ? 1 : 0;

            if (link_now != link_prev) {
                kprintf("Link %s\n", link_now ? "up" : "down");
                link_prev = link_now;
                board_led(LED_GREEN, link_now);
            }

            if (link_now && !started) {
                uint16_t scsr = eth_mdio_read(ETH_PHY_ADDR, PHY_REG_SCSR);
                unsigned hcd  = (scsr >> PHY_SCSR_HCD_SHIFT) & PHY_SCSR_HCD_MASK;
                unsigned s100 = (hcd & PHY_HCD_100)  ? 1u : 0u;
                unsigned fd   = (hcd & PHY_HCD_FULL) ? 1u : 0u;

                kprintf("negotiated %s Mbps, %s duplex -- starting MAC\n",
                        s100 ? "100" : "10", fd ? "full" : "half");
                eth_start(s100, fd);
                kprintf("MAC started (swr_timeout=%u)\n",
                        eth_swr_timed_out);
                started = 1;
            }
        }

        /* Step J: send one broadcast frame per second and report whether the
         * DMA accepted it and then finished with it (OWN back to 0). */
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

                /* Only the first few, or the console drowns at 100 Mbit. */
                if (frames <= 12u) {
                    print_frame(rx_frame, len);
                }

                net_handle_frame(rx_frame, len);
            }
        }

        delay_ms(1);
    }
}
