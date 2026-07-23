/* STM32H745ZI, Cortex-M7 side.
 * Code in flash bank 1; data + every stack in DTCM (zero-wait-state, core-private,
 * not reachable by DMA — no coherency concerns).
 * RM0399 §2.3: flash bank 1 = 1 MiB @0x08000000, DTCM = 128 KiB @0x20000000.
 */
MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K
  DTCM  (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

ENTRY(Reset_Handler)

_estack = ORIGIN(DTCM) + LENGTH(DTCM);   /* MSP: boot + all exception handlers */

SECTIONS
{
  /* Must stay first: VTOR points at 0x08000000, which needs 1 KiB alignment
   * for a 166-entry table (ARMv7-M B1.5.3) — ORIGIN(FLASH) satisfies it. */
  .vectors : {
    KEEP(*(.vectors))
  } > FLASH

  .text : {
    *(.text*)
    *(.rodata*)
    . = ALIGN(4);
  } > FLASH

  /* libgcc can drag in unwind index entries even freestanding; keep them. */
  .ARM.exidx : { *(.ARM.exidx*) } > FLASH

  _sidata = LOADADDR(.data);
  .data : {
    . = ALIGN(4);
    _sdata = .;
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } > DTCM AT> FLASH

  .bss : {
    . = ALIGN(4);
    _sbss = .;
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } > DTCM

  /* Fail the link if statics leave less than 8 KiB for the MSP. */
  ASSERT(_estack - _ebss >= 8K, "MSP headroom below 8 KiB")
}
