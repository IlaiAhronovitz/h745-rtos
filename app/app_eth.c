/* app_eth.c — Ethernet demo running on the kernel.  make APP=eth flash
 *
 * Three tasks share the driver:
 *
 *   prio 2  rx    drains the receive ring, prints a summary of the first few
 *                 frames, and hands every frame to net_handle_frame()
 *   prio 3  link  watches the PHY, and starts the MAC once the link is up
 *   prio 4  tx    sends one broadcast frame per second and reports counters
 *
 * The kernel supplies its own idle task, so nothing here has to.
 *
 * rx sits at the highest priority of the three because the receive ring is
 * only four descriptors deep: whenever a frame is waiting, draining it is the
 * most urgent thing in the system.
 *
 * Expected at boot (LAN8742A, DS00002165 section 3.9):
 *   ID1 (reg 2) = 0x0007
 *   ID2 (reg 3) = 0xC13x   -- model 0x13, low nibble is the silicon revision
 */
#include <stdint.h>
#include <stdarg.h>
#include "stm32h745.h"
#include "uart.h"
#include "board.h"
#include "kprintf.h"
#include "kernel.h"
#include "sem.h"
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
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,        /* dst: broadcast            */
    0x02, 0x00, 0x00, 0x74, 0x50, 0x01,        /* src: locally administered */
    0x88, 0xB5,                                 /* EtherType                */
    'h','7','4','5','-','r','t','o','s',' ','h','e','l','l','o'
};

/* ---- shared state --------------------------------------------------------
 * kprintf is not reentrant and a line is several calls long, so tasks
 * serialise on a binary semaphore -- the same arrangement app_demo.c uses.
 *
 * eth_running is written by the link task and read by the other two. It is a
 * single word, written once, so a plain volatile flag is sufficient; anything
 * richer than that would need the kernel lock.
 */
static k_sem_t print_lock;
static volatile int      eth_running;
static volatile uint32_t frames, total, sent;

/* Signalled by the Ethernet ISR, waited on by the rx task. Starts at 0: the
 * task blocks until a frame actually arrives. */
static k_sem_t rx_sem;

/* Installed into eth_rx_notify, so it runs in interrupt context. k_sem_give()
 * is documented as safe from an ISR at or below the kernel's BASEPRI level,
 * which is the priority eth_irq_enable() was given. */
static void rx_notify(void)
{
    k_sem_give(&rx_sem);
}


__attribute__((format(printf, 1, 2)))
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    k_sem_take(&print_lock);
    kvprintf(fmt, ap);
    k_sem_give(&print_lock);
    va_end(ap);
}

/* ---- frame summaries -----------------------------------------------------
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

/* Takes the print lock around the whole line: a summary is several kprintf
 * calls, and another task printing in between would interleave them. */
static void print_frame(const uint8_t *f, uint16_t len)
{
    /* The EtherType is big-endian on the wire and this core is little-endian,
     * so it is assembled by hand rather than read as a uint16_t. */
    uint16_t type = (uint16_t)((f[12] << 8) | f[13]);

    k_sem_take(&print_lock);
    print_mac(f);
    kprintf(" <- ");
    print_mac(f + 6);
    kprintf("  type=%04x %s  %u B\n", type, ethertype_name(type), len);
    k_sem_give(&print_lock);
}

/* ---- tasks ---------------------------------------------------------------*/

static void link_body(void *arg)
{
    (void)arg;

    int link_prev = -1;

    for (;;) {
        uint16_t bsr      = eth_mdio_read(ETH_PHY_ADDR, PHY_REG_BSR);
        int      link_now = (bsr & PHY_BSR_LINK) ? 1 : 0;

        /* link_prev starts at -1, a value the link bit can never hold, so the
         * first reading always counts as a change and reports once. */
        if (link_now != link_prev) {
            say("Link %s\n", link_now ? "up" : "down");
            link_prev = link_now;
            board_led(LED_GREEN, link_now);
        }

        /* The MAC's speed and duplex must match what the PHY negotiated, so
         * it cannot be started until the link is up. */
        if (link_now && !eth_running) {
            uint16_t scsr = eth_mdio_read(ETH_PHY_ADDR, PHY_REG_SCSR);
            unsigned hcd  = (scsr >> PHY_SCSR_HCD_SHIFT) & PHY_SCSR_HCD_MASK;
            unsigned s100 = (hcd & PHY_HCD_100)  ? 1u : 0u;
            unsigned fd   = (hcd & PHY_HCD_FULL) ? 1u : 0u;

            say("negotiated %s Mbps, %s duplex -- starting MAC\n",
                s100 ? "100" : "10", fd ? "full" : "half");

            eth_start(s100, fd);
            eth_irq_enable(K_BASEPRI_MASK);
            say("MAC started (swr_timeout=%u)\n", eth_swr_timed_out);

            eth_running = 1;
        }

        k_sleep_ms(200);
    }
}

static void rx_body(void *arg)
{
    (void)arg;

    /* static, not on the stack: 1536 bytes would be a large fraction of this
     * task's stack, and there is only ever one of these tasks. */
    static uint8_t rx_frame[ETH_BUF_SIZE];

    for (;;) {
        /* Blocks with zero CPU cost until the ISR signals. Before the MAC
         * starts no interrupt can fire, so this simply stays blocked --
         * which is why no eth_running check is needed here.
         *
         * The drain loop matters: the semaphore guarantees at least one
         * frame is waiting, not exactly one. Handling a single frame per
         * wake would fall behind on a burst, and with four descriptors the
         * ring would overflow. */
        k_sem_take(&rx_sem);
        uint16_t len;
        while ((len = eth_rx_poll(rx_frame, sizeof rx_frame)) != 0u) {
            frames++;
            total += len;

            if (frames <= 5u) {
                print_frame(rx_frame, len);
            }

            net_handle_frame(rx_frame, len);
        }
    }
}

static void tx_body(void *arg)
{
    (void)arg;

    for (;;) {
        if (eth_running) {
            if (eth_tx_send(tx_frame, (uint16_t)sizeof tx_frame) == 0) {
                sent++;
            }

            say("tx %lu | rx %lu frames, %lu bytes | rx irqs %lu\n",
                (unsigned long)sent,
                (unsigned long)frames, (unsigned long)total,
                (unsigned long)eth_rx_irq_count);
        }

        k_sleep_ms(1000);
    }
}

/* ---- bring-up and start --------------------------------------------------*/

#define STACK_WORDS 512u

static k_task_t t_link, t_rx, t_tx;
static uint32_t s_link[STACK_WORDS] __attribute__((aligned(8)));
static uint32_t s_rx[STACK_WORDS]   __attribute__((aligned(8)));
static uint32_t s_tx[STACK_WORDS]   __attribute__((aligned(8)));

int main(void)
{
    board_init();
    uart_init(115200);

    kprintf("\n=== h745-rtos: ethernet on the kernel ===\n");
    kprintf("HCLK %lu MHz, PHY addr %u\n",
            (unsigned long)(SYS_CLK_HZ / 1000000u), ETH_PHY_ADDR);

    /* Driver bring-up happens before the scheduler starts: it is a fixed
     * sequence with ordering constraints (see eth.c) and nothing about it
     * benefits from concurrency. Order matters. */
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

    k_sem_init(&print_lock, 1);
    k_sem_init(&rx_sem, 0);

    /* Install the hook before enabling the interrupt, so it can never fire
     * with eth_rx_notify still NULL. */
    eth_rx_notify = rx_notify;

    k_task_create(&t_rx,   "rx   ", rx_body,   0, 2, s_rx,   STACK_WORDS);
    k_task_create(&t_link, "link ", link_body, 0, 3, s_link, STACK_WORDS);
    k_task_create(&t_tx,   "tx   ", tx_body,   0, 4, s_tx,   STACK_WORDS);

    k_start();                  /* never returns */
}
