/* port/gdx_fiber_switch.S -- register-level context switch for the Android fiber backend.
 *
 * Bionic ships <ucontext.h> as a thin alias for <sys/ucontext.h>: the mcontext TYPES are
 * there, but getcontext/makecontext/swapcontext are NOT implemented anywhere in libc. The
 * POSIX backend (port/gdx_fiber_ucontext.c) therefore does not link on Android, and the
 * scheduler in port/n64_sched.c has no way to run the decomp's threads without one.
 *
 * This is the missing primitive, and only that: save the callee-saved registers of the
 * running context onto its own stack, park the resulting stack pointer, adopt another
 * context's parked stack pointer, and restore. Everything above it -- stack allocation,
 * guard pages, the trampoline, the thread-affinity id -- stays in C, in
 * port/gdx_fiber_asm.c, which is what the two backends have in common.
 *
 * A fresh fiber is started by hand-building one of these frames at the top of its stack
 * with the return address pointing at the trampoline, so the very first switch INTO a
 * fiber is an ordinary restore-and-return; there is no separate "start" path.
 *
 * The signal mask is deliberately not saved. swapcontext() carries a sigprocmask syscall
 * on every switch to preserve it; the scheduler switches often enough per VI frame that
 * the syscall is pure overhead, and nothing in the port masks signals per thread.
 *
 * void gdx_fiber_asm_switch(void** saveSp, void* newSp);
 *     saveSp  where to store the outgoing context's stack pointer
 *     newSp   the incoming context's previously stored stack pointer
 */

#if defined(__aarch64__)

    .text
    .globl gdx_fiber_asm_switch
    .hidden gdx_fiber_asm_switch
    .type gdx_fiber_asm_switch, %function
    .align 4
gdx_fiber_asm_switch:
    /* AAPCS64 callee-saved: x19-x28, x29 (fp), x30 (lr), and the low 64 bits of v8-v15.
       12 general + 8 vector = 160 bytes, which is already a multiple of the 16-byte
       stack alignment the ABI requires. */
    sub     sp, sp, #160
    stp     x19, x20, [sp, #0]
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     x29, x30, [sp, #80]
    stp     d8,  d9,  [sp, #96]
    stp     d10, d11, [sp, #112]
    stp     d12, d13, [sp, #128]
    stp     d14, d15, [sp, #144]

    mov     x2, sp
    str     x2, [x0]          /* park the outgoing stack pointer */
    mov     sp, x1            /* adopt the incoming one */

    ldp     x19, x20, [sp, #0]
    ldp     x21, x22, [sp, #16]
    ldp     x23, x24, [sp, #32]
    ldp     x25, x26, [sp, #48]
    ldp     x27, x28, [sp, #64]
    ldp     x29, x30, [sp, #80]
    ldp     d8,  d9,  [sp, #96]
    ldp     d10, d11, [sp, #112]
    ldp     d12, d13, [sp, #128]
    ldp     d14, d15, [sp, #144]
    add     sp, sp, #160
    ret
    .size gdx_fiber_asm_switch, . - gdx_fiber_asm_switch

#elif defined(__x86_64__)

    .text
    .globl gdx_fiber_asm_switch
    .hidden gdx_fiber_asm_switch
    .type gdx_fiber_asm_switch, @function
    .align 16
gdx_fiber_asm_switch:
    /* System V AMD64 callee-saved: rbx, rbp, r12-r15. The call itself has already
       pushed the return address, so the frame is 6 pushes + that = 56 bytes, and the
       stack was 16-byte aligned before the call -- see the matching arithmetic in
       gdx_fiber_asm.c when a fresh frame is laid out. */
    pushq   %rbp
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15

    movq    %rsp, (%rdi)      /* park the outgoing stack pointer */
    movq    %rsi, %rsp        /* adopt the incoming one */

    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    ret
    .size gdx_fiber_asm_switch, . - gdx_fiber_asm_switch

#else
#error "gdx_fiber_switch.S: no context switch for this architecture (arm64-v8a and x86_64 only)"
#endif

/* Mark the stack non-executable; without this the linker conservatively marks the whole
   shared object's stack executable because an assembly file said nothing about it. */
#if defined(__linux__)
    .section .note.GNU-stack, "", %progbits
#endif
