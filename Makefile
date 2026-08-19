# h745-rtos — minimal preemptive RTOS kernel for STM32H745 (Cortex-M7), bare metal.
# No HAL, no CMSIS, no libc: startup, linker script, drivers and kernel are all in-tree.
#
# Machine-specific paths (this PC):
#   - ARM GNU toolchain 14.2 (not on PATH)
#   - xPack OpenOCD 0.12 (ST-Link; serial pinned — two ST-Links exist on this PC)

GCC_BIN := C:/Program Files (x86)/Arm GNU Toolchain arm-none-eabi/14.2 rel1/bin
CC      := "$(GCC_BIN)/arm-none-eabi-gcc.exe"
OBJDUMP := "$(GCC_BIN)/arm-none-eabi-objdump.exe"
SIZE    := "$(GCC_BIN)/arm-none-eabi-size.exe"

OCD_ROOT := C:/Users/ghkh1/AppData/Local/Microsoft/WinGet/Packages/xpack-dev-tools.openocd-xpack_Microsoft.Winget.Source_8wekyb3d8bbwe/xpack-openocd-0.12.0-7
OPENOCD  := $(OCD_ROOT)/bin/openocd.exe
OCD_SCR  := $(OCD_ROOT)/openocd/scripts
# On-board ST-Link V3 of the Nucleo-H745ZI-Q. Never flash without pinning the
# serial: a second ST-Link (the STM32L452 board) is often attached to this PC.
STLINK_SERIAL := 002300373331511434333834

# connect_assert_srst is mandatory here: the idle task sits in WFI, the D1 domain
# gates the debug clock, and a normal attach reads a garbage CPUID (0xa05f0000)
# then fails examination. Asserting SRST on connect catches the core before it
# reaches WFI. (RM0399 §60.3 DBGMCU; the alternative is setting DBGMCU_CR
# D1DBGCKEN/DBGSLPD1 in firmware, which only helps once firmware already runs.)
OCD := "$(OPENOCD)" -s "$(OCD_SCR)" -f interface/stlink.cfg \
       -c "adapter serial $(STLINK_SERIAL)" \
       -c "reset_config srst_only srst_nogate connect_assert_srst" \
       -f target/stm32h7x.cfg

BUILD := build
APP   ?= demo

ARCH := -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard

CFLAGS := $(ARCH) -O2 -g3 -std=c11 -Wall -Wextra -Werror \
          -ffreestanding -ffunction-sections -fdata-sections \
          -Isrc -Isrc/kernel -MMD -MP
ASFLAGS := $(ARCH) -g3
LDFLAGS := -T linker/stm32h745_cm7.ld -nostartfiles -nostdlib \
           -Wl,--gc-sections -Wl,-Map=$(BUILD)/kernel.map

SRC_C := $(wildcard src/*.c) $(wildcard src/kernel/*.c) app/app_$(APP).c
SRC_S := $(wildcard src/*.s) $(wildcard src/kernel/*.s)

OBJ := $(SRC_C:%.c=$(BUILD)/%.o) $(SRC_S:%.s=$(BUILD)/%.o)
DEP := $(OBJ:.o=.d)

.PHONY: all size flash reset stub flash-stub disasm clean

all: $(BUILD)/kernel.elf size

$(BUILD)/kernel.elf: $(OBJ) linker/stm32h745_cm7.ld
	$(CC) $(ARCH) $(OBJ) $(LDFLAGS) -lgcc -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.s
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -c $< -o $@

size: $(BUILD)/kernel.elf
	$(SIZE) $<

disasm: $(BUILD)/kernel.elf
	$(OBJDUMP) -d $< > $(BUILD)/kernel.lst

flash: all
	$(OCD) -c "program $(BUILD)/kernel.elf verify reset exit"

reset:
	$(OCD) -c "init" -c "reset run" -c "exit"

# --- CM4 parking stub -------------------------------------------------------
# The H745 boots BOTH cores; flash bank 2 (0x08100000) still holds whatever the
# CM4 last ran (the factory demo drives the Nucleo LEDs). Park it once.
stub: $(BUILD)/cm4_stub.elf

$(BUILD)/cm4_stub.elf: stub/cm4_stub.s linker/cm4_stub.ld
	@mkdir -p $(@D)
	$(CC) -mcpu=cortex-m4 -mthumb -g3 -nostartfiles -nostdlib \
	  -T linker/cm4_stub.ld stub/cm4_stub.s -o $@

flash-stub: stub
	$(OCD) -c "program $(BUILD)/cm4_stub.elf verify reset exit"

clean:
	rm -rf $(BUILD)

-include $(DEP)
