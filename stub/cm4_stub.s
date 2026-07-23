/* Cortex-M4 parking stub.
 * The H745 boots both cores. Bank 2 shipped with ST demo firmware that drives
 * the Nucleo LEDs; this image replaces it so the M4 sleeps forever and the M7
 * owns the whole board.
 */
    .syntax unified
    .cpu cortex-m4
    .thumb

    .section .vectors, "a", %progbits
    .word 0x10048000          /* SP: top of D2 SRAM, 288 KiB @0x10000000 */
    .word cm4_reset

    .text
    .thumb_func
    .global cm4_reset
cm4_reset:
    wfi
    b cm4_reset
