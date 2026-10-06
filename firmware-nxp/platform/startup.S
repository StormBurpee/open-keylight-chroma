.syntax unified
.cpu cortex-m0
.thumb

.section .vectors,"a",%progbits
.balign 256
.global nxp_vectors
nxp_vectors:
    .word _stack_top
    .word Reset_Handler
    .rept 13
    .word Default_Handler
    .endr
    .word SysTick_Handler
    .rept 14
    .word Default_Handler
    .endr
    .word SSP1_Handler
    .rept 17
    .word Default_Handler
    .endr

.section .text.Reset_Handler,"ax",%progbits
.thumb_func
.global Reset_Handler
Reset_Handler:
    cpsid i
    bl nxp_reset_c
    b Default_Handler

.section .text.Default_Handler,"ax",%progbits
.thumb_func
.global Default_Handler
Default_Handler:
    cpsid i
    bl nxp_panic
1:  wfi
    b 1b
