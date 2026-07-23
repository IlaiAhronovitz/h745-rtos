#pragma once
#include <stdarg.h>

/* Minimal printf to the UART: %c %s %d %u %x (+ optional 'l' length modifier —
 * uint32_t is 'unsigned long' on arm-none-eabi — and zero-pad width, e.g. %08lx).
 * "\n" is expanded to "\r\n". No heap, no libc, not reentrancy-locked —
 * callers that share the UART serialize with a semaphore (see app).
 */
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kvprintf(const char *fmt, va_list ap);
