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
