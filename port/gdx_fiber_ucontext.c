/* port/gdx_fiber_ucontext.c -- POSIX (ucontext) fiber backend for gdx_fiber.h.
 *
 * Mirrors the Win32 backend's semantics using getcontext/makecontext/swapcontext.
 *
 * makecontext passes its extra arguments as ints, so a 64-bit pointer would be split across two
 * int slots on LP64. The trampoline sidesteps that by taking ZERO makecontext arguments and
 * reading the target fiber from a thread-local instead -- safe on any ABI.
 *
 * makecontext() is deferred to the first gdx_fiber_switch (getcontext() runs at create time,
 * since it only needs a valid uc_stack) so sTrampolineArg is written in the same breath as the
 * resume, leaving no window where a stale value could be read.
 *
 * Stacks are mmap'd with a PROT_NONE guard page at the LOW end: stacks grow down, so an overflow
 * faults on the guard instead of silently smashing neighbouring memory. That approximates Win32's
 * stack-reserve-with-guard behavior closely enough for the scheduler.
 *
 * swapcontext performs a sigprocmask syscall on every switch to save/restore the signal mask. The
 * scheduler switches only a few times per VI frame, so that is acceptable; if it ever is not, the
 * escape hatch is a ~40-line x86-64 asm switch behind this same API, with no caller change in
 * n64_sched.c.
 */
/* ucontext.h functions (getcontext, makecontext, swapcontext) are deprecated on macOS but remain
   functional and are the only portable POSIX coroutine API. macOS only exposes them with
   _XOPEN_SOURCE defined, and a feature macro must come before the first header of the translation
   unit, so this block sits above every #include. Deprecation warnings are suppressed. */
#ifdef __APPLE__
#define _XOPEN_SOURCE
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

#include "gdx_fiber.h"

#include <stdlib.h>
#include <stdint.h>
#include <ucontext.h>
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

struct GdxFiber {
    ucontext_t    ctx;
    void*         mapping;    /* mmap base (guard page + usable stack); NULL for host */
    size_t        mappingLen; /* full mapping length for munmap */
    GdxFiberEntry entry;
    void*         arg;
    int           started;    /* makecontext done? */
    int           isHost;     /* converted-thread context (no mmap stack) */
};

/* Who is currently running on this OS thread: the save slot for swapcontext. */
static __thread GdxFiber* sCurrent = NULL;
/* Target of the pending first switch; read once at trampoline entry. */
static __thread GdxFiber* sTrampolineArg = NULL;

static void gdx_fiber_ucontext_trampoline(void) {
    GdxFiber* f = sTrampolineArg;
    f->entry(f->arg);
    /* Entry is not expected to return (the decomp threads reschedule through
     * __osCleanupThread), and uc_link is NULL by design, so there is nowhere to go. Abort
     * loudly rather than execute undefined behavior. */
    abort();
}

GdxFiber* gdx_fiber_convert_thread(void) {
    GdxFiber* f = (GdxFiber*) calloc(1, sizeof(*f));
    if (f == NULL) {
        return NULL;
    }
    f->isHost = 1;
    f->started = 1; /* the host context is already "running" */
    sCurrent = f;   /* the calling thread IS this context right now */
    return f;
}

GdxFiber* gdx_fiber_create(GdxFiberEntry entry, void* arg, size_t stackSize) {
    long pageSc;
    size_t page;
    size_t usable;
    size_t total;
    void* mem;
    GdxFiber* f;

    if (stackSize == 0) {
        stackSize = 1024u * 1024u; /* 1 MB default, matching Win32 */
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

    if (getcontext(&f->ctx) != 0) {
        munmap(mem, total);
        free(f);
        return NULL;
    }
    f->ctx.uc_link = NULL; /* entry never returns; see trampoline */
    f->ctx.uc_stack.ss_sp = (char*) mem + page; /* above the guard page */
    f->ctx.uc_stack.ss_size = usable;
    f->ctx.uc_stack.ss_flags = 0;

    f->mapping = mem;
    f->mappingLen = total;
    f->entry = entry;
    f->arg = arg;
    f->started = 0; /* makecontext deferred to first switch */
    f->isHost = 0;
    return f;
}

void gdx_fiber_switch(GdxFiber* to) {
    GdxFiber* from = sCurrent;
    sCurrent = to;
    if (!to->started) {
        to->started = 1;
        /* Immediately before arming/resuming, so the thread-local read at trampoline entry
         * sees exactly this fiber. */
        sTrampolineArg = to;
        makecontext(&to->ctx, gdx_fiber_ucontext_trampoline, 0);
    }
    swapcontext(&from->ctx, &to->ctx);
}

unsigned long gdx_fiber_current_thread_id(void) {
#ifdef SYS_gettid
    return (unsigned long) syscall(SYS_gettid);
#else
    return (unsigned long) (uintptr_t) pthread_self();
#endif
}

#ifdef __APPLE__
#pragma clang diagnostic pop
#endif
