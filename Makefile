# h745-rtos — minimal preemptive RTOS kernel for STM32H745 (Cortex-M7), bare metal.
# No HAL, no CMSIS, no libc: startup, linker script, drivers and kernel are all in-tree.
#
# Machine-specific paths (override on the command line or in the environment):
#   - ARM GNU toolchain 14.2 (GCC_BIN)
#   - xPack OpenOCD 0.12 (OCD_ROOT)

GCC_BIN ?= C:/Program Files (x86)/Arm GNU Toolchain arm-none-eabi/14.2 rel1/bin
CC      := "$(GCC_BIN)/arm-none-eabi-gcc.exe"
OBJDUMP := "$(GCC_BIN)/arm-none-eabi-objdump.exe"
SIZE    := "$(GCC_BIN)/arm-none-eabi-size.exe"

OCD_ROOT ?= $(LOCALAPPDATA)/Microsoft/WinGet/Packages/xpack-dev-tools.openocd-xpack_Microsoft.Winget.Source_8wekyb3d8bbwe/xpack-openocd-0.12.0-7
OPENOCD  := $(OCD_ROOT)/bin/openocd.exe
OCD_SCR  := $(OCD_ROOT)/openocd/scripts
# Pin the Nucleo's on-board ST-Link when more than one probe is attached:
#   make flash STLINK_SERIAL=<serial>      (list probes: openocd -c "adapter list")
# Left empty, OpenOCD uses the first ST-Link it finds.
STLINK_SERIAL ?=
ifneq ($(strip $(STLINK_SERIAL)),)
OCD_SERIAL := -c "adapter serial $(STLINK_SERIAL)"
endif

# connect_assert_srst is mandatory here: the idle task sits in WFI, the D1 domain
# gates the debug clock, and a normal attach reads a garbage CPUID (0xa05f0000)
# then fails examination. Asserting SRST on connect catches the core before it
# reaches WFI. (RM0399 §60.3 DBGMCU; the alternative is setting DBGMCU_CR
# D1DBGCKEN/DBGSLPD1 in firmware, which only helps once firmware already runs.)
OCD := "$(OPENOCD)" -s "$(OCD_SCR)" -f interface/stlink.cfg \
       $(OCD_SERIAL) \
       -c "reset_config srst_only srst_nogate connect_assert_srst" \
       -f target/stm32h7x.cfg

BUILD := build
APP   ?= demo

ARCH := -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard

CFLAGS := $(ARCH) -O2 -g3 -std=c11 -Wall -Wextra -Werror \
          -ffreestanding -ffunction-sections -fdata-sections \
          -Isrc -Isrc/kernel -MMD -MP
# Build-time parameters (APN, test call number, ...) go in CFLAGS_EXTRA, e.g.
#   make APP=modem CFLAGS_EXTRA='-DDATA_APN=\"<apn>\"'
CFLAGS += $(CFLAGS_EXTRA)
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
