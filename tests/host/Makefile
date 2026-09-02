# Host-side simulation tests for src/usbh.c (run under WSL/Linux gcc).
#   make && ./test_m1
# -no-pie keeps statics below 4 GB so the driver's 32-bit HCDMA values are
# lossless host pointers (sim_reset() asserts this at startup).
# -O0 -g3: no optimization + full debug info, so single-stepping in gdb maps
# 1:1 to the source and every variable reads its true value (at -O1 the
# stepping is jumpy and variables can show stale register copies).
CC     := gcc
CFLAGS := -O0 -g3 -Wall -Wextra -no-pie -fno-pie \
          -Ishim -I. -I../../src

all: test_m1 test_m2 test_m3 test_m4

# Driver sources are compiled from copies in build/ so their quoted #include
# of stm32h745.h / libmin.h resolves to the shim (a quoted include searches
# the including file's own directory before any -I path — the originals
# would find the real ARM headers sitting next to them).
build/%.c: ../../src/%.c
	mkdir -p build
	cp $< $@

test_m1: test_m1.c sim_hw.c build/usbh.c shim/stm32h745.h sim_hw.h
	$(CC) $(CFLAGS) test_m1.c sim_hw.c build/usbh.c -o $@

test_m2: test_m2.c sim_hw.c build/usbh.c build/atc.c shim/stm32h745.h sim_hw.h
	$(CC) $(CFLAGS) test_m2.c sim_hw.c build/usbh.c build/atc.c -o $@

test_m3: test_m3.c sim_hw.c build/usbh.c build/atc.c build/modem.c shim/stm32h745.h sim_hw.h
	$(CC) $(CFLAGS) test_m3.c sim_hw.c build/usbh.c build/atc.c build/modem.c -o $@

test_m4: test_m4.c sim_hw.c build/usbh.c build/qmi.c shim/stm32h745.h sim_hw.h
	$(CC) $(CFLAGS) test_m4.c sim_hw.c build/usbh.c build/qmi.c -o $@

clean:
	rm -rf test_m1 test_m2 test_m3 test_m4 build
