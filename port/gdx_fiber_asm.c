/* port/gdx_fiber_asm.c -- Android fiber backend for gdx_fiber.h.
 *
 * Same contract and the same stack discipline as the POSIX ucontext backend
 * (port/gdx_fiber_ucontext.c); only the context switch itself differs, because bionic has no
 * getcontext/makecontext/swapcontext to build one out of -- see the header comment in
 * port/gdx_fiber_switch.S. Read the two files together: everything here is the ucontext
 * backend with `ucontext_t ctx` replaced by a parked stack pointer.
 *
 * Stacks are mmap'd with a PROT_NONE guard page at the LOW end, so an overflow faults on the
 * guard instead of quietly smashing the neighbouring mapping. On a phone that matters more
 * than on a desktop: the decomp threads have no headroom to spare and a silent corruption
 * here surfaces as an unattributable crash minutes later.
 *
 * A fresh fiber never takes a special "start" path. Its first switch is an ordinary restore
 * from a frame laid out by hand at the top of its own stack, with the saved return address
 * pointing at the trampoline. That keeps gdx_fiber_switch free of a branch that would only
 * ever be taken once per fiber, and it is why the frame arithmetic below has to agree with
 * the register list in the .S file exactly.
 */
#include "gdx_fiber.h"

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/syscall.h>

#ifndef MAP_STACK
#define MAP_STACK 0
#endif
#ifndef MAP_ANONYMOUS
#ifdef MAP_ANON
#define MAP_ANONYMOUS MAP_ANON
#else
#define MAP_ANONYMOUS 0
#endif
#endif

/* Implemented in gdx_fiber_switch.S. */
void gdx_fiber_asm_switch(void** saveSp, void* newSp);

#if defined(__aarch64__)
/* x19-x28, x29, x30 (12 * 8) + d8-d15 (8 * 8). x30 -- the address the final `ret` jumps to --
 * sits at byte 88, immediately after x29 in the `stp x29, x30` pair. */
#define GDX_FIBER_FRAME_BYTES 160
#define GDX_FIBER_RETADDR_OFFSET 88
#elif defined(__x86_64__)
/* Six pushes (r15, r14, r13, r12, rbx, rbp in pop order) followed by the return address the
 * trailing `ret` consumes, so the address lives at byte 48. */
#define GDX_FIBER_FRAME_BYTES 56
#define GDX_FIBER_RETADDR_OFFSET 48
#else
#error "gdx_fiber_asm.c: no frame layout for this architecture"
#endif

struct GdxFiber {
    void*         sp;         /* parked stack pointer; meaningless while this fiber is running */
    void*         mapping;    /* mmap base (guard page + usable stack); NULL for the host context */
    size_t        mappingLen; /* full mapping length for munmap */
    GdxFiberEntry entry;
    void*         arg;
    int           isHost;     /* converted-thread context (no mmap stack) */
};

/* Who is currently running on this OS thread: the save slot for the switch. */
static __thread GdxFiber* sCurrent = NULL;
/* Target of the pending first switch; read once at trampoline entry. Same reason as the
 * ucontext backend: passing the fiber in a register would depend on the argument-passing ABI
 * of a frame we forged ourselves, and a thread-local is correct on every architecture. */
static __thread GdxFiber* sTrampolineArg = NULL;

static void gdx_fiber_asm_trampoline(void) {
    GdxFiber* f = sTrampolineArg;
    f->entry(f->arg);
    /* Entry is not expected to return (the decomp threads reschedule through
     * __osCleanupThread), and the forged frame has no caller to return to. Abort loudly
     * rather than execute undefined behavior. */
    abort();
}

GdxFiber* gdx_fiber_convert_thread(void) {
    GdxFiber* f = (GdxFiber*) calloc(1, sizeof(*f));
    if (f == NULL) {
        return NULL;
    }
    f->isHost = 1;
    sCurrent = f; /* the calling thread IS this context right now */
    return f;
}

GdxFiber* gdx_fiber_create(GdxFiberEntry entry, void* arg, size_t stackSize) {
    long pageSc;
    size_t page;
    size_t usable;
    size_t total;
    void* mem;
    uintptr_t top;
    uintptr_t frame;
    GdxFiber* f;

    if (stackSize == 0) {
        stackSize = 1024u * 1024u; /* 1 MB default, matching the other backends */
    }

    pageSc = sysconf(_SC_PAGESIZE);
    page = (pageSc > 0) ? (size_t) pageSc : 4096u;

    usable = (stackSize + page - 1u) & ~(page - 1u);
    total = usable + page;

    f = (GdxFiber*) calloc(1, sizeof(*f));
    if (f == NULL) {
        return NULL;
    }

    mem = mmap(NULL, total, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (mem == MAP_FAILED) {
        free(f);
        return NULL;
    }

    if (mprotect(mem, page, PROT_NONE) != 0) {
        /* Non-fatal: without the guard an overflow is undiagnosed, but the fiber still works. */
    }

    /* Lay the first frame out at the top of the usable stack, 16-byte aligned. Both ABIs here
     * require sp to stay 16-byte aligned, and both reach the trampoline through the switch's
     * trailing `ret`, so aligning the frame base is what makes the alignment hold on arrival. */
    top = (uintptr_t) mem + total;
    frame = (top - GDX_FIBER_FRAME_BYTES) & ~(uintptr_t) 15u;
    memset((void*) frame, 0, GDX_FIBER_FRAME_BYTES);
    *(void**) (frame + GDX_FIBER_RETADDR_OFFSET) = (void*) &gdx_fiber_asm_trampoline;

    f->sp = (void*) frame;
    f->mapping = mem;
    f->mappingLen = total;
    f->entry = entry;
    f->arg = arg;
    f->isHost = 0;
    return f;
}

void gdx_fiber_switch(GdxFiber* to) {
    GdxFiber* from = sCurrent;
    sCurrent = to;
    /* Written unconditionally, immediately before the switch: on a fiber's first resume the
     * trampoline reads it, and on every later resume nothing does, so a stale value is never
     * observable. Cheaper than tracking a `started` flag the switch would have to branch on. */
    sTrampolineArg = to;
    gdx_fiber_asm_switch(&from->sp, to->sp);
}

unsigned long gdx_fiber_current_thread_id(void) {
#ifdef SYS_gettid
    return (unsigned long) syscall(SYS_gettid);
#else
    return (unsigned long) (uintptr_t) pthread_self();
#endif
}
