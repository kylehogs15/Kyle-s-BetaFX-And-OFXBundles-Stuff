/* Minimal memset/memcpy so the DLL needs no C runtime (compilers may emit calls to these). */
#include <stddef.h>

__attribute__((used)) void *memset(void *d, int c, size_t n)
{
    volatile unsigned char *p = (volatile unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

__attribute__((used)) void *memcpy(void *d, const void *s, size_t n)
{
    volatile unsigned char *p = (volatile unsigned char *)d;
    const unsigned char *q = (const unsigned char *)s;
    while (n--) *p++ = *q++;
    return d;
}
int _fltused = 0;
