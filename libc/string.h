#ifndef SOL_LIBC_STRING_H
#define SOL_LIBC_STRING_H

#include <stddef.h>

void  *memcpy(void *dest, const void *src, size_t n);
void  *memset(void *dest, int c, size_t n);
void  *memmove(void *dest, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
char  *strcpy(char *dest, const char *src);

#endif /* SOL_LIBC_STRING_H */
