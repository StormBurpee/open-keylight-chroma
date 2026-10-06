#include <stddef.h>
void *memcpy(void *destination, const void *source, size_t size) {
    unsigned char *to = destination; const unsigned char *from = source;
    while (size--) *to++ = *from++;
    return destination;
}
void *memset(void *destination, int value, size_t size) {
    unsigned char *to = destination;
    while (size--) *to++ = (unsigned char)value;
    return destination;
}
/* ARM EABI helpers have void return conventions. */
void __aeabi_memcpy(void *to, const void *from, size_t size) { (void)memcpy(to, from, size); }
void __aeabi_memcpy4(void *to, const void *from, size_t size) { (void)memcpy(to, from, size); }
void __aeabi_memclr(void *to, size_t size) { (void)memset(to, 0, size); }
void __aeabi_memclr4(void *to, size_t size) { (void)memset(to, 0, size); }
