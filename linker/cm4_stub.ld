/* CM4 parking stub — lives in flash bank 2, where the Cortex-M4 boots
 * (RM0399 §2.4: CM4 boot address option BOOT_CM4_ADD0 defaults to 0x08100000). */
MEMORY
{
  FLASH2 (rx) : ORIGIN = 0x08100000, LENGTH = 1024K
}

ENTRY(cm4_reset)

SECTIONS
{
  .vectors : {
    KEEP(*(.vectors))
    *(.text*)
  } > FLASH2
}
