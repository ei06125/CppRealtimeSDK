/*
 * Construct the vector table and initialize memory before entering the C++ runtime.
 * Enable the FPU before static initialization can execute floating-point instructions.
 */
.syntax unified
.cpu cortex-m4
.thumb
.section .isr_vector,"a",%progbits
.word _estack
.word Reset_Handler
.rept 9
.word Default_Handler
.endr
.word SVC_Handler
.word Default_Handler
.word 0
.word PendSV_Handler
.word SysTick_Handler
.rept 48
.word Default_Handler
.endr
.section .text.Reset_Handler,"ax",%progbits
.thumb_func
.global Reset_Handler
Reset_Handler:
 ldr r0, =_sidata
 ldr r1, =_sdata
 ldr r2, =_edata
1: cmp r1, r2
 bcs 2f
 ldr r3, [r0], #4
 str r3, [r1], #4
 b 1b
2: ldr r1, =_sbss
 ldr r2, =_ebss
 movs r3, #0
3: cmp r1, r2
 bcs 4f
 str r3, [r1], #4
 b 3b
4: ldr r0, =0xe000ed88
 ldr r1, [r0]
 orr r1, r1, #0xf00000
 str r1, [r0]
 dsb
 isb
 bl initialise_monitor_handles
 bl __libc_init_array
 bl main
 b Default_Handler
.thumb_func
.global Default_Handler
Default_Handler:
 bl board_fault
 b Default_Handler
.thumb_func
.global _init
_init:
 bx lr
.thumb_func
.global _fini
_fini:
 bx lr
