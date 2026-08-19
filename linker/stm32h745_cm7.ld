/* STM32H745ZI, Cortex-M7 side.
 * Code in flash bank 1; data + every stack in DTCM (zero-wait-state, core-private,
 * not reachable by DMA — no coherency concerns).
 * RM0399 §2.3: flash bank 1 = 1 MiB @0x08000000, DTCM = 128 KiB @0x20000000.
 */
MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K
  DTCM  (rwx) : ORIGIN = 0x20000000, LENGTH = 128K

  /* D2 domain SRAM3 (RM0399 p.136: 0x30040000-0x30047FFF).
   * The ETH DMA cannot reach DTCM, so descriptors and packet buffers must
   * live out here where both the CPU and the DMA can see them. */
  SRAM3 (rw)  : ORIGIN = 0x30040000, LENGTH = 32K
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

  /* Anything tagged __attribute__((section(".eth_ram"))) lands in SRAM3.
   * NOLOAD: this is RAM the DMA scribbles on, there is nothing to copy from
   * flash at boot, so do not waste image space on it. ALIGN(32) matches the
   * Cortex-M7 cache line, which keeps the MPU region tidy. */
  .eth_ram (NOLOAD) : {
    . = ALIGN(32);
    _seth_ram = .;
    *(.eth_ram*)
    . = ALIGN(32);
    _eeth_ram = .;
  } > SRAM3

  /* Fail the link if statics leave less than 8 KiB for the MSP. */
  ASSERT(_estack - _ebss >= 8K, "MSP headroom below 8 KiB")
}
