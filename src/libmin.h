/* libmin.h — the handful of libc functions this project implements itself.
 *
 * The definitions are in libmin.c. gcc emits calls to memcpy/memset on its own
 * (struct assignment, array init), so these must exist even in code that never
 * names them -- which is why they are not simply static helpers.
 */
#pragma once
#include <stddef.h>

void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);

/* String trio for the AT engine (atc.c). */
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
char  *strncpy(char *dst, const char *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
