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

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    char *p = dst;
    while (n && *src) { *p++ = *src++; n--; }
    while (n--) *p++ = 0;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    while (n--) {
        if (*p != *q) return (int)*p - (int)*q;
        p++; q++;
    }
    return 0;
}
