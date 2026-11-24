/* smclog port for host builds: monotonic or virtual clock, a mutex, and the program's own build ID:
 * LC_UUID on macOS (16 bytes), the GNU build ID note on Linux (20, as the dictionary reads it). */
#include "smclog_host.h"
#include "smclog_port.h"

#include <pthread.h>
#include <string.h>
#include <time.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#elif defined(__linux__)
#include <elf.h>
#include <sys/auxv.h> /* not <link.h>: -Ilink would find firmware/link/link.h */
#else
#error "smclog host port: macOS and Linux only (the build ID's place)"
#endif

#define ID_MAX 32 /* smclog.c keeps up to 32 bytes */

static uint32_t (*clock_fn)(void);
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void smclog_host_set_clock(uint32_t (*now_us)(void))
{
    clock_fn = now_us;
}

uint32_t smclog_port_time_us(void)
{
    struct timespec ts;

    if (clock_fn)
        return clock_fn();
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u);
}

uint32_t smclog_port_lock(void)
{
    pthread_mutex_lock(&lock);
    return 0;
}

void smclog_port_unlock(uint32_t state)
{
    (void)state;
    pthread_mutex_unlock(&lock);
}

#if defined(__APPLE__)
static size_t find_build_id(uint8_t *id)
{
    const struct mach_header_64 *mh = (const struct mach_header_64 *)_dyld_get_image_header(0);
    const uint8_t *lc = (const uint8_t *)(mh + 1);

    for (uint32_t i = 0; mh && mh->magic == MH_MAGIC_64 && i < mh->ncmds; i++) {
        const struct load_command *c = (const struct load_command *)lc;
        if (c->cmd == LC_UUID) {
            memcpy(id, ((const struct uuid_command *)c)->uuid, 16);
            return 16;
        }
        lc += c->cmdsize;
    }
    return 0;
}
#else
/* The program's headers (AT_PHDR) hold its PT_NOTE segments, one with the NT_GNU_BUILD_ID note (the
 * linker's --build-id, Debian's gcc default; firmware/Makefile asks for it too). The load bias is
 * AT_PHDR less PT_PHDR's address. */
static size_t find_build_id(uint8_t *id)
{
    const Elf64_Phdr *ph = (const Elf64_Phdr *)getauxval(AT_PHDR);
    unsigned long n = getauxval(AT_PHNUM);
    uintptr_t bias = 0;

    if (!ph || sizeof(void *) != 8)
        return 0;
    for (unsigned long i = 0; i < n; i++)
        if (ph[i].p_type == PT_PHDR)
            bias = (uintptr_t)ph - (uintptr_t)ph[i].p_vaddr;
    for (unsigned long i = 0; i < n; i++) {
        const uint8_t *p, *end;
        if (ph[i].p_type != PT_NOTE)
            continue;
        p = (const uint8_t *)(bias + (uintptr_t)ph[i].p_vaddr);
        end = p + ph[i].p_memsz;
        while (p + sizeof(Elf64_Nhdr) <= end) {
            const Elf64_Nhdr *nh = (const Elf64_Nhdr *)p;
            const uint8_t *name = p + sizeof *nh;
            const uint8_t *desc = name + ((nh->n_namesz + 3u) & ~3u);
            if (nh->n_type == NT_GNU_BUILD_ID && nh->n_namesz == 4 && memcmp(name, "GNU", 4) == 0 &&
                nh->n_descsz >= 1 && nh->n_descsz <= ID_MAX) {
                memcpy(id, desc, nh->n_descsz);
                return nh->n_descsz;
            }
            p = desc + ((nh->n_descsz + 3u) & ~3u);
        }
    }
    return 0;
}
#endif

const uint8_t *smclog_port_build_id(size_t *len)
{
    static uint8_t id[ID_MAX];
    static size_t found = (size_t)-1;

    if (found == (size_t)-1)
        found = find_build_id(id);
    *len = found;
    return found ? id : NULL;
}
