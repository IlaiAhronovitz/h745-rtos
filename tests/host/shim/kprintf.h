/* Host shim: kprintf goes to stdout (implemented in sim_hw.c). */
#pragma once
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kvprintf(const char *fmt, __builtin_va_list ap);
