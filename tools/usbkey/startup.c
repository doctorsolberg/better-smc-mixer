/*
 * Vector table, reset handler and the few C library functions the firmware and TinyUSB need
 * (there is no C library: the toolchain provides only libgcc).
 */
#include <stddef.h>
#include <stdint.h>

extern uint32_t __data_start[], __data_end[], __data_load[], __bss_start[], __bss_end[];
extern uint32_t __stack_top[];

int main(void);

void reset_handler(void)
{
    uint32_t *src = __data_load;
    for (uint32_t *p = __data_start; p < __data_end; p++)
        *p = *src++;
    for (uint32_t *p = __bss_start; p < __bss_end; p++)
        *p = 0;
    main();
    for (;;)
        ;
}

/* No interrupt is enabled; a fault stops here, where a debugger finds it. */
static void default_handler(void)
{
    for (;;)
        ;
}

/* Initial stack pointer, then 15 system exceptions and 26 interrupts. */
__attribute__((section(".vectors"), used))
static void (*const vectors[16 + 26])(void) = {
    (void (*)(void))__stack_top,
    reset_handler,
    [2 ... 16 + 25] = default_handler,
};

/* Built with -fno-builtin and without loop-to-call transformation (Makefile), so these do not
 * call themselves. */
void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        while (n--)
            d[n] = s[n];
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (; n; n--, x++, y++) {
        if (*x != *y)
            return *x - *y;
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}
