/* The handful of <string.h> functions vendored ESP-IDF code needs.
 *
 * We link -nostdlib, so there is no libc. libgcc supplies arithmetic helpers
 * only. These are also emitted implicitly by GCC for struct assignment and
 * array initialisation, so they would be needed even if no source called them
 * by name.
 *
 * The ROM has no memset/memcmp/memcpy at a documented address in
 * esp32p4.rom.eco0_4.ld, so unlike printf and the flash reads these cannot be
 * borrowed and have to exist here.
 *
 * Deliberately simple. Byte-at-a-time is slower than a word-wise version, but
 * the callers are MSPI timing tuning (a few tens of bytes per comparison) and
 * struct copies, none of it on a path where throughput matters. The one place
 * bulk speed WOULD matter -- moving 5.5 MB of kernel out of flash -- does not
 * come through here: esp_rom_spiflash_read() writes into PSRAM directly.
 */

#include <stddef.h>
#include <stdint.h>

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    while (n--) {
        *d++ = (unsigned char)c;
    }
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    while (n--) {
        if (*x != *y) {
            return (int)*x - (int)*y;
        }
        x++;
        y++;
    }
    return 0;
}

/* memmove must handle overlap; memcpy above does not. GCC can emit this one
 * too, so define it rather than discover the difference at runtime. */
void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    if (d == s || n == 0) {
        return dst;
    }
    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dst;
}
