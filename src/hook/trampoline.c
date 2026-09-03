#include "hook/trampoline.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#define TRAMPOLINE_X86_64 1
#endif

#ifdef TRAMPOLINE_X86_64

/* Layout of the page. The data sits at fixed offsets so the code can reach it
   with constant rip-relative displacements.

     0   code
     64  armed flag, 8 bytes
     72  target pointer, 8 bytes */
#define TRAMPOLINE_PAGE 4096u
#define TRAMPOLINE_ARMED 64u
#define TRAMPOLINE_TARGET 72u

static const uint8_t k_prologue[] = {
    0xF3, 0x0F, 0x1E, 0xFA,                          /* endbr64                   */
    0x48, 0x83, 0x3D, 0x34, 0x00, 0x00, 0x00, 0x00,  /* cmp qword [rip+0x34], 0   */
    0x74, 0x06,                                      /* je   +6 (to the return)   */
    0xFF, 0x25, 0x34, 0x00, 0x00, 0x00,              /* jmp  [rip+0x34]           */
};

/* System V and Microsoft x64 agree on where a return value goes: integers and
   references in rax, float in xmm0, double in xmm0. */
static size_t emit_zero_return(uint8_t *out, char return_kind)
{
    switch (return_kind)
    {
    case 'V':
        out[0] = 0xC3; /* ret */
        return 1;
    case 'J':
    case 'L':
    case '[':
        out[0] = 0x48; /* xor rax, rax */
        out[1] = 0x31;
        out[2] = 0xC0;
        out[3] = 0xC3;
        return 4;
    case 'F':
        out[0] = 0x0F; /* xorps xmm0, xmm0 */
        out[1] = 0x57;
        out[2] = 0xC0;
        out[3] = 0xC3;
        return 4;
    case 'D':
        out[0] = 0x66; /* xorpd xmm0, xmm0 */
        out[1] = 0x0F;
        out[2] = 0x57;
        out[3] = 0xC0;
        out[4] = 0xC3;
        return 5;
    default:
        out[0] = 0x31; /* xor eax, eax, which also zeroes the upper half */
        out[1] = 0xC0;
        out[2] = 0xC3;
        return 3;
    }
}

static void *reserve_page(void)
{
#if defined(_WIN32)
    return VirtualAlloc(NULL, TRAMPOLINE_PAGE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void *page = mmap(NULL, TRAMPOLINE_PAGE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return page == MAP_FAILED ? NULL : page;
#endif
}

static bool protect_page(void *page, bool executable)
{
#if defined(_WIN32)
    DWORD previous = 0;
    return VirtualProtect(page, TRAMPOLINE_PAGE,
                          executable ? PAGE_EXECUTE_READ : PAGE_READWRITE, &previous) != 0;
#else
    return mprotect(page, TRAMPOLINE_PAGE,
                    executable ? (PROT_READ | PROT_EXEC) : (PROT_READ | PROT_WRITE)) == 0;
#endif
}

bool trampoline_supported(void)
{
    return true;
}

void *trampoline_create(void *target, char return_kind)
{
    if (target == NULL)
        return NULL;

    uint8_t *page = (uint8_t *) reserve_page();
    if (page == NULL)
        return NULL;

    memcpy(page, k_prologue, sizeof(k_prologue));
    emit_zero_return(page + sizeof(k_prologue), return_kind);

    const uint64_t armed = 1;
    memcpy(page + TRAMPOLINE_ARMED, &armed, sizeof(armed));
    memcpy(page + TRAMPOLINE_TARGET, &target, sizeof(target));

    if (!protect_page(page, true))
    {
#if !defined(_WIN32)
        munmap(page, TRAMPOLINE_PAGE);
#endif
        return NULL;
    }

    return page;
}

void trampoline_disarm(void *trampoline)
{
    if (trampoline == NULL)
        return;

    uint8_t *page = (uint8_t *) trampoline;
    const uint64_t armed = 0;

    if (!protect_page(page, false))
        return;
    memcpy(page + TRAMPOLINE_ARMED, &armed, sizeof(armed));
    protect_page(page, true);
}

#else

bool trampoline_supported(void)
{
    return false;
}

void *trampoline_create(void *target, char return_kind)
{
    (void) target;
    (void) return_kind;
    return NULL;
}

void trampoline_disarm(void *trampoline)
{
    (void) trampoline;
}

#endif
