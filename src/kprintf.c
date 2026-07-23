#include "kprintf.h"
#include "uart.h"
#include <stdint.h>

static void out_num(uint32_t v, uint32_t base, int neg, int width, char pad)
{
    char d[11];                         /* 32-bit decimal: 10 digits max */
    int n = 0;

    do {
        uint32_t q = v % base;
        d[n++] = (char)(q < 10 ? '0' + q : 'a' + (q - 10));
        v /= base;
    } while (v);

    int total = n + (neg ? 1 : 0);
    if (neg && pad == '0') uart_putc('-');      /* -00123, not 00-123 */
    for (int i = total; i < width; i++) uart_putc(pad);
    if (neg && pad != '0') uart_putc('-');
    while (n--) uart_putc(d[n]);
}

void kvprintf(const char *f, va_list ap)
{
    for (; *f; f++) {
        if (*f != '%') {
            if (*f == '\n') uart_putc('\r');
            uart_putc(*f);
            continue;
        }
        f++;
        char pad = ' ';
        int width = 0;
        if (*f == '0') { pad = '0'; f++; }
        while (*f >= '0' && *f <= '9') { width = width * 10 + (*f - '0'); f++; }
        if (*f == 'l') f++;             /* long == int on ILP32; accept %lu/%lx */

        switch (*f) {
        case 'c': uart_putc((char)va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s) uart_putc(*s++);
            break;
        }
        case 'd': {
            int32_t x = va_arg(ap, int32_t);
            uint32_t mag = x < 0 ? 0u - (uint32_t)x : (uint32_t)x;
            out_num(mag, 10, x < 0, width, pad);
            break;
        }
        case 'u': out_num(va_arg(ap, uint32_t), 10, 0, width, pad); break;
        case 'x': out_num(va_arg(ap, uint32_t), 16, 0, width, pad); break;
        case '%': uart_putc('%'); break;
        default:  uart_putc('?'); break;
        }
    }
}

void kprintf(const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    kvprintf(f, ap);
    va_end(ap);
}
