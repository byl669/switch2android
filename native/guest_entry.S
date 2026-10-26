// AAPCS64 boundary. Guest x18 is unrestricted; restore the host value before C.
.text
.p2align 2
.macro FIND_THREAD
    mrs x9, tpidr_el0
    adrp x10, ot2_threads
    add x10, x10, :lo12:ot2_threads
    mov x12, #256
1:  ldar x11, [x10]
    cmp x11, x9
    b.eq 2f
    add x10, x10, #64
    subs x12, x12, #1
    b.ne 1b
    mov x10, xzr
2:
.endm
.global ot2_signal_entry
.type ot2_signal_entry, %function
ot2_signal_entry:
    FIND_THREAD
    cbz x10, 3f
    ldr x18, [x10, #8]
3:
    b ot2_signal_handler
.size ot2_signal_entry, .-ot2_signal_entry

.global ot2_guest_call
.type ot2_guest_call, %function
ot2_guest_call: // entry, thread context, eight argument words, optional nested stack top
    sub sp, sp, #288
    stp x18, x19, [sp, #0]
    stp x20, x21, [sp, #16]
    stp x22, x23, [sp, #32]
    stp x24, x25, [sp, #48]
    stp x26, x27, [sp, #64]
    stp x28, x29, [sp, #80]
    str x30, [sp, #96]
    stp q8, q9, [sp, #112]
    stp q10, q11, [sp, #144]
    stp q12, q13, [sp, #176]
    stp q14, q15, [sp, #208]
    mrs x9, fpcr
    mrs x10, fpsr
    stp x9, x10, [sp, #240]
    ldr x9, [x1, #16]
    stp x9, x1, [sp, #256]
    mov x9, sp
    str x9, [x1, #16]
    str x18, [x1, #8]
    mov x16, x0
    mov x17, x2
    ldp x9, x10, [x1, #40]
    add x9, x9, x10
    cmp x3, #0
    csel x9, x9, x3, eq
    mov sp, x9
    ldp x0, x1, [x17, #0]
    ldp x2, x3, [x17, #16]
    ldp x4, x5, [x17, #32]
    ldp x6, x7, [x17, #48]
    mov x8, #0
    mov x18, #0
    mov x29, #0
    adr x30, 3f
    br x16
3:  FIND_THREAD
    ldr x9, [x10, #16]
    mov sp, x9
    ldp x9, x10, [sp, #256]
    str x9, [x10, #16]
    ldp x9, x10, [sp, #240]
    msr fpcr, x9
    msr fpsr, x10
    ldp q8, q9, [sp, #112]
    ldp q10, q11, [sp, #144]
    ldp q12, q13, [sp, #176]
    ldp q14, q15, [sp, #208]
    ldp x18, x19, [sp, #0]
    ldp x20, x21, [sp, #16]
    ldp x22, x23, [sp, #32]
    ldp x24, x25, [sp, #48]
    ldp x26, x27, [sp, #64]
    ldp x28, x29, [sp, #80]
    ldr x30, [sp, #96]
    add sp, sp, #288
    ret
.size ot2_guest_call, .-ot2_guest_call

// Run a service below the suspended native frame. x19 keeps the small guest
// stack while AAPCS64 callees, including nested guest callbacks, preserve it.
.global ot2_host_stack_call
.type ot2_host_stack_call, %function
ot2_host_stack_call:
    stp x29,x30,[sp,#-32]!
    stp x19,x20,[sp,#16]
    mov x29,sp
    mov x19,sp
    mov x16,x0
    mov x0,x1
    mov sp,x2
    blr x16
    mov sp,x19
    ldp x19,x20,[sp,#16]
    ldp x29,x30,[sp],#32
    ret
.size ot2_host_stack_call, .-ot2_host_stack_call

// ExitThread can arrive from a nested signal or native callback. Restore the
// outermost pthread frame before publishing thread completion and running TLS
// destructors; those destructors own the guest/signal stack allocations.
.global guest_thread_exit
.type guest_thread_exit, %function
guest_thread_exit:
    FIND_THREAD
    cbz x10, 9f
    ldr x9, [x10, #16]
    cbz x9, 9f
3:  ldr x11, [x9, #256]
    cbz x11, 4f
    mov x9, x11
    b 3b
4:  mov sp, x9
    str xzr, [x10, #16]
    ldp x9, x10, [sp, #240]
    msr fpcr, x9
    msr fpsr, x10
    ldp q8, q9, [sp, #112]
    ldp q10, q11, [sp, #144]
    ldp q12, q13, [sp, #176]
    ldp q14, q15, [sp, #208]
    ldp x18, x19, [sp, #0]
    ldp x20, x21, [sp, #16]
    ldp x22, x23, [sp, #32]
    ldp x24, x25, [sp, #48]
    ldp x26, x27, [sp, #64]
    ldp x28, x29, [sp, #80]
    ldr x30, [sp, #96]
    add sp, sp, #288
    b svc_thread_exit_on_host
9:  brk #0x7ff
.size guest_thread_exit, .-guest_thread_exit

// Native AAPCS64 call from a saved guest context. Preserve integer and SIMD
// arguments, stack arguments, indirect-result x8, and both return register sets.
.global ot2_host_invoke
.type ot2_host_invoke, %function
ot2_host_invoke:
    stp x29,x30,[sp,#-48]!
    stp x19,x20,[sp,#16]
    stp x21,x22,[sp,#32]
    mov x29,sp
    mov x19,x1
    mov x20,x2
    mov x21,x0
    sub sp,sp,#256
    mov x9,#0
4:  ldp x10,x11,[x3]
    stp x10,x11,[sp]
    add x3,x3,#16
    add sp,sp,#16
    add x9,x9,#16
    cmp x9,#256
    b.ne 4b
    sub sp,sp,#256
    ldp q0,q1,[x20,#0]
    ldp q2,q3,[x20,#32]
    ldp q4,q5,[x20,#64]
    ldp q6,q7,[x20,#96]
    ldp x0,x1,[x19,#0]
    ldp x2,x3,[x19,#16]
    ldp x4,x5,[x19,#32]
    ldp x6,x7,[x19,#48]
    ldr x8,[x19,#64]
    blr x21
    stp x0,x1,[x19,#0]
    stp q0,q1,[x20,#0]
    stp q2,q3,[x20,#32]
    add sp,sp,#256
    ldp x19,x20,[sp,#16]
    ldp x21,x22,[sp,#32]
    ldp x29,x30,[sp],#48
    ret
.size ot2_host_invoke, .-ot2_host_invoke

// Fast native AAPCS bridge. x16 is the gate index. Keep guest x18 and the
// original stack arguments intact while using the real Android x18 in C/C++.
.global ot2_native_entry
.hidden ot2_native_function
.type ot2_native_entry, %function
ot2_native_entry:
    adrp x17,ot2_register_gates_enabled
    add x17,x17,:lo12:ot2_register_gates_enabled
    ldar w17,[x17]
    cbz w17,ot2_native_generic
    adrp x17,ot2_native_abis
    add x17,x17,:lo12:ot2_native_abis
    add x17,x17,x16,lsl #2
    ldar w17,[x17]
    cbz w17,ot2_native_generic
    cmp w17,#1
    b.eq ot2_native_integer
    b ot2_native_registers
ot2_native_generic:
    sub sp,sp,#512
    stp x0,x1,[sp,#256]
    stp x2,x3,[sp,#272]
    stp x4,x5,[sp,#288]
    stp x6,x7,[sp,#304]
    stp x8,x18,[sp,#320]
    stp x29,x30,[sp,#336]
    stp q0,q1,[sp,#368]
    stp q2,q3,[sp,#400]
    stp q4,q5,[sp,#432]
    stp q6,q7,[sp,#464]
    mrs x9,fpcr
    mrs x10,fpsr
    stp x9,x10,[sp,#496]
    FIND_THREAD
    cbz x10,9f
    ldr x18,[x10,#8]
    ldp x9,x11,[x10,#40]
    add x9,x9,x11
    add x11,sp,#512
    sub x9,x9,x11
    mov x11,#256
    cmp x9,x11
    csel x9,x9,x11,lo
    str x9,[sp,#352]
    add x29,sp,#336
    msr fpcr,xzr
    msr fpsr,xzr
    mov x0,x16
    bl ot2_native_function
    mov x16,x0
    // Copy only the ABI argument area, below all saved registers.
    add x9,sp,#512
    mov x12,#0
    ldr x14,[sp,#352]
    cbz x14,7f
8:  ldp x10,x11,[x9],#16
    add x13,sp,x12
    stp x10,x11,[x13]
    add x12,x12,#16
    cmp x12,x14
    b.lo 8b
7:  ldp q0,q1,[sp,#368]
    ldp q2,q3,[sp,#400]
    ldp q4,q5,[sp,#432]
    ldp q6,q7,[sp,#464]
    ldp x0,x1,[sp,#256]
    ldp x2,x3,[sp,#272]
    ldp x4,x5,[sp,#288]
    ldp x6,x7,[sp,#304]
    ldr x8,[sp,#320]
    blr x16
    ldp x9,x10,[sp,#496]
    msr fpcr,x9
    msr fpsr,x10
    ldr x18,[sp,#328]
    ldp x29,x30,[sp,#336]
    add sp,sp,#512
    ret
9:  brk #0x707
.size ot2_native_entry, .-ot2_native_entry

// Stand-in for a native API the host has no implementation for. It is called
// with the caller's own arguments already restored, so ignoring them is safe;
// both result registers are cleared so a caller that reads an integer or a
// float back sees zero rather than whatever the lookup left behind. Used only
// in diagnostic mode, where the goal is to reach the next real blocker.
.global ot2_native_zero
.hidden ot2_native_zero
.type ot2_native_zero, %function
ot2_native_zero:
    mov x0, xzr
    fmov d0, xzr
    fmov s0, wzr
    ret
.size ot2_native_zero, .-ot2_native_zero

// Typed scalar-only AAPCS64 calls never have stack arguments. The integer
// variant also needs no FP argument saves around the native-function lookup.
// Unknown, aggregate, variadic and stack-argument prototypes use generic above.
.macro REGISTER_GATE name, frame, fp
.type \name, %function
\name:
    sub sp,sp,#\frame
    stp x0,x1,[sp,#0]
    stp x2,x3,[sp,#16]
    stp x4,x5,[sp,#32]
    stp x6,x7,[sp,#48]
    stp x8,x18,[sp,#64]
    stp x29,x30,[sp,#80]
    mrs x9,fpcr
    mrs x10,fpsr
    stp x9,x10,[sp,#96]
    .if \fp
    stp q0,q1,[sp,#112]
    stp q2,q3,[sp,#144]
    stp q4,q5,[sp,#176]
    stp q6,q7,[sp,#208]
    .endif
    FIND_THREAD
    cbz x10,9f
    ldr x18,[x10,#8]
    add x29,sp,#80
    msr fpcr,xzr
    msr fpsr,xzr
    mov x0,x16
    bl ot2_native_function
    mov x16,x0
    .if \fp
    ldp q0,q1,[sp,#112]
    ldp q2,q3,[sp,#144]
    ldp q4,q5,[sp,#176]
    ldp q6,q7,[sp,#208]
    .endif
    ldp x0,x1,[sp,#0]
    ldp x2,x3,[sp,#16]
    ldp x4,x5,[sp,#32]
    ldp x6,x7,[sp,#48]
    ldr x8,[sp,#64]
    blr x16
    ldp x9,x10,[sp,#96]
    msr fpcr,x9
    msr fpsr,x10
    ldr x18,[sp,#72]
    ldp x29,x30,[sp,#80]
    add sp,sp,#\frame
    ret
9:  brk #0x707
.size \name, .-\name
.endm
REGISTER_GATE ot2_native_integer,112,0
REGISTER_GATE ot2_native_registers,240,1

.global ot2_abi_fixture
.type ot2_abi_fixture, %function
ot2_abi_fixture:
    sub sp,sp,#48
    stp x29,x30,[sp,#16]
    str x18,[sp,#32]
    mov x18,#1972
    mov x16,x0
    mov x0,#1
    mov x1,#2
    mov x2,#3
    mov x3,#4
    mov x4,#5
    mov x5,#6
    mov x6,#7
    mov x7,#8
    mov x8,#9
    str x8,[sp]
    fmov d0,#1.5
    fmov d1,#2.5
    blr x16
    fcvtzu x0,d0
    cmp x18,#1972
    mov x9,#-1
    csel x0,x0,x9,eq
    ldr x18,[sp,#32]
    ldp x29,x30,[sp,#16]
    add sp,sp,#48
    ret
.size ot2_abi_fixture, .-ot2_abi_fixture
.section .note.GNU-stack,"",%progbits

.text
.p2align 2
// System-register and intercepted SDK gate. Preserve all guest state.
.global ot2_system_entry
.hidden ot2_system_dispatch
.type ot2_system_entry, %function
ot2_system_entry:
    sub sp,sp,#800
    stp x0,x1,[sp,#0]
    stp x2,x3,[sp,#16]
    stp x4,x5,[sp,#32]
    stp x6,x7,[sp,#48]
    stp x8,x9,[sp,#64]
    stp x10,x11,[sp,#80]
    stp x12,x13,[sp,#96]
    stp x14,x15,[sp,#112]
    stp x16,x17,[sp,#128]
    stp x18,x19,[sp,#144]
    stp x20,x21,[sp,#160]
    stp x22,x23,[sp,#176]
    stp x24,x25,[sp,#192]
    stp x26,x27,[sp,#208]
    stp x28,x29,[sp,#224]
    stp x30,x30,[sp,#240]
    mrs x9,nzcv
    mrs x10,fpcr
    mrs x11,fpsr
    stp x9,x10,[sp,#256]
    str x11,[sp,#272]
    stp q0,q1,[sp,#288]
    stp q2,q3,[sp,#320]
    stp q4,q5,[sp,#352]
    stp q6,q7,[sp,#384]
    stp q8,q9,[sp,#416]
    stp q10,q11,[sp,#448]
    stp q12,q13,[sp,#480]
    stp q14,q15,[sp,#512]
    stp q16,q17,[sp,#544]
    stp q18,q19,[sp,#576]
    stp q20,q21,[sp,#608]
    stp q22,q23,[sp,#640]
    stp q24,q25,[sp,#672]
    stp q26,q27,[sp,#704]
    stp q28,q29,[sp,#736]
    stp q30,q31,[sp,#768]
    FIND_THREAD
    cbz x10,9f
    ldr x18,[x10,#8]
    msr fpcr,xzr
    msr fpsr,xzr
    ldr x0,[sp,#128]
    mov x1,sp
    bl ot2_system_dispatch
    ldp q0,q1,[sp,#288]
    ldp q2,q3,[sp,#320]
    ldp q4,q5,[sp,#352]
    ldp q6,q7,[sp,#384]
    ldp q8,q9,[sp,#416]
    ldp q10,q11,[sp,#448]
    ldp q12,q13,[sp,#480]
    ldp q14,q15,[sp,#512]
    ldp q16,q17,[sp,#544]
    ldp q18,q19,[sp,#576]
    ldp q20,q21,[sp,#608]
    ldp q22,q23,[sp,#640]
    ldp q24,q25,[sp,#672]
    ldp q26,q27,[sp,#704]
    ldp q28,q29,[sp,#736]
    ldp q30,q31,[sp,#768]
    ldp x9,x10,[sp,#256]
    ldr x11,[sp,#272]
    msr nzcv,x9
    msr fpcr,x10
    msr fpsr,x11
    ldp x0,x1,[sp,#0]
    ldp x2,x3,[sp,#16]
    ldp x4,x5,[sp,#32]
    ldp x6,x7,[sp,#48]
    ldp x8,x9,[sp,#64]
    ldp x10,x11,[sp,#80]
    ldp x12,x13,[sp,#96]
    ldp x14,x15,[sp,#112]
    ldp x16,x17,[sp,#128]
    ldp x18,x19,[sp,#144]
    ldp x20,x21,[sp,#160]
    ldp x22,x23,[sp,#176]
    ldp x24,x25,[sp,#192]
    ldp x26,x27,[sp,#208]
    ldp x28,x29,[sp,#224]
    ldr x30,[sp,#248]
    add sp,sp,#800
    ret
9:  brk #0x707
.size ot2_system_entry, .-ot2_system_entry
