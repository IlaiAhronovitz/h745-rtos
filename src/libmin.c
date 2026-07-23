/* Freestanding build still needs these: GCC emits calls to memset/memcpy for
 * struct init and array fills even with -ffreestanding. */
#include <stddef.h>

void *memset(void *dst, int c, size_t n)
{
    unsigned char *p = dst;
    while (n--) *p++ = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *p = dst;
    const unsigned char *q = src;
    while (n--) *p++ = *q++;
    return dst;
}
